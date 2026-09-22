// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  ALSA HDMI audio capture (hd-pro60 #57).
 *
 *  The card DMAs 2-ch S16_LE PCM into the four MZ0380_AUDIO_SLOT_BYTES slots
 *  registered by SET_BUF op 0x03 and signals each completion through the
 *  BAR0+0x30 EVENT bits; mz0380_audio_deliver_slot() copies a completed slot
 *  into the substream ring.  enable_audio is still off by default.
 *
 *  Audio only flows while the card's video pipeline is streaming, and that
 *  pipeline is started by the V4L2 node - see the audio gate below.  SET_AIC
 *  in the video path is separate: it releases tinyvenc's audio_ready gate and
 *  is required even when no ALSA device is registered.
 */

#include "mz0380.h"

#if IS_ENABLED(CONFIG_SND)

#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

struct mz0380_pcm {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	struct mz0380_dev *dev;
	unsigned int period_bytes;
	unsigned int buffer_bytes;
	unsigned int hw_ptr_bytes;
	spinlock_t lock;
	bool running;
	/*
	 * Pre-start silence: drives the ring at the nominal slot rate while the
	 * card's video pipeline - and with it the audio DMA - is not up yet.
	 * Stops for good at the first real slot; see mz0380_pcm_silence_tick().
	 */
	struct hrtimer silence_timer;
	ktime_t silence_period;
	bool silence_running;
};

#define MZ0380_AUDIO_RATE      48000
#define MZ0380_AUDIO_CHANNELS  2

static const struct snd_pcm_hardware mz0380_pcm_hw = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_BLOCK_TRANSFER |
			    SNDRV_PCM_INFO_MMAP |
			    SNDRV_PCM_INFO_MMAP_VALID,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = MZ0380_AUDIO_RATE,
	.rate_max         = MZ0380_AUDIO_RATE,
	.channels_min     = MZ0380_AUDIO_CHANNELS,
	.channels_max     = MZ0380_AUDIO_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	/* One completion bit represents exactly one 4096-byte hardware slot. */
	.period_bytes_min = MZ0380_AUDIO_SLOT_BYTES,
	.period_bytes_max = MZ0380_AUDIO_SLOT_BYTES,
	.periods_min      = 2,
	.periods_max      = 16,
};

static int mz0380_pcm_open(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	ss->runtime->hw = mz0380_pcm_hw;
	snd_pcm_hw_constraint_step(ss->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   MZ0380_AUDIO_SLOT_BYTES);
	spin_lock_irq(&pcm->lock);
	pcm->substream = ss;
	pcm->hw_ptr_bytes = 0;
	spin_unlock_irq(&pcm->lock);
	return 0;
}

static int mz0380_pcm_close(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	spin_lock_irq(&pcm->lock);
	pcm->substream = NULL;
	pcm->running = false;
	pcm->silence_running = false;
	spin_unlock_irq(&pcm->lock);
	/* Process context here, so wait the tick out rather than try_to_cancel. */
	hrtimer_cancel(&pcm->silence_timer);
	return 0;
}

static int mz0380_pcm_hw_params(struct snd_pcm_substream *ss,
				struct snd_pcm_hw_params *p)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	pcm->period_bytes = params_period_bytes(p);
	pcm->buffer_bytes = params_buffer_bytes(p);
	return 0;
}

static int mz0380_pcm_hw_free(struct snd_pcm_substream *ss)
{
	return 0;
}

/*
 * The audio gate: the card only DMAs PCM while its video pipeline is up, and
 * SET_BUF op 0x03 must have been acked for that stream so the slots the EVENT
 * bits refer to actually point at our buffers.  Both halves are set by
 * mz0380_dma_start(), which is driven by the V4L2 node, never by the PCM.
 */
static bool mz0380_audio_gate_open(struct mz0380_dev *dev)
{
	return READ_ONCE(dev->pipeline_running) &&
	       READ_ONCE(dev->audio_bufs_registered);
}

static void mz0380_audio_gate_complain(struct mz0380_dev *dev)
{
	/*
	 * Name this card's own node.  The old message hard-coded /dev/video0,
	 * which on a host that also has a v4l2loopback (OBS's virtual camera
	 * claims video0) sends the user to the wrong device and keeps them
	 * broken - observed on the Bazzite host, hd-pro60 #57.
	 */
	pr_warn_ratelimited("%s: audio capture needs the video pipeline running - start capture on /dev/%s first (pipeline_running=%d audio_bufs_registered=%d)\n",
			    dev->name, video_device_node_name(&dev->vdev),
			    READ_ONCE(dev->pipeline_running),
			    READ_ONCE(dev->audio_bufs_registered));
}

/*
 * Push one slot into the ALSA ring, wrapping at the end of the buffer.
 * src NULL writes silence.  Caller holds pcm->lock and has checked substream.
 */
static void mz0380_pcm_push_slot(struct mz0380_pcm *pcm, const void *src)
{
	u8 *dst = pcm->substream->runtime->dma_area;
	size_t nbytes = MZ0380_AUDIO_SLOT_BYTES;

	if (pcm->hw_ptr_bytes + nbytes <= pcm->buffer_bytes) {
		if (src)
			memcpy(dst + pcm->hw_ptr_bytes, src, nbytes);
		else
			memset(dst + pcm->hw_ptr_bytes, 0, nbytes);
		pcm->hw_ptr_bytes += nbytes;
	} else {
		size_t first = pcm->buffer_bytes - pcm->hw_ptr_bytes;
		size_t rest = nbytes - first;

		if (src) {
			memcpy(dst + pcm->hw_ptr_bytes, src, first);
			memcpy(dst, (const u8 *)src + first, rest);
		} else {
			memset(dst + pcm->hw_ptr_bytes, 0, first);
			memset(dst, 0, rest);
		}
		pcm->hw_ptr_bytes = rest;
	}
	/*
	 * C15 on mugen: "invalid position: pos = 24000, buffer size = 24000"
	 * - a slot that ends exactly on the buffer end must report 0, not
	 * buffer_size, or ALSA rejects the pointer.
	 */
	if (pcm->hw_ptr_bytes >= pcm->buffer_bytes)
		pcm->hw_ptr_bytes -= pcm->buffer_bytes;
}

/*
 * Pre-start silence.
 *
 * The card only DMAs audio while its video pipeline is up, and that pipeline is
 * started by the V4L2 node, never by the PCM.  Failing the stream in that window
 * is what made OBS record a silent track for a whole session: it starts its
 * v4l2_input and pulse_input_capture sources within the same tick, so its audio
 * source dies before its own video source has brought the pipeline up.  It also
 * kept the card out of PipeWire's source list entirely, because WirePlumber's
 * probe opens the PCM before anything has opened video.
 *
 * So do what a capture device with no signal yet should do: run, and hand back
 * silence.  This timer drives the ring at the nominal slot rate until the first
 * real slot lands, at which point the EVENT path takes over and it stops for
 * good.  The consumer sees a short silent head and then live audio, instead of
 * an error it cannot recover from.
 */
static enum hrtimer_restart mz0380_pcm_silence_tick(struct hrtimer *t)
{
	struct mz0380_pcm *pcm = container_of(t, struct mz0380_pcm, silence_timer);
	struct snd_pcm_substream *ss;
	unsigned long flags;

	spin_lock_irqsave(&pcm->lock, flags);
	if (!pcm->running || !pcm->substream || !pcm->silence_running) {
		pcm->silence_running = false;
		spin_unlock_irqrestore(&pcm->lock, flags);
		return HRTIMER_NORESTART;
	}
	if (mz0380_audio_gate_open(pcm->dev)) {
		/* Real slots are on their way; never interleave with them. */
		pcm->silence_running = false;
		spin_unlock_irqrestore(&pcm->lock, flags);
		pr_info("%s: audio capture: video pipeline up, live samples take over from pre-start silence\n",
			pcm->dev->name);
		return HRTIMER_NORESTART;
	}
	mz0380_pcm_push_slot(pcm, NULL);
	pcm->dev->audio_silence_slots++;
	ss = pcm->substream;
	spin_unlock_irqrestore(&pcm->lock, flags);

	hrtimer_forward_now(t, pcm->silence_period);
	snd_pcm_period_elapsed(ss);
	return HRTIMER_RESTART;
}

static int mz0380_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned int frames = MZ0380_AUDIO_SLOT_BYTES /
			      (MZ0380_AUDIO_CHANNELS * 2);
	unsigned int rate = ss->runtime->rate ?: MZ0380_AUDIO_RATE;

	pcm->hw_ptr_bytes = 0;
	/*
	 * One slot's worth of wall time at the negotiated rate - 1024 frames,
	 * so 21.33 ms at 48 kHz. Never block here: prepare() is called by
	 * WirePlumber's probe long before any video client exists, and a
	 * sleeping prepare() starves its event loop.
	 */
	pcm->silence_period = ns_to_ktime(div_u64((u64)frames * NSEC_PER_SEC,
						  rate));
	return 0;
}

static int mz0380_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME: {
		bool gate = mz0380_audio_gate_open(dev);

		/*
		 * -EIO, not -EAGAIN, on the opt-out path: arecord treats EAGAIN
		 * from a start as "wait 100 ms and retry" and spins forever
		 * (seen on mugen, C14); EIO makes it print "read error" and exit.
		 */
		if (!gate && !mz0380_audio_prestart_silence) {
			mz0380_audio_gate_complain(dev);
			return -EIO;
		}
		/*
		 * Trigger runs under ALSA's stream lock with IRQs already off:
		 * irqsave, never spin_unlock_irq(), or we would re-enable them
		 * inside the stream lock.
		 */
		spin_lock_irqsave(&pcm->lock, flags);
		pcm->hw_ptr_bytes = 0;
		pcm->running = true;
		pcm->silence_running = !gate;
		spin_unlock_irqrestore(&pcm->lock, flags);
		WRITE_ONCE(dev->audio_running, true);
		if (!gate) {
			mz0380_audio_gate_complain(dev);
			hrtimer_start(&pcm->silence_timer, pcm->silence_period,
				      HRTIMER_MODE_REL_SOFT);
		}
		return 0;
	}

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&pcm->lock, flags);
		pcm->running = false;
		pcm->silence_running = false;
		spin_unlock_irqrestore(&pcm->lock, flags);
		WRITE_ONCE(dev->audio_running, false);
		/*
		 * Atomic context (stream lock, IRQs off), so try_to_cancel, not
		 * cancel. It returns -1 when called from inside the callback;
		 * silence_running is already false there, so that tick is the
		 * last one either way.
		 */
		hrtimer_try_to_cancel(&pcm->silence_timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t mz0380_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	return bytes_to_frames(ss->runtime, pcm->hw_ptr_bytes);
}

static const struct snd_pcm_ops mz0380_pcm_ops = {
	.open      = mz0380_pcm_open,
	.close     = mz0380_pcm_close,
	.hw_params = mz0380_pcm_hw_params,
	.hw_free   = mz0380_pcm_hw_free,
	.prepare   = mz0380_pcm_prepare,
	.trigger   = mz0380_pcm_trigger,
	.pointer   = mz0380_pcm_pointer,
};

void mz0380_audio_deliver_slot(struct mz0380_dev *dev, unsigned int slot)
{
	struct mz0380_pcm *pcm = dev->snd_pcm;
	struct snd_pcm_substream *ss = NULL;
	void *src;
	unsigned long flags;

	if (!pcm || slot >= MZ0380_AUDIO_NR_BUFS)
		return;

	src = dev->audio_bufs[slot].vaddr;
	if (!src)
		return;

	spin_lock_irqsave(&pcm->lock, flags);
	if (!pcm->running || !pcm->substream) {
		dev->audio_slots_dropped++;
		spin_unlock_irqrestore(&pcm->lock, flags);
		return;
	}

	/*
	 * A real slot ends the pre-start silence for good. The tick checks this
	 * flag under the same lock, so the two never interleave.
	 */
	pcm->silence_running = false;
	mz0380_pcm_push_slot(pcm, src);

	dev->audio_slots_delivered++;
	ss = pcm->substream;
	spin_unlock_irqrestore(&pcm->lock, flags);

	/*
	 * Kernel 6.14: ALSA's stream lock protects close() from running concurrently
	 * with period_elapsed(), and our spinlock protects substream pointer mutation.
	 * Trigger cannot run while a close() is in progress (it's gated by stream state).
	 * So after unlocking our own lock, we re-check ss under the lock to ensure it
	 * wasn't cleared by a concurrent close().
	 */
	if (ss) {
		spin_lock_irqsave(&pcm->lock, flags);
		if (pcm->substream)
			ss = pcm->substream;
		else
			ss = NULL;
		spin_unlock_irqrestore(&pcm->lock, flags);
		if (ss)
			snd_pcm_period_elapsed(ss);
	}
}
EXPORT_SYMBOL_GPL(mz0380_audio_deliver_slot);

int mz0380_audio_register(struct mz0380_dev *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm_dev;
	struct mz0380_pcm *pcm;
	int ret;

	if (dev->audio_registered)
		return 0;

	ret = snd_card_new(&dev->pci->dev, SNDRV_DEFAULT_IDX1, "mz0380",
			   THIS_MODULE, sizeof(*pcm), &card);
	if (ret)
		return ret;

	pcm = card->private_data;
	pcm->card = card;
	pcm->dev = dev;
	spin_lock_init(&pcm->lock);
	hrtimer_setup(&pcm->silence_timer, mz0380_pcm_silence_tick,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	dev->snd_card = card;
	dev->snd_pcm  = pcm;

	strscpy(card->driver, "mz0380", sizeof(card->driver));
	strscpy(card->shortname, "mz0380 HDMI", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s on %s", mz0380_boards[dev->board].name,
		 pci_name(dev->pci));

	ret = snd_pcm_new(card, "mz0380 HDMI", 0, 0, 1, &pcm_dev);
	if (ret)
		goto fail_card;
	pcm->pcm = pcm_dev;
	pcm_dev->private_data = dev;
	pcm_dev->info_flags = 0;
	strscpy(pcm_dev->name, "mz0380 HDMI", sizeof(pcm_dev->name));
	snd_pcm_set_ops(pcm_dev, SNDRV_PCM_STREAM_CAPTURE, &mz0380_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm_dev,
				       SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, 256 * 1024);

	ret = snd_card_register(card);
	if (ret)
		goto fail_card;

	dev->audio_registered = true;
	pr_info("%s: ALSA card '%s' registered\n",
		dev->name, card->shortname);
	return 0;

fail_card:
	snd_card_free(card);
	dev->snd_card = NULL;
	dev->snd_pcm  = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_audio_register);

void mz0380_audio_unregister(struct mz0380_dev *dev)
{
	struct mz0380_pcm *pcm = dev->snd_pcm;

	if (!dev->audio_registered)
		return;

	if (pcm)
		hrtimer_cancel(&pcm->silence_timer);
	if (dev->snd_card) {
		snd_card_free(dev->snd_card);
		dev->snd_card = NULL;
	}
	dev->snd_pcm = NULL;
	dev->audio_registered = false;
}
EXPORT_SYMBOL_GPL(mz0380_audio_unregister);

#endif /* CONFIG_SND */
