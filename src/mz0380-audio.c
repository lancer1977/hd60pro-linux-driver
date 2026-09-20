// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Experimental ALSA HDMI audio scaffold.
 *
 *  The card can extract PCM from HDMI, but the host address, ownership and
 *  completion protocol for that PCM is not yet proven.  dev->audio_ring is
 *  therefore never allocated or programmed and enable_audio remains off by
 *  default.  SET_AIC in the video path is separate: it releases tinyvenc's
 *  audio_ready gate and is required even when no ALSA device is registered.
 */

#include "mz0380.h"

#if IS_ENABLED(CONFIG_SND)

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
};

#define MZ0380_AUDIO_RATE_MIN  32000
#define MZ0380_AUDIO_RATE_MAX  48000
#define MZ0380_AUDIO_CHANNELS  2

static const struct snd_pcm_hardware mz0380_pcm_hw = {
	.info             = SNDRV_PCM_INFO_INTERLEAVED |
			    SNDRV_PCM_INFO_BLOCK_TRANSFER |
			    SNDRV_PCM_INFO_MMAP |
			    SNDRV_PCM_INFO_MMAP_VALID,
	.formats          = SNDRV_PCM_FMTBIT_S16_LE,
	.rates            = SNDRV_PCM_RATE_32000 |
			    SNDRV_PCM_RATE_44100 |
			    SNDRV_PCM_RATE_48000,
	.rate_min         = MZ0380_AUDIO_RATE_MIN,
	.rate_max         = MZ0380_AUDIO_RATE_MAX,
	.channels_min     = MZ0380_AUDIO_CHANNELS,
	.channels_max     = MZ0380_AUDIO_CHANNELS,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 64 * 1024,
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
	spin_unlock_irq(&pcm->lock);
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

static int mz0380_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	pcm->hw_ptr_bytes = 0;
	return 0;
}

static int mz0380_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		/*
		 * -EIO, not -EAGAIN: arecord treats EAGAIN from a start as
		 * "wait 100 ms and retry" and spins forever (seen on mugen,
		 * C14); EIO makes it print "read error" and exit.
		 */
		if (!READ_ONCE(dev->pipeline_running) || !dev->audio_bufs_registered) {
			pr_info_once("%s: audio capture requires the video pipeline to be started (open /dev/video0 first)\n",
				     dev->name);
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
		spin_unlock_irqrestore(&pcm->lock, flags);
		WRITE_ONCE(dev->audio_running, true);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&pcm->lock, flags);
		pcm->running = false;
		spin_unlock_irqrestore(&pcm->lock, flags);
		WRITE_ONCE(dev->audio_running, false);
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
	size_t nbytes = MZ0380_AUDIO_SLOT_BYTES;
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
	 * Copy slot data into the ring buffer with wraparound. The existing
	 * wraparound logic from the old ring model ensures hw_ptr_bytes is
	 * always mod buffer_bytes after the copy.
	 */
	if (pcm->hw_ptr_bytes + nbytes <= pcm->buffer_bytes) {
		memcpy(pcm->substream->runtime->dma_area + pcm->hw_ptr_bytes,
		       src, nbytes);
		pcm->hw_ptr_bytes += nbytes;
	} else {
		size_t first = pcm->buffer_bytes - pcm->hw_ptr_bytes;
		size_t rest = nbytes - first;
		memcpy(pcm->substream->runtime->dma_area + pcm->hw_ptr_bytes,
		       src, first);
		memcpy(pcm->substream->runtime->dma_area, (u8 *)src + first, rest);
		pcm->hw_ptr_bytes = rest;
	}
	/*
	 * C15 on mugen: "invalid position: pos = 24000, buffer size = 24000"
	 * - a slot that ends exactly on the buffer end must report 0, not
	 * buffer_size, or ALSA rejects the pointer.
	 */
	if (pcm->hw_ptr_bytes >= pcm->buffer_bytes)
		pcm->hw_ptr_bytes -= pcm->buffer_bytes;

	dev->audio_slots_delivered++;
	ss = pcm->substream;
	spin_unlock_irqrestore(&pcm->lock, flags);

	/* Re-poison the slot buffer outside the lock (card won't reuse it for 3 more periods) */
	memset(src, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_SLOT_BYTES);

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
	if (!dev->audio_registered)
		return;

	if (dev->snd_card) {
		snd_card_free(dev->snd_card);
		dev->snd_card = NULL;
	}
	dev->snd_pcm = NULL;
	dev->audio_registered = false;
}
EXPORT_SYMBOL_GPL(mz0380_audio_unregister);

#endif /* CONFIG_SND */
