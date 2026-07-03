/*
 *  Driver for MZ0380 based capture cards.
 *
 *  DMA buffer ring + MSI interrupt handling.
 *
 *  Two rings live in coherent host memory; the card DMAs encoded H.264
 *  bitstream into video_ring and HDMI-extracted PCM samples into
 *  audio_ring. The card produces (advancing TAIL); the host consumes
 *  (advancing HEAD). Both indices are exposed in BAR5 mailbox slots
 *  (see mz0380-reg.h; offsets are CHECKME until verified).
 *
 *  Each ring entry has a small header (flags, byte count, PTS) so the
 *  driver knows how much of the entry actually contains valid payload
 *  for a given completion.
 */

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

static void mz0380_program_ring(struct mz0380_dev *dev,
				const struct mz0380_ring *r)
{
	u64 base = (u64)r->dma;

	mz_cfg_write(dev, r->base_reg_lo, lower_32_bits(base));
	mz_cfg_write(dev, r->base_reg_hi, upper_32_bits(base));
	mz_cfg_write(dev, r->size_reg,    r->entry_size);
	mz_cfg_write(dev, r->entries_reg, r->nr_entries);
	mz_cfg_write(dev, r->head_reg,    0);
}

/*
 * MSI ISR. Reads IRQ_STATUS, dispatches to per-source handlers, and
 * acks. Workqueue takes the heavy lifting (copy bytes into vb2
 * buffers) so the ISR stays short.
 *
 * IRQ status/mask/ack live in BAR0 per sc0710 cross-reference;
 * BAR5 is the property/mailbox window only.
 */
static irqreturn_t mz0380_isr(int irq, void *data)
{
	struct mz0380_dev *dev = data;
	u32 status;

	status = mz_mmio_read(dev, MZ0380_REG_IRQ_STATUS);
	if (!status || status == 0xffffffff)
		return IRQ_NONE;

	atomic_inc(&dev->irq_count);

	if (status & MZ0380_IRQ_CMD_COMPLETE) {
		unsigned int i;
		/* mailbox lives in BAR0 (RE-confirmed) */
		dev->cmd_last_status = mz_mmio_read(dev, MZ0380_MB_STATUS);
		for (i = 0; i < MZ0380_REG_PARAM_MAX; i++)
			dev->cmd_last_param[i] =
				mz_mmio_read(dev, MZ0380_MB_PARAM(i));
		smp_wmb();
		dev->cmd_complete = true;
		wake_up_all(&dev->cmd_wait);
	}

	if (status & MZ0380_IRQ_VIDEO_RING_READY) {
		atomic_inc(&dev->irq_video_count);
		schedule_work(&dev->drain_work);
	}

	if (status & MZ0380_IRQ_AUDIO_RING_READY) {
		atomic_inc(&dev->irq_audio_count);
		mz0380_audio_period_elapsed(dev);
	}

	if (status & MZ0380_IRQ_SIGNAL_CHANGE) {
		atomic_inc(&dev->irq_signal_count);
		mz0380_signal_event(dev);
	}

	if (status & MZ0380_IRQ_FW_READY)
		wake_up_all(&dev->fw_wait);

	if (status & MZ0380_IRQ_ERROR)
		pr_warn("%s: ISR error bit set (status=0x%08x)\n",
			dev->name, status);

	/* ack */
	mz_mmio_write(dev, MZ0380_REG_IRQ_ACK, status);
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

	/* mask everything on the device first */
	mz_mmio_write(dev, MZ0380_REG_IRQ_MASK, 0);

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

	dev->video_ring.base_reg_lo = MZ0380_REG_VIDEO_RING_BASE_LO;
	dev->video_ring.base_reg_hi = MZ0380_REG_VIDEO_RING_BASE_HI;
	dev->video_ring.size_reg    = MZ0380_REG_VIDEO_RING_SIZE;
	dev->video_ring.entries_reg = MZ0380_REG_VIDEO_RING_ENTRIES;
	dev->video_ring.head_reg    = MZ0380_REG_VIDEO_RING_HEAD;
	dev->video_ring.tail_reg    = MZ0380_REG_VIDEO_RING_TAIL;

	ret = mz0380_dma_ring_alloc(dev, &dev->video_ring,
				    mz0380_video_ring_entries,
				    mz0380_video_ring_entry_size);
	if (ret) {
		pr_err("%s: video ring alloc failed (%d)\n",
		       dev->name, ret);
		return ret;
	}

	dev->audio_ring.base_reg_lo = MZ0380_REG_AUDIO_RING_BASE_LO;
	dev->audio_ring.base_reg_hi = MZ0380_REG_AUDIO_RING_BASE_HI;
	dev->audio_ring.size_reg    = MZ0380_REG_AUDIO_RING_SIZE;
	dev->audio_ring.entries_reg = MZ0380_REG_AUDIO_RING_ENTRIES;
	dev->audio_ring.head_reg    = MZ0380_REG_AUDIO_RING_HEAD;
	dev->audio_ring.tail_reg    = MZ0380_REG_AUDIO_RING_TAIL;

	ret = mz0380_dma_ring_alloc(dev, &dev->audio_ring,
				    mz0380_audio_ring_entries,
				    mz0380_audio_ring_entry_size);
	if (ret) {
		pr_err("%s: audio ring alloc failed (%d)\n",
		       dev->name, ret);
		mz0380_dma_ring_free(dev, &dev->video_ring);
		return ret;
	}

	pr_info("%s: video ring %u x %u B @ %pad, audio ring %u x %u B @ %pad\n",
		dev->name,
		dev->video_ring.nr_entries, dev->video_ring.entry_size,
		&dev->video_ring.dma,
		dev->audio_ring.nr_entries, dev->audio_ring.entry_size,
		&dev->audio_ring.dma);

	/* program addresses BEFORE enabling bus master to avoid IOMMU fault */
	mz0380_program_ring(dev, &dev->video_ring);
	mz0380_program_ring(dev, &dev->audio_ring);

	/* unmask interesting IRQ sources (BAR0 per sc0710) */
	mz_mmio_write(dev, MZ0380_REG_IRQ_MASK,
		      MZ0380_IRQ_CMD_COMPLETE |
		      MZ0380_IRQ_VIDEO_RING_READY |
		      MZ0380_IRQ_AUDIO_RING_READY |
		      MZ0380_IRQ_SIGNAL_CHANGE |
		      MZ0380_IRQ_FW_READY |
		      MZ0380_IRQ_ERROR);

	pci_set_master(dev->pci);
	dev->dma_armed = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_dma_setup);

void mz0380_dma_teardown(struct mz0380_dev *dev)
{
	if (dev->dma_armed) {
		pci_clear_master(dev->pci);
		mz_mmio_write(dev, MZ0380_REG_IRQ_MASK, 0);
		dev->dma_armed = false;
	}
	mz0380_dma_ring_free(dev, &dev->audio_ring);
	mz0380_dma_ring_free(dev, &dev->video_ring);
}
EXPORT_SYMBOL_GPL(mz0380_dma_teardown);

/*
 * Plan B start path: XDMA-style scatter-gather, modeled directly on
 * sc0710_dma_channel_start_prep() + sc0710_dma_channel_start().
 *
 * Used only if mz0380_dma_start() (mailbox path) fails to produce
 * any IRQ traffic, suggesting the card actually expects XDMA-shaped
 * programming via BAR0 instead of a custom mailbox ring.
 */
int mz0380_dma_start_xdma(struct mz0380_dev *dev, u32 base, dma_addr_t pt)
{
	if (!dev->dma_armed)
		return -ENODEV;

	/* Reset run bit */
	mz_mmio_write(dev, base + MZ0380_XDMA_CTRL_W1C, MZ0380_XDMA_CTRL_RUN);

	/* Program SG descriptor table base */
	mz_mmio_write(dev, base + MZ0380_XDMA_SG_OFFSET + MZ0380_XDMA_SG_START_H,
		      upper_32_bits(pt));
	mz_mmio_write(dev, base + MZ0380_XDMA_SG_OFFSET + MZ0380_XDMA_SG_START_L,
		      lower_32_bits(pt));
	mz_mmio_write(dev, base + MZ0380_XDMA_SG_OFFSET + MZ0380_XDMA_SG_ADJ, 0);

	/* Reset completed-descriptor-count */
	mz_mmio_write(dev, base + MZ0380_XDMA_CDC, 1);

	/* Set RUN */
	mz_mmio_write(dev, base + MZ0380_XDMA_CTRL_W1S, MZ0380_XDMA_CTRL_RUN);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_dma_start_xdma);

int mz0380_dma_start(struct mz0380_dev *dev)
{
	u32 params[3];

	if (!dev->dma_armed)
		return -ENODEV;

	params[0] = dev->capture.width;
	params[1] = dev->capture.height;
	params[2] = (dev->capture.timeperframe.denominator <<  0) |
		    (dev->capture.timeperframe.numerator   << 16);

	return mz0380_send_command(dev, MZ0380_CMD_START_STREAMING,
				   params, 3, NULL, 2000);
}
EXPORT_SYMBOL_GPL(mz0380_dma_start);

void mz0380_dma_stop(struct mz0380_dev *dev)
{
	if (dev->dma_armed)
		mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				    NULL, 0, NULL, 2000);
}
EXPORT_SYMBOL_GPL(mz0380_dma_stop);

/*
 * Pull all freshly produced entries out of the video ring and hand
 * payload bytes to whichever vb2 buffer is currently at the head of
 * dev->buf_list. Splits or coalesces ring entries to fit V4L2 buffer
 * sizes; sets payload byte count on each completed v4l2 buffer.
 */
void mz0380_dma_drain_video(struct mz0380_dev *dev)
{
	struct mz0380_ring *r = &dev->video_ring;
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	u32 tail;

	if (!r->buf)
		return;

	tail = mz_cfg_read(dev, r->tail_reg);

	while (r->head != tail) {
		void *slot = (u8 *)r->buf + (size_t)r->head * r->entry_size;
		u32 flags_w = *(u32 *)(slot + MZ0380_DESC_FLAGS_OFFSET);
		u32 nbytes  = *(u32 *)(slot + MZ0380_DESC_BYTECOUNT_OFFSET);
		u32 pts_lo  = *(u32 *)(slot + MZ0380_DESC_PTS_LO_OFFSET);
		u32 pts_hi  = *(u32 *)(slot + MZ0380_DESC_PTS_HI_OFFSET);
		void *payload = (u8 *)slot + MZ0380_DESC_PAYLOAD_OFFSET;
		size_t avail;

		if (nbytes == 0 || nbytes > r->entry_size)
			goto advance;

		avail = (size_t)nbytes;

		spin_lock_irqsave(&dev->buf_lock, flags);
		vbuf = list_first_entry_or_null(&dev->buf_list,
						struct mz0380_vb_buffer, list);
		if (vbuf)
			list_del(&vbuf->list);
		spin_unlock_irqrestore(&dev->buf_lock, flags);

		if (!vbuf) {
			/* no consumer ready - drop this slot */
			goto advance;
		}

		{
			void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
			size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
			size_t cpy = min(avail, plane);

			if (dst)
				memcpy(dst, payload, cpy);
			vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, cpy);
		}

		vbuf->vb.vb2_buf.timestamp =
			((u64)pts_hi << 32) | pts_lo;
		vbuf->vb.flags = (flags_w & MZ0380_DESC_FLAG_KEY_FRAME) ?
				 V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
		vb2_buffer_done(&vbuf->vb.vb2_buf,
				(flags_w & MZ0380_DESC_FLAG_ERROR) ?
					VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);

advance:
		r->head = (r->head + 1) % r->nr_entries;
		mz_cfg_write(dev, r->head_reg, r->head);
	}

	r->tail_seen = tail;
}
EXPORT_SYMBOL_GPL(mz0380_dma_drain_video);

void mz0380_dma_drain_audio(struct mz0380_dev *dev)
{
	struct mz0380_ring *r = &dev->audio_ring;
	u32 tail;

	if (!r->buf)
		return;

	tail = mz_cfg_read(dev, r->tail_reg);

	/*
	 * Audio drain is handled in mz0380-audio.c via the ALSA period
	 * callback; here we just bookkeep the consumer index so the
	 * card can keep producing.
	 */
	if (r->head != tail) {
		r->head = tail;
		mz_cfg_write(dev, r->head_reg, r->head);
	}
}
EXPORT_SYMBOL_GPL(mz0380_dma_drain_audio);

static void mz0380_drain_work_fn(struct work_struct *w)
{
	struct mz0380_dev *dev = container_of(w, struct mz0380_dev, drain_work);

	mz0380_dma_drain_video(dev);
	mz0380_dma_drain_audio(dev);
}
