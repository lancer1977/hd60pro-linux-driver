/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Frame delivery + MSI interrupt handling (RE_FINDINGS.md M17).
 *
 *  The card DMAs encoded H.264 frames into host-allocated buffers via a
 *  PCIe iATU outbound window it programs from the physical addresses we
 *  hand it with the buffer-setter mailbox opcodes. SET_VIC configures the
 *  encoder, START_STREAMING arms it, and a finished frame signals an EVENT
 *  (BAR0+0x30) whose token
 *  (BAR0+0x40, low 3 bits) names the completed buffer.
 *
 *  The endpoint does not mirror its card-side encoded-byte count to a verified
 *  host register: BAR0 payload words are packed slot counters, not lengths.
 *  Delivery therefore snapshots each one-shot event before ACK and infers a
 *  bounded, aligned extent from a per-buffer poison suffix before copying.
 */

#include <linux/delay.h>	/* msleep() for the SET_VIC -> START_STREAMING gap */
#include <linux/iommu.h>	/* M26: 4GiB-aligned IOVA placement of the stream bufs */
#include <linux/kthread.h>	/* M36: write-extent watcher */

#include "mz0380.h"

/* forward */
static void mz0380_drain_work_fn(struct work_struct *w);
static void mz0380_enc_stat_ack(struct mz0380_dev *dev);
static void mz0380_frame_buffer_repoison(struct mz0380_dev *dev, u32 idx);

/* SET_VIC below configures video channel zero; channel_done sets its EVENT bit. */
#define MZ0380_VIDEO_EVENT_BIT BIT(MZ0380_STREAM_VIDEO_CHANNEL)

#define MZ0380_ENC_VALID_FPS	BIT(0)
#define MZ0380_ENC_VALID_GOP	BIT(1)
#define MZ0380_ENC_VALID_BITRATE	BIT(6)
#define MZ0380_ENC_SAFE_FPS	60
#define MZ0380_ENC_SAFE_GOP	60
#define MZ0380_ENC_SAFE_BITRATE	(4 * 1024 * 1024)

int mz0380_dma_ring_alloc(struct mz0380_dev *dev, struct mz0380_ring *r,
			  u32 nr_entries, u32 entry_size)
{
	size_t total;

	if (!nr_entries || !entry_size)
		return -EINVAL;

	total = (size_t)nr_entries * entry_size;
	r->buf = dma_alloc_coherent(&dev->pci->dev, total, &r->dma,
				    GFP_KERNEL);
	if (!r->buf)
		return -ENOMEM;

	r->total_size = total;
	r->entry_size = entry_size;
	r->nr_entries = nr_entries;
	r->head = 0;
	r->tail_seen = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_dma_ring_alloc);

void mz0380_dma_ring_free(struct mz0380_dev *dev, struct mz0380_ring *r)
{
	if (r->buf) {
		dma_free_coherent(&dev->pci->dev, r->total_size,
				  r->buf, r->dma);
		r->buf = NULL;
	}
	r->total_size = 0;
	r->nr_entries = 0;
}
EXPORT_SYMBOL_GPL(mz0380_dma_ring_free);

/* --- streaming buffer set (video channel 0) ------------------------------ */

static void mz0380_extent_watch_stop(struct mz0380_dev *dev);

static void mz0380_stream_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	mz0380_extent_watch_stop(dev);	/* M36: never scan freed buffers */

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->stream_bufs[i];

		if (!b->va)
			continue;

		if (b->pages) {
			/* M26 path: our own pages behind an explicit mapping */
			if (b->dma && domain)
				iommu_unmap(domain, b->dma,
					    MZ0380_STREAM_BUF_SIZE);
			__free_pages(b->pages,
				     get_order(MZ0380_STREAM_BUF_SIZE));
			b->pages = NULL;
		} else {
			dma_free_coherent(&dev->pci->dev, MZ0380_STREAM_BUF_SIZE,
					  b->va, b->dma);
		}
		b->va = NULL;
		b->dma = 0;
	}
}

/*
 * M26. Put buffer i at IOVA (dma_iova_base + (i << 32)) so its low 32 bits are
 * zero, because that is the only half of the host target the card's outbound
 * window actually latches (M25, proven on hw: host_addr == (word0 << 32) +
 * aperture_offset). dma_alloc_coherent cannot be steered to a specific IOVA, so
 * we allocate raw pages and map them into the device's IOMMU domain ourselves.
 */
static int mz0380_stream_bufs_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int order = get_order(MZ0380_STREAM_BUF_SIZE);
	unsigned int i;
	int ret;

	if (mz0380_dma_iova_base & 0xffffffffULL) {
		pr_err("%s: dma_iova_base 0x%llx is not 4GiB-aligned - the card would DMA to (base>>32)<<32, missing the buffer\n",
		       dev->name, mz0380_dma_iova_base);
		return -EINVAL;
	}

	if (!domain) {
		pr_err("%s: no IOMMU domain for the device - cannot place buffers at a 4GiB-aligned IOVA\n",
		       dev->name);
		return -ENODEV;
	}
	if (domain->type != IOMMU_DOMAIN_DMA &&
	    domain->type != IOMMU_DOMAIN_DMA_FQ &&
	    domain->type != IOMMU_DOMAIN_UNMANAGED) {
		pr_err("%s: IOMMU domain type %u is pass-through/identity - no address translation to exploit (boot without iommu=pt)\n",
		       dev->name, domain->type);
		return -ENODEV;
	}

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->stream_bufs[i];
		/*
		 * M28: map card_frame_offset ABOVE the address we will
		 * advertise, so the card's constant +offset lands on the
		 * buffer base and word1 can stay 0 (no 32-bit wrap).
		 */
		dma_addr_t iova = mz0380_dma_iova_base + ((u64)i << 32) +
				  mz0380_card_frame_offset +
				  mz0380_dma_iova_offset;
		phys_addr_t phys;

		b->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->va = page_address(b->pages);
		phys = page_to_phys(b->pages);

		if (iommu_iova_to_phys(domain, iova)) {
			pr_err("%s: IOVA 0x%llx is already mapped - pick another dma_iova_base\n",
			       dev->name, (unsigned long long)iova);
			ret = -EBUSY;
			goto err;
		}

		ret = iommu_map(domain, iova, phys, MZ0380_STREAM_BUF_SIZE,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			pr_err("%s: iommu_map(0x%llx -> %pa, %u) failed (%d)\n",
			       dev->name, (unsigned long long)iova, &phys,
			       MZ0380_STREAM_BUF_SIZE, ret);
			goto err;
		}
		b->dma = iova;

		pr_info("%s: stream buf[%u] phys=%pa mapped at IOVA 0x%llx (high32=0x%08x, low32=0)\n",
			dev->name, i, &phys, (unsigned long long)iova,
			upper_32_bits(iova));
	}

	dev->stream_head = 0;
	return 0;

err:
	mz0380_stream_bufs_free(dev);
	return ret;
}

static int mz0380_stream_bufs_alloc(struct mz0380_dev *dev)
{
	unsigned int i;
	int ret;

	if (mz0380_dma_iova_remap) {
		ret = mz0380_stream_bufs_alloc_iova(dev);
		if (!ret)
			return 0;
		pr_err("%s: 4GiB-aligned IOVA setup failed (%d); refusing to arm unreachable stream buffers\n",
			dev->name, ret);
		return ret;
	}

	/* Explicit diagnostic opt-out; normal hardware requires the IOVA path. */
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		dev->stream_bufs[i].va =
			dma_alloc_coherent(&dev->pci->dev,
					   MZ0380_STREAM_BUF_SIZE,
					   &dev->stream_bufs[i].dma, GFP_KERNEL);
		if (!dev->stream_bufs[i].va) {
			mz0380_stream_bufs_free(dev);
			return -ENOMEM;
		}
	}
	dev->stream_head = 0;
	return 0;
}

/*
 * Hand the card the buffer physical addresses (M17): one 12-word command,
 * params = { channel, stride, (phys_hi,phys_lo) x NR_BUFS }. The card copies
 * these into its channels[] array and programs the iATU outbound window from
 * them, so its encoder DMA lands in our buffers.
 */
static int mz0380_stream_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS];
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = mz0380_set_buf_stride;	/* M27: cmd[0x8], runtime knob */
	/*
	 * ep.ko op2 copies cmd[0xc+8i]->channels[ch]+0 (iATU UPPER target reg 0x58 =
	 * host addr HIGH) and cmd[0x10+8i]->+4 (iATU LOWER reg 0x54 = host addr LOW).
	 * Pair order is {high32, low32} (RE_FINDINGS.md M23; polarity proven on hw -
	 * the cmd[0xc] value surfaced in the fault's high dword 0xfff8_0000_00000000).
	 * cmd[8] stride is ignored (M27: sending 0 changed nothing); the ATU limit is
	 * a hardcoded 32 MB aperture. The card
	 * cycles bufindex 1..N over 8-byte slots, so load all NR_BUFS. CRITICAL: this
	 * op2 must reach channels[] AFTER SET_VIC spawns the encoder and BEFORE op6 -
	 * START latches channels[] into the iATU (vpl_dmac StartTail); an op2 sent
	 * before the spawn is clobbered by the encoder's channel init -> low latches
	 * as reset 0 -> DMA to host ~0 + IOMMU fault.
	 */
	/*
	 * M25 probe (mz0380_buf_pair_swap): default pair order is
	 * {word0=high32, word1=low32}; swapped it is {word0=low32, word1=high32}.
	 * word0 reaches ELBI 0x58, word1 reaches ELBI 0x54. See the param's
	 * comment in mz0380-core.c for how to read the resulting fault address.
	 */
	/*
	 * M28: the card writes at (advertised target) + card_frame_offset, and
	 * the low half adds WITHOUT carrying into the high half. So advertise
	 * the buffer address minus that offset - with the M26/M28 mapping the
	 * buffers already sit card_frame_offset above a 4GiB boundary, which
	 * makes the advertised low word exactly 0 and the wrap unreachable.
	 */
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		u64 target = (u64)dev->stream_bufs[i].dma -
			     mz0380_card_frame_offset;
		u32 hi = upper_32_bits(target);
		u32 lo = lower_32_bits(target);

		params[2 + 2 * i]     = mz0380_buf_pair_swap ? lo : hi; /* -> ELBI 0x58 */
		params[2 + 2 * i + 1] = mz0380_buf_pair_swap ? hi : lo; /* -> ELBI 0x54 */

		pr_info("%s: SET_BUF[%u] buf=%pad target=0x%016llx -> slot {%08x, %08x}%s\n",
			dev->name, i, &dev->stream_bufs[i].dma, target,
			params[2 + 2 * i], params[2 + 2 * i + 1],
			mz0380_buf_pair_swap ? " (pair swapped)" : "");

		if (lo > U32_MAX - mz0380_card_frame_offset)
			pr_warn("%s: SET_BUF[%u] low word 0x%08x + offset 0x%x wraps 32 bits - the card will DMA to the wrong place\n",
				dev->name, i, lo, mz0380_card_frame_offset);
	}

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_2, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (ret || !mz0380_probe_windows)
		return ret;

	/*
	 * M32 probe. op2 fills window0 only. The card DMAs a raw ~3.1 MiB frame
	 * into window0 buf0 and then never signals channel_done - and ep.ko's
	 * event path is demonstrably armed (our command-done MSIs work and use
	 * the same one-shot token), so the card's userspace is simply not
	 * declaring a finished frame. The likely reason is that the ENCODER's
	 * bitstream destination is a different outbound window, one we have
	 * never programmed, so tinyvenc has nowhere to put its output.
	 *
	 * Point the other windows at the three buffers window0 is not using
	 * (the card only ever writes buf0), so no extra memory is needed. All
	 * four slots of a window get the same address: we only need to learn
	 * WHICH window comes alive, not where inside it the card writes.
	 */
	{
		static const struct {
			u8 opcode;
			u8 buf;
			const char *what;
		} windows[] = {
			{ MZ0380_CMD_SET_BUF_4, 1, "window1" },
			{ MZ0380_CMD_SET_BUF_5, 2, "window2" },
			{ MZ0380_CMD_SET_BUF_3, 3, "window3" },
		};
		unsigned int w;

		for (w = 0; w < ARRAY_SIZE(windows); w++) {
			u64 target = (u64)dev->stream_bufs[windows[w].buf].dma -
				     mz0380_card_frame_offset;
			int r;

			for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
				params[2 + 2 * i]     = upper_32_bits(target);
				params[2 + 2 * i + 1] = lower_32_bits(target);
			}

			r = mz0380_send_command(dev, windows[w].opcode, params,
						ARRAY_SIZE(params), NULL, 2000);
			pr_info("%s: probe %s (op 0x%02x) -> buf[%u] 0x%016llx ret=%d\n",
				dev->name, windows[w].what, windows[w].opcode,
				windows[w].buf, target, r);
		}
	}

	return 0;
}

/*
 * MSI ISR. The interrupt cause is the EVENT word (BAR0+0x30).  The central ACK
 * primitive snapshots command replies and frame payloads together under its
 * event lock before clearing this one-shot mailbox.
 *
 * Frame completion is deliberately not an else branch here.  EVENT may carry
 * command and frame bits together, and the command-poll path can consume the
 * same one-shot mailbox without entering this ISR.  mz0380_mb_ack_event()
 * invokes mz0380_handle_event_snapshot() before every ACK, which is the single
 * place that snapshots frame TOKEN/PAYLOAD and queues drain work.
 */
static irqreturn_t mz0380_isr(int irq, void *data)
{
	struct mz0380_dev *dev = data;
	u32 event;

	event = mz_mmio_read(dev, MZ0380_MB_EVENT);
	if (!event || event == 0xffffffff)
		return IRQ_NONE;

	atomic_inc(&dev->irq_count);

	mz0380_mb_ack_event(dev);
	return IRQ_HANDLED;
}

int mz0380_irq_request(struct mz0380_dev *dev)
{
	int nvec, ret;

	if (dev->irq_requested)
		return 0;

	ret = dma_set_mask_and_coherent(&dev->pci->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&dev->pci->dev,
						DMA_BIT_MASK(32));
		if (ret) {
			pr_err("%s: no usable DMA mask\n", dev->name);
			return ret;
		}
	}

	nvec = pci_alloc_irq_vectors(dev->pci, 1, 1,
				     PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (nvec < 1) {
		pr_err("%s: pci_alloc_irq_vectors failed (%d)\n",
		       dev->name, nvec);
		return nvec;
	}

	dev->msi_enabled = (nvec >= 1) && dev->pci->msi_enabled;
	dev->irq = pci_irq_vector(dev->pci, 0);

	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	dev->frame_events_accepting = false;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops = 0;
	dev->video_sequence = 0;
	dev->frame_poison_active = false;
	INIT_WORK(&dev->drain_work, mz0380_drain_work_fn);

	ret = request_irq(dev->irq, mz0380_isr,
			  dev->msi_enabled ? 0 : IRQF_SHARED,
			  dev->name, dev);
	if (ret) {
		pr_err("%s: request_irq(%d) failed (%d)\n",
		       dev->name, dev->irq, ret);
		pci_free_irq_vectors(dev->pci);
		return ret;
	}

	dev->irq_requested = true;
	pr_info("%s: IRQ %d ready (%s)\n",
		dev->name, dev->irq,
		dev->msi_enabled ? "MSI" : "INTx");
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_irq_request);

void mz0380_irq_release(struct mz0380_dev *dev)
{
	if (!dev->irq_requested)
		return;

	mz0380_dma_flush_events(dev);
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(dev->pci);
	dev->irq_requested = false;
	dev->msi_enabled = false;
}
EXPORT_SYMBOL_GPL(mz0380_irq_release);

int mz0380_dma_setup(struct mz0380_dev *dev)
{
	int ret;

	ret = mz0380_stream_bufs_alloc(dev);
	if (ret) {
		pr_err("%s: stream buffer alloc failed (%d)\n", dev->name, ret);
		return ret;
	}

	pr_info("%s: %u stream buffers x %u KiB (buf0 @ %pad)\n",
		dev->name, MZ0380_STREAM_NR_BUFS,
		MZ0380_STREAM_BUF_SIZE >> 10, &dev->stream_bufs[0].dma);

	pci_set_master(dev->pci);
	dev->dma_armed = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_dma_setup);

/*
 * M30. With zero IOMMU faults we can no longer see the card's writes: an IOMMU
 * only reports the ones that MISS. So look in the buffers themselves. Reports,
 * per buffer, the first 16 bytes and how far into it anything non-zero reaches
 * - which separates "the card is writing and only the completion signal is
 * missing" from "the card stopped writing".
 */
/*
 * M40. Acknowledge consumed bitstreams: write 0 to the card's per-stream
 * enc_stat bytes (BAR0 + 0x50 + idx). See MZ0380_MB_ENC_STATUS in
 * mz0380-reg.h for the protocol - without this ack the card's encoder
 * produces exactly one bitstream and then skips every subsequent frame.
 * The whole word is cleared: we drive a single channel with one bitstream,
 * so idx 0 is ours and the rest are unused.
 */
static void mz0380_enc_stat_ack(struct mz0380_dev *dev)
{
	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();
}

/*
 * Snapshot the frame mailbox while it still belongs to @event.  The endpoint
 * event channel is a one-shot ping-pong: ACK re-arms it and TOKEN/PAYLOAD may
 * then be overwritten immediately.  This hook is called centrally by
 * mz0380_mb_ack_event(), not just by the ISR, because command polling can win
 * the race and ACK a combined CMD_DONE|frame event itself.
 *
 * The helper is IRQ-safe and does not sleep.  Before IRQ/work setup and outside
 * a real stream frame_events_accepting is false, so early firmware ACKs are a
 * cheap no-op here.
 */
void mz0380_handle_event_snapshot(struct mz0380_dev *dev, u32 event)
{
	struct mz0380_frame_event snapshot;
	unsigned long flags;
	u16 next;
	bool dropped = false;
	bool ack_now = false;

	if (!(event & MZ0380_VIDEO_EVENT_BIT) ||
	    !READ_ONCE(dev->frame_events_accepting))
		return;

	snapshot.timestamp_ns = ktime_get_ns();
	snapshot.event = event;
	snapshot.token = mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN);
	snapshot.payload[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
	snapshot.payload[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);
	snapshot.payload[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	snapshot.enc_status = mz_mmio_read(dev, MZ0380_MB_ENC_STATUS);
	/* Pair the endpoint's completion notification with its coherent DMA. */
	dma_rmb();

	atomic_inc(&dev->irq_video_count);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	if (!dev->frame_events_accepting) {
		dev->frame_event_drops++;
		dropped = true;
	} else {
		next = (dev->frame_event_head + 1) %
		       MZ0380_FRAME_EVENT_FIFO_SIZE;
		if (next == dev->frame_event_tail) {
			dev->frame_event_drops++;
			dropped = true;
		} else {
			dev->frame_events[dev->frame_event_head] = snapshot;
			dev->frame_event_head = next;
			if (!dev->frame_event_drain_scheduled) {
				dev->frame_event_drain_scheduled = true;
				/*
				 * Publish and queue atomically with respect to streamoff:
				 * cancel_work_sync() must not miss a not-yet-queued kick.
				 */
				schedule_work(&dev->drain_work);
			}
		}
	}
	if (dropped) {
		/*
		 * Do not free the encoder while an older snapshot is queued or
		 * being copied: it could immediately reuse and overwrite that
		 * token.  The drain batch (or streamoff flush) ACKs after all
		 * older buffers have been consumed/re-poisoned.
		 */
		ack_now = !dev->frame_event_drain_scheduled &&
			  dev->frame_event_head == dev->frame_event_tail;
		if (!ack_now) {
			dev->frame_event_ack_deferred = true;
			if ((snapshot.token & 7) < MZ0380_STREAM_NR_BUFS)
				dev->frame_event_drop_tokens |=
					BIT(snapshot.token & 7);
		}
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	if (dropped) {
		if (ack_now)
			mz0380_enc_stat_ack(dev);
		pr_warn_ratelimited("%s: frame event dropped before drain (event=%08x token=%08x payload=%08x/%08x/%08x enc=%08x); enc_stat ACK %s\n",
				    dev->name, snapshot.event, snapshot.token,
				    snapshot.payload[0], snapshot.payload[1],
				    snapshot.payload[2], snapshot.enc_status,
				    ack_now ? "sent" : "deferred until older snapshots drain");
		return;
	}

}
EXPORT_SYMBOL_GPL(mz0380_handle_event_snapshot);

static void mz0380_frame_events_start(struct mz0380_dev *dev)
{
	unsigned long flags;

	if (!dev->irq_requested)
		return;

	/* Flush also resolves an ACK deferred by a full FIFO. */
	mz0380_dma_flush_events(dev);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops = 0;
	dev->video_sequence = 0;
	dev->frame_events_accepting = true;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);
}

void mz0380_dma_flush_events(struct mz0380_dev *dev)
{
	unsigned long flags;
	unsigned int pending = 0;
	unsigned int i;
	bool ack_deferred;
	u8 drop_tokens;

	if (!dev->irq_requested)
		return;

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_events_accepting = false;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	/* Process-context API: synchronize before vb2 buffers or DMA memory go away. */
	cancel_work_sync(&dev->drain_work);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	while (dev->frame_event_tail != dev->frame_event_head) {
		u32 token = dev->frame_events[dev->frame_event_tail].token & 7;

		if (token < MZ0380_STREAM_NR_BUFS)
			dev->frame_event_drop_tokens |= BIT(token);
		dev->frame_event_tail = (dev->frame_event_tail + 1) %
					MZ0380_FRAME_EVENT_FIFO_SIZE;
		pending++;
	}
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	ack_deferred = dev->frame_event_ack_deferred;
	dev->frame_event_ack_deferred = false;
	drop_tokens = dev->frame_event_drop_tokens;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops += pending;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
		if (drop_tokens & BIT(i))
			mz0380_frame_buffer_repoison(dev, i);

	if (pending || ack_deferred || drop_tokens)
		mz0380_enc_stat_ack(dev);
}
EXPORT_SYMBOL_GPL(mz0380_dma_flush_events);

/* --- M36 write-extent watch ---------------------------------------------- */

/* M37: poison value is a module param so a collision can be ruled out */
static inline u8 mz0380_poison_b(void)
{
	return mz0380_poison_byte & 0xff;
}

static inline u32 mz0380_poison_w(void)
{
	return 0x01010101u * mz0380_poison_b();
}

static inline u32 mz0380_frame_poison_w(const struct mz0380_dev *dev)
{
	return 0x01010101u * dev->frame_poison_byte;
}

static inline u64 mz0380_frame_poison_q(const struct mz0380_dev *dev)
{
	return 0x0101010101010101ULL * dev->frame_poison_byte;
}

/*
 * Real H.264 completion uses the untouched poison suffix as the only
 * host-visible length delimiter.  Re-poisoning happens while enc_stat is still
 * owned by the host, before its ACK permits the endpoint to reuse the slot.
 */
static void mz0380_frame_buffer_repoison(struct mz0380_dev *dev, u32 idx)
{
	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->stream_bufs[idx].va ||
	    !smp_load_acquire(&dev->frame_poison_active))
		return;

	memset(dev->stream_bufs[idx].va, dev->frame_poison_byte,
	       MZ0380_STREAM_BUF_SIZE);
	dev->extent_last[idx] = 0;
	dma_wmb();
}

static void mz0380_frame_buffers_poison_start(struct mz0380_dev *dev)
{
	unsigned int i;

	/* The diagnostic scanner and per-frame ownership must never race. */
	mz0380_extent_watch_stop(dev);
	dev->frame_poison_byte = mz0380_poison_b();
	WRITE_ONCE(dev->frame_poison_active, false);
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		if (dev->stream_bufs[i].va)
			memset(dev->stream_bufs[i].va,
			       dev->frame_poison_byte,
			       MZ0380_STREAM_BUF_SIZE);
		dev->extent_last[i] = 0;
	}
	dma_wmb();
	smp_store_release(&dev->frame_poison_active, true);
	pr_info("%s: real-frame buffers primed with 0x%02x poison; H.264 payload size will be inferred from the bounded changed prefix because firmware does not expose its byte count\n",
		dev->name, dev->frame_poison_byte);
}

/*
 * Track how far into each poisoned buffer the card has written, including
 * zeros. Forward-incremental scan: everything below extent_last is known
 * card-written, so each pass only walks the newly overwritten range - O(new
 * bytes), not O(buffer). A card-written dword that happens to equal the
 * poison word stops the scan early; acceptable for a diagnostic (the fake
 * frame is 0x11 fill + zero padding, neither matches 0xAAAAAAAA).
 */
static int mz0380_extent_thread(void *data)
{
	struct mz0380_dev *dev = data;
	unsigned long next_print = jiffies;

	while (!kthread_should_stop()) {
		unsigned int i;

		for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
			const u32 *p = dev->stream_bufs[i].va;
			size_t off = dev->extent_last[i];
			size_t prev = off;

			if (!p)
				continue;
			while (off < MZ0380_STREAM_BUF_SIZE &&
			       p[off / 4] != mz0380_poison_w())
				off += 4;
			if (off != prev) {
				dev->extent_last[i] = off;
				if (time_after_eq(jiffies, next_print)) {
					pr_info("%s: extent buf[%u]=0x%zx (+0x%zx)\n",
						dev->name, i, off, off - prev);
					next_print = jiffies + HZ / 20;
				}
			}
		}
		msleep(20);
	}
	return 0;
}

/*
 * M37. The live extent watch freezes at the FIRST still-poisoned dword, but
 * the hardware run showed data beyond it (extent froze at 0x6d384 while the
 * page sampling saw touches out to 0x30a000): the card's writes are not a
 * contiguous prefix. Full hole map at stop: every data<->poison transition,
 * so the write pattern (structural stride holes vs a single lost TLP vs
 * scattered corruption) becomes visible.
 */
static void mz0380_extent_hole_map(struct mz0380_dev *dev)
{
	unsigned int i;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		const u32 *p = dev->stream_bufs[i].va;
		size_t data = 0, last = 0, hole_start = 0;
		unsigned int holes = 0;
		bool in_data = false;
		size_t off;

		if (!p)
			continue;

		for (off = 0; off < MZ0380_STREAM_BUF_SIZE; off += 4) {
			bool d = p[off / 4] != mz0380_poison_w();

			if (d) {
				data += 4;
				last = off;
			}
			if (d && !in_data) {
				if (off && holes < 8)
					pr_info("%s: holemap buf[%u] hole #%u @0x%zx len 0x%zx\n",
						dev->name, i, holes,
						hole_start, off - hole_start);
				if (off)
					holes++;
				in_data = true;
			} else if (!d && in_data) {
				hole_start = off;
				in_data = false;
			}
		}
		if (data)
			pr_info("%s: holemap buf[%u]: data=0x%zx bytes, last data @0x%zx, interior holes=%u\n",
				dev->name, i, data, last, holes);
	}
}

/*
 * M38. If the card is writing IDENTICAL frames to buf0 over and over, every
 * metric so far is blind to it (same bytes, extent frozen, tokens silent).
 * Re-poisoning mid-stream discriminates: data reappearing after a repoison
 * means the frame loop is alive and completing transfers continuously - and
 * the blocker is tinyvenc5's completion semantics, not a parked DMA.
 * Triggered via "echo repoison > /proc/mz0380-events".
 */
void mz0380_extent_repoison(struct mz0380_dev *dev)
{
	unsigned int i;

	if (!mz0380_buf_poison || !dev->extent_task) {
		pr_info("%s: repoison ignored (extent watch not running)\n",
			dev->name);
		return;
	}
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		if (dev->stream_bufs[i].va)
			memset(dev->stream_bufs[i].va, mz0380_poison_b(),
			       MZ0380_STREAM_BUF_SIZE);
		dev->extent_last[i] = 0;
	}
	wmb();
	pr_info("%s: REPOISON: buffers re-filled, extents reset - any new extent growth = the card is still writing\n",
		dev->name);
}
EXPORT_SYMBOL_GPL(mz0380_extent_repoison);

static void mz0380_extent_watch_stop(struct mz0380_dev *dev)
{
	unsigned int i;

	if (!dev->extent_task)
		return;
	kthread_stop(dev->extent_task);
	dev->extent_task = NULL;
	pr_info("%s: extent final: buf0=0x%zx buf1=0x%zx buf2=0x%zx buf3=0x%zx\n",
		dev->name, dev->extent_last[0], dev->extent_last[1],
		dev->extent_last[2], dev->extent_last[3]);
	mz0380_extent_hole_map(dev);
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
		dev->extent_last[i] = 0;
}

static void mz0380_extent_watch_start(struct mz0380_dev *dev)
{
	unsigned int i;

	mz0380_extent_watch_stop(dev);
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		dev->extent_last[i] = 0;
		if (dev->stream_bufs[i].va)
			memset(dev->stream_bufs[i].va, mz0380_poison_b(),
			       MZ0380_STREAM_BUF_SIZE);
	}
	wmb();	/* poison visible before the card is started */
	dev->extent_task = kthread_run(mz0380_extent_thread, dev,
				       "mz0380-extent/%u", dev->nr);
	if (IS_ERR(dev->extent_task))
		dev->extent_task = NULL;
}

static void mz0380_stream_bufs_dump(struct mz0380_dev *dev, const char *tag)
{
	unsigned int i;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		const u8 *p = dev->stream_bufs[i].va;
		size_t last = 0, nonzero = 0, off;

		if (!p)
			continue;

		/*
		 * Sample the whole buffer cheaply: every 4 KiB, plus the head.
		 * With M36 poisoning, "touched" means != the 0xAA poison, so
		 * card-written zeros count too; without poisoning it degrades
		 * to the old non-zero test.
		 */
		for (off = 0; off < MZ0380_STREAM_BUF_SIZE; off += 4096) {
			u8 untouched = dev->frame_poison_active ?
				       dev->frame_poison_byte :
				       (mz0380_buf_poison ? mz0380_poison_b() : 0x00);

			if (p[off] != untouched) {
				nonzero++;
				last = off;
			}
		}

		pr_info("%s: %s buf[%u] @%pad head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | %zu/%u sampled pages touched, last @0x%zx\n",
			dev->name, tag, i, &dev->stream_bufs[i].dma,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
			nonzero, MZ0380_STREAM_BUF_SIZE / 4096, last);
	}
}

void mz0380_dma_teardown(struct mz0380_dev *dev)
{
	if (dev->dma_armed) {
		pci_clear_master(dev->pci);
		dev->dma_armed = false;
	}
	mz0380_stream_bufs_free(dev);
}
EXPORT_SYMBOL_GPL(mz0380_dma_teardown);

/*
 * Frames per second of a detected timing, rounded to the nearest whole frame
 * (the card's fps field is a single byte). For interlaced modes this is the
 * FRAME rate, matching how the card labels 1080i60 as 30 frames.
 */
static u8 mz0380_timings_fps(const struct v4l2_dv_timings *t)
{
	const struct v4l2_bt_timings *bt = &t->bt;
	u64 total;

	total = (u64)V4L2_DV_BT_FRAME_WIDTH(bt) * V4L2_DV_BT_FRAME_HEIGHT(bt);
	if (!total || !bt->pixelclock)
		return 60;
	return (u8)min_t(u64, 255, div_u64(bt->pixelclock + total / 2, total));
}

/*
 * Windows sends opcode 0x2d as opcode plus ten arguments and waits for its
 * EVENT completion (PARAM10 overlaps the short-command STATUS word).  The
 * bundled tinyvenc5 independently verifies the fields selected below:
 *
 *   arg0 / card+0x04: validity mask
 *   arg1 / card+0x08: gop<<24 | fps<<16 | main_or_sub<<8 | channel
 *   arg3 / card+0x10: bitrate
 *
 * Mask bits 0, 1 and 6 select exactly FPS, GOP and bitrate.  Leave profile,
 * QP and geometry masked out until their userspace enum/range ABI is proven.
 */
static int mz0380_stream_configure_encoder(struct mz0380_dev *dev, u32 fps)
{
	u32 enc[10] = { 0 };
	u32 gop = dev->capture.gop_size;
	u32 bitrate = dev->capture.bitrate;
	bool fallback = false;
	int ret;

	if (!fps || fps > U8_MAX) {
		fps = MZ0380_ENC_SAFE_FPS;
		fallback = true;
	}
	if (!gop || gop > U8_MAX) {
		gop = MZ0380_ENC_SAFE_GOP;
		fallback = true;
	}
	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE) {
		bitrate = MZ0380_ENC_SAFE_BITRATE;
		fallback = true;
	}

	enc[0] = MZ0380_ENC_VALID_FPS | MZ0380_ENC_VALID_GOP |
		 MZ0380_ENC_VALID_BITRATE;
	enc[1] = (gop << 24) | (fps << 16); /* main stream, channel zero */
	enc[3] = bitrate;

	/* Match Windows' EVENT-wait path; timeout 0 would permit mailbox reuse. */
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_ENC_PARAMS, enc,
				  ARRAY_SIZE(enc), NULL, 5000);
	pr_info("%s: stream start: SET_ENC_PARAMS(op 0x2d, mask=0x%02x, main ch0, fps=%u, gop=%u, bitrate=%u%s) ret=%d\n",
		dev->name, enc[0], fps, gop, bitrate,
		fallback ? ", conservative fallback applied" : "", ret);
	return ret;
}

/*
 * Start streaming: program the buffer physaddrs into the card, then arm the
 * encoder with SET_VIC_PARAMS (the same op 0x29 path input-select uses). Per
 * M17 the card's own userspace flips its internal enables in response, so no
 * further host kick should be needed - a live run will confirm.
 */
int mz0380_dma_start(struct mz0380_dev *dev)
{
	u32 params[9] = { 0 };
	u32 out_w = dev->capture.width, out_h = dev->capture.height;
	u32 in_w = dev->capture.source_width ?: dev->detected_timings.bt.width;
	u32 in_h = dev->capture.source_height ?: dev->detected_timings.bt.height;
	bool interlaced = dev->capture.source_interlaced;
	u32 fps = dev->capture.source_fps ?:
		  mz0380_timings_fps(&dev->detected_timings);
	u32 fmt = interlaced ? 3 : 2;   /* byte6: venc5 H.264, 2=prog 3=interlaced */
	u32 input_bus = interlaced ? 7 : 6; /* byte7: 6=BT1120p, 7=BT1120i */
	bool aic_newly_armed = false;
	bool vic_fired = false;
	int ret;

	if (!dev->dma_armed)
		return -ENODEV;
	if (!in_w)
		in_w = out_w;
	if (!in_h)
		in_h = out_h;

	/*
	 * SET_VIC_PARAMS 44-byte struct - byte offsets VERIFIED by disassembling
	 * video_capture_mgr's op-41 handler (RE_FINDINGS.md M23). The mailbox puts
	 * the opcode at struct[0..3]; our params[i] lands at struct[4+4i..7+4i]
	 * (params[0]=struct[4..7]). Authoritative field map:
	 *   [4]=ch  [5]=fps  [6]=fw/format(2 prog|3 interlaced)
	 *   [7]=input bus (6 BT1120 progressive | 7 BT1120 interlaced)
	 *   [8..9]=width  [10..11]=height  [12]=m  [16..19]=color_info
	 *   [20..21]=x_start  [22..23]=y_start
	 *   [24..25]=input_frame_width  [26..27]=input_frame_height
	 *   [28]=bitstream_num(MUST be >=1)  [29]=osd_en  [30]=osd_size
	 *   [31]=is_nosg  [35]=is_slave  [36..39]=nosg back/y/u/v
	 * params[8] explicitly clears those final no-signal colour bytes; the
	 * following struct word is PARAM10/STATUS and is never SET_VIC payload.
	 * The old M20/M22 packing put input_w/input_h/bitstream_num two bytes early,
	 * so the firmware read bitstream_num=0 (byte28 unset) and garbage input dims
	 * -> tinyvenc5 spawned but silent. This is the corrected layout.
	 * is_nosg=1 spawns the card's fake_frame_process (black-frame generator,
	 * no capture/SSM dependency): a stream_nosg test lever that bisects the
	 * encode+DMA path from the upstream BT1120 capture path.
	 */
	/*
	 * [5] = fps. We had always left this 0, which the card patches up with
	 * "Error!!! vic_fps cannot be 0, set fps to 60" - harmless while the
	 * fake-frame generator paced itself, but the real capture path derives
	 * its frame cadence from it, so send the rate we actually detected.
	 */
	params[0] = (fps << 8) | (fmt << 16) | (input_bus << 24);
	params[1] = ((out_h & 0xffff) << 16) | (out_w & 0xffff);
	params[5] = ((in_h & 0xffff) << 16) | (in_w & 0xffff);
	params[6] = 1u | ((mz0380_stream_nosg ? 1u : 0u) << 24); /* [28]=bitstream_num=1, [31]=is_nosg */

	/* Even a timed-out transaction may already have spawned tinyvenc5. */
	vic_fired = true;
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (mz0380_stream_nosg)
		pr_info("%s: stream start: SET_VIC(synthetic no-signal source %ux%u@%u, bus=BT1120%s; host output is polled NV12, not live HDMI/H.264) ret=%d\n",
			dev->name, in_w, in_h, fps,
			interlaced ? "i" : "p", ret);
	else
		pr_info("%s: stream start: SET_VIC(input=%ux%u%s@%u BT1120%s -> H.264 output=%ux%u, bitstreams=1) ret=%d\n",
			dev->name, in_w, in_h, interlaced ? "i" : "p",
			fps, interlaced ? "i" : "p", out_w, out_h, ret);
	if (ret)
		goto err_events;

	/*
	 * SET_VIC only spawns the encoder (via video_capture_mgr) and clears
	 * no_signal; tinyvenc5 then blocks on /sys/vpl_pciep/epint waiting for a
	 * separate START_STREAMING (op 0x06) before it DMAs any frame (M22).
	 * Give the freshly system()-forked tinyvenc5 time to exec, open epint and
	 * consume the SET_VIC(0x29) it reads first, so our op6 lands as the next
	 * distinct command rather than racing its start-up read. The delay is a
	 * heuristic for the on-card process spawn; tune against hardware via the
	 * start_delay_ms module param if the first frame is missed (symptom: no
	 * rising IRQ/token count after op6, IRQ 164 stuck at the idle value 3).
	 */
	msleep(mz0380_start_delay_ms);

	/*
	 * SET_VIC launches tinyvenc5, but opcode 0x2d is the host-owned encoder
	 * configuration transaction.  Send it only after tinyvenc5 has consumed
	 * its mandatory first SET_VIC read.  Its non-zero timeout serializes the
	 * full-width mailbox packet through the command EVENT before SET_BUF can
	 * overwrite the shared words.  The NOSG diagnostic emits raw synthetic
	 * NV12 and deliberately skips H.264 configuration/spawn work.
	 */
	if (!mz0380_stream_nosg) {
		ret = mz0380_stream_configure_encoder(dev, fps);
		if (ret)
			goto err_events;
	}

	/*
	 * NOW program the buffer physaddrs into channels[] - AFTER the SET_VIC spawn
	 * settled and BEFORE op6. START(op6) makes vpl_dmac latch channels[] into
	 * the outbound iATU; doing SET_BUF here (not before SET_VIC) guarantees our
	 * addresses are the ones latched, so the iATU low target = our buffer, not 0
	 * (RE_FINDINGS.md M23). op2 does not sysfs_notify, so it won't disturb the
	 * tinyvenc5 that is blocked waiting for op6.
	 */
	ret = mz0380_stream_program_bufs(dev);
	if (ret) {
		pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
			dev->name, ret);
		goto err_events;
	}
	if (!mz0380_stream_nosg)
		pr_info("%s: SET_BUF_2 provides four collision-free buffers (tokens 0..3); 3-bit tokens 4..7 are rejected and acknowledged until a second four-buffer allocation is wired to SET_BUF_8\n",
			dev->name);

	/*
	 * Real H.264 needs an owned poison suffix for bounded length inference.
	 * NOSG keeps the older live extent diagnostic; its fixed raw size does not
	 * use the completion FIFO or inferred-length path.
	 */
	if (!mz0380_stream_nosg) {
		mz0380_frame_buffers_poison_start(dev);
		/* Accept completions only after every token has a poison baseline. */
		mz0380_frame_events_start(dev);
	} else if (mz0380_buf_poison) {
		mz0380_extent_watch_start(dev);
	}

	/*
	 * M40: declare every bitstream slot free before the encoder starts.
	 * enc_stat<idx> (BAR0+0x50+idx) is the per-frame "may I encode into
	 * the host buffer" handshake; the card only ever sets it to 1 (after
	 * a bitstream DMA) and never clears it, so a stale 1 left by a
	 * previous stream would make the encoder retry 10x and then skip
	 * every frame.
	 */
	mz0380_enc_stat_ack(dev);

	/*
	 * M33: release the audio gate BEFORE START. tinyvenc5's is_nosg path
	 * blocks on /sys/audio_status/audio_ready ("[tiny5]is_nosg wait audio
	 * timeout, i2s_num=%d, audio ready[%d]") before it will ACK
	 * START_STREAMING, and the only thing that sets that file is
	 * video_capture_mgr's SET_AIC_PARAMS handler when on=1. Without it the
	 * card DMAs a raw frame but never writes channel_done, which is exactly
	 * the state M30-M32 left us in: data in the buffer, no completion.
	 */
	if (mz0380_aic_on && (mz0380_aic_every_frame || !dev->aic_armed)) {
		bool was_armed = dev->aic_armed;
		u32 aic[4] = {
			/* cmd+4 channel_num | cmd+5 mono<<8 | cmd+6 bits<<16 */
			(mz0380_aic_channels & 0xff) |
			((u32)(mz0380_aic_channels == 1 ? 1 : 0) << 8) |
			((u32)(mz0380_aic_bits & 0xffff) << 16),
			mz0380_aic_freq,			/* cmd+8  freq  */
			(mz0380_aic_period_frames & 0xffff) |	/* cmd+12       */
			((u32)(mz0380_aic_periods & 0xffff) << 16), /* cmd+14   */
			1u,					/* cmd+16 on=1  */
		};

		ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS, aic,
					  ARRAY_SIZE(aic), NULL, 2000);
		pr_info("%s: stream start: SET_AIC(on=1, %u ch, %u bit, %u Hz, %u frames x %u periods) ret=%d\n",
			dev->name, mz0380_aic_channels, mz0380_aic_bits,
			mz0380_aic_freq, mz0380_aic_period_frames,
			mz0380_aic_periods, ret);
		if (!ret) {
			dev->aic_armed = true;
			aic_newly_armed = !was_armed;
		} else {
			/* Without audio_ready, tinyvenc5 never completes video frames. */
			goto err_events;
		}
	}

	/*
	 * op 0x06 is FIRE-AND-FORGET. Unlike INIT/SET_VIC, its ep.ko handler
	 * (@0x1854, M22) only does sysfs_notify(epint) to wake tinyvenc5 - it
	 * posts NO mailbox completion (no STATUS bit0, no EVENT bit11). Waiting
	 * for one always burns the full timeout and returns a bogus -ETIMEDOUT
	 * (proven on hw: SET_VIC->op6 gap == msleep + full 2000ms, zero EVENT
	 * lines logged). So send with timeout_ms=0: fire the doorbell and return.
	 * Frame arrival is confirmed downstream by the MSI/outbound-ATU path, not
	 * by a command ack.
	 */
	ret = mz0380_send_command(dev, MZ0380_CMD_START_STREAMING,
				  NULL, 0, NULL, 0);
	pr_info("%s: stream start: START_STREAMING(op 0x06) fired (async, ret=%d)\n",
		dev->name, ret);
	dev->stream_head = 0;
	if (ret)
		goto err_events;
	return 0;

err_events:
	if (!mz0380_stream_nosg)
		WRITE_ONCE(dev->streaming, false);

	/*
	 * SET_VIC forks an encoder process before it replies.  Every failure after
	 * the doorbell therefore owes the card a STOP, including SET_VIC timeout
	 * itself; otherwise retries permanently consume the small spawn budget.
	 * Keep NOSG's normal successful stop/respawn loop unchanged--this is only
	 * the failed-start unwind.
	 */
	if (vic_fired) {
		int stop_ret = mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
						NULL, 0, NULL, 2000);

		pr_warn("%s: stream start failed (%d) after SET_VIC; best-effort STOP_STREAMING ret=%d\n",
			dev->name, ret, stop_ret);
	}
	if (aic_newly_armed) {
		u32 aic_off[4] = { 0 };
		int aic_ret;

		aic_ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS,
					       aic_off, ARRAY_SIZE(aic_off),
					       NULL, 2000);
		pr_warn("%s: failed-start unwind: SET_AIC(on=0) ret=%d\n",
			dev->name, aic_ret);
		dev->aic_armed = false;
	}
	if (!mz0380_stream_nosg) {
		mz0380_dma_flush_events(dev);
		WRITE_ONCE(dev->frame_poison_active, false);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_dma_start);

static void __mz0380_dma_stop(struct mz0380_dev *dev, bool verbose)
{
	if (verbose)
		/* Freeze the diagnostic reader before changing buffer ownership. */
		mz0380_extent_watch_stop(dev);

	if (dev->dma_armed)
		mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				    NULL, 0, NULL, 2000);

	if (verbose) {
		/*
		 * STOP first, then cancel/drain completion work before inspecting
		 * memory.  This makes streamoff safe against a worker copying or
		 * re-poisoning while the diagnostic dump walks the same buffer.
		 */
		mz0380_dma_flush_events(dev);
		pr_info("%s: stream stop: EVENT[0x30]=%08x token[0x40]=%08x 0x44=%08x 0x48=%08x 0x4c=%08x enc[0x50]=%08x irq_total=%d frame_events=%d fifo_drops=%llu\n",
			dev->name,
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3),
			mz_mmio_read(dev, MZ0380_MB_ENC_STATUS),
			atomic_read(&dev->irq_count),
			atomic_read(&dev->irq_video_count),
			(unsigned long long)READ_ONCE(dev->frame_event_drops));
		mz0380_stream_bufs_dump(dev, "stop");
		WRITE_ONCE(dev->frame_poison_active, false);
	}
}

void mz0380_dma_stop(struct mz0380_dev *dev)
{
	__mz0380_dma_stop(dev, true);
}
EXPORT_SYMBOL_GPL(mz0380_dma_stop);

/*
 * The true encoded-byte count exists in a card-side enc_stat structure, but
 * ep.ko discards it and exposes only packed slot counters at 0x40/0x44/0x48.
 * Infer a bounded length from buffer ownership instead: every slot is filled
 * with a repeated poison dword before the endpoint may use it, and completion
 * holds enc_stat until this worker copies/drops and re-poisons it.  Scan from
 * the end for the last changed dword.  A compressed final dword colliding with
 * the poison can undercount by four bytes (probability 2^-32); unlike the old
 * 4 MiB fallback, the result is bounded by an observed DMA write boundary.
 */
static int mz0380_infer_frame_length(struct mz0380_dev *dev, u32 idx,
				     size_t *length)
{
	const u64 *qwords;
	const u32 *dwords;
	u64 poison_q;
	u32 poison_w;
	size_t i;

	*length = 0;
	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->stream_bufs[idx].va ||
	    !smp_load_acquire(&dev->frame_poison_active))
		return -EINVAL;

	qwords = dev->stream_bufs[idx].va;
	dwords = dev->stream_bufs[idx].va;
	poison_q = mz0380_frame_poison_q(dev);
	poison_w = mz0380_frame_poison_w(dev);
	dma_rmb();

	for (i = MZ0380_STREAM_BUF_SIZE / sizeof(*qwords); i; i--) {
		size_t dword = (i - 1) * 2;

		if (READ_ONCE(qwords[i - 1]) == poison_q)
			continue;

		if (READ_ONCE(dwords[dword + 1]) != poison_w)
			*length = (dword + 2) * sizeof(*dwords);
		else
			*length = (dword + 1) * sizeof(*dwords);

		/* No untouched suffix means truncation, not a proven frame end. */
		if (*length == MZ0380_STREAM_BUF_SIZE)
			return -ENOSPC;
		return 0;
	}

	return -ENODATA;
}

static bool mz0380_frame_event_pop(struct mz0380_dev *dev,
				   struct mz0380_frame_event *snapshot)
{
	unsigned long flags;
	bool have_event = false;

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	if (dev->frame_event_tail != dev->frame_event_head) {
		*snapshot = dev->frame_events[dev->frame_event_tail];
		dev->frame_event_tail = (dev->frame_event_tail + 1) %
					MZ0380_FRAME_EVENT_FIFO_SIZE;
		have_event = true;
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	return have_event;
}

static void
mz0380_drain_frame_snapshot(struct mz0380_dev *dev,
			    const struct mz0380_frame_event *snapshot)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	u32 idx = snapshot->token & 7;
	u8 *payload;
	size_t len;
	int ret;

	if (idx >= MZ0380_STREAM_NR_BUFS) {
		pr_warn_ratelimited("%s: frame dropped: 3-bit token=%08x selects unprogrammed slot %u (only 0..%u are backed; SET_BUF_8 second set not allocated), event=%08x payload=%08x/%08x/%08x enc=%08x\n",
				    dev->name, snapshot->token, idx,
				    MZ0380_STREAM_NR_BUFS - 1, snapshot->event,
				    snapshot->payload[0], snapshot->payload[1],
				    snapshot->payload[2], snapshot->enc_status);
		return;
	}

	payload = dev->stream_bufs[idx].va;
	if (!payload) {
		pr_warn_ratelimited("%s: frame dropped: token %u has no DMA buffer; enc_stat ACK follows drain batch\n",
				    dev->name, idx);
		return;
	}
	if (!READ_ONCE(dev->streaming)) {
		pr_info_ratelimited("%s: frame token %u arrived during streamoff; dropped, re-poisoned and acknowledged\n",
				    dev->name, idx);
		goto repoison;
	}

	ret = mz0380_infer_frame_length(dev, idx, &len);
	if (ret || !len || len >= MZ0380_STREAM_BUF_SIZE) {
		pr_warn_ratelimited("%s: frame dropped: no bounded poison-suffix length (ret=%d event=%08x token=%08x payload counters=%08x/%08x/%08x enc=%08x head=%02x %02x %02x %02x); mailbox counters are not byte lengths\n",
				    dev->name, ret, snapshot->event, snapshot->token,
				    snapshot->payload[0], snapshot->payload[1],
				    snapshot->payload[2], snapshot->enc_status,
				    payload[0], payload[1], payload[2], payload[3]);
		goto repoison;
	}

	pr_info_ratelimited("%s: frame token %u inferred H.264 length=%zu from 4-byte poison boundary (tail collision risk 2^-32; payload counters %08x/%08x/%08x were not used)\n",
				    dev->name, idx, len, snapshot->payload[0],
				    snapshot->payload[1], snapshot->payload[2]);

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		pr_info_ratelimited("%s: frame token %u length=%zu has no queued vb2 buffer; dropped and re-poisoned\n",
				    dev->name, idx, len);
		goto repoison;
	}

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);

		if (!dst || len > plane) {
			pr_warn_ratelimited("%s: inferred frame %zu bytes does not fit vb2 plane %zu; buffer failed\n",
					    dev->name, len, plane);
			vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
			vb2_buffer_done(&vbuf->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			goto repoison;
		}
		dma_rmb();
		memcpy(dst, payload, len);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, len);
	}

	vbuf->vb.vb2_buf.timestamp = snapshot->timestamp_ns;
	vbuf->vb.field = V4L2_FIELD_NONE;
	vbuf->vb.sequence = dev->video_sequence++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

repoison:
	mz0380_frame_buffer_repoison(dev, idx);
}

void mz0380_dma_drain_video(struct mz0380_dev *dev)
{
	struct mz0380_frame_event snapshot;
	unsigned long event_flags, flags;
	unsigned int i;
	bool handled = false;

	/*
	 * Keep drain_scheduled asserted until the queue-empty observation and the
	 * ownership ACK are one transaction.  Merely clearing it in event_pop()
	 * leaves a window where the ACK can free a token that the pre-ACK hook has
	 * just snapshotted but this worker has not copied yet.
	 */
	for (;;) {
		bool ack_deferred;
		u8 drop_tokens;

		/*
		 * Drain/copy/re-poison the complete batch before the one ownership
		 * ACK.  An overflowed newest event therefore cannot release the
		 * endpoint early and overwrite an older queued token before its copy.
		 */
		while (mz0380_frame_event_pop(dev, &snapshot)) {
			handled = true;
			mz0380_drain_frame_snapshot(dev, &snapshot);
		}

		spin_lock_irqsave(&dev->frame_event_lock, flags);
		ack_deferred = dev->frame_event_ack_deferred;
		dev->frame_event_ack_deferred = false;
		drop_tokens = dev->frame_event_drop_tokens;
		dev->frame_event_drop_tokens = 0;
		spin_unlock_irqrestore(&dev->frame_event_lock, flags);

		for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
			if (drop_tokens & BIT(i))
				mz0380_frame_buffer_repoison(dev, i);
		handled |= ack_deferred || drop_tokens;

		/*
		 * The core calls our snapshot hook while holding event_lock, and the
		 * hook then takes frame_event_lock.  Take the same lock order here so
		 * no snapshot can sit between the final empty check and the whole-word
		 * enc_stat ACK.  If an event arrived while we copied/re-poisoned, loop
		 * without clearing drain_scheduled; schedule_work() coalescing is then
		 * harmless because this invocation owns the retry.
		 */
		spin_lock_irqsave(&dev->event_lock, event_flags);
		spin_lock(&dev->frame_event_lock);
		if (dev->frame_event_tail != dev->frame_event_head ||
		    dev->frame_event_ack_deferred ||
		    dev->frame_event_drop_tokens) {
			spin_unlock(&dev->frame_event_lock);
			spin_unlock_irqrestore(&dev->event_lock, event_flags);
			continue;
		}
		dev->frame_event_drain_scheduled = false;
		if (handled)
			mz0380_enc_stat_ack(dev);
		spin_unlock(&dev->frame_event_lock);
		spin_unlock_irqrestore(&dev->event_lock, event_flags);
		return;
	}
}
EXPORT_SYMBOL_GPL(mz0380_dma_drain_video);

void mz0380_dma_drain_audio(struct mz0380_dev *dev)
{
	/* audio DMA path is milestone-C follow-up; no-op for now */
}
EXPORT_SYMBOL_GPL(mz0380_dma_drain_audio);

static void mz0380_drain_work_fn(struct work_struct *w)
{
	struct mz0380_dev *dev = container_of(w, struct mz0380_dev, drain_work);

	mz0380_dma_drain_video(dev);
}

/* --- stream_nosg polling capture ----------------------------------------- */

/*
 * The fake-frame path has no host-visible completion: the card DMAs one
 * contiguous 0x30a5c0-byte burst into buf0 and then its encoder loop parks
 * forever, IRQ-less, for a card-internal reason no host action can fix (M41).
 * But the burst itself is proven contiguous front-to-back (M37), so "the
 * final dwords are no longer poison" == "the whole frame has landed". And a
 * fresh encoder spawn reliably yields exactly one more frame (M39). Those
 * two facts make a polling capture loop: poison buf0, spawn, poll the tail,
 * deliver as NV12, stop, respawn.
 */

static bool mz0380_nosg_frame_landed(struct mz0380_dev *dev)
{
	const u32 *p = dev->stream_bufs[0].va;
	size_t end = MZ0380_STREAM_RAW_FRAME_SIZE / 4;

	/*
	 * Two independent tail dwords must have been overwritten: one could
	 * collide with frame data that happens to equal the poison word (the
	 * M37 poison_byte experiment saw exactly such collisions mid-frame),
	 * two adjacent collisions at the fixed frame tail are not credible.
	 */
	return p[end - 1] != mz0380_poison_w() &&
	       p[end - 2] != mz0380_poison_w();
}

static void mz0380_nosg_deliver(struct mz0380_dev *dev)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;

	if (!READ_ONCE(dev->streaming))
		return;

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	if (!vbuf) {
		pr_info_ratelimited("%s: nosg frame %u landed but no buffer queued - dropped\n",
				    dev->name, dev->nosg_sequence);
		return;
	}

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
		size_t cpy = min_t(size_t, MZ0380_NOSG_NV12_SIZEIMAGE, plane);

		if (dst)
			memcpy(dst, dev->stream_bufs[0].va, cpy);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, cpy);
	}

	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = V4L2_FIELD_NONE;
	vbuf->vb.sequence = dev->nosg_sequence++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static int mz0380_nosg_thread(void *data)
{
	struct mz0380_dev *dev = data;

	while (!kthread_should_stop()) {
		unsigned long deadline;
		bool landed = false;
		int ret;

		/*
		 * Poison the landing zone ourselves so frame arrival is
		 * detectable regardless of the buf_poison diagnostic knob
		 * (dma_start re-poisons the whole set when that is on; a
		 * second memset is harmless).
		 */
		memset(dev->stream_bufs[0].va, mz0380_poison_b(),
		       MZ0380_STREAM_RAW_FRAME_SIZE);
		wmb();

		/*
		 * M60: count the spawns. The card wedges for good after
		 * roughly 8-18 of them and nothing short of removing slot
		 * power brings it back (M52), so the count is the single
		 * most useful number in the log - and a warning before the
		 * cliff is worth more than a post-mortem after it.
		 */
		dev->nosg_spawns++;
		if (dev->nosg_spawns == 8)
			pr_warn("%s: nosg: 8 encoder spawns this session - entering the range where the card has wedged before (needs a mains-off cold boot)\n",
				dev->name);

		ret = mz0380_dma_start(dev);
		if (ret) {
			pr_warn("%s: nosg spawn failed (%d) - retrying in 500 ms\n",
				dev->name, ret);
			if (!kthread_should_stop())
				msleep(500);
			continue;
		}

		deadline = jiffies +
			   msecs_to_jiffies(mz0380_nosg_frame_timeout_ms);
		while (!kthread_should_stop() &&
		       time_before(jiffies, deadline)) {
			if (mz0380_nosg_frame_landed(dev)) {
				landed = true;
				break;
			}
			msleep(10);
		}

		if (landed)
			mz0380_nosg_deliver(dev);
		else if (!kthread_should_stop())
			pr_warn("%s: nosg frame did not land within %u ms - respawning\n",
				dev->name, mz0380_nosg_frame_timeout_ms);

		/*
		 * Quiet stop: op7 ends this spawn so the next SET_VIC forks a
		 * fresh tinyvenc5 whose VPL_DMAC_Open clears the parked state
		 * (M39/M41) - that reset is what makes the next frame possible.
		 * The verbose stop's dumps would spam dmesg once per frame.
		 */
		__mz0380_dma_stop(dev, false);
	}
	return 0;
}

int mz0380_nosg_capture_start(struct mz0380_dev *dev)
{
	struct task_struct *task;

	if (!dev->dma_armed || !dev->stream_bufs[0].va)
		return -ENODEV;
	if (dev->nosg_task)
		return 0;

	dev->nosg_sequence = 0;
	dev->nosg_spawns = 0;
	dev->aic_armed = false;
	task = kthread_run(mz0380_nosg_thread, dev, "mz0380-nosg/%u", dev->nr);
	if (IS_ERR(task))
		return PTR_ERR(task);
	dev->nosg_task = task;

	pr_info("%s: nosg synthetic/no-signal polling started (NV12 %ux%u generated on-card; not HDMI input; diagnostic path spawns one encoder process per frame and is not real-time)\n",
		dev->name, MZ0380_NOSG_NV12_WIDTH,
		MZ0380_NOSG_NV12_HEIGHT);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_nosg_capture_start);

void mz0380_nosg_capture_stop(struct mz0380_dev *dev)
{
	if (!dev->nosg_task)
		return;
	kthread_stop(dev->nosg_task);
	dev->nosg_task = NULL;

	/*
	 * M60: hand the card's audio side back. Every session so far armed
	 * audio_ready and never cleared it, once per frame, which is exactly
	 * the kind of one-way resource churn that fits a card that dies after
	 * a fixed number of spawns.
	 */
	if (dev->aic_armed) {
		u32 aic[4] = { 0, 0, 0, 0 };   /* cmd+16 on=0 */
		int ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS,
					      aic, ARRAY_SIZE(aic), NULL, 2000);

		pr_info("%s: nosg: SET_AIC(on=0) ret=%d\n", dev->name, ret);
		dev->aic_armed = false;
	}

	pr_info("%s: nosg polling capture stopped after %u frames, %u encoder spawns\n",
		dev->name, dev->nosg_sequence, dev->nosg_spawns);
}
EXPORT_SYMBOL_GPL(mz0380_nosg_capture_stop);
