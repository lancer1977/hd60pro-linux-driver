/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Copyright (c) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef _MZ0380_H_
#define _MZ0380_H_

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/version.h>

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include "mz0380-reg.h"

#define MZ0380_VERSION_CODE KERNEL_VERSION(0, 1, 0)

#define MZ0380_MAXBOARDS 8
#define MZ0380_MAX_MAPS 2
#define UNSET (-1U)

#define MZ0380_BOARD_NOAUTO             UNSET
#define MZ0380_BOARD_UNKNOWN            0
#define MZ0380_BOARD_ELGATO_HD60_PRO    1
#define MZ0380_BOARD_ELGATO_HD60_PRO_REV3 2

#define MZ0380_INPUT_COUNT              5
#define MZ0380_INPUT_SELECT_PROPERTY    201
#define MZ0380_INPUT_SELECT_HW_REG      0x0040
#define MZ0380_INPUT_SELECT_HW_MASK     0x7
#define MZ0380_INPUT_SELECT_HW_SHIFT    0
#define MZ0380_BITRATE_PROPERTY         403
#define MZ0380_BITRATE_HW_REG           0x005c
#define MZ0380_BITRATE_HW_MASK          0xffffffff
#define MZ0380_BITRATE_HW_SHIFT         0
#define MZ0380_QUALITY_PROPERTY         404
#define MZ0380_QUALITY_HW_REG           0x0060
#define MZ0380_QUALITY_HW_MASK          0xffffffff
#define MZ0380_QUALITY_HW_SHIFT         0
#define MZ0380_GOP_PROPERTY             405
#define MZ0380_GOP_HW_REG               0x0080
#define MZ0380_GOP_HW_MASK              0xffffffff
#define MZ0380_GOP_HW_SHIFT             0
#define MZ0380_B_FRAMES_PROPERTY        411
#define MZ0380_B_FRAMES_HW_REG          0x0088
#define MZ0380_B_FRAMES_HW_MASK         0xffffffff
#define MZ0380_B_FRAMES_HW_SHIFT        0
#define MZ0380_DEFAULT_B_FRAMES         0
#define MZ0380_MAX_B_FRAMES             2
#define MZ0380_QP_STEP_PROPERTY         408
#define MZ0380_QP_STEP_HW_REG           0x0084
#define MZ0380_QP_STEP_HW_MASK          0xffffffff
#define MZ0380_QP_STEP_HW_SHIFT         0
#define MZ0380_DEFAULT_BITRATE          (12 * 1024 * 1024)
#define MZ0380_MIN_BITRATE              (256 * 1024)
#define MZ0380_MAX_BITRATE              (12 * 1024 * 1024)
#define MZ0380_MIN_QUALITY              0
#define MZ0380_DEFAULT_QUALITY          80
#define MZ0380_MAX_QUALITY              100
#define MZ0380_DEFAULT_QP_STEP          0
#define MZ0380_RECORD_MODE_COUNT        3
#define MZ0380_RECORD_MODE_PROPERTY     407
#define MZ0380_RECORD_MODE_HW_REG       0x0058
#define MZ0380_RECORD_MODE_HW_MASK      0x3
#define MZ0380_RECORD_MODE_HW_SHIFT     0
#define MZ0380_CFG_TRACE_START          0x0000
/*
 * M0 observability widen: extend the before/after CFG trace past the old
 * 0x00bc ceiling to cover the hypothesised command/status/param mailbox
 * (0x00c0..0x00e4), so /proc/mz0380-experiment diffs can see whether a
 * mailbox write actually lands. BAR5 is always mapped, so this stays a
 * read-only, cannot-fault extension.
 */
#define MZ0380_CFG_TRACE_END            0x00e4
#define MZ0380_CFG_TRACE_COUNT \
	(((MZ0380_CFG_TRACE_END - MZ0380_CFG_TRACE_START) / sizeof(u32)) + 1)

struct snd_card; /* mz0380-audio.c */

enum mz0380_property_experiment_result {
	MZ0380_PROPERTY_EXPERIMENT_NONE = 0,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG,
	MZ0380_PROPERTY_EXPERIMENT_CACHED_ONLY,
	MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED,
	MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH,
};

struct mz0380_board {
	const char *name;
	const char *windows_driver;
	const char *firmware_name;       /* main firmware: "MZ0380.HD.HEX"   */
	const char *firmware_base_name;  /* base/loader firmware (optional)  */
};

/*
 * Firmware upload state machine.
 *
 * The card boots its onboard ARM Linux from the uploaded blob. Bus
 * mastering must stay disabled until READY, otherwise an AMD-Vi
 * IO_PAGE_FAULT was observed at 0x90000000.
 */
enum mz0380_fw_state {
	MZ0380_FW_STATE_NONE = 0,
	MZ0380_FW_STATE_REQUESTED,
	MZ0380_FW_STATE_UPLOADING,
	MZ0380_FW_STATE_READY,
	MZ0380_FW_STATE_FAILED,
};

/*
 * Per-ring DMA bookkeeping. Video and audio each get one of these.
 *
 * The card writes encoded H.264 NALs (video) or PCM samples (audio)
 * into the page-aligned `buf` allocation, structured as `nr_entries`
 * fixed-size slots of `entry_size` bytes each. Card advances its tail;
 * host advances head as it consumes.
 */
struct mz0380_ring {
	void *buf;              /* kernel virt addr                       */
	dma_addr_t dma;         /* DMA / bus addr                         */
	size_t total_size;
	u32 entry_size;
	u32 nr_entries;
	u32 head;               /* host consumer index                    */
	u32 tail_seen;          /* last tail value drained from card      */
	u32 base_reg_lo;        /* BAR5 offset for ring-base-low          */
	u32 base_reg_hi;
	u32 size_reg;
	u32 entries_reg;
	u32 head_reg;
	u32 tail_reg;
};

/*
 * vb2 buffer wrapper. The encoded payload from one or more ring slots
 * is copied / scatter-gathered into the vb2_buffer-backed memory and
 * handed to userspace via DQBUF.
 */
struct mz0380_vb_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mz0380_subid {
	u16 subvendor;
	u16 subdevice;
	u32 card;
};

struct mz0380_capture_state {
	u32 pixelformat;
	u32 width;
	u32 height;
	struct v4l2_fract timeperframe;
	u32 input;
	u32 record_mode;
	u32 bitrate;
	u32 quality;
	u32 bitrate_peak;
	u32 gop_size;
	u32 qp_step;
	u32 b_frames;
	u32 h264_profile;
	u32 h264_level;
	u32 brightness;
	u32 contrast;
	u32 hue;
	u32 saturation;
	u32 sharpness;
};

struct mz0380_property_experiment {
	bool valid;
	bool hardware_write;
	u32 property_id;
	u32 previous_value;
	u32 requested_value;
	u32 reg;
	u32 mask;
	u32 shift;
	u32 before_word;
	u32 programmed_word;
	u32 readback_word;
	enum mz0380_property_experiment_result result;
	char source[16];
	u32 before_cfg_trace[MZ0380_CFG_TRACE_COUNT];
	u32 after_cfg_trace[MZ0380_CFG_TRACE_COUNT];
};

/*
 * Live card-event watcher (diagnostic). A kthread samples the BAR0 EVENT word
 * at high rate and records each edge - with the payload words the card writes
 * at BAR0+0x40..0x4c - into a ring exposed at /proc/mz0380-events. This is how
 * signal-change / no-signal notifications are caught, since the card pushes
 * them as edge events rather than exposing a pollable status register.
 */
struct mz0380_event_rec {
	u64 t_ns;		/* local_clock() timestamp                */
	u32 event;		/* BAR0 EVENT word (0x30)                 */
	u32 payload[4];		/* BAR0 0x40..0x4c payload/DPC-arg words  */
	u32 status;		/* BAR0 STATUS (0x2c)                     */
	u32 intflag;		/* BAR5 CFG interrupt flag                */
};

#define MZ0380_EVENT_RING_SIZE 256

struct mz0380_dev {
	struct list_head devlist;
	struct pci_dev *pci;
	struct mutex lock;
	struct mutex ctrl_lock;
	char name[32];
	unsigned int nr;
	unsigned int board;
	void __iomem *lmmio[MZ0380_MAX_MAPS];
	u8 __iomem *bmmio[MZ0380_MAX_MAPS];
	int bar_nr[MZ0380_MAX_MAPS];
	resource_size_t bar_start[MZ0380_MAX_MAPS];
	resource_size_t bar_len[MZ0380_MAX_MAPS];
	u8 pci_rev;
	u8 pci_lat;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device vdev;
	struct v4l2_ctrl *bitrate_ctrl;
	struct v4l2_ctrl *quality_ctrl;
	struct v4l2_ctrl *gop_ctrl;
	struct v4l2_ctrl *b_frames_ctrl;
	struct v4l2_ctrl *record_mode_ctrl;
	struct mz0380_capture_state capture;

	/* Firmware */
	enum mz0380_fw_state fw_state;
	struct mutex fw_lock;
	u16 fw_version_major;
	u16 fw_version_minor;
	wait_queue_head_t fw_wait;

	/* IRQ */
	int irq;
	bool irq_requested;
	bool msi_enabled;
	atomic_t irq_count;
	atomic_t irq_video_count;
	atomic_t irq_audio_count;
	atomic_t irq_signal_count;

	/* Command channel serialisation */
	struct mutex cmd_lock;
	wait_queue_head_t cmd_wait;
	u32 cmd_last_status;
	u32 cmd_last_param[MZ0380_REG_PARAM_MAX];
	bool cmd_complete;

	/* DMA rings */
	struct mz0380_ring video_ring;
	struct mz0380_ring audio_ring;
	bool dma_armed;

	// pattern-check: skip preloaded DMA buffer array, plain state, no abstraction
	/*
	 * Streaming buffer set (RE_FINDINGS.md M17). The card DMAs encoded
	 * frames into these host buffers via its iATU outbound window; we hand
	 * their physaddrs over with the buffer-setter mailbox opcodes. The
	 * completion EVENT carries the finished buffer index in (token & 7).
	 */
	struct mz0380_stream_buf {
		void *va;
		dma_addr_t dma;
	} stream_bufs[MZ0380_STREAM_NR_BUFS];
	u32 stream_head;	/* next buffer index we expect from the card */

	/* vb2 video streaming */
	struct vb2_queue vb_queue;
	struct mutex queue_lock;
	struct list_head buf_list;
	spinlock_t buf_lock;
	struct work_struct drain_work;
	bool streaming;

	/* HDMI signal */
	// pattern-check: skip adding one bool state flag to existing struct
	struct v4l2_dv_timings detected_timings;
	bool signal_locked;
	bool mst3367_ready;	/* receiver reset released + init applied */

	/* ALSA */
	struct snd_card *snd_card;
	void *snd_pcm; /* opaque to avoid pulling snd headers in this header */
	bool audio_registered;
	struct mz0380_property_experiment input_experiment;
	struct mz0380_property_experiment bitrate_experiment;
	struct mz0380_property_experiment quality_experiment;
	struct mz0380_property_experiment gop_experiment;
	struct mz0380_property_experiment b_frames_experiment;
	struct mz0380_property_experiment qp_step_experiment;
	struct mz0380_property_experiment record_mode_experiment;
	bool v4l2_registered;
	bool ctrl_handler_initialized;
	bool video_registered;

	/* Live event watcher */
	struct mz0380_event_rec event_ring[MZ0380_EVENT_RING_SIZE];
	unsigned int event_head;
	unsigned int event_count;
	u64 event_seen;
	spinlock_t event_lock;
	struct task_struct *event_kthread;
	bool event_watching;
};

extern struct mz0380_board mz0380_boards[];
extern const unsigned int mz0380_bcount;
extern struct mz0380_subid mz0380_subids[];
extern const unsigned int mz0380_idcount;
extern bool mz0380_enable_video;

void mz0380_card_list(struct mz0380_dev *dev);
int mz0380_card_setup(struct mz0380_dev *dev);
void mz0380_card_cleanup(struct mz0380_dev *dev);

int mz0380_video_register(struct mz0380_dev *dev);
void mz0380_video_unregister(struct mz0380_dev *dev);
void mz0380_video_state_dump(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_capture_state_init(struct mz0380_dev *dev);
const char *mz0380_input_name(u32 input);
const char *mz0380_record_mode_name(u32 mode);
bool mz0380_read_hw_input_select(struct mz0380_dev *dev, u32 *input,
				 u32 *raw_word);
bool mz0380_sync_hw_input_select(struct mz0380_dev *dev, const char *reason);
int mz0380_request_input_select(struct mz0380_dev *dev, u32 input,
			       const char *source);
bool mz0380_read_hw_bitrate(struct mz0380_dev *dev, u32 *bitrate,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_sync_hw_bitrate(struct mz0380_dev *dev, const char *reason);
bool mz0380_read_candidate_bitrate(struct mz0380_dev *dev, u32 *bitrate,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_candidate_bitrate(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_bitrate_locked(struct mz0380_dev *dev, u32 bitrate,
				  const char *source);
int mz0380_request_bitrate(struct mz0380_dev *dev, u32 bitrate,
			   const char *source);
bool mz0380_read_hw_quality(struct mz0380_dev *dev, u32 *quality,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_sync_hw_quality(struct mz0380_dev *dev, const char *reason);
bool mz0380_read_candidate_quality(struct mz0380_dev *dev, u32 *quality,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_candidate_quality(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_quality_locked(struct mz0380_dev *dev, u32 quality,
				  const char *source);
int mz0380_request_quality(struct mz0380_dev *dev, u32 quality,
			   const char *source);
bool mz0380_read_candidate_gop(struct mz0380_dev *dev, u32 *gop,
			       u32 *raw_word, u32 *reg,
			       u32 *mask, u32 *shift);
bool mz0380_read_hw_gop(struct mz0380_dev *dev, u32 *gop,
			u32 *raw_word, u32 *reg,
			u32 *mask, u32 *shift);
bool mz0380_sync_hw_gop(struct mz0380_dev *dev, const char *reason);
bool mz0380_sync_candidate_gop(struct mz0380_dev *dev,
			       const char *reason);
int mz0380_request_gop_locked(struct mz0380_dev *dev, u32 gop,
			      const char *source);
int mz0380_request_gop(struct mz0380_dev *dev, u32 gop,
		       const char *source);
bool mz0380_read_hw_b_frames(struct mz0380_dev *dev, u32 *b_frames,
			     u32 *raw_word, u32 *reg,
			     u32 *mask, u32 *shift);
bool mz0380_read_candidate_b_frames(struct mz0380_dev *dev, u32 *b_frames,
				    u32 *raw_word, u32 *reg,
				    u32 *mask, u32 *shift);
bool mz0380_sync_hw_b_frames(struct mz0380_dev *dev,
			     const char *reason);
bool mz0380_sync_candidate_b_frames(struct mz0380_dev *dev,
				    const char *reason);
int mz0380_request_b_frames_locked(struct mz0380_dev *dev, u32 b_frames,
				   const char *source);
int mz0380_request_b_frames(struct mz0380_dev *dev, u32 b_frames,
			    const char *source);
bool mz0380_read_hw_qp_step(struct mz0380_dev *dev, u32 *qp_step,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_read_candidate_qp_step(struct mz0380_dev *dev, u32 *qp_step,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_hw_qp_step(struct mz0380_dev *dev, const char *reason);
bool mz0380_sync_candidate_qp_step(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_qp_step_locked(struct mz0380_dev *dev, u32 qp_step,
				  const char *source);
int mz0380_request_qp_step(struct mz0380_dev *dev, u32 qp_step,
			   const char *source);
bool mz0380_read_hw_record_mode(struct mz0380_dev *dev, u32 *mode,
				u32 *raw_word, u32 *reg,
				u32 *mask, u32 *shift);
bool mz0380_sync_hw_record_mode(struct mz0380_dev *dev,
				const char *reason);
bool mz0380_read_candidate_record_mode(struct mz0380_dev *dev, u32 *mode,
				       u32 *raw_word, u32 *reg,
				       u32 *mask, u32 *shift);
bool mz0380_sync_candidate_record_mode(struct mz0380_dev *dev,
				       const char *reason);
int mz0380_request_record_mode_locked(struct mz0380_dev *dev, u32 mode,
				      const char *source);
int mz0380_request_record_mode(struct mz0380_dev *dev, u32 mode,
			       const char *source);

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg);
void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value);

/* Convenience BAR5 accessors */
static inline u32 mz_cfg_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
}
static inline void mz_cfg_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_CFG, reg, v);
}

/* Convenience BAR0 accessors (post-firmware) */
static inline u32 mz_mmio_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_MMIO, reg);
}
static inline void mz_mmio_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_MMIO, reg, v);
}
static inline void mz_mmio_set(struct mz0380_dev *dev, u32 reg, u32 bits)
{
	mz_mmio_write(dev, reg, mz_mmio_read(dev, reg) | bits);
}
static inline void mz_mmio_clr(struct mz0380_dev *dev, u32 reg, u32 bits)
{
	mz_mmio_write(dev, reg, mz_mmio_read(dev, reg) & ~bits);
}

/* Command/mailbox helpers (mz0380-core.c) */
int mz0380_send_command(struct mz0380_dev *dev, u32 opcode,
			const u32 *params, unsigned int nparams,
			u32 *status_out, unsigned int timeout_ms);
void mz0380_mb_ack_event(struct mz0380_dev *dev);
int mz0380_periph_read(struct mz0380_dev *dev, u8 chip, u8 reg, u32 *val);
int mz0380_periph_write(struct mz0380_dev *dev, u8 chip, u8 reg, u32 val);

/* Firmware (mz0380-fw.c) */
int mz0380_firmware_load(struct mz0380_dev *dev);
void mz0380_firmware_release(struct mz0380_dev *dev);
int mz0380_card_init(struct mz0380_dev *dev);
void mz0380_mailbox_scan(struct mz0380_dev *dev);
const char *mz0380_fw_state_name(enum mz0380_fw_state s);
void mz0380_fw_info_dump(struct seq_file *m, struct mz0380_dev *dev);

/* DMA + IRQ (mz0380-dma.c) */
int mz0380_irq_request(struct mz0380_dev *dev);
void mz0380_irq_release(struct mz0380_dev *dev);
int mz0380_dma_setup(struct mz0380_dev *dev);
void mz0380_dma_teardown(struct mz0380_dev *dev);
int mz0380_dma_start(struct mz0380_dev *dev);
void mz0380_dma_stop(struct mz0380_dev *dev);
int mz0380_dma_ring_alloc(struct mz0380_dev *dev, struct mz0380_ring *r,
			  u32 nr_entries, u32 entry_size);
void mz0380_dma_ring_free(struct mz0380_dev *dev, struct mz0380_ring *r);
void mz0380_dma_drain_video(struct mz0380_dev *dev);
void mz0380_dma_drain_audio(struct mz0380_dev *dev);

/* HDMI signal detect (mz0380-video.c) */
int mz0380_query_signal(struct mz0380_dev *dev,
			struct v4l2_dv_timings *timings);
void mz0380_signal_event(struct mz0380_dev *dev);

// pattern-check: skip two function prototypes, procedural module, no abstraction
/* MST3367 receiver bring-up + signal detect (mz0380-mst3367.c) */
int mz0380_mst3367_bringup(struct mz0380_dev *dev);
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out);

/* ALSA audio (mz0380-audio.c) */
#if IS_ENABLED(CONFIG_SND)
int mz0380_audio_register(struct mz0380_dev *dev);
void mz0380_audio_unregister(struct mz0380_dev *dev);
void mz0380_audio_period_elapsed(struct mz0380_dev *dev);
#else
static inline int mz0380_audio_register(struct mz0380_dev *dev) { return 0; }
static inline void mz0380_audio_unregister(struct mz0380_dev *dev) {}
static inline void mz0380_audio_period_elapsed(struct mz0380_dev *dev) {}
#endif

/* Module-param gates */
extern bool mz0380_firmware_upload_enabled;
extern bool mz0380_enable_dma;
extern bool mz0380_dma_handshake;
extern bool mz0380_enable_audio;
extern unsigned int mz0380_video_ring_entries;
extern unsigned int mz0380_video_ring_entry_size;
extern unsigned int mz0380_audio_ring_entries;
extern unsigned int mz0380_audio_ring_entry_size;

#endif
