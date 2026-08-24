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
static void mz0380_poll_drain_start(struct mz0380_dev *dev);
static void mz0380_poll_drain_stop(struct mz0380_dev *dev);

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
	/*
	 * M92: word[3] is a buffer SIZE IN BYTES, not a stride. The Windows
	 * driver builds every buffer-registration command (0x02, 0x03, 0x04,
	 * 0x05, 0x08) with the identical shape - word[2] = channel, word[3] =
	 * size, word[4..11] = four {hi,lo} address pairs, count = 12 - and the
	 * sizes it sends decode exactly:
	 *
	 *   0x466000 = 2048 x 1125 x 2   + 4096   (YUV422, stride 2048, vtotal)
	 *   0x34BD00 = 2048 x 1125 x 1.5 + 256    (YUV420, same geometry)
	 *   0x10F000 = 1024 x  540 x 2   + 4096   (preview, YUV422)
	 *   0x0CA900 = 1024 x  540 x 1.5 + 256    (preview, YUV420)
	 *
	 * i.e. frame bytes plus a small header, at a power-of-two stride. That
	 * also identifies the "third region" in the DriverEntry log line
	 * [MEMORY] [00466000] [0034BD00] [0034BD00], which the Windows
	 * collection had left unexplained.
	 *
	 * Our value is the size of the buffer we actually allocate, which is
	 * the same thing Windows sends, so the number was right even though
	 * M27 named it "stride". Nothing changes here but the meaning.
	 */
	params[1] = mz0380_set_buf_stride;	/* cmd[0x8] = size, see above */
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

	ret = mz0380_send_command(dev, mz0380_set_buf_opcode, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (ret)
		return ret;

	/*
	 * M92: Windows sends 0x08 immediately after 0x02, with the same channel,
	 * the same size word and four more address pairs - the pair is issued
	 * back to back at 0x14027b62d / 0x14027b752 and never one without the
	 * other. ep.ko treats them asymmetrically: op2 fills window0 slots 1..4
	 * and CLEARS host_ready (G[0]), while op8 fills slots 5..8 and SETS
	 * wency_ready = 8. We have only ever sent the one that clears a ready
	 * flag, and never the one that sets the other.
	 *
	 * Same four buffers: the point is the slots and the wency_ready side
	 * effect, not extra memory. Off by default - the M88 baseline renders
	 * the splash without it, and after M84 a change that has not been
	 * measured does not get to be a default.
	 */
	if (mz0380_set_buf_op8) {
		int r8 = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_8, params,
					     ARRAY_SIZE(params), NULL, 2000);

		pr_info("%s: SET_BUF_8(op 0x08, window0 slots 5..8, sets wency_ready) ret=%d\n",
			dev->name, r8);
	}

	if (!mz0380_probe_windows)
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

	/*
	 * M84: a SHARED INTx handler is called for every other device on the
	 * line, including during our own teardown. If the BAR has already been
	 * unmapped the very first MMIO read is a NULL dereference - which is
	 * exactly how the first INTx run died (oops in mz_read, CR2 = 0x30 =
	 * MZ0380_MB_EVENT, "rmmod exited with irqs disabled"). The ordering bug
	 * that opened that window is fixed in mz0380_finidev(), but a shared
	 * handler must not depend on ordering alone.
	 */
	if (!dev || !dev->bmmio[MZ0380_MAP_BAR_MMIO])
		return IRQ_NONE;

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

	/*
	 * M82. The endpoint ADVERTISES MSI - Windows derives
	 * DEVPKEY_PciDevice_InterruptSupport = 3 (line + message) and
	 * InterruptMessageMaximum = 1 straight from its config space - but the
	 * retail driver never uses it. On the Windows box the card's
	 * Interrupt Management\MessageSignaledInterruptProperties key is absent
	 * (14 other PCI devices on the same board have it), the INF carries no
	 * MSISupported directive, AllocConfig assigns a level-sensitive shared
	 * line with CM_RESOURCE_INTERRUPT_MESSAGE clear, and the trace logs
	 * "INTERRUPT = 00000000" on IRQ 29.
	 *
	 * So PCI_IRQ_MSI | PCI_IRQ_INTX picks the one interrupt path the
	 * shipping stack has never exercised. If the card's MSI path is unwired
	 * in firmware the symptom is precisely ours: commands complete, the
	 * encoder starts, no completion ever arrives. Default to what Windows
	 * does and treat MSI as the experiment.
	 */
	nvec = pci_alloc_irq_vectors(dev->pci, 1, 1,
				     mz0380_irq_intx ? PCI_IRQ_INTX
						     : (PCI_IRQ_MSI |
							PCI_IRQ_INTX));
	if (nvec < 1) {
		pr_err("%s: pci_alloc_irq_vectors failed (%d)\n",
		       dev->name, nvec);
		return nvec;
	}

	dev->msi_enabled = (nvec >= 1) && dev->pci->msi_enabled;
	/* Legacy delivery needs the line unmasked; probe masks it by default. */
	if (!dev->msi_enabled)
		pci_intx(dev->pci, 1);
	pr_info("%s: interrupt: %s, irq %u (Windows uses INTx)\n",
		dev->name, dev->msi_enabled ? "MSI" : "legacy INTx",
		pci_irq_vector(dev->pci, 0));
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
	/*
	 * M151: the knob covers the per-frame ack only. The single clear at
	 * stream start (mz0380_dma_start_stream) is deliberately left in - it
	 * establishes a known value to read back, and every result in the file
	 * was taken with it.
	 */
	if (!mz0380_enc_stat_ack_on)
		return;

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
		/* M168: per-stream, to match the extent report's "at stop". */
		dev->stream_bufs[i].delivered = 0;
	}
	dma_wmb();
	smp_store_release(&dev->frame_poison_active, true);
	/* M168: "H.264 payload size" - it is a raw I420 frame on this path. */
	pr_info("%s: real-frame buffers primed with 0x%02x poison; the payload size will be inferred from the bounded changed prefix because firmware does not expose its byte count\n",
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
		/* M168: per-stream, to match the extent report's "at stop". */
		dev->stream_bufs[i].delivered = 0;
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

		/*
		 * M168: say how many frames came OUT of this buffer. Without
		 * it a buffer that delivered a whole frame and was re-poisoned
		 * reads exactly like one the card never wrote to - the M168
		 * run printed "0/1024 sampled pages touched" on the buffer that
		 * had just produced a correct 1080p frame.
		 */
		pr_info("%s: %s buf[%u] @%pad head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | %zu/%u sampled pages touched, last @0x%zx | %u frame(s) delivered from it%s\n",
			dev->name, tag, i, &dev->stream_bufs[i].dma,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
			nonzero, MZ0380_STREAM_BUF_SIZE / 4096, last,
			dev->stream_bufs[i].delivered,
			dev->stream_bufs[i].delivered ?
				" (so it is poison again by design, not untouched)" : "");
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
static int mz0380_stream_configure_encoder(struct mz0380_dev *dev, u32 fps,
					   u32 main_or_sub)
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

	/*
	 * M82: the sub stream is a second, independent encoder. Windows always
	 * configures both, back to back, and only the second carries
	 * main_or_sub = 1 (its dword build ORs a literal 0x100 into card+0x08
	 * at 0x14028c643 - the byte-9 position, which is exactly where our
	 * tinyvenc5-derived map already put the field). Its own numbers in the
	 * traces were gop = 30 and bitrate = 4 Mbit against the main stream's
	 * gop = 32 / 4 MiB; nothing suggests the card cares, so the sub stream
	 * just mirrors the main configuration here.
	 */
	enc[0] = mz0380_enc_mask ?: (MZ0380_ENC_VALID_FPS |
				     MZ0380_ENC_VALID_GOP |
				     MZ0380_ENC_VALID_BITRATE);
	enc[1] = (gop << 24) | (fps << 16) |
		 ((main_or_sub & 0xff) << 8);	/* channel zero */
	enc[3] = bitrate;

	/* Match Windows' EVENT-wait path; timeout 0 would permit mailbox reuse. */
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_ENC_PARAMS, enc,
				  ARRAY_SIZE(enc), NULL, 5000);
	pr_info("%s: stream start: SET_ENC_PARAMS(op 0x2d, mask=0x%04x, %s ch0, fps=%u, gop=%u, bitrate=%u%s) ret=%d\n",
		dev->name, enc[0], main_or_sub ? "sub" : "main", fps, gop,
		bitrate, fallback ? ", conservative fallback applied" : "",
		ret);
	return ret;
}

/*
 * STOP_STREAMING as Windows sends it: word[2] = 0xFFFFFFFF ("all channels"),
 * count 3. We have always sent a bare opcode, i.e. channel 0 implicitly and
 * count 2. M82.
 */
static int mz0380_stream_stop_all(struct mz0380_dev *dev, unsigned int timeout_ms)
{
	u32 stop_all = 0xffffffffu;

	return mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				   &stop_all, 1, NULL, timeout_ms);
}

/*
 * op 0x31, the command that closes every Windows capture-start sequence. We
 * have always called it POST_PROC; M128's static decode of tinyvenc5 names it
 * SET_PREVIEW_PARAMS and gives the whole 20-byte payload:
 *
 *   [4..7]=mask  [8]=ch  [9]=fps  [0x0a]=skip  [0x0b]=avg  [0x0c]=die_en
 *   [0x0d]=preview_off  [0x0e]=fake_frame_off  [0x0f]=preview_no_osd
 *   [0x10]=mirror  [0x11]=flip  [0x12]=hw_d
 *
 * so post_di is die_en. On this board every field except fps and die_en is
 * zero in the Windows traces, and die_en is 1 even for a progressive source.
 * fake_frame_off is ours, not Windows': see mz0380_fake_frame_off in core.c.
 */
static int mz0380_stream_post_proc(struct mz0380_dev *dev, u32 fps)
{
	u32 post[5] = { 0 };
	int ret;

	post[0] = mz0380_post_mask;
	post[1] = (fps & 0xff) << 8;			/* [8]=ch [9]=fps  */
	post[2] = (mz0380_post_di & 0xff) |		/* [12]=die_en     */
		  ((mz0380_fake_frame_off ? 1u : 0u) << 16); /* [14] */

	ret = mz0380_send_command(dev, mz0380_post_proc_opcode, post,
				  ARRAY_SIZE(post), NULL, 5000);
	pr_info("%s: stream start: SET_PREVIEW_PARAMS(op 0x%02x, mask=0x%02x, fps=%u, die_en=%u, fake_frame_off=%u) ret=%d\n",
		dev->name, mz0380_post_proc_opcode, post[0], fps,
		mz0380_post_di, mz0380_fake_frame_off, ret);
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
	/*
	 * byte6 "fw": encoder selector (M71) AND cfg output-format selector
	 * (M79: vcm writes "output format" = 2/YUY2 when fw == 6, else 1/YV12;
	 * fw == 7 spawns ./tinyvenc7, fw == 8 ./tinyvenc8, anything else
	 * ./tinyvenc5). M82: the Windows driver only ever sends 6 or 7, picked
	 * by frame rate - 6 for 1080p30 and 1080p29.97, 7 for 1080p60, in
	 * every [CH00] line of every trace. It never sends 5, which is what we
	 * have always sent. 0 here means "use the Windows rule".
	 */
	u32 fw = mz0380_vic_fw ?: (fps > 30 ? 7u : 6u);   /* 0 = Windows rule; see M88 */
	/*
	 * byte7: VideoCap INPUT FORMAT, per the SDK capture config
	 * (re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg): "input format
	 * (1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4:Bayer, 5:16-bits Raw,
	 * 6:BT1120p, 7:BT1120i)". The card's own printf calls the field
	 * "interlace", which is why M71 read it as a boolean and M103 briefly
	 * reclassified it back - but the label is loose, not wrong: 6 vs 7 is
	 * BT1120p vs BT1120i.
	 *
	 * M104 settled it ON HARDWARE and the enum reading won. 3, 6 and 7 all
	 * reach the card's NOSG splash and are indistinguishable; 0 - which is
	 * not in the enum - produces nothing at all, i.e. VideoCap never opens
	 * and tinyvenc exits before it can draw. That also PROVES the byte is
	 * consumed, which the three indistinguishable values could not.
	 *
	 * MZ0380_VIC_IN_FMT_AUTO exists because the old `vic_in_fmt ?: derived`
	 * idiom could not express 0 at all, so the field's own sweep knob could
	 * not reach part of its range. The derived default is unchanged.
	 */
	u32 in_fmt = (mz0380_vic_in_fmt == MZ0380_VIC_IN_FMT_AUTO)
			? (interlaced ? 7u : 6u) : mz0380_vic_in_fmt;
	u32 out_fmt = mz0380_vic_out_format;    /* byte12 "m", see M72 */
	u32 vic_in_w, vic_in_h;
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
	 *   [4]=ch  [5]=fps  [6]=fw  [7]=input format (6=BT1120p, 7=BT1120i)
	 *   [8..9]=width  [10..11]=height  [12]=m  [13]=flip  [14]=mirror
	 *   [16..19]=color_info  [20..21]=x_start  [22..23]=y_start
	 *   [24..25]=input_frame_width  [26..27]=input_frame_height
	 *   [28]=bitstream_num(MUST be >=1)  [29]=osd_en  [30]=osd_size
	 *   [31]=is_nosg  [32]=vanc_lines  [33]=fast_kill
 *   [34]=frame-completion interrupt enable (M136 - was guessed as "mix")
 *   [35]=is_slave
	 *   [36..39]=nosg back/y/u/v
	 *
	 * M71: bytes 6 and 7 corrected against the card's OWN printf. The
	 * format string at video_capture_mgr .rodata 0xb734 is
	 *   "[Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d)
	 *    interlace(%d), m(%d), color_info ..."
	 * and decoding its call at vcm 0x8f60..0x8fbc under AAPCS gives
	 * r1=[cmd+4]=ch, r2=[cmd+6]=fw, r3=[cmd+5]=fps, then stack args
	 * [cmd+8]=W, [cmd+10]=H, [cmd+7]=interlace, [cmd+12]=m, ... So byte6
	 * is the ENCODER SELECTOR (vcm 0x9250 compares it to 7 -> ./tinyvenc7,
	 * and 8 -> ./tinyvenc8, else ./tinyvenc5).
	 *
	 * M127/M128 CORRECTION to the rest of that paragraph: byte7 is NOT an
	 * interlace boolean. video_capture_mgr only *labels* it "interlace(%d)"
	 * in the printf above; the value is sprintf'd RAW into the cfg line
	 * matching "input format", whose enum is 6 = BT1120p and 7 = BT1120i.
	 * The code below (in_fmt) is right and always was - it was this comment
	 * that was wrong. Historical note: the pre-M71 packing had byte6 = 2|3,
	 * which spawned tinyvenc5 only by fallthrough.
	 * params[8] carries the no-signal colour bytes (M82: 0.00.80.80); the
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
	params[0] = (fps << 8) | (fw << 16) | (in_fmt << 24);
	/*
	 * M72: byte12 = output format, bytes 16..19 = brightness, contrast,
	 * saturation, field-invert. Both blocks were left at zero for this
	 * driver's whole life; zero is not a legal output format and zero
	 * saturation is the config's documented "mono".
	 */
	params[2] = out_fmt & 0xff;
	/*
	 * M82: bytes 16..19 are color_info[0..3], and the Windows driver sends
	 * a fixed 1,1,1,2 on every single SET_VIC in all four traces - never a
	 * per-picture value. Decoded from the dword build at 0x14028bcb9,
	 *   eax = c3<<24 | c1<<16 | c2<<8 | c1
	 * cross-checked against its printf "color = %d.%d.%d.%d" which prints
	 * the same three stack slots in the order c3,c1,c2,c1 and read
	 * "color = 2.1.1.1" in every trace. So c1=1, c2=1, c3=2 and the bytes
	 * on the wire are [16]=1 [17]=1 [18]=1 [19]=2.
	 *
	 * This supersedes the M72 reading of the same four bytes as
	 * brightness/contrast/saturation/field-invert: that guess put 128 in
	 * byte 18 and zero everywhere else, which is not a value the retail
	 * driver ever sends. vic_saturation is kept only as an override.
	 */
	params[3] = mz0380_vic_color_info;
	if (mz0380_vic_saturation != MZ0380_VIC_SATURATION_UNSET)
		params[3] = (params[3] & ~0x00ff0000u) |
			    ((mz0380_vic_saturation & 0xff) << 16);
	/*
	 * M139: bytes 8..9 / 10..11.  The cfg patcher turns these into the
	 * card's capture geometry AND into m_vic_width, which is the value
	 * img_handler's frame gate tests.  Overridable so the gate can be
	 * moved without moving the v4l2 format.
	 */
	if (mz0380_vic_out_w)
		out_w = mz0380_vic_out_w;
	if (mz0380_vic_out_h)
		out_h = mz0380_vic_out_h;
	params[1] = ((out_h & 0xffff) << 16) | (out_w & 0xffff);
	/*
	 * M76: bytes 24..27 are the VIC's own width/height register, patched
	 * into the card's cfg as "input frame width/height" independently of
	 * the capture geometry. Overridable to test the 8-bit double-rate
	 * hypothesis (3840) against the VIC's width check.
	 */
	vic_in_w = mz0380_vic_in_w ?: in_w;
	vic_in_h = mz0380_vic_in_h ?: in_h;
	params[5] = ((vic_in_h & 0xffff) << 16) | (vic_in_w & 0xffff);
	/* [28]=bitstream_num (M148 knob, def 1), [31]=is_nosg */
	params[6] = (mz0380_bitstream_num & 0xffu) |
		    ((mz0380_stream_nosg ? 1u : 0u) << 24);
	/*
	 * M82: byte 33 is fast_kill and Windows sends 1, always ("fk=1" in
	 * every [CH00] line of every trace). Bytes 32 (vanc_lines) and 34..35
	 * (mix, is_slave) are zero there too, which is what we already send.
	 *
	 * M157: byte 33 is the only field where we deliberately diverge from
	 * Windows - it picks SIGKILL (1) versus SIGINT-and-wait (0) for the
	 * card's teardown of the previous tinyvenc5, and only the second runs
	 * the encoder's destructor and atexit cleanup. Default 0. See the
	 * mz0380_vic_fast_kill block in mz0380-core.c.
	 */
	params[7] = ((mz0380_vic_fast_kill & 0xff) << 8) |	/* [33] */
		    ((mz0380_vic_int_mode & 0xff) << 16);	/* [34] M136 */
	/*
	 * M82: bytes 36..39 are the no-signal fill colour, and Windows sends
	 * back=0, Y=0x00, U=0x80, V=0x80 - neutral grey, not the all-zero
	 * (green) we have been sending. Only consumed when the card falls back
	 * to its NOSG generator, but it costs nothing to match.
	 */
	params[8] = mz0380_vic_nosg;

	/*
	 * M82: Windows precedes EVERY reconfiguration with a stop, and its
	 * "[FIRMWARE RESET]" log line is exactly that - opcode 0x07 with
	 * word[2] = 0xFFFFFFFF ("all channels"), count 3, flag 1
	 * (fire-and-forget), sent from 0x14028cf38 with the reconfiguration
	 * function called on the very next instruction. We have always sent
	 * STOP with an empty payload, and only on the unwind path.
	 *
	 * The traces show a consistent 1.84-1.91 s between that stop and the
	 * SET_VIC that follows it. Part of it is eight msleep() calls inside
	 * the config function; the rest is unattributed, so the safe reading
	 * is that the card is not ready for 0x29 immediately.
	 */
	if (mz0380_win_seq) {
		int stop_ret = mz0380_stream_stop_all(dev, 0);

		pr_info("%s: stream start: pre-STOP(op 0x07, all channels) ret=%d, settling %u ms\n",
			dev->name, stop_ret, mz0380_stop_settle_ms);
		msleep(mz0380_stop_settle_ms);

		/*
		 * Windows registers its capture buffers when the pin opens,
		 * which is before the reconfiguration - the opposite of the
		 * M23 placement below, which was chosen so that op6's iATU
		 * latch would see our addresses. The two only agree when there
		 * is no op6; win_bufs_first=0 keeps the M23 placement.
		 */
		if (mz0380_win_bufs_first) {
			ret = mz0380_stream_program_bufs(dev);
			if (ret) {
				pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
					dev->name, ret);
				goto err_events;
			}
		}
	}

	/*
	 * M155: optionally skip SET_VIC on every cycle after the first, so the
	 * cycle does not respawn tinyvenc5. See the setvic_once parameter.
	 */
	if (mz0380_setvic_once && dev->stream_cycles > 0) {
		pr_info("%s: stream start: SET_VIC SKIPPED (setvic_once=1, cycle %u) - no respawn; if a frame still arrives it did not come from the spawn\n",
			dev->name, dev->stream_cycles);
		ret = 0;
		goto vic_done;
	}

	/* Even a timed-out transaction may already have spawned tinyvenc5. */
	vic_fired = true;
	/*
	 * M169: count it here, on the fire rather than on the result, for
	 * exactly the reason the line above gives - a SET_VIC that times out
	 * may still have forked an encoder, and a spawn budget that only counts
	 * successes would under-report precisely when the card is in trouble.
	 *
	 * This is the single SET_VIC site in the driver, so this counter sees
	 * every spawn including the nosg loop's per-frame respawns, which reach
	 * it through mz0380_dma_start().
	 */
	dev->encoder_spawns++;
	if (dev->encoder_spawns == 8)
		pr_warn("%s: 8 encoder spawns since insmod - entering the 8-18 range where the card has wedged before (recovery is a mains-off cold boot, or mz0380-m52-card-recovery.sh). The budget is per POWER CYCLE, not per insmod: ./mz0380-spawns.sh has the running total\n",
			dev->name);
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (mz0380_stream_nosg)
		pr_info("%s: stream start: SET_VIC(synthetic no-signal source %ux%u@%u %s, fw=%u; host output is polled NV12, not live HDMI/H.264) ret=%d\n",
			dev->name, in_w, in_h, fps,
			interlaced ? "i" : "p", fw, ret);
	else
		/*
		 * M137: fk and int_mode added. Byte 34 (int_mode) was set for
		 * the first time in M136 and this line did not print it, so the
		 * negative result could not be distinguished from "the knob
		 * never landed" - method rule 9, in a line that has been short
		 * of these two bytes all along.
		 */
		pr_info("%s: stream start: SET_VIC(input=%ux%u%s@%u fw=%u in_fmt=%u out_fmt=%u vic_in=%ux%u fk=%u int_mode=%u -> H.264 output=%ux%u, bitstreams=%u) ret=%d\n",
			dev->name, in_w, in_h, interlaced ? "i" : "p",
			fps, fw, in_fmt, out_fmt, vic_in_w, vic_in_h,
			mz0380_vic_fast_kill & 0xff,
			mz0380_vic_int_mode & 0xff,
			out_w, out_h,
			mz0380_bitstream_num & 0xffu, ret);
vic_done:
	dev->stream_cycles++;
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
	/* M82: Windows inserts no settle at all between 0x29 and 0x2a. */
	if (!mz0380_win_seq)
		msleep(mz0380_start_delay_ms);

	/*
	 * SET_VIC launches tinyvenc5, but opcode 0x2d is the host-owned encoder
	 * configuration transaction.  Send it only after tinyvenc5 has consumed
	 * its mandatory first SET_VIC read.  Its non-zero timeout serializes the
	 * full-width mailbox packet through the command EVENT before SET_BUF can
	 * overwrite the shared words.  The NOSG diagnostic emits raw synthetic
	 * NV12 and deliberately skips H.264 configuration/spawn work.
	 *
	 * M82: in the Windows order the encoder commands come AFTER SET_AIC,
	 * so this runs further down instead.
	 */
	if (!mz0380_win_seq && !mz0380_stream_nosg) {
		ret = mz0380_stream_configure_encoder(dev, fps, 0);
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
	if (!mz0380_win_seq || !mz0380_win_bufs_first) {
		ret = mz0380_stream_program_bufs(dev);
		if (ret) {
			pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
				dev->name, ret);
			goto err_events;
		}
	}
	if (!mz0380_stream_nosg)
		pr_info("%s: SET_BUF(op 0x%02x) provides four collision-free buffers (tokens 0..3); 3-bit tokens 4..7 are rejected and acknowledged until a second four-buffer allocation is wired to SET_BUF_8\n",
			dev->name, mz0380_set_buf_opcode);

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
	 *
	 * M151: this one is unconditional on purpose - the enc_stat_ack knob
	 * covers only the per-frame ack. Clearing once at start establishes a
	 * known value to read back at stop, and every result in the file was
	 * taken with it.
	 */
	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();

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
			1u |					/* cmd+16 on=1  */
			((u32)(mz0380_aic_int_mode & 0xff) << 8),/* cmd+17     */
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
	 * M73: photograph the receiver's output stage on both sides of the
	 * encoder kick. If the VIC never sees a clock, the evidence is here and
	 * nowhere the host can otherwise reach - the card's own log is on a
	 * serial port we do not have.
	 */
	/*
	 * M130/M133: the source is locked by now, so 0x48 finally means
	 * something - pick the CSC mode from the colour space it is actually
	 * sending. The diag runs FIRST because its read of 0x48 is the one that
	 * works, and it caches the value we then use; M133 had these the other
	 * way round and the standalone read returned 0x00, selecting the RGB
	 * matrix for a YUV444 source.
	 */
	if (!mz0380_stream_nosg) {
		mz0380_mst3367_output_diag(dev, "before START");
		mz0380_mst3367_apply_csc_mode(dev);
	}

	/*
	 * M82: the Windows tail. Both encoder streams, then POST_PROC. ep.ko
	 * routes 0x2d (45) and 0x31 (49) to a bare sysfs_notify("epint"), the
	 * same wake op 0x06 performs, so this sequence is itself the kick and
	 * Windows never sends 0x06 on the capture path.
	 */
	if (mz0380_win_seq && !mz0380_stream_nosg) {
		ret = mz0380_stream_configure_encoder(dev, fps, 0);
		if (ret)
			goto err_events;
		if (mz0380_enc_sub) {
			ret = mz0380_stream_configure_encoder(dev, fps, 1);
			if (ret)
				goto err_events;
		}
		ret = mz0380_stream_post_proc(dev, fps);
		if (ret)
			goto err_events;
	}

	/*
	 * M128: the baseline (win_seq=0) never sends 0x31 at all, so the only
	 * way to reach fake_frame_off on the path that actually renders is to
	 * send it here. It has to precede op 0x06: the standby-thread guard runs
	 * inside 0x06's handler (new EncodingGroup -> on_start_thread -> Start ->
	 * init_func), and it reads preview_params_settings[ch].byte[0x0a] once,
	 * at pthread_create time. Nothing is sent unless the knob is set, so the
	 * default baseline is byte-identical to before.
	 */
	if (!mz0380_win_seq && !mz0380_stream_nosg &&
	    (mz0380_fake_frame_off || mz0380_post_proc)) {
		ret = mz0380_stream_post_proc(dev, fps);
		if (ret)
			goto err_events;
		/* M128b: see mz0380_post_proc_gap_ms. 0x06 otherwise lands on
		 * the doorbell 9 us later, while the card is still inside the
		 * 0x31 handler. */
		if (mz0380_post_proc_gap_ms) {
			pr_info("%s: stream start: waiting %u ms before op 0x06 (M128b cadence test)\n",
				dev->name, mz0380_post_proc_gap_ms);
			msleep(mz0380_post_proc_gap_ms);
		}
	}

	/*
	 * M139: seed the frame-token registers with a sentinel before the card
	 * can touch them.
	 *
	 * M138's watch reported "0 changes, final 40/44/48 = 0", which is
	 * ambiguous: the card's store_channel_done() writes report[N] - 1 into
	 * one nibble per channel, so a single report carrying buffer index 1
	 * writes zero over a register that was already zero and looks identical
	 * to never having run at all.
	 *
	 * The write is a read-modify-write of one nibble
	 * (reg = (reg & ~(0xf << ch*4)) | (val << ch*4)), so a sentinel makes
	 * it unmistakable: the upper bits survive and only channel 0's nibble
	 * changes.  a5a5a5a5 -> a5a5a5a0 means the encoder reported exactly one
	 * frame from buffer 1; a5a5a5a5 unchanged means it never reported at
	 * all.  Nothing card-side ever reads these, so seeding them is inert.
	 *
	 * Self-validating: if BAR0 0x40 is not host-writable the producer
	 * watch's baseline line reads back something other than the sentinel
	 * and the whole test is void.
	 */
	if (mz0380_token_seed) {
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD0, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD1, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD2, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD3, mz0380_token_seed);
		wmb();
		pr_info("%s: stream start: frame-token sentinel %08x seeded into BAR0 40/44/48/4c; readback %08x/%08x/%08x/%08x\n",
			dev->name, mz0380_token_seed,
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3));
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
	if (!mz0380_win_seq || mz0380_win_start_op6) {
		ret = mz0380_send_command(dev, MZ0380_CMD_START_STREAMING,
					  NULL, 0, NULL, 0);
		pr_info("%s: stream start: START_STREAMING(op 0x06) fired (async, ret=%d)\n",
			dev->name, ret);
	} else {
		ret = 0;
	}

	if (!mz0380_stream_nosg) {
		msleep(500);   /* let the encoder settle into its capture loop */
		mz0380_mst3367_output_diag(dev, "after START");
	}
	dev->stream_head = 0;
	if (ret)
		goto err_events;

	/* M111: last, so it only runs once the stream is genuinely started. */
	mz0380_poll_drain_start(dev);
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
		int stop_ret = mz0380_stream_stop_all(dev, 2000);

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
	/*
	 * M111: stop the poll-drain before anything touches buffer ownership -
	 * it copies out of the stream buffers and re-poisons them, exactly the
	 * race the extent watcher is frozen for below.
	 */
	mz0380_poll_drain_stop(dev);

	if (verbose)
		/* Freeze the diagnostic reader before changing buffer ownership. */
		mz0380_extent_watch_stop(dev);

	if (dev->dma_armed) {
		if (mz0380_stop_on_streamoff) {
			mz0380_stream_stop_all(dev, 2000);
		} else {
			/*
			 * M156: leave the card streaming. Buffers stay mapped
			 * until unload, and teardown clears bus mastering
			 * before freeing them.
			 */
			pr_info("%s: stream stop: STOP_STREAMING SKIPPED (stop_on_streamoff=0) - the card is left streaming on purpose\n",
				dev->name);
		}
	}

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
		mz0380_mst3367_output_diag(dev, "at stop");
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

	/*
	 * M160: the same completeness rule the poll-drain has had since M115,
	 * on the path that actually fires now.
	 *
	 * The poison boundary marks how far the DMA has GOT, not that it has
	 * finished. M115 learned that on the poll path and guarded it there;
	 * this path was left unguarded because `frame_events` was 0 in every
	 * run until fw=7 (M159), so it had never once executed against a live
	 * producer. It then did the exact thing M115 warns about: delivered a
	 * torn 16-byte prefix at ~60 Hz and re-poisoned the buffer underneath
	 * an in-flight transfer, which also destroys the rest of the frame.
	 *
	 * `return` rather than `goto repoison` is the whole point. Leave the
	 * partial buffer exactly as it is and the card finishes writing it;
	 * a later event or the poll-drain then sees a complete frame.
	 */
	if (mz0380_event_require_complete) {
		size_t want = (size_t)dev->capture.source_width *
			      dev->capture.source_height * 3 / 2;

		if (want && len < want) {
			pr_info_ratelimited("%s: frame token %u holds %zu of %zu bytes on the completion event - DMA still in flight, left un-poisoned (M160; event_require_complete=0 to deliver torn prefixes)\n",
					    dev->name, idx, len, want);
			return;
		}
	}

	/*
	 * M168: "H.264 length" was wrong on the path that actually runs. The
	 * poison-boundary inference is format-agnostic and what it measured
	 * here, on every capture this project has, is a raw I420 frame.
	 */
	pr_info_ratelimited("%s: frame token %u inferred payload length=%zu from the 4-byte poison boundary (tail collision risk 2^-32; payload counters %08x/%08x/%08x were not used)\n",
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
	vbuf->vb.field = mz0380_current_field(dev);	/* M172 */
	vbuf->vb.sequence = dev->video_sequence++;
	dev->stream_bufs[idx].delivered++;	/* M168: see the extent report */
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

repoison:
	mz0380_frame_buffer_repoison(dev, idx);
}

/*
 * M111: the poll-drain fallback.
 *
 * The card writes a whole frame and then never tells us. mz0380_drain_frame_snapshot()
 * already knows how to turn "buffer idx holds a frame" into a vb2 delivery - it
 * infers the length from the poison suffix, copies, and re-poisons. The only
 * thing missing on the real path is something to call it, because the
 * completion event that normally supplies the token has never fired.
 *
 * So synthesise the snapshot. token = the buffer index, timestamp = now,
 * everything else zero (the drain only reads those fields for diagnostics).
 * Re-poisoning inside the drain is what makes this idempotent: a frame is
 * picked up on the first pass that sees it and cannot be delivered twice.
 *
 * mz0380_infer_frame_length() returns -ENODATA for a fully-poisoned buffer, so
 * it doubles as the "is there anything here" test and no separate dirty check
 * is needed.
 */
static int mz0380_poll_drain_thread(void *data)
{
	struct mz0380_dev *dev = data;
	unsigned long next_kick = jiffies;
	unsigned long last_delivery = 0;
	bool stall_reported = false;
	unsigned int delivered = 0;
	unsigned int kicks = 0;
	u32 tok[3] = {};
	bool tok_seeded = false;
	unsigned int tok_changes = 0;

	while (!kthread_should_stop()) {
		bool handled = false;
		bool kick_due = false;
		unsigned int idx;

		/*
		 * M138: the producer watch.  BAR0 0x40/0x44/0x48 are updated by
		 * the card's store_channel_done() before its credit test, so
		 * they advance on every card-side frame completion even when no
		 * EVENT is raised.  Until now they were only sampled from
		 * mz0380_handle_event_snapshot(), which never runs after the
		 * first frame - so nobody has ever watched them over time.
		 *
		 *   stays put  -> the encoder wrote channel_done ONCE and is
		 *                 parked in SSM_ReleaseAndReceive; the card's
		 *                 producer is stalled, upstream of tinyvenc5.
		 *   ticks      -> the producer is fine and the loss is ours.
		 *
		 * Three MMIO reads per poll interval, no mailbox traffic, no
		 * I2C: this cannot perturb capture the way M74's watch did.
		 */
		if (READ_ONCE(dev->streaming)) {
			u32 now[3];

			now[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0);
			now[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
			now[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);

			if (!tok_seeded) {
				tok_seeded = true;
				pr_info("%s: producer watch: baseline 40=%08x 44=%08x 48=%08x\n",
					dev->name, now[0], now[1], now[2]);
			} else if (memcmp(now, tok, sizeof(tok))) {
				tok_changes++;
				pr_info("%s: producer watch: change #%u 40=%08x 44=%08x 48=%08x (was %08x/%08x/%08x)\n",
					dev->name, tok_changes, now[0], now[1],
					now[2], tok[0], tok[1], tok[2]);
			}
			memcpy(tok, now, sizeof(tok));
		}


		for (idx = 0; idx < MZ0380_STREAM_NR_BUFS; idx++) {
			struct mz0380_frame_event snapshot = {};
			size_t want;
			size_t len;

			if (!READ_ONCE(dev->streaming))
				break;
			if (mz0380_infer_frame_length(dev, idx, &len) || !len)
				continue;

			/*
			 * M115: only deliver a COMPLETE frame. The poison
			 * boundary marks how far the DMA has got, not that it
			 * has finished, so polling a burst in flight yields a
			 * torn prefix - the first run delivered 794368 bytes
			 * and then the real 3110400, and ffplay rejected the
			 * fragment. The nosg path has always had this check
			 * (mz0380_nosg_frame_landed); the real path needs it
			 * for the same reason.
			 *
			 * The expected size is exact and known: the card
			 * writes width*height*3/2 of 4:2:0. Anything short is
			 * a burst still in progress - leave it alone and it
			 * will be complete on a later pass.
			 */
			want = (size_t)dev->capture.source_width *
			       dev->capture.source_height * 3 / 2;
			if (want && len < want) {
				pr_info_ratelimited("%s: poll-drain: buf %u holds %zu of %zu bytes - DMA still in flight, waiting\n",
						    dev->name, idx, len, want);
				continue;
			}

			snapshot.token = idx;
			snapshot.timestamp_ns = ktime_get_ns();
			pr_info_ratelimited("%s: poll-drain: buf %u holds %zu bytes with no completion event; delivering\n",
					    dev->name, idx, len);
			mz0380_drain_frame_snapshot(dev, &snapshot);
			delivered++;
			last_delivery = jiffies;
			handled = true;
		}

		/*
		 * M117: hand the slot back. mz0380_enc_stat_ack()'s own comment
		 * says it outright - "without this ack the card's encoder
		 * produces exactly one bitstream and then skips every
		 * subsequent frame" - which is precisely the cadence we
		 * measured: one frame per stream, buffers 1-3 never touched.
		 *
		 * mz0380_dma_drain_video() acks after its drain batch, but the
		 * poll path calls mz0380_drain_frame_snapshot() directly and so
		 * skipped it. The card sets enc_stat and never clears it
		 * itself; the driver clears it once at stream start (M40) and,
		 * until now, never again on this path.
		 */
		if (handled) {
			mz0380_enc_stat_ack(dev);
			/*
			 * M118: and the other half of what a real completion
			 * would have done. enc_stat alone (M117) did not move
			 * the cadence off one frame, so the slot handshake is
			 * not the gate. This re-arms the card's one-shot
			 * completion credit, which nothing has ever restored on
			 * this path because the ISR that normally does it runs
			 * only for an event that never fires.
			 */
			if (mz0380_poll_drain_credit)
				mz0380_credit_rearm(dev);

			/*
			 * M120: ask for the NEXT frame, once per frame we just
			 * consumed. tinyvenc5 delivers one frame per
			 * sysfs_notify on /sys/vpl_pciep/epint and op6's ep.ko
			 * handler does nothing but that notify (M22).
			 *
			 * M119 fired this free-running from stream start and it
			 * was WORSE than not firing at all - zero frames, and
			 * the receiver dropped its lock. Our own comment at the
			 * op6 send site says why: op6 must land after the
			 * freshly forked tinyvenc5 has exec'd and consumed the
			 * SET_VIC it reads first, "rather than racing its
			 * start-up read". A timer starting at t=0 races exactly
			 * that, 62 times a second.
			 *
			 * Kicking only after a delivered frame makes the race
			 * impossible: the first kick cannot happen until the
			 * first frame has already arrived. op6_kick_ms is now a
			 * minimum spacing, not a period.
			 */
			kick_due = true;
		}

		/*
		 * M126: keep kicking after the FIRST delivery, not only after
		 * each one. A kick that fires only inside the handled branch
		 * gives exactly one kick per frame, so a one-frame stream is a
		 * one-kick experiment - which cannot tell "the card ignored the
		 * wake-up" apart from "we only ever asked once". The M119/M120
		 * hazard was kicking BEFORE the first frame, racing tinyvenc5's
		 * start-up read of SET_VIC; gating on delivered > 0 keeps that
		 * impossible while letting the wake-up repeat.
		 */
		if (mz0380_kick_repeat && delivered)
			kick_due = true;

		if (kick_due && mz0380_op6_kick_ms &&
		    time_after_eq(jiffies, next_kick)) {
			/*
			 * M126: 0x06 by default; 0x2f / 0x09 are the same
			 * epint notify without the audio_ctrl side effect
			 * (M125).
			 */
			int kick_ret = mz0380_send_command(dev,
							   mz0380_kick_opcode & 0xff,
							   NULL, 0, NULL, 0);

			if (!kicks)
				pr_info("%s: poll-drain: first kick op 0x%02x ret=%d\n",
					dev->name, mz0380_kick_opcode & 0xff,
					kick_ret);
			kicks++;
			next_kick = jiffies +
				msecs_to_jiffies(mz0380_op6_kick_ms);
		}

		/*
		 * M168: the stream is over, so say so rather than blocking the
		 * reader forever. See the mz0380_stall_eos_ms comment in
		 * mz0380-core.c for why one frame is the hardware's bound.
		 */
		if (delivered && !stall_reported && mz0380_stall_eos_ms &&
		    READ_ONCE(dev->streaming) &&
		    time_after(jiffies,
			       last_delivery +
			       msecs_to_jiffies(mz0380_stall_eos_ms))) {
			stall_reported = true;
			pr_info("%s: poll-drain: %u frame(s) delivered and nothing for %u ms - signalling end of stream (DQBUF will return -EIO). fw=%u is a single-shot grabber (M166); stop and restart the stream for another frame, or set stall_eos_ms=0 to block instead\n",
				dev->name, delivered, mz0380_stall_eos_ms,
				mz0380_vic_fw);
			vb2_queue_error(&dev->vb_queue);
		}

		msleep_interruptible(max_t(unsigned int, 1,
					   mz0380_poll_drain_ms));
	}

	pr_info("%s: poll-drain stopped after %u deliveries, %u kicks (op 0x%02x); producer watch saw %u change(s), final 40=%08x 44=%08x 48=%08x\n",
		dev->name, delivered, kicks, mz0380_kick_opcode & 0xff,
		tok_changes, tok[0], tok[1], tok[2]);
	return 0;
}

static void mz0380_poll_drain_start(struct mz0380_dev *dev)
{
	struct task_struct *task;

	if (!mz0380_poll_drain_ms || dev->poll_task || mz0380_stream_nosg)
		return;

	task = kthread_run(mz0380_poll_drain_thread, dev,
			   "mz0380-polldrain/%u", dev->nr);
	if (IS_ERR(task)) {
		pr_warn("%s: poll-drain thread failed to start (%ld)\n",
			dev->name, PTR_ERR(task));
		return;
	}
	dev->poll_task = task;
	pr_info("%s: poll-drain armed every %u ms - frames the card has already written will be delivered without a completion event (M111)\n",
		dev->name, mz0380_poll_drain_ms);
}

static void mz0380_poll_drain_stop(struct mz0380_dev *dev)
{
	if (!dev->poll_task)
		return;
	kthread_stop(dev->poll_task);
	dev->poll_task = NULL;
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
