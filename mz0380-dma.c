/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Frame delivery + MSI interrupt handling (RE_FINDINGS.md M17).
 *
 *  The card DMAs encoded H.264 frames into host-allocated buffers via a
 *  PCIe iATU outbound window it programs from the physical addresses we
 *  hand it with the buffer-setter mailbox opcodes. It arms on SET_VIC and
 *  signals a finished frame with an EVENT (BAR0+0x30) whose token
 *  (BAR0+0x40, low 3 bits) names the completed buffer.
 *
 *  FIRST CUT - unverified on hardware. Three things are still open (M17):
 *  whether SET_VIC alone starts frames, the exact frame byte-length
 *  register, and the op->stream mapping. The ISR/drain therefore logs the
 *  candidate status registers and the head of each delivered buffer so a
 *  live run can resolve them.
 */

#include <linux/delay.h>	/* msleep() for the SET_VIC -> START_STREAMING gap */
#include <linux/iommu.h>	/* M26: 4GiB-aligned IOVA placement of the stream bufs */
#include <linux/kthread.h>	/* M36: write-extent watcher */

#include "mz0380.h"

/* forward */
static void mz0380_drain_work_fn(struct work_struct *w);

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
		pr_warn("%s: 4GiB-aligned IOVA setup failed (%d) - falling back to dma_alloc_coherent; the card's DMA will NOT reach these buffers (M25/M26)\n",
			dev->name, ret);
	}

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
 * MSI ISR. The interrupt cause is the EVENT word (BAR0+0x30): bit11 =
 * command-done (snapshot the mailbox params and wake the command waiter),
 * any other non-zero value = a per-channel frame-done (defer to the drain
 * work). Ack via the standard event ack (INT_FLAG=2, EVENT=0, doorbell 0x400).
 */
static irqreturn_t mz0380_isr(int irq, void *data)
{
	struct mz0380_dev *dev = data;
	u32 event;

	event = mz_mmio_read(dev, MZ0380_MB_EVENT);
	if (!event || event == 0xffffffff)
		return IRQ_NONE;

	atomic_inc(&dev->irq_count);

	if (event & MZ0380_MB_EVENT_CMD_DONE) {
		unsigned int i;

		dev->cmd_last_status = mz_mmio_read(dev, MZ0380_MB_STATUS);
		for (i = 0; i < MZ0380_REG_PARAM_MAX; i++)
			dev->cmd_last_param[i] =
				mz_mmio_read(dev, MZ0380_MB_PARAM(i));
		smp_wmb();
		dev->cmd_complete = true;
		wake_up_all(&dev->cmd_wait);
	} else {
		/* frame-done lane(s) - hand off to the drain worker */
		atomic_inc(&dev->irq_video_count);
		schedule_work(&dev->drain_work);
	}

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

	free_irq(dev->irq, dev);
	pci_free_irq_vectors(dev->pci);
	cancel_work_sync(&dev->drain_work);
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
			u8 untouched = mz0380_buf_poison ? mz0380_poison_b() : 0x00;

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
 * Start streaming: program the buffer physaddrs into the card, then arm the
 * encoder with SET_VIC_PARAMS (the same op 0x29 path input-select uses). Per
 * M17 the card's own userspace flips its internal enables in response, so no
 * further host kick should be needed - a live run will confirm.
 */
int mz0380_dma_start(struct mz0380_dev *dev)
{
	u32 params[8] = { 0 };
	u32 w = dev->capture.width, h = dev->capture.height;
	bool interlaced = dev->detected_timings.bt.interlaced;
	u32 fmt = interlaced ? 3 : 2;   /* byte6: venc5 H.264, 2=prog 3=interlaced */
	int ret;

	if (!dev->dma_armed)
		return -ENODEV;

	/*
	 * SET_VIC_PARAMS 44-byte struct - byte offsets VERIFIED by disassembling
	 * video_capture_mgr's op-41 handler (RE_FINDINGS.md M23). The mailbox puts
	 * the opcode at struct[0..3]; our params[i] lands at struct[4+4i..7+4i]
	 * (params[0]=struct[4..7]). Authoritative field map:
	 *   [4]=ch  [5]=fps  [6]=fw/format(2 prog|3 interlaced)  [7]=interlace
	 *   [8..9]=width  [10..11]=height  [12]=m  [16..19]=color_info
	 *   [20..21]=x_start  [22..23]=y_start
	 *   [24..25]=input_frame_width  [26..27]=input_frame_height
	 *   [28]=bitstream_num(MUST be >=1)  [29]=osd_en  [30]=osd_size
	 *   [31]=is_nosg  [35]=is_slave  [36..39]=nosg back/y/u/v
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
	params[0] = ((u32)mz0380_timings_fps(&dev->detected_timings) << 8) |
		    (fmt << 16);                           /* [5]=fps [6]=fw/format */
	params[1] = ((h & 0xffff) << 16) | (w & 0xffff);   /* [8..9]=w [10..11]=h    */
	params[5] = ((h & 0xffff) << 16) | (w & 0xffff);   /* [24..25]=in_w [26..27]=in_h */
	params[6] = 1u | ((mz0380_stream_nosg ? 1u : 0u) << 24); /* [28]=bitstream_num=1, [31]=is_nosg */

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 2000);
	pr_info("%s: stream start: SET_VIC(%ux%u %s H.264, bitstreams=1) ret=%d\n",
		dev->name, w, h, interlaced ? "i" : "p", ret);
	if (ret)
		return ret;

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
		return ret;
	}

	/* M36: poison the buffers and watch the card's write extent live */
	if (mz0380_buf_poison)
		mz0380_extent_watch_start(dev);

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
	if (mz0380_aic_on) {
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
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_dma_start);

void mz0380_dma_stop(struct mz0380_dev *dev)
{
	/* M36: freeze + report the write extents before dumping/stopping */
	mz0380_extent_watch_stop(dev);

	/*
	 * M30: dump the buffers BEFORE telling the card to stop, and the card's
	 * status words with them. This is the only view we have left now that
	 * the DMA no longer faults - successful writes are invisible to the
	 * IOMMU, so the buffer contents are the evidence.
	 */
	pr_info("%s: stream stop: EVENT[0x30]=%08x token[0x40]=%08x 0x44=%08x 0x48=%08x 0x4c=%08x enc[0x50]=%08x irqs=%d/%d\n",
		dev->name,
		mz_mmio_read(dev, MZ0380_MB_EVENT),
		mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN),
		mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1),
		mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2),
		mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3),
		mz_mmio_read(dev, MZ0380_MB_ENC_STATUS),
		atomic_read(&dev->irq_count),
		atomic_read(&dev->irq_video_count));
	mz0380_stream_bufs_dump(dev, "stop");

	if (dev->dma_armed)
		mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				    NULL, 0, NULL, 2000);
}
EXPORT_SYMBOL_GPL(mz0380_dma_stop);

/*
 * Deliver completed frames. The EVENT told us a frame is ready; the token at
 * BAR0+0x40 names the buffer (low 3 bits). FIRST CUT: we don't yet know the
 * authoritative byte-length register, so we log the candidates + the head of
 * the buffer (H.264 access units start with 00 00 00 01) and deliver using the
 * best length candidate, clamped to the buffer size.
 */
void mz0380_dma_drain_video(struct mz0380_dev *dev)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	u32 token, idx, cand0, cand1, cand2, encstat;
	u8 *payload;
	size_t len;

	token   = mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN);
	cand0   = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);   /* 0x44 */
	cand1   = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);   /* 0x48 */
	cand2   = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);   /* 0x4c */
	encstat = mz_mmio_read(dev, MZ0380_MB_ENC_STATUS);     /* 0x50 */
	idx     = token & 7;

	if (idx >= MZ0380_STREAM_NR_BUFS) {
		pr_info_ratelimited("%s: frame token=0x%08x idx=%u out of range (0x44=%08x 0x48=%08x 0x4c=%08x enc=%08x)\n",
				    dev->name, token, idx, cand0, cand1, cand2,
				    encstat);
		return;
	}

	payload = dev->stream_bufs[idx].va;

	/* length candidate: prefer a plausible non-zero payload word */
	len = cand0 && cand0 <= MZ0380_STREAM_BUF_SIZE ? cand0 :
	      cand1 && cand1 <= MZ0380_STREAM_BUF_SIZE ? cand1 :
	      MZ0380_STREAM_BUF_SIZE;

	pr_info_ratelimited("%s: frame token=0x%08x idx=%u len~%zu head=%02x %02x %02x %02x %02x %02x %02x %02x (0x44=%08x 0x48=%08x 0x4c=%08x enc=%08x)\n",
			    dev->name, token, idx, len,
			    payload[0], payload[1], payload[2], payload[3],
			    payload[4], payload[5], payload[6], payload[7],
			    cand0, cand1, cand2, encstat);

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	if (!vbuf)
		return;   /* no consumer ready - drop */

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
		size_t cpy = min(len, plane);

		if (dst)
			memcpy(dst, payload, cpy);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, cpy);
	}

	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	/*
	 * M40: the payload is copied out, so the card's bitstream buffer is
	 * free again - clear enc_stat, which is the only thing that lets the
	 * encoder produce another frame (it sets the byte to 1 itself after
	 * each bitstream DMA and never clears it).
	 */
	mz0380_enc_stat_ack(dev);
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
