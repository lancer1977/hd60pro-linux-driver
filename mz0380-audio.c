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
	pcm->substream = ss;
	pcm->hw_ptr_bytes = 0;
	return 0;
}

static int mz0380_pcm_close(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	pcm->substream = NULL;
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
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
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

void mz0380_audio_period_elapsed(struct mz0380_dev *dev)
{
	struct mz0380_pcm *pcm = dev->snd_pcm;
	struct snd_pcm_substream *ss;
	struct mz0380_ring *r = &dev->audio_ring;
	u32 tail;
	size_t produced;
	void *dst;

	if (!pcm || !pcm->substream)
		return;
	ss = pcm->substream;

	tail = mz_cfg_read(dev, r->tail_reg);

	while (r->head != tail) {
		void *src = (u8 *)r->buf + (size_t)r->head * r->entry_size;
		u32 nbytes = *(u32 *)(src + MZ0380_DESC_BYTECOUNT_OFFSET);

		if (nbytes == 0 || nbytes > r->entry_size)
			goto advance;

		produced = nbytes;

		if (pcm->hw_ptr_bytes + produced <= pcm->buffer_bytes) {
			dst = ss->runtime->dma_area + pcm->hw_ptr_bytes;
			memcpy(dst, (u8 *)src + MZ0380_DESC_PAYLOAD_OFFSET,
			       produced);
			pcm->hw_ptr_bytes += produced;
		} else {
			size_t first = pcm->buffer_bytes - pcm->hw_ptr_bytes;
			size_t rest  = produced - first;
			dst = ss->runtime->dma_area + pcm->hw_ptr_bytes;
			memcpy(dst,
			       (u8 *)src + MZ0380_DESC_PAYLOAD_OFFSET,
			       first);
			memcpy(ss->runtime->dma_area,
			       (u8 *)src + MZ0380_DESC_PAYLOAD_OFFSET + first,
			       rest);
			pcm->hw_ptr_bytes = rest;
		}

advance:
		r->head = (r->head + 1) % r->nr_entries;
		mz_cfg_write(dev, r->head_reg, r->head);
	}

	snd_pcm_period_elapsed(ss);
}
EXPORT_SYMBOL_GPL(mz0380_audio_period_elapsed);

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
