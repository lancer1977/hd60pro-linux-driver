// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 DMA buffer poisoning, extent diagnostics, and teardown.
 */

#include "mz0380-dma-internal.h"

/* --- M36 write-extent watch ---------------------------------------------- */

/* M37: poison value is a module param so a collision can be ruled out */
u8 mz0380_poison_b(void)
{
	return mz0380_poison_byte & 0xff;
}

u32 mz0380_poison_w(void)
{
	return 0x01010101u * mz0380_poison_b();
}

u32 mz0380_frame_poison_w(const struct mz0380_dev *dev)
{
	return 0x01010101u * dev->frame_poison_byte;
}

u64 mz0380_frame_poison_q(const struct mz0380_dev *dev)
{
	return 0x0101010101010101ULL * dev->frame_poison_byte;
}

/*
 * Real H.264 completion uses the untouched poison suffix as the only
 * host-visible length delimiter.  Re-poisoning happens while enc_stat is still
 * owned by the host, before its ACK permits the endpoint to reuse the slot.
 */
void mz0380_frame_buffer_repoison(struct mz0380_dev *dev, u32 idx)
{
	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->stream_bufs[idx].va ||
	    !smp_load_acquire(&dev->frame_poison_active))
		return;

	memset(dev->stream_bufs[idx].va, dev->frame_poison_byte,
	       MZ0380_STREAM_BUF_SIZE);
	dev->extent_last[idx] = 0;
	dma_wmb();
}

void mz0380_frame_buffers_poison_start(struct mz0380_dev *dev)
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

int mz0380_raw_probe_infer_length(struct mz0380_dev *dev, u32 idx,
				  size_t *length)
{
	const struct mz0380_raw_probe_buf *b;
	const u32 *dwords;
	u32 poison;
	size_t i;

	*length = 0;
	if (idx >= MZ0380_RAW_PROBE_NR_BUFS)
		return -EINVAL;
	b = &dev->raw_probe_bufs[idx];
	if (!b->va)
		return -EINVAL;

	poison = 0x01010101u *
		(idx < MZ0380_STREAM_NR_BUFS ?
		 MZ0380_RAW_PROBE_BANK0_POISON :
		 MZ0380_RAW_PROBE_BANK1_POISON);
	dwords = b->va;
	dma_rmb();
	for (i = MZ0380_RAW_PROBE_BUF_SIZE / sizeof(*dwords); i; i--) {
		if (READ_ONCE(dwords[i - 1]) == poison)
			continue;
		*length = i * sizeof(*dwords);
		if (*length == MZ0380_RAW_PROBE_BUF_SIZE)
			return -ENOSPC;
		return 0;
	}

	return -ENODATA;
}

/*
 * M217: is there a whole frame in this slot, in O(1)?
 *
 * mz0380_raw_probe_infer_length() answers a harder question - "how many bytes
 * did the card write" - by scanning backwards from the end of a 4.6 MB buffer.
 * For the 16-byte stub that walks the entire buffer, so at 60 fps it costs
 * ~276 MB/s of reads before the whole-buffer re-poison writes it back. That is
 * affordable for a diagnostic that runs a few hundred times; it is not
 * affordable for a capture path.
 *
 * We no longer need to infer anything. M213 read the transfer length out of
 * tinyvenc7 (ALIGN16(W) * H * 3/2) and M215 confirmed it on hardware, and M214
 * showed the card only ever writes that length or a 16-byte stub. So the only
 * question is which of the two arrived, and a handful of sentinels answers it.
 *
 * The sentinels sit near the END of the frame - a stub touches only the first
 * sixteen bytes, so anything past that distinguishes the two - and they are
 * SPREAD rather than adjacent. Adjacent sentinels all land in one image region,
 * and a saturated flat area of a real frame could in principle carry the poison
 * value across a few contiguous bytes. Spreading them over different rows and
 * planes makes a false "not landed" require the same coincidence in four
 * unrelated places at once.
 */
/*
 * M221: the raw frame is not always 1920x1080.
 *
 * M213 read the transfer length out of tinyvenc7 as ALIGN16(W) * H * 3/2, and
 * the W/H it uses come from the argv the card's video_capture_mgr builds out of
 * SET_VIC - i.e. from our own capture geometry. M218 then advertised YU12 at
 * 1280x720, 720x480 and 720x576 as well, at which point every use of the
 * 1080p-sized MZ0380_RAW_PROBE_FRAME_SIZE became wrong: sentinels landing past
 * the end of a shorter frame stay poison forever, so the frame never reads as
 * landed and nothing is delivered at all. One of the fixed offsets was even
 * negative below 1048576 bytes.
 */
static u32 mz0380_raw_poison_dword(u32 idx)
{
	return 0x01010101u * (idx < MZ0380_STREAM_NR_BUFS ?
			      MZ0380_RAW_PROBE_BANK0_POISON :
			      MZ0380_RAW_PROBE_BANK1_POISON);
}

size_t mz0380_raw_frame_bytes(struct mz0380_dev *dev)
{
	u32 w = dev->capture.width;
	u32 h = dev->capture.height;

	if (!w || !h)
		return MZ0380_RAW_PROBE_FRAME_SIZE;
	return (size_t)ALIGN(w, 16) * h * 3 / 2;
}

/*
 * Sentinels, as offsets into the frame rather than fixed byte positions.
 *
 * [0] is the last dword and is what actually proves completion, because an
 * ascending DMA writes it last. The rest are spread back through the frame and
 * exist only because that ascending-order assumption is inferred rather than
 * proven: if the card ever writes planes or blocks out of order, requiring the
 * others too still rejects a partial frame. They cost a false negative only
 * when real pixel data happens to equal the poison dword at one of these
 * offsets, which costs one frame and cannot persist.
 */
static void mz0380_raw_sentinel_offsets(size_t frame, size_t out[4])
{
	out[0] = frame - 4;
	out[1] = frame - frame / 16;
	out[2] = frame - frame / 4;
	out[3] = frame / 2;
}

bool mz0380_raw_probe_frame_landed(struct mz0380_dev *dev, u32 idx)
{
	const struct mz0380_raw_probe_buf *b;
	size_t frame, off[4];
	u32 poison;
	unsigned int i;

	if (idx >= MZ0380_RAW_PROBE_NR_BUFS)
		return false;
	b = &dev->raw_probe_bufs[idx];
	if (!b->va)
		return false;

	frame = mz0380_raw_frame_bytes(dev);
	if (frame < 4096 || frame > MZ0380_RAW_PROBE_BUF_SIZE)
		return false;
	mz0380_raw_sentinel_offsets(frame, off);

	poison = mz0380_raw_poison_dword(idx);
	dma_rmb();
	/*
	 * EVERY sentinel must be written, not any of them.
	 *
	 * The first version of this returned true as soon as one sentinel
	 * differed from poison, which defeated the whole guard: a transfer
	 * still in flight has written the early sentinels and not yet the late
	 * ones, so a half-written frame passed the test and was handed to
	 * userspace with new content in its top half and the previous frame
	 * still in its bottom. In OBS that is a continuous flicker, and it was
	 * visible on I420 while H.264 - a different path entirely - was clean.
	 *
	 * Requiring all of them restores the property the comment above always
	 * claimed: mz0380_raw_sentinels[0] is the LAST dword of the frame, so
	 * the test cannot pass until the transfer has reached the end. It is
	 * checked first so an in-flight frame is rejected on the first read.
	 *
	 * The false-negative this trades for is a real frame whose own data
	 * happens to equal the poison dword at one of these four offsets, which
	 * costs one frame and cannot persist, because the next frame's content
	 * differs.
	 */
	for (i = 0; i < 4; i++) {
		const u32 *p = b->va + off[i];

		if (READ_ONCE(*p) == poison)
			return false;
	}
	return true;
}

/*
 * Re-arm only the sentinels. The rest of the buffer does not need restoring:
 * the frame is copied out to a vb2 plane, and the next transfer overwrites the
 * same bytes anyway. Re-poisoning 4.6 MB per frame to protect a test that reads
 * sixteen of them is the same trade the scan above was rejected for.
 */
void mz0380_raw_probe_sentinel_repoison(struct mz0380_dev *dev, u32 idx)
{
	struct mz0380_raw_probe_buf *b;
	size_t frame, off[4];
	u32 poison;
	unsigned int i;

	if (idx >= MZ0380_RAW_PROBE_NR_BUFS)
		return;
	b = &dev->raw_probe_bufs[idx];
	if (!b->va)
		return;

	frame = mz0380_raw_frame_bytes(dev);
	if (frame < 4096 || frame > MZ0380_RAW_PROBE_BUF_SIZE)
		return;
	mz0380_raw_sentinel_offsets(frame, off);

	poison = mz0380_raw_poison_dword(idx);
	for (i = 0; i < 4; i++)
		WRITE_ONCE(*(u32 *)(b->va + off[i]), poison);
	dma_wmb();
}

void mz0380_raw_probe_buffer_repoison(struct mz0380_dev *dev, u32 idx)
{
	struct mz0380_raw_probe_buf *b;
	u8 poison;

	if (idx >= MZ0380_RAW_PROBE_NR_BUFS)
		return;
	b = &dev->raw_probe_bufs[idx];
	if (!b->va)
		return;
	poison = idx < MZ0380_STREAM_NR_BUFS ?
		 MZ0380_RAW_PROBE_BANK0_POISON :
		 MZ0380_RAW_PROBE_BANK1_POISON;
	memset(b->va, poison, MZ0380_RAW_PROBE_BUF_SIZE);
	dma_wmb();
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

void mz0380_extent_watch_stop(struct mz0380_dev *dev)
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

void mz0380_extent_watch_start(struct mz0380_dev *dev)
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

void mz0380_stream_bufs_dump(struct mz0380_dev *dev, const char *tag)
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

void mz0380_raw_probe_bufs_dump(struct mz0380_dev *dev, const char *tag)
{
	unsigned int i;

	if (!mz0380_raw_bank_probe && !mz0380_raw_bank_observe &&
	    !mz0380_raw_deliver)
		return;

	dma_rmb();
	for (i = 0; i < MZ0380_RAW_PROBE_NR_BUFS; i++) {
		const struct mz0380_raw_probe_buf *b = &dev->raw_probe_bufs[i];
		const u8 poison = i < MZ0380_STREAM_NR_BUFS ?
			MZ0380_RAW_PROBE_BANK0_POISON :
			MZ0380_RAW_PROBE_BANK1_POISON;
		const u8 *p = b->va;
		size_t extent = 0, touched = 0, off;

		if (!p)
			continue;
		for (off = MZ0380_RAW_PROBE_BUF_SIZE; off; off--)
			if (READ_ONCE(p[off - 1]) != poison) {
				extent = off;
				break;
			}
		for (off = 0; off < MZ0380_RAW_PROBE_BUF_SIZE;
		     off += PAGE_SIZE)
			if (READ_ONCE(p[off]) != poison)
				touched++;

		pr_info("%s: %s raw bank%u op0x%02x buf[%u] @%pad extent=0x%zx (%zu bytes), %zu/%lu sampled pages touched, poison=0x%02x, completions=%u delivered=%u last_completion_extent=0x%zx, head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dev->name, tag, i / MZ0380_STREAM_NR_BUFS,
			i < MZ0380_STREAM_NR_BUFS ? MZ0380_CMD_SET_BUF_2 :
				MZ0380_CMD_SET_BUF_8,
			i % MZ0380_STREAM_NR_BUFS, &b->dma,
			extent, extent, touched,
			MZ0380_RAW_PROBE_BUF_SIZE / PAGE_SIZE, poison,
			b->completions, b->delivered, b->last_extent,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}
}

static size_t mz0380_h264_written_extent(const u8 *p)
{
	size_t off;

	for (off = MZ0380_H264_BUF_SIZE; off; off--)
		if (READ_ONCE(p[off - 1]) != MZ0380_H264_POISON_BYTE)
			return off;
	return 0;
}

void mz0380_h264_bufs_dump(struct mz0380_dev *dev, const char *tag)
{
	unsigned int i;

	if (!mz0380_h264_probe)
		return;

	dma_rmb();
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		const u8 *p = dev->h264_bufs[i].va;
		size_t extent, touched = 0, off;

		if (!p)
			continue;
		extent = mz0380_h264_written_extent(p);
		for (off = 0; off < MZ0380_H264_BUF_SIZE; off += PAGE_SIZE)
			if (READ_ONCE(p[off]) != MZ0380_H264_POISON_BYTE)
				touched++;

		pr_info("%s: %s H.264 window1 buf[%u] @%pad extent=0x%zx (%zu bytes), %zu/%lu sampled pages touched, %u access unit(s) delivered, head=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			dev->name, tag, i, &dev->h264_bufs[i].dma,
			extent, extent, touched, MZ0380_H264_BUF_SIZE / PAGE_SIZE,
			dev->h264_bufs[i].delivered,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		if (!extent)
			continue;
		print_hex_dump(KERN_INFO, "mz0380 H.264 header: ",
			       DUMP_PREFIX_OFFSET, 16, 1, p,
			       min_t(size_t, extent, 128), false);
		if (extent > 4096)
			print_hex_dump(KERN_INFO, "mz0380 H.264 +0x1000: ",
				       DUMP_PREFIX_OFFSET, 16, 1, p + 4096,
				       min_t(size_t, extent - 4096, 128), false);
	}
}

void mz0380_dma_teardown(struct mz0380_dev *dev)
{
	if (dev->dma_armed) {
		pci_clear_master(dev->pci);
		dev->dma_armed = false;
	}
	kfree(dev->h264_parameter_sets);
	dev->h264_parameter_sets = NULL;
	dev->h264_parameter_sets_len = 0;
	mz0380_raw_probe_bufs_free(dev);
	mz0380_h264_bufs_free(dev);
	mz0380_stream_bufs_free(dev);
}
EXPORT_SYMBOL_GPL(mz0380_dma_teardown);

/*
 * Frames per second of a detected timing, rounded to the nearest whole frame
 * (the card's fps field is a single byte). For interlaced modes this is the
 * FRAME rate, matching how the card labels 1080i60 as 30 frames.
 */
u8 mz0380_timings_fps(const struct v4l2_dv_timings *t)
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
