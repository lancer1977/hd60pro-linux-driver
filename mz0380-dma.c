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

static void mz0380_stream_bufs_free(struct mz0380_dev *dev)
{
	unsigned int i;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		if (dev->stream_bufs[i].va) {
			dma_free_coherent(&dev->pci->dev, MZ0380_STREAM_BUF_SIZE,
					  dev->stream_bufs[i].va,
					  dev->stream_bufs[i].dma);
			dev->stream_bufs[i].va = NULL;
		}
	}
}

static int mz0380_stream_bufs_alloc(struct mz0380_dev *dev)
{
	unsigned int i;

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

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_STREAM_BUF_STRIDE;
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		u64 phys = (u64)dev->stream_bufs[i].dma;

		params[2 + 2 * i]     = upper_32_bits(phys);
		params[2 + 2 * i + 1] = lower_32_bits(phys);
	}

	return mz0380_send_command(dev, MZ0380_CMD_SET_BUF_2, params,
				   ARRAY_SIZE(params), NULL, 2000);
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

	ret = mz0380_stream_program_bufs(dev);
	if (ret) {
		pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
			dev->name, ret);
		return ret;
	}

	/*
	 * SET_VIC_PARAMS 44-byte struct (RE_FINDINGS.md M20, from video_capture_mgr
	 * op-41). Each param word is 4 struct bytes: params[0]=struct[4..7], etc.
	 *   byte4 channel=0, byte6 format (2 prog / 3 interlaced, selects tinyvenc5)
	 *   byte8..9 width, byte10..11 height, byte22..23 input_w, byte24..25
	 *   input_h, byte26..27 bitstream_num (MUST be >=1, else the encoder emits
	 *   nothing - this was the bug that produced a spawned-but-silent encoder).
	 */
	params[0] = fmt << 16;                        /* struct[6] = format */
	params[1] = ((h & 0xffff) << 16) | (w & 0xffff);
	params[4] = (w & 0xffff) << 16;               /* struct[22..23] input_w */
	params[5] = (1u << 16) | (h & 0xffff);        /* struct[26..27]=1, [24..25]=input_h */

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 2000);
	pr_info("%s: stream start: SET_VIC(%ux%u %s H.264, bitstreams=1) ret=%d\n",
		dev->name, w, h, interlaced ? "i" : "p", ret);
	dev->stream_head = 0;
	return ret;
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
