// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host-side H.264 NO SIGNAL presentation.
 *
 * Hardware M114 established that the card produces no access unit at all
 * when the MST3367 has no lock. A self-contained IDR is therefore replayed
 * from host memory while VB2 remains attached. It costs no mailbox command,
 * DMA ownership change, or encoder spawn. The embedded access unit is a
 * 1920x1080 High Profile level-4.2 Annex-B frame containing AUD/SPS/PPS/IDR.
 * Its centred 320x240 picture is the card's original NOSG_LOGO_Y, extracted
 * from the user's local tinyvenc5 dump and identity-checked by SHA-256.
 */

#include "mz0380-internal.h"
#include "mz0380-no-signal-data.h"

static unsigned long mz0380_no_signal_interval(void)
{
	unsigned int fps = clamp_t(unsigned int, mz0380_no_signal_fps, 1, 10);

	return max_t(unsigned long, 1, msecs_to_jiffies(DIV_ROUND_UP(1000, fps)));
}

/*
 * M225: the placeholder is an H.264 access unit, so it may only ever be handed
 * to a node that is delivering H.264.
 *
 * S_FMT can put the node in I420 (M218) while every condition that activates
 * this replay still holds. Raw delivery REQUIRES the encoder to keep running -
 * that is what makes the card produce raw at all (M210b) - so the persistent
 * pipeline reattachment in start_streaming and every recovery blip in the
 * receiver monitor activate the placeholder exactly as they would for an
 * encoded session. Nothing downstream of that consulted deliver_raw. The work
 * then took buffers off the shared queue at up to no_signal_fps and returned
 * them holding a few kilobytes of Annex-B with the payload length to match,
 * which an I420 consumer renders as a black or garbage frame.
 *
 * That is what "flicker" on the raw formats is: not stutter, and not a torn
 * picture - the delivered raw frames were measured clean - but correct frames
 * interrupted by placeholder frames wearing the wrong pixel format. It never
 * showed on H264 because there the placeholder is exactly what the node
 * promised. In raw the queue simply holds instead; a stalled last frame is
 * what a consumer expects while a source is unlocked.
 */
static bool mz0380_no_signal_replay_allowed(struct mz0380_dev *dev)
{
	return mz0380_h264_probe && !READ_ONCE(dev->deliver_raw);
}

static void mz0380_no_signal_work_fn(struct work_struct *work)
{
	struct mz0380_dev *dev = container_of(to_delayed_work(work),
					      struct mz0380_dev, no_signal_work);
	struct mz0380_vb_buffer *vbuf = NULL;
	unsigned long flags;
	void *dst;
	size_t plane;

	mutex_lock(&dev->h264_delivery_lock);
	if (!READ_ONCE(dev->streaming) ||
	    !READ_ONCE(dev->no_signal_active) ||
	    !mz0380_h264_probe)
		goto out_unlock;

	if (!mz0380_no_signal_replay_allowed(dev)) {
		/*
		 * The fix, and the measurement that justifies it. Every tick
		 * counted here is one buffer the previous code took off the
		 * queue and handed back holding H.264 while the node was in
		 * I420. A raw session that shows this non-zero is the flicker
		 * accounted for; one that shows zero while still flickering
		 * says this diagnosis is wrong too, before another theory is
		 * built on it.
		 */
		dev->no_signal_frames_suppressed++;
		goto out_unlock;
	}

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		dev->no_signal_frames_missed++;
		goto out_unlock;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
	if (!dst || mz0380_no_signal_h264_len > plane) {
		dev->no_signal_frames_missed++;
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
		vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		goto out_unlock;
	}

	memcpy(dst, mz0380_no_signal_h264, mz0380_no_signal_h264_len);
	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0,
			      mz0380_no_signal_h264_len);
	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = V4L2_FIELD_NONE;
	vbuf->vb.sequence = dev->video_sequence++;
	vbuf->vb.flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			   V4L2_BUF_FLAG_BFRAME);
	vbuf->vb.flags |= V4L2_BUF_FLAG_KEYFRAME;
	dev->no_signal_frames_delivered++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

out_unlock:
	mutex_unlock(&dev->h264_delivery_lock);
	/*
	 * Rescheduled on the same cadence whether or not the placeholder is
	 * allowed to deliver: in raw mode the tick is what counts the frames
	 * the old code would have injected. It costs one no-op work item at
	 * no_signal_fps and only while the source is unlocked.
	 */
	if (READ_ONCE(dev->streaming) && READ_ONCE(dev->no_signal_active))
		mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->no_signal_work,
				 mz0380_no_signal_interval());
}

void mz0380_no_signal_init(struct mz0380_dev *dev)
{
	INIT_DELAYED_WORK(&dev->no_signal_work, mz0380_no_signal_work_fn);
}

void mz0380_no_signal_activate(struct mz0380_dev *dev, const char *reason)
{
	bool was_active = READ_ONCE(dev->no_signal_active);

	WRITE_ONCE(dev->no_signal_active, true);
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	if (!was_active) {
		if (mz0380_no_signal_replay_allowed(dev))
			dev_info(&dev->pci->dev,
				 "NO SIGNAL presentation active (%s): host H.264 IDR replay at %u fps; no card command sent\n",
				 reason,
				 clamp_t(unsigned int, mz0380_no_signal_fps, 1, 10));
		else
			dev_info(&dev->pci->dev,
				 "NO SIGNAL state entered (%s): node is delivering raw, so no placeholder is sent and the queue holds\n",
				 reason);
	}
	/*
	 * Recovery may confirm the same unlocked state every retry interval. Do
	 * not pull an already scheduled placeholder forward to zero each time;
	 * that adds the receiver-poll cadence on top of no_signal_fps.
	 */
	if (!was_active && READ_ONCE(dev->streaming))
		mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->no_signal_work, 0);
}

void mz0380_no_signal_deactivate(struct mz0380_dev *dev)
{
	if (!READ_ONCE(dev->no_signal_active))
		return;
	WRITE_ONCE(dev->no_signal_active, false);
	cancel_delayed_work(&dev->no_signal_work);
	dev_info(&dev->pci->dev,
		 "NO SIGNAL presentation ended at clean live IDR\n");
}

void mz0380_no_signal_stop(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->no_signal_active, false);
	cancel_delayed_work_sync(&dev->no_signal_work);
}
