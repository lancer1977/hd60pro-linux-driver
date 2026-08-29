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
	if (!was_active)
		dev_info(&dev->pci->dev,
			 "NO SIGNAL presentation active (%s): host H.264 IDR replay at %u fps; no card command sent\n",
			 reason, clamp_t(unsigned int, mz0380_no_signal_fps, 1, 10));
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
