/*
 *  Probe-safe V4L2 scaffolding for MZ0380 based capture cards.
 */

#include <linux/limits.h>
#include <linux/string.h>

#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "mz0380.h"

#define MZ0380_DEFAULT_COLOR        128
#define MZ0380_MAX_COLOR            255
/* M171: buffers vb2's read() fileio path uses; reported by G/S_PARM. */
#define MZ0380_READ_BUFFERS         2
#define MZ0380_SIZEIMAGE_MIN        (256 * 1024)
#define MZ0380_SIZEIMAGE_MAX        (4 * 1024 * 1024)
#define MZ0380_CID_SC540_RECORD_MODE (V4L2_CID_USER_BASE + 0x10f0)

enum mz0380_record_mode {
	MZ0380_RECORD_MODE_VBR = 0,
	MZ0380_RECORD_MODE_CBR = 1,
	MZ0380_RECORD_MODE_HBR = 2,
};

struct mz0380_mode {
	u32 width;
	u32 height;
};

static const struct mz0380_mode mz0380_modes[] = {
	{ 1920, 1080 },
	{ 1280,  720 },
	{  720,  480 },
	{  720,  576 },
};

static const struct v4l2_fract mz0380_ntsc_frame_intervals[] = {
	{ .numerator = 1, .denominator = 60 },
	{ .numerator = 1, .denominator = 30 },
	{ .numerator = 1, .denominator = 15 },
	{ .numerator = 2, .denominator = 15 },
	{ .numerator = 4, .denominator = 15 },
};

static const struct v4l2_fract mz0380_pal_frame_intervals[] = {
	{ .numerator = 1, .denominator = 50 },
	{ .numerator = 1, .denominator = 25 },
	{ .numerator = 2, .denominator = 25 },
	{ .numerator = 4, .denominator = 25 },
	{ .numerator = 8, .denominator = 25 },
};

static const char * const mz0380_input_names[MZ0380_INPUT_COUNT] = {
	"HDMI",
	"DVI-D",
	"COMPONENTS (YCBCR)",
	"DVI-A (RGB)",
	"SDI",
};

static const char * const mz0380_record_mode_qmenu[] = {
	"VBR",
	"CBR",
	"HBR",
};

static const struct v4l2_ctrl_ops mz0380_ctrl_ops;

static const struct v4l2_ctrl_config mz0380_ctrl_record_mode = {
	.ops = &mz0380_ctrl_ops,
	.id = MZ0380_CID_SC540_RECORD_MODE,
	.name = "SC540 Record Mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = MZ0380_RECORD_MODE_VBR,
	.max = MZ0380_RECORD_MODE_HBR,
	.def = MZ0380_RECORD_MODE_CBR,
	.qmenu = mz0380_record_mode_qmenu,
};

static const struct video_device mz0380_video_template = {
	.name = "mz0380-h264",
};

const char *mz0380_record_mode_name(u32 mode)
{
	if (mode >= ARRAY_SIZE(mz0380_record_mode_qmenu))
		return "unknown";

	return mz0380_record_mode_qmenu[mode];
}

const char *mz0380_input_name(u32 input)
{
	if (input >= ARRAY_SIZE(mz0380_input_names))
		return "unknown";

	return mz0380_input_names[input];
}

static const char *mz0380_h264_profile_name(u32 profile)
{
	switch (profile) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		return "baseline";
	case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
		return "constrained-baseline";
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		return "main";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		return "high";
	default:
		return "other";
	}
}

static const char *mz0380_h264_level_name(u32 level)
{
	switch (level) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
		return "3.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
		return "3.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
		return "3.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
		return "4.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
		return "4.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
		return "4.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		return "5.0";
	default:
		return "other";
	}
}

static const struct v4l2_fract *
mz0380_interval_table(const struct mz0380_capture_state *capture,
		      unsigned int *count)
{
	if (capture->width == 720 && capture->height == 576) {
		*count = ARRAY_SIZE(mz0380_pal_frame_intervals);
		return mz0380_pal_frame_intervals;
	}

	if (capture->width == 720 && capture->height == 480) {
		*count = ARRAY_SIZE(mz0380_ntsc_frame_intervals);
		return mz0380_ntsc_frame_intervals;
	}

	*count = ARRAY_SIZE(mz0380_ntsc_frame_intervals) +
		 ARRAY_SIZE(mz0380_pal_frame_intervals);
	return NULL;
}

static const struct v4l2_fract *
mz0380_interval_by_index(const struct mz0380_capture_state *capture,
			 unsigned int index)
{
	unsigned int count;
	const struct v4l2_fract *table;

	table = mz0380_interval_table(capture, &count);
	if (table)
		return index < count ? &table[index] : NULL;

	if (index < ARRAY_SIZE(mz0380_ntsc_frame_intervals))
		return &mz0380_ntsc_frame_intervals[index];

	index -= ARRAY_SIZE(mz0380_ntsc_frame_intervals);
	if (index < ARRAY_SIZE(mz0380_pal_frame_intervals))
		return &mz0380_pal_frame_intervals[index];

	return NULL;
}

static bool mz0380_interval_equal(const struct v4l2_fract *a,
				  const struct v4l2_fract *b)
{
	return a->numerator == b->numerator &&
	       a->denominator == b->denominator;
}

static const struct v4l2_fract *
mz0380_default_interval_for_size(u32 width, u32 height)
{
	if (width == 720 && height == 576)
		return &mz0380_pal_frame_intervals[0];

	return &mz0380_ntsc_frame_intervals[0];
}

static const struct mz0380_mode *
mz0380_find_mode(u32 width, u32 height)
{
	unsigned int i;
	const struct mz0380_mode *best = &mz0380_modes[0];
	u64 best_delta = U64_MAX;

	for (i = 0; i < ARRAY_SIZE(mz0380_modes); i++) {
		u64 dw = abs((int)mz0380_modes[i].width - (int)width);
		u64 dh = abs((int)mz0380_modes[i].height - (int)height);
		u64 delta = dw + dh;

		if (delta < best_delta) {
			best = &mz0380_modes[i];
			best_delta = delta;
		}
	}

	return best;
}

static const struct v4l2_fract *
mz0380_find_interval(u32 width, u32 height, const struct v4l2_fract *wanted)
{
	unsigned int i;
	unsigned int count = 0;
	const struct v4l2_fract *table;

	if (!wanted || !wanted->numerator || !wanted->denominator)
		return mz0380_default_interval_for_size(width, height);

	table = mz0380_interval_table(&(struct mz0380_capture_state) {
		.width = width,
		.height = height,
	}, &count);
	if (table) {
		for (i = 0; i < count; i++) {
			if (mz0380_interval_equal(wanted, &table[i]))
				return &table[i];
		}
		return mz0380_default_interval_for_size(width, height);
	}

	for (i = 0; i < ARRAY_SIZE(mz0380_ntsc_frame_intervals); i++) {
		if (mz0380_interval_equal(wanted, &mz0380_ntsc_frame_intervals[i]))
			return &mz0380_ntsc_frame_intervals[i];
	}

	for (i = 0; i < ARRAY_SIZE(mz0380_pal_frame_intervals); i++) {
		if (mz0380_interval_equal(wanted, &mz0380_pal_frame_intervals[i]))
			return &mz0380_pal_frame_intervals[i];
	}

	return mz0380_default_interval_for_size(width, height);
}

static u32 mz0380_sizeimage(const struct mz0380_capture_state *capture)
{
	u32 bitrate = max(capture->bitrate, capture->bitrate_peak);
	u32 sizeimage = DIV_ROUND_UP(bitrate, 8);

	sizeimage = clamp(sizeimage, (u32)MZ0380_SIZEIMAGE_MIN,
			  (u32)MZ0380_SIZEIMAGE_MAX);

	return sizeimage;
}

/*
 * With stream_nosg the card's fake-frame generator delivers raw NV12 (M36:
 * one 1920x1107x1.5 contiguous burst; the leading 1920x1080 is the picture),
 * captured by the polling loop in mz0380-dma.c - not H.264. The node
 * advertises whichever format the current mode actually produces.
 */
/*
 * M111: the real path delivers RAW frames too, not H.264, whenever the
 * poll-drain is doing the delivering. buf0 receives exactly
 * 1920*1080*3/2 = 3110400 bytes of 4:2:0 - the card's own splash - and the
 * encoder never produces a bitstream for it. Advertising H.264 there would
 * hand userspace 3 MB of planar YUV labelled as a bytestream, and sizing the
 * plane from the bitrate (12 Mbit/8 = 1.5 MB) would make the drain reject
 * every frame as "does not fit vb2 plane" before it got that far.
 *
 * So both the format and the plane size follow the same switch.
 */
static u32 mz0380_raw_frame_size(const struct mz0380_capture_state *capture)
{
	return capture->width * capture->height * 3 / 2;
}

static u32 mz0380_current_sizeimage(struct mz0380_dev *dev)
{
	u32 size;

	if (mz0380_stream_nosg)
		return MZ0380_NOSG_NV12_SIZEIMAGE;
	if (mz0380_poll_drain_ms)
		size = max(mz0380_raw_frame_size(&dev->capture),
			   mz0380_sizeimage(&dev->capture));
	else
		size = mz0380_sizeimage(&dev->capture);

	/*
	 * M175: a declared frame expectation is also a plane requirement.
	 *
	 * expect_frame_bytes exists because the card does not always write
	 * source-sized frames - tinyvenc8 writes 960x540, and fw=6 is
	 * documented to switch the card's output format to YUY2, which at
	 * 1080p is 4147200 bytes against this plane's 3110400. Without this
	 * the drain would measure a complete frame and then reject it with
	 * "does not fit vb2 plane", which is a different way to lose the same
	 * frame - and a harder one to read, because it looks like a driver bug
	 * rather than an expectation mismatch.
	 *
	 * Bounded by the DMA buffer, since nothing larger can arrive anyway.
	 */
	if (mz0380_expect_frame_bytes)
		size = max_t(u32, size,
			     min_t(u32, mz0380_expect_frame_bytes,
				   MZ0380_STREAM_BUF_SIZE));

	return size;
}

/*
 * M168: ONE place decides the advertised fourcc, because three places used to
 * and they had drifted apart - ENUM_FMT said NV12 while ENUM_FRAMESIZES and
 * ENUM_FRAMEINTERVALS still tested for H.264 and so returned -EINVAL for the
 * very format the node had just enumerated. Every caller now asks here.
 *
 * The poll-drain payload is planar I420 - Y, then a 960x540 U plane, then a
 * 960x540 V plane (M130, confirmed visually AND by correlation: the U/V-swapped
 * rendering gives the textbook red/blue swap, plain yuv420p does not). It was
 * advertised as NV12, which is the same byte count with the chroma
 * INTERLEAVED, so every V4L2 application - ffmpeg, GStreamer, OBS - rendered
 * the magenta/green interleave banding RE_FINDINGS describes. The frame was
 * always right; the label was wrong. V4L2_PIX_FMT_YUV420 is I420.
 *
 * The nosg fake-frame path keeps NV12: its layout was never confirmed either
 * way (different producer, different 1920x1107 geometry, flat logo content
 * where interleave banding would not show), so correcting it would be a guess
 * rather than a measurement. It is a diagnostic path and defaults off.
 */
static u32 mz0380_current_pixelformat(void)
{
	if (mz0380_stream_nosg)
		return V4L2_PIX_FMT_NV12;
	if (mz0380_poll_drain_ms)
		return V4L2_PIX_FMT_YUV420;
	return V4L2_PIX_FMT_H264;
}

static void mz0380_fill_pix_format(struct mz0380_dev *dev,
				   struct v4l2_pix_format *pix)
{
	memset(pix, 0, sizeof(*pix));

	pix->pixelformat = mz0380_current_pixelformat();

	if (mz0380_stream_nosg) {
		pix->width = MZ0380_NOSG_NV12_WIDTH;
		pix->height = MZ0380_NOSG_NV12_HEIGHT;
		pix->bytesperline = MZ0380_NOSG_NV12_WIDTH;
	} else if (mz0380_poll_drain_ms) {
		/* M111: real geometry, raw payload. */
		pix->width = dev->capture.width;
		pix->height = dev->capture.height;
		pix->bytesperline = dev->capture.width;
	} else {
		pix->width = dev->capture.width;
		pix->height = dev->capture.height;
		pix->bytesperline = 0;
	}

	pix->field = mz0380_current_field(dev);
	pix->sizeimage = mz0380_current_sizeimage(dev);
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static void mz0380_apply_try_fmt(struct mz0380_dev *dev, struct v4l2_format *f)
{
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;

	mode = mz0380_find_mode(f->fmt.pix.width, f->fmt.pix.height);
	interval = mz0380_find_interval(mode->width, mode->height,
					&dev->capture.timeperframe);

	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	dev->capture.timeperframe = *interval;
	mz0380_fill_pix_format(dev, &f->fmt.pix);
}

static int mz0380_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct mz0380_dev *dev = video_drvdata(file);

	strscpy(cap->driver, "mz0380", sizeof(cap->driver));
	strscpy(cap->card, mz0380_boards[dev->board].name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(dev->pci));
	/* must equal vdev.device_caps exactly or the v4l2 core WARNs */
	cap->device_caps = dev->vdev.device_caps;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int mz0380_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = mz0380_current_pixelformat();

	if (mz0380_stream_nosg) {
		f->flags = 0;
		strscpy(f->description, "NV12 raw (fake-frame path)",
			sizeof(f->description));
	} else if (mz0380_poll_drain_ms) {
		f->flags = 0;
		strscpy(f->description, "I420 raw (poll-drain path)",
			sizeof(f->description));
	} else {
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		strscpy(f->description, "H.264 bytestream",
			sizeof(f->description));
	}

	return 0;
}

static int mz0380_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mz0380_fill_pix_format(dev, &f->fmt.pix);
	return 0;
}

static int mz0380_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	struct mz0380_capture_state saved = dev->capture;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	/* the format is not negotiable; mz0380_fill_pix_format() sets it */
	mz0380_apply_try_fmt(dev, f);
	dev->capture = saved;

	return 0;
}

static int mz0380_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	/* the format is not negotiable; mz0380_fill_pix_format() sets it */
	mode = mz0380_find_mode(f->fmt.pix.width, f->fmt.pix.height);
	interval = mz0380_find_interval(mode->width, mode->height,
					&dev->capture.timeperframe);

	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	dev->capture.timeperframe = *interval;
	mz0380_fill_pix_format(dev, &f->fmt.pix);

	return 0;
}

static int mz0380_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	if (fsize->pixel_format != mz0380_current_pixelformat())
		return -EINVAL;

	if (mz0380_stream_nosg) {
		if (fsize->index != 0)
			return -EINVAL;
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = MZ0380_NOSG_NV12_WIDTH;
		fsize->discrete.height = MZ0380_NOSG_NV12_HEIGHT;
		return 0;
	}

	if (fsize->index >= ARRAY_SIZE(mz0380_modes))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = mz0380_modes[fsize->index].width;
	fsize->discrete.height = mz0380_modes[fsize->index].height;

	return 0;
}

static int mz0380_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;
	struct mz0380_capture_state capture = { 0 };

	if (fival->pixel_format != mz0380_current_pixelformat())
		return -EINVAL;

	mode = mz0380_find_mode(fival->width, fival->height);
	if (mode->width != fival->width || mode->height != fival->height)
		return -EINVAL;

	capture.width = fival->width;
	capture.height = fival->height;
	interval = mz0380_interval_by_index(&capture, fival->index);
	if (!interval)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = *interval;

	return 0;
}

/*
 * M170: an HDMI capture input that could not say whether anything was plugged
 * into it.
 *
 * `capabilities` was left at 0 even though this driver implements the whole
 * DV-timings ioctl set. V4L2_IN_CAP_DV_TIMINGS is how an application learns
 * that QUERY_DV_TIMINGS is worth calling at all, so without it a well-behaved
 * client never asks - and asking is the only way to get the geometry off this
 * card, since the card never pushes format to the host (M6).
 *
 * `status` was left at 0, which in V4L2 means "no problems". So the node
 * asserted a healthy source unconditionally, including with the cable out. The
 * standard bit for this is V4L2_IN_ST_NO_SIGNAL and it is precisely the thing
 * this project has spent whole sessions determining by hand.
 *
 * The live query is skipped while streaming: it reads the MST3367 over the
 * mailbox I2C proxy, and M74 is the reminder that a watch running alongside a
 * capture can perturb the capture. Streaming already knows what it locked, so
 * the cached flag is both cheaper and correct there.
 *
 * Only the selected input can be measured - the receiver serves one at a time -
 * so the others report NO_SIGNAL rather than claiming a clean bill of health
 * for a path nothing has looked at.
 */
static int mz0380_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	struct mz0380_dev *dev = video_drvdata(file);
	unsigned int index = inp->index;
	bool locked = false;

	if (index >= ARRAY_SIZE(mz0380_input_names))
		return -EINVAL;

	if (index == dev->capture.input) {
		if (READ_ONCE(dev->streaming)) {
			locked = dev->signal_locked;
		} else {
			struct v4l2_dv_timings live;

			locked = !mz0380_query_signal(dev, &live);
		}
	}

	memset(inp, 0, sizeof(*inp));
	inp->index = index;
	strscpy(inp->name, mz0380_input_names[index], sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	if (!locked)
		inp->status = V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

static int mz0380_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct mz0380_dev *dev = video_drvdata(file);

	*i = dev->capture.input;
	return 0;
}

static int mz0380_s_input(struct file *file, void *priv, unsigned int i)
{
	struct mz0380_dev *dev = video_drvdata(file);

	return mz0380_request_input_select(dev, i, "v4l2");
}

static int mz0380_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *a)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = dev->capture.timeperframe;
	/*
	 * M171: the node advertises V4L2_CAP_READWRITE, so read() is a
	 * supported I/O method and V4L2 requires this field to say how many
	 * buffers it uses. The memset left it 0, which v4l2-compliance reports
	 * as "!cap->readbuffers" - a device claiming read() support while
	 * declaring it has no buffers to read into.
	 */
	a->parm.capture.readbuffers = MZ0380_READ_BUFFERS;

	return 0;
}

static int mz0380_s_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *a)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct v4l2_fract *interval;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	interval = mz0380_find_interval(dev->capture.width, dev->capture.height,
					&a->parm.capture.timeperframe);
	dev->capture.timeperframe = *interval;

	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = dev->capture.timeperframe;
	a->parm.capture.readbuffers = MZ0380_READ_BUFFERS;

	return 0;
}

/*
 * M170: V4L2_EVENT_SOURCE_CHANGE was unreachable from both ends.
 *
 * mz0380_signal_event() builds and queues one, and the ops table pointed
 * vidioc_subscribe_event straight at v4l2_ctrl_subscribe_event, which accepts
 * V4L2_EVENT_CTRL and nothing else. So SUBSCRIBE_EVENT(SOURCE_CHANGE) returned
 * -EINVAL to every application, and the event - had anything emitted one - had
 * no subscriber it could reach. The feature was listed as implemented in
 * PLAN.md and was not reachable by any client.
 */
static int mz0380_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

static int mz0380_log_status(struct file *file, void *priv)
{
	struct mz0380_dev *dev = video_drvdata(file);

	v4l2_ctrl_handler_log_status(&dev->ctrl_handler, dev->name);
	return 0;
}

static int mz0380_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctrl->val = dev->capture.bitrate;
		return 0;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		ctrl->val = dev->capture.quality;
		return 0;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctrl->val = dev->capture.gop_size;
		return 0;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		ctrl->val = dev->capture.b_frames;
		return 0;
	case MZ0380_CID_SC540_RECORD_MODE:
		ctrl->val = dev->capture.record_mode;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mz0380_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	switch (ctrl->id) {
	case MZ0380_CID_SC540_RECORD_MODE:
		return mz0380_request_record_mode_locked(dev, ctrl->val,
							 "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		return mz0380_request_bitrate_locked(dev, ctrl->val,
						     "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		return mz0380_request_quality_locked(dev, ctrl->val,
						     "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_BITRATE_PEAK:
		dev->capture.bitrate_peak = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		return mz0380_request_gop_locked(dev, ctrl->val,
						 "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		return mz0380_request_b_frames_locked(dev, ctrl->val,
						      "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		dev->capture.h264_profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		dev->capture.h264_level = ctrl->val;
		break;
	case V4L2_CID_BRIGHTNESS:
		dev->capture.brightness = ctrl->val;
		break;
	case V4L2_CID_CONTRAST:
		dev->capture.contrast = ctrl->val;
		break;
	case V4L2_CID_HUE:
		dev->capture.hue = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		dev->capture.saturation = ctrl->val;
		break;
	case V4L2_CID_SHARPNESS:
		dev->capture.sharpness = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mz0380_ctrl_ops = {
	.g_volatile_ctrl = mz0380_g_volatile_ctrl,
	.s_ctrl = mz0380_s_ctrl,
};

/* ===== vb2 queue ops ================================================= */

static int mz0380_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers, unsigned int *nplanes,
			      unsigned int sizes[], struct device *alloc_devs[])
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = size;
	if (*nbuffers < 3)
		*nbuffers = 3;
	return 0;
}

static int mz0380_buf_prepare(struct vb2_buffer *vb)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&dev->pci->dev,
			"buffer too small (%lu < %u)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

static void mz0380_buf_queue(struct vb2_buffer *vb)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mz0380_vb_buffer *buf =
		container_of(vbuf, struct mz0380_vb_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static u32 mz0380_source_fps(const struct v4l2_dv_timings *timings);
static int mz0380_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	int ret;

	if (!dev->dma_armed) {
		dev_warn(&dev->pci->dev,
			 "start_streaming called but DMA is not armed (enable_dma=1?)\n");
		ret = -ENODEV;
		goto error;
	}

	/*
	 * Real capture is driven by whatever the receiver is actually locked
	 * to, so re-detect here: SET_VIC carries the geometry AND the frame
	 * rate down to the card, and a stale timing would arm the encoder for
	 * the wrong cadence. The fake-frame generator ignores the input, so a
	 * lock failure is only fatal for the real path.
	 */
	if (!mz0380_stream_nosg) {
		struct v4l2_dv_timings live;

		ret = mz0380_query_signal(dev, &live);
		if (ret && dev->have_last_good && mz0380_signal_cache_ms &&
		    time_before(jiffies, dev->last_good_stamp +
				msecs_to_jiffies(mz0380_signal_cache_ms))) {
			/*
			 * M65: no live lock, but we detected this source's mode
			 * moments ago. A source that transmits in ~300ms bursts
			 * around a power-cycle can never be locked at the
			 * instant STREAMON runs, and SET_VIC only needs the
			 * geometry - which has not changed. Arm with what we
			 * measured; the frames are black until the source
			 * transmits again, which is an EDID problem, not a
			 * reason to refuse to stream.
			 */
			live = dev->last_good_timings;
			dev->detected_timings = live;
			dev->signal_locked = true;
			dev->capture.source_width = live.bt.width;
			dev->capture.source_height = live.bt.height;
			dev->capture.source_fps = mz0380_source_fps(&live);
			dev->capture.source_interlaced = live.bt.interlaced;
			if (dev->capture.source_fps) {
				dev->capture.timeperframe.numerator = 1;
				dev->capture.timeperframe.denominator =
					dev->capture.source_fps;
			}
			dev_info(&dev->pci->dev,
				 "no live lock (%d) - arming with the detection from %ums ago (%ux%u%c) [signal_cache_ms=%u]\n",
				 ret,
				 jiffies_to_msecs(jiffies - dev->last_good_stamp),
				 live.bt.width, live.bt.height,
				 live.bt.interlaced ? 'i' : 'p',
				 mz0380_signal_cache_ms);
			ret = 0;
		} else if (ret && mz0380_stream_without_signal) {
			/*
			 * M113: no source, on purpose. Arm at 1080p60 so the
			 * card runs its normal capture path and falls back to
			 * its own no-signal splash - which is what Windows
			 * delivers to OBS in this exact situation.
			 */
			dev->capture.source_width = 1920;
			dev->capture.source_height = 1080;
			dev->capture.source_fps = 60;
			dev->capture.source_interlaced = false;
			dev->capture.timeperframe.numerator = 1;
			dev->capture.timeperframe.denominator = 60;
			dev->signal_locked = false;
			dev_info(&dev->pci->dev,
				 "no HDMI signal locked (%d) - arming anyway at 1920x1080p60 [stream_without_signal=1]; expect the card's no-signal splash, not source pixels\n",
				 ret);
			ret = 0;
		} else if (ret) {
			dev_warn(&dev->pci->dev,
				 "no HDMI signal locked (%d) - check the source, HPD and EDID load\n",
				 ret);
			goto error;
		}

		dev_info(&dev->pci->dev,
			 "live input %ux%u%c@%u -> encoder output %ux%u@%u\n",
			 dev->capture.source_width, dev->capture.source_height,
			 dev->capture.source_interlaced ? 'i' : 'p',
			 dev->capture.source_fps, dev->capture.width,
			 dev->capture.height,
			 dev->capture.timeperframe.denominator /
			 max_t(u32, dev->capture.timeperframe.numerator, 1));

		/*
		 * M174: the buffers and the card must agree on the frame size.
		 *
		 * Two geometries have always existed side by side and nothing
		 * checked they matched. mz0380_queue_setup() sizes the vb2
		 * plane from capture.width/height - what the application
		 * negotiated, 1920x1080 unless it said otherwise - while the
		 * drain measures and delivers
		 * source_width * source_height * 3/2, what the card actually
		 * wrote. On the only source this project has ever tested those
		 * are the same number, so the gap has never shown.
		 *
		 * With a 720p source they differ: the card writes 1382400
		 * bytes, the drain delivers them, and the application is
		 * holding a buffer it was told is 1920x1080 with 1382400 bytes
		 * of payload in it. Nothing errors. It just renders garbage,
		 * which is the worst available outcome and precisely the class
		 * of defect M168 was about.
		 *
		 * Refusing is the honest answer, and it is also the useful one:
		 * the source-change event queued below tells a well-behaved
		 * client to re-negotiate, S_FMT to the detected geometry then
		 * resizes the buffers, and the capture works. Delivering the
		 * mislabelled frame would leave the client no way to find out.
		 *
		 * NOTE: the non-1080p path is CORRECT BY CONSTRUCTION here, not
		 * verified - this project has never had a non-1080p source. All
		 * that is proven is that the old behaviour was wrong.
		 */
		if (mz0380_strict_geometry && dev->signal_locked &&
		    dev->capture.source_width &&
		    dev->capture.source_height &&
		    (dev->capture.source_width != dev->capture.width ||
		     dev->capture.source_height != dev->capture.height)) {
			dev_err(&dev->pci->dev,
				"refusing to stream: the source is %ux%u but the buffers were allocated for %ux%u, and this path has no scaler - the card would write %u bytes into a buffer described as %u. S_FMT to %ux%u (or VIDIOC_SUBSCRIBE_EVENT the source change) and try again\n",
				dev->capture.source_width,
				dev->capture.source_height,
				dev->capture.width, dev->capture.height,
				dev->capture.source_width *
				dev->capture.source_height * 3 / 2,
				dev->capture.width * dev->capture.height * 3 / 2,
				dev->capture.source_width,
				dev->capture.source_height);
			if (dev->video_registered)
				mz0380_signal_event(dev);
			ret = -EPIPE;
			goto error;
		}
	}

	/*
	 * Fake-frame path: no completion IRQ exists (M41), so streaming is a
	 * polling kthread that spawns the encoder itself, once per frame
	 * (M39). The real path arms the encoder here and delivers from the
	 * MSI-driven drain.
	 */
	/* Completion work may run as soon as START is posted. */
	dev->streaming = true;
	if (mz0380_stream_nosg)
		ret = mz0380_nosg_capture_start(dev);
	else
		ret = mz0380_dma_start(dev);
	if (ret) {
		dev->streaming = false;
		dev_err(&dev->pci->dev,
			"%s failed (%d)\n",
			mz0380_stream_nosg ? "nosg_capture_start" : "dma_start",
			ret);
		goto error;
	}

	return 0;

error:
	{
		struct mz0380_vb_buffer *buf, *tmp;
		unsigned long flags;
		spin_lock_irqsave(&dev->buf_lock, flags);
		list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_QUEUED);
		}
		spin_unlock_irqrestore(&dev->buf_lock, flags);
	}
	return ret;
}

static void mz0380_stop_streaming(struct vb2_queue *vq)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	struct mz0380_vb_buffer *buf, *tmp;
	unsigned long flags;

	/*
	 * nosg: the capture thread owns the start/stop cycle; its last
	 * iteration already sent STOP, so only the thread needs joining. The
	 * verbose dma_stop still runs for its end-of-stream diagnostics dump.
	 */
	dev->streaming = false;
	mz0380_nosg_capture_stop(dev);
	mz0380_dma_stop(dev);

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static const struct vb2_ops mz0380_qops = {
	.queue_setup     = mz0380_queue_setup,
	.buf_prepare     = mz0380_buf_prepare,
	.buf_queue       = mz0380_buf_queue,
	.start_streaming = mz0380_start_streaming,
	.stop_streaming  = mz0380_stop_streaming,
#ifdef MZ0380_HAVE_VB2_WAIT_OPS
	/*
	 * Kernels up to 6.x require the driver to drop q->lock around a
	 * blocking DQBUF itself. The ops and the vb2_ops_wait_* helpers were
	 * removed once vb2 core took that over, so newer kernels must not set
	 * them. The Makefile probes videobuf2-v4l2.h rather than testing
	 * LINUX_VERSION_CODE - the removal release is not worth guessing.
	 */
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};

/* ===== HDMI signal detect ============================================ */

static const struct v4l2_dv_timings mz0380_no_signal = {
	.type = V4L2_DV_BT_656_1120,
};

static const struct v4l2_dv_timings_cap mz0380_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 640,
		.max_width = 1920,
		.min_height = 480,
		.max_height = 1080,
		.min_pixelclock = 24000000,
		.max_pixelclock = 297000000,
		.standards = V4L2_DV_BT_STD_CEA861,
		.capabilities = V4L2_DV_BT_CAP_INTERLACED |
				V4L2_DV_BT_CAP_PROGRESSIVE,
	},
};

static u32 mz0380_source_fps(const struct v4l2_dv_timings *timings)
{
	const struct v4l2_bt_timings *bt = &timings->bt;
	u64 total = (u64)V4L2_DV_BT_FRAME_WIDTH(bt) *
		    V4L2_DV_BT_FRAME_HEIGHT(bt);

	if (!total || !bt->pixelclock)
		return 0;

	return min_t(u64, 255,
		     div_u64(bt->pixelclock + total / 2, total));
}

/*
 * HDMI signal query. The card never pushes format to the host (RE_FINDINGS.md
 * M6), but the host reads it straight off the MST3367 receiver over the mailbox
 * I2C proxy once the receiver is out of reset (M15). We delegate to
 * mz0380_mst3367_read_signal(), which brings the receiver up on first use, then
 * reads the mode-detect registers and maps the geometry to a v4l2_dv_timings.
 * -ENOLCK = receiver locked to nothing; -ENODEV = firmware not ready.
 */
int mz0380_query_signal(struct mz0380_dev *dev,
			struct v4l2_dv_timings *out)
{
	/*
	 * M170: remember what we last told userspace, so a change can be
	 * reported as one. mz0380_signal_event() has existed since bring-up
	 * and NOTHING HAS EVER CALLED IT - the source-change event was
	 * generated by dead code, to a subscription the ops table refused to
	 * accept. Both halves are fixed together or neither is worth fixing.
	 *
	 * There is no background signal poller in this driver, so the event
	 * fires when something asks - QUERY_DV_TIMINGS, ENUM_INPUT, or arming
	 * a stream. That is honest and useful for a client that polls; a
	 * client that only sleeps on the event still needs a poller, which is
	 * deliberately not added here because it would drive receiver I2C on a
	 * timer and M74 is what that costs during a capture.
	 */
	bool was_locked = dev->signal_locked;
	struct v4l2_dv_timings prev = dev->detected_timings;
	int ret = mz0380_mst3367_read_signal(dev, out);

	if (ret) {
		dev->signal_locked = false;
		dev->detected_timings = mz0380_no_signal;
		dev->capture.source_width = 0;
		dev->capture.source_height = 0;
		dev->capture.source_fps = 0;
		dev->capture.source_interlaced = false;
		*out = mz0380_no_signal;
		if (was_locked && dev->video_registered)
			mz0380_signal_event(dev);
		return ret == -ENODEV ? -ENOLCK : ret;
	}

	if (dev->video_registered &&
	    (!was_locked || !v4l2_match_dv_timings(&prev, out, 250000, false)))
		mz0380_signal_event(dev);

	dev->signal_locked = true;
	dev->detected_timings = *out;
	dev->set_timings = *out;	/* M171: G tracks reality absent an S */
	dev->last_good_timings = *out;      /* M65: survives later failures */
	dev->last_good_stamp = jiffies;
	dev->have_last_good = true;
	dev->capture.source_width = out->bt.width;
	dev->capture.source_height = out->bt.height;
	dev->capture.source_fps = mz0380_source_fps(out);
	dev->capture.source_interlaced = out->bt.interlaced;
	if (dev->capture.source_fps) {
		/* The firmware exposes one whole-frame fps byte; report that cadence. */
		dev->capture.timeperframe.numerator = 1;
		dev->capture.timeperframe.denominator =
			dev->capture.source_fps;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_query_signal);

void mz0380_signal_event(struct mz0380_dev *dev)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue(&dev->vdev, &ev);
}
EXPORT_SYMBOL_GPL(mz0380_signal_event);

static int mz0380_query_dv_timings(struct file *file, void *fh,
				   struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);
	return mz0380_query_signal(dev, t);
}

/*
 * M171: G returns what was SET, QUERY returns what is DETECTED.
 *
 * Both used to answer with the live detection, and S_DV_TIMINGS was a comment
 * saying "no-op-but-validate" that neither noted nor validated. So
 * S_DV_TIMINGS(x) followed by G_DV_TIMINGS returned something else entirely,
 * which is what v4l2-compliance reported as
 * "g_timings.bt.width != enumtimings.timings.bt.width".
 *
 * The card does auto-detect and nothing host-side changes what it receives, so
 * the set value does not steer the hardware - stream start uses the detection.
 * But the ioctl contract is still a contract, and a successful detection
 * updates the set value too, so G tracks reality for anyone who never calls S.
 */
static int mz0380_g_dv_timings(struct file *file, void *fh,
			       struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);

	*t = dev->set_timings;
	return 0;
}

static int mz0380_s_dv_timings(struct file *file, void *fh,
			       struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;

	if (!v4l2_valid_dv_timings(t, &mz0380_timings_cap, NULL, NULL))
		return -EINVAL;

	/*
	 * M172: busy only blocks a CHANGE.
	 *
	 * A bare vb2_is_busy() test failed v4l2-compliance's
	 * testCanSetSameTimings: it allocates buffers and then sets the timings
	 * it already has, which must succeed because nothing about the buffers
	 * is invalidated by setting a value to itself. -EBUSY is for the case
	 * where the new timings would resize the format out from under
	 * allocated buffers.
	 */
	if (vb2_is_busy(&dev->vb_queue) &&
	    !v4l2_match_dv_timings(&dev->set_timings, t, 0, false))
		return -EBUSY;

	dev->set_timings = *t;

	/*
	 * M172: and the format follows the timings.
	 *
	 * v4l2-compliance caught this as
	 * "fmt.fmt.pix.width >= enumtimings.timings.bt.width * 1.5" - it set a
	 * small timing, asked for the format, and was still told 1920x1080,
	 * because S_DV_TIMINGS touched nothing but its own stored copy.
	 *
	 * For an HDMI receiver the capture format IS the source geometry; there
	 * is no scaler on this path, and the delivered frame is exactly
	 * width*height*3/2 of I420. So a client that declares the incoming
	 * timings must get a format that matches them, snapped to a mode this
	 * driver supports.
	 */
	mode = mz0380_find_mode(t->bt.width, t->bt.height);
	interval = mz0380_find_interval(mode->width, mode->height,
					&dev->capture.timeperframe);
	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	dev->capture.timeperframe = *interval;

	return 0;
}

static int mz0380_enum_dv_timings(struct file *file, void *fh,
				  struct v4l2_enum_dv_timings *t)
{
	return v4l2_enum_dv_timings_cap(t, &mz0380_timings_cap, NULL, NULL);
}

static int mz0380_dv_timings_cap(struct file *file, void *fh,
				 struct v4l2_dv_timings_cap *cap)
{
	*cap = mz0380_timings_cap;
	return 0;
}

/* ===== ioctl table & file ops ======================================== */

static const struct v4l2_ioctl_ops mz0380_video_ioctl_ops = {
	.vidioc_querycap = mz0380_querycap,
	.vidioc_enum_fmt_vid_cap = mz0380_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = mz0380_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = mz0380_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = mz0380_s_fmt_vid_cap,
	.vidioc_enum_framesizes = mz0380_enum_framesizes,
	.vidioc_enum_frameintervals = mz0380_enum_frameintervals,
	.vidioc_enum_input = mz0380_enum_input,
	.vidioc_g_input = mz0380_g_input,
	.vidioc_s_input = mz0380_s_input,
	.vidioc_g_parm = mz0380_g_parm,
	.vidioc_s_parm = mz0380_s_parm,
	.vidioc_log_status = mz0380_log_status,
	.vidioc_subscribe_event = mz0380_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	/* streaming */
	.vidioc_reqbufs       = vb2_ioctl_reqbufs,
	.vidioc_create_bufs   = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf   = vb2_ioctl_prepare_buf,
	.vidioc_querybuf      = vb2_ioctl_querybuf,
	.vidioc_qbuf          = vb2_ioctl_qbuf,
	.vidioc_dqbuf         = vb2_ioctl_dqbuf,
	.vidioc_expbuf        = vb2_ioctl_expbuf,
	.vidioc_streamon      = vb2_ioctl_streamon,
	.vidioc_streamoff     = vb2_ioctl_streamoff,

	/* HDMI signal */
	.vidioc_query_dv_timings  = mz0380_query_dv_timings,
	.vidioc_g_dv_timings      = mz0380_g_dv_timings,
	.vidioc_s_dv_timings      = mz0380_s_dv_timings,
	.vidioc_enum_dv_timings   = mz0380_enum_dv_timings,
	.vidioc_dv_timings_cap    = mz0380_dv_timings_cap,
};

static const struct v4l2_file_operations mz0380_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int mz0380_ctrls_init(struct mz0380_dev *dev)
{
	v4l2_ctrl_handler_init(&dev->ctrl_handler, 14);
	dev->ctrl_handler.lock = &dev->ctrl_lock;
	dev->ctrl_handler_initialized = true;

	dev->record_mode_ctrl =
		v4l2_ctrl_new_custom(&dev->ctrl_handler,
				     &mz0380_ctrl_record_mode, NULL);
	if (dev->record_mode_ctrl)
		dev->record_mode_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->bitrate_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_BITRATE,
				  MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
				  256 * 1024, MZ0380_DEFAULT_BITRATE);
	if (dev->bitrate_ctrl)
		dev->bitrate_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->quality_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
				  MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY,
				  1, MZ0380_DEFAULT_QUALITY);
	if (dev->quality_ctrl)
		dev->quality_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
			  MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
			  256 * 1024, MZ0380_DEFAULT_BITRATE);
	dev->gop_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
				  MZ0380_MIN_GOP, MZ0380_MAX_GOP, 1,
				  MZ0380_DEFAULT_GOP);
	if (dev->gop_ctrl)
		dev->gop_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->b_frames_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_B_FRAMES,
				  0, MZ0380_MAX_B_FRAMES, 1,
				  MZ0380_DEFAULT_B_FRAMES);
	if (dev->b_frames_ctrl)
		dev->b_frames_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std_menu(&dev->ctrl_handler, &mz0380_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, 0,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
	v4l2_ctrl_new_std_menu(&dev->ctrl_handler, &mz0380_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_HUE, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_SATURATION, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_SHARPNESS, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);

	if (dev->ctrl_handler.error) {
		int err = dev->ctrl_handler.error;

		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
		return err;
	}

	return 0;
}

void mz0380_capture_state_init(struct mz0380_dev *dev)
{
	/*
	 * M171: give G_DV_TIMINGS a valid answer before anything has been
	 * detected or set. A zeroed struct has type 0, which is not a valid
	 * v4l2_dv_timings type; mz0380_no_signal carries the right type with
	 * empty timings, which is the honest "nothing here yet".
	 */
	dev->set_timings = mz0380_no_signal;
	dev->capture.width = 1920;
	dev->capture.height = 1080;
	dev->capture.timeperframe = mz0380_ntsc_frame_intervals[0];
	dev->capture.source_width = 1920;
	dev->capture.source_height = 1080;
	dev->capture.source_fps = 60;
	dev->capture.source_interlaced = false;
	dev->capture.input = 0;
	dev->capture.record_mode = MZ0380_RECORD_MODE_CBR;
	dev->capture.bitrate = MZ0380_DEFAULT_BITRATE;
	dev->capture.quality = MZ0380_DEFAULT_QUALITY;
	dev->capture.bitrate_peak = MZ0380_DEFAULT_BITRATE;
	dev->capture.gop_size = MZ0380_DEFAULT_GOP;
	dev->capture.qp_step = MZ0380_DEFAULT_QP_STEP;
	dev->capture.b_frames = MZ0380_DEFAULT_B_FRAMES;
	dev->capture.h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	dev->capture.h264_level = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
	dev->capture.brightness = MZ0380_DEFAULT_COLOR;
	dev->capture.contrast = MZ0380_DEFAULT_COLOR;
	dev->capture.hue = MZ0380_DEFAULT_COLOR;
	dev->capture.saturation = MZ0380_DEFAULT_COLOR;
	dev->capture.sharpness = MZ0380_DEFAULT_COLOR;
}

static int mz0380_vb2_init(struct mz0380_dev *dev)
{
	struct vb2_queue *q = &dev->vb_queue;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF | VB2_USERPTR;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct mz0380_vb_buffer);
	q->ops = &mz0380_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &dev->queue_lock;
	q->gfp_flags = GFP_KERNEL;
	q->min_queued_buffers = 2;
	q->dev = &dev->pci->dev;

	return vb2_queue_init(q);
}

int mz0380_video_register(struct mz0380_dev *dev)
{
	int err;

	err = v4l2_device_register(&dev->pci->dev, &dev->v4l2_dev);
	if (err)
		return err;
	dev->v4l2_registered = true;

	err = mz0380_ctrls_init(dev);
	if (err)
		goto fail_v4l2;
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	err = mz0380_vb2_init(dev);
	if (err)
		goto fail_ctrls;

	memcpy(&dev->vdev, &mz0380_video_template, sizeof(dev->vdev));
	dev->vdev.fops = &mz0380_video_fops;
	dev->vdev.ioctl_ops = &mz0380_video_ioctl_ops;
	dev->vdev.lock = &dev->lock;
	dev->vdev.queue = &dev->vb_queue;
	dev->vdev.release = video_device_release_empty;
	dev->vdev.v4l2_dev = &dev->v4l2_dev;
	dev->vdev.ctrl_handler = &dev->ctrl_handler;
	dev->vdev.vfl_dir = VFL_DIR_RX;
	dev->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	dev->vdev.dev_parent = &dev->pci->dev;
	strscpy(dev->vdev.name, "mz0380 H.264", sizeof(dev->vdev.name));
	video_set_drvdata(&dev->vdev, dev);

	err = video_register_device(&dev->vdev,
				    VFL_TYPE_VIDEO, -1);
	if (err)
		goto fail_ctrls;

	dev->video_registered = true;

	printk(KERN_INFO
	       "%s: registered %s (H.264 capture, streaming=%s)\n",
	       dev->name, video_device_node_name(&dev->vdev),
	       dev->dma_armed ? "armed" : "disarmed");

	return 0;

fail_ctrls:
	if (dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
	}
fail_v4l2:
	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
	return err;
}

void mz0380_video_unregister(struct mz0380_dev *dev)
{
	if (dev->video_registered) {
		video_unregister_device(&dev->vdev);
		dev->video_registered = false;
	}

	if (dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
	}

	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
}

void mz0380_video_state_dump(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int fps_milli;
	u32 hw_input;
	u32 hw_word;
	u32 hw_mode;
	u32 hw_mode_word;
	u32 hw_mode_reg;
	u32 hw_mode_mask;
	u32 hw_mode_shift;
	u32 hw_quality;
	u32 hw_quality_word;
	u32 hw_quality_reg;
	u32 hw_quality_mask;
	u32 hw_quality_shift;
	u32 hw_gop;
	u32 hw_gop_word;
	u32 hw_gop_reg;
	u32 hw_gop_mask;
	u32 hw_gop_shift;
	u32 candidate_qp_step;
	u32 candidate_qp_step_word;
	u32 candidate_qp_step_reg;
	u32 candidate_qp_step_mask;
	u32 candidate_qp_step_shift;
	u32 hw_qp_step;
	u32 hw_qp_step_word;
	u32 hw_qp_step_reg;
	u32 hw_qp_step_mask;
	u32 hw_qp_step_shift;
	u32 candidate_b_frames;
	u32 candidate_b_frames_word;
	u32 candidate_b_frames_reg;
	u32 candidate_b_frames_mask;
	u32 candidate_b_frames_shift;
	u32 hw_b_frames;
	u32 hw_b_frames_word;
	u32 hw_b_frames_reg;
	u32 hw_b_frames_mask;
	u32 hw_b_frames_shift;
	u32 hw_bitrate;
	u32 hw_bitrate_word;
	u32 hw_bitrate_reg;
	u32 hw_bitrate_mask;
	u32 hw_bitrate_shift;
	u32 fourcc = mz0380_current_pixelformat();

	fps_milli = DIV_ROUND_CLOSEST(dev->capture.timeperframe.denominator * 1000,
				      max_t(u32, dev->capture.timeperframe.numerator, 1));

	if (!mz0380_enable_video) {
		seq_puts(m,
			 "  capture    : cached encoded-first control model (V4L2 node disabled; load with enable_video=1 to register /dev/video*)\n");
	} else {
		seq_puts(m, "  capture    : encoded-first V4L2 scaffolding\n");
	}

	if (dev->video_registered)
		seq_printf(m, "  video node : /dev/%s\n",
			   video_device_node_name(&dev->vdev));
	seq_printf(m, "  pixelformat: %4.4s\n", (char *)&fourcc);
	/*
	 * M169: the spawn budget, which is the number that decides whether the
	 * next hardware run is safe. Per insmod here - the driver cannot see
	 * across a module reload - so ./mz0380-spawns.sh accumulates it per
	 * boot, which is the granularity the card's wedge actually has.
	 */
	seq_printf(m, "  enc spawns : %u since insmod (card wedges somewhere in 8-18 per POWER CYCLE)\n",
		   dev->encoder_spawns);
	seq_printf(m, "  frame size : %ux%u\n",
		   dev->capture.width, dev->capture.height);
	seq_printf(m, "  frame rate : %u.%03u fps\n",
		   fps_milli / 1000, fps_milli % 1000);
	if (dev->capture.source_width && dev->capture.source_height)
		seq_printf(m, "  source     : %ux%u%c @ %u fps (SET_VIC input geometry)\n",
			   dev->capture.source_width,
			   dev->capture.source_height,
			   dev->capture.source_interlaced ? 'i' : 'p',
			   dev->capture.source_fps);
	else
		seq_puts(m, "  source     : unlocked\n");
	seq_printf(m, "  input      : %s\n",
		   mz0380_input_name(dev->capture.input));
	if (mz0380_read_hw_input_select(dev, &hw_input, &hw_word)) {
		if (hw_input < MZ0380_INPUT_COUNT)
			seq_printf(m,
				   "  input hw   : %s (%u) raw=%08x via BAR5[0x%04x][2:0]\n",
				   mz0380_input_name(hw_input), hw_input,
				   hw_word, MZ0380_INPUT_SELECT_HW_REG);
		else
			seq_printf(m,
				   "  input hw   : invalid (%u) raw=%08x via BAR5[0x%04x][2:0]\n",
				   hw_input, hw_word,
				   MZ0380_INPUT_SELECT_HW_REG);
	}
	seq_printf(m, "  recordmode : %s\n",
		   mz0380_record_mode_name(dev->capture.record_mode));
	seq_printf(m, "               property=%u value=%u\n",
		   MZ0380_RECORD_MODE_PROPERTY, dev->capture.record_mode);
	if (mz0380_read_hw_record_mode(dev, &hw_mode, &hw_mode_word,
				       &hw_mode_reg, &hw_mode_mask,
				       &hw_mode_shift)) {
		seq_printf(m,
			   "  record hw  : %s (%u) raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   mz0380_record_mode_name(hw_mode), hw_mode,
			   hw_mode_word, hw_mode_reg, hw_mode_mask,
			   hw_mode_shift);
	}
	seq_printf(m, "  quality    : %u\n", dev->capture.quality);
	if (mz0380_read_hw_quality(dev, &hw_quality, &hw_quality_word,
				   &hw_quality_reg, &hw_quality_mask,
				   &hw_quality_shift)) {
		seq_printf(m,
			   "  quality hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_quality, hw_quality_word,
			   hw_quality_reg, hw_quality_mask,
			   hw_quality_shift);
	}
	seq_printf(m, "  bitrate    : %u peak=%u\n",
		   dev->capture.bitrate, dev->capture.bitrate_peak);
	if (mz0380_read_hw_bitrate(dev, &hw_bitrate, &hw_bitrate_word,
				   &hw_bitrate_reg, &hw_bitrate_mask,
				   &hw_bitrate_shift)) {
		seq_printf(m,
			   "  bitrate hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_bitrate, hw_bitrate_word,
			   hw_bitrate_reg, hw_bitrate_mask,
			   hw_bitrate_shift);
	}
	seq_printf(m, "  gop        : %u\n", dev->capture.gop_size);
	if (mz0380_read_hw_gop(dev, &hw_gop, &hw_gop_word,
			       &hw_gop_reg, &hw_gop_mask,
			       &hw_gop_shift)) {
		seq_printf(m,
			   "  gop hw     : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_gop, hw_gop_word,
			   hw_gop_reg, hw_gop_mask,
			   hw_gop_shift);
	}
	seq_printf(m, "  qpstep     : %u\n", dev->capture.qp_step);
	if (mz0380_read_hw_qp_step(dev, &hw_qp_step, &hw_qp_step_word,
				   &hw_qp_step_reg, &hw_qp_step_mask,
				   &hw_qp_step_shift)) {
		seq_printf(m,
			   "  qpstep hw  : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_qp_step, hw_qp_step_word,
			   hw_qp_step_reg, hw_qp_step_mask,
			   hw_qp_step_shift);
	}
	if (mz0380_read_candidate_qp_step(dev, &candidate_qp_step,
					  &candidate_qp_step_word,
					  &candidate_qp_step_reg,
					  &candidate_qp_step_mask,
					  &candidate_qp_step_shift)) {
		seq_printf(m,
			   "  qpstep cand: %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   candidate_qp_step, candidate_qp_step_word,
			   candidate_qp_step_reg, candidate_qp_step_mask,
			   candidate_qp_step_shift);
	}
	seq_printf(m, "  b-frames   : %u\n", dev->capture.b_frames);
	if (mz0380_read_hw_b_frames(dev, &hw_b_frames, &hw_b_frames_word,
				    &hw_b_frames_reg, &hw_b_frames_mask,
				    &hw_b_frames_shift)) {
		seq_printf(m,
			   "  bframes hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_b_frames, hw_b_frames_word,
			   hw_b_frames_reg, hw_b_frames_mask,
			   hw_b_frames_shift);
	}
	if (mz0380_read_candidate_b_frames(dev, &candidate_b_frames,
					   &candidate_b_frames_word,
					   &candidate_b_frames_reg,
					   &candidate_b_frames_mask,
					   &candidate_b_frames_shift)) {
		seq_printf(m,
			   "  bframes cand: %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   candidate_b_frames, candidate_b_frames_word,
			   candidate_b_frames_reg, candidate_b_frames_mask,
			   candidate_b_frames_shift);
	}
	seq_printf(m, "  h264       : profile=%s level=%s\n",
		   mz0380_h264_profile_name(dev->capture.h264_profile),
		   mz0380_h264_level_name(dev->capture.h264_level));
	seq_printf(m, "  image ctrl : bright=%u contrast=%u hue=%u sat=%u sharp=%u\n",
		   dev->capture.brightness, dev->capture.contrast,
		   dev->capture.hue, dev->capture.saturation,
		   dev->capture.sharpness);
	seq_puts(m, "  stream     : disabled until control path and DMA are understood\n");
}
