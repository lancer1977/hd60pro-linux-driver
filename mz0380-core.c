/*
 *  Driver for MZ0380 based capture cards.
 */

#include <linux/uaccess.h>

#include "mz0380.h"

MODULE_DESCRIPTION("Driver for MZ0380 based capture cards");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");

unsigned int procfs_verbosity = 1;
module_param(procfs_verbosity, int, 0644);
MODULE_PARM_DESC(procfs_verbosity,
		 "procfs debug level; wide BAR scans are disabled as unsafe");

static unsigned int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "enable debug messages");

static bool allow_bus_master;
module_param(allow_bus_master, bool, 0644);
MODULE_PARM_DESC(allow_bus_master,
		 "allow PCI bus mastering during probe; unsafe until DMA is understood");

static unsigned int snapshot_profile = 1;
module_param(snapshot_profile, int, 0644);
MODULE_PARM_DESC(snapshot_profile,
		 "targeted register snapshot profile: 1=known-safe baseline, 2=baseline plus experimental BAR5 low-offset window, 3=profiles 1-2 plus experimental BAR0 signal window, 4=profiles 1-3 plus extended BAR5 window, 5=profiles 1-4 plus second extended BAR5 window, 6=profiles 1-5 plus M0 mailbox/ring/firmware BAR5 windows (0x00c0..0x00e4, 0x0200..0x0234, 0x0300..0x0410)");

static bool allow_experimental_writes;
module_param(allow_experimental_writes, bool, 0644);
MODULE_PARM_DESC(allow_experimental_writes,
		 "allow tightly scoped BAR5 control experiments; disabled by default");

static unsigned int input_select_reg = ~0U;
module_param(input_select_reg, uint, 0644);
MODULE_PARM_DESC(input_select_reg,
		 "BAR5 candidate register offset for SDK property 201 input-select experiments; 0xffffffff disables hardware writes");

static unsigned int input_select_mask = 0x7;
module_param(input_select_mask, uint, 0644);
MODULE_PARM_DESC(input_select_mask,
		 "bit mask for the property-201 input-select candidate field");

static unsigned int input_select_shift;
module_param(input_select_shift, uint, 0644);
MODULE_PARM_DESC(input_select_shift,
		 "bit shift for the property-201 input-select candidate field");

static unsigned int bitrate_reg = ~0U;
module_param(bitrate_reg, uint, 0644);
MODULE_PARM_DESC(bitrate_reg,
		 "BAR5 candidate register offset for SDK property 403 bitrate experiments; 0xffffffff disables hardware writes");

static unsigned int bitrate_mask = ~0U;
module_param(bitrate_mask, uint, 0644);
MODULE_PARM_DESC(bitrate_mask,
		 "bit mask for the property-403 bitrate candidate field");

static unsigned int bitrate_shift;
module_param(bitrate_shift, uint, 0644);
MODULE_PARM_DESC(bitrate_shift,
		 "bit shift for the property-403 bitrate candidate field");

static unsigned int quality_reg = ~0U;
module_param(quality_reg, uint, 0644);
MODULE_PARM_DESC(quality_reg,
		 "BAR5 candidate register offset for SDK property 404 quality experiments; 0xffffffff disables hardware writes");

static unsigned int quality_mask = ~0U;
module_param(quality_mask, uint, 0644);
MODULE_PARM_DESC(quality_mask,
		 "bit mask for the property-404 quality candidate field");

static unsigned int quality_shift;
module_param(quality_shift, uint, 0644);
MODULE_PARM_DESC(quality_shift,
		 "bit shift for the property-404 quality candidate field");

static unsigned int gop_reg = ~0U;
module_param(gop_reg, uint, 0644);
MODULE_PARM_DESC(gop_reg,
		 "BAR5 candidate register offset for SDK property 405 GOP experiments; 0xffffffff disables hardware writes");

static unsigned int gop_mask = ~0U;
module_param(gop_mask, uint, 0644);
MODULE_PARM_DESC(gop_mask,
		 "bit mask for the property-405 GOP candidate field");

static unsigned int gop_shift;
module_param(gop_shift, uint, 0644);
MODULE_PARM_DESC(gop_shift,
		 "bit shift for the property-405 GOP candidate field");

static unsigned int b_frames_reg = ~0U;
module_param(b_frames_reg, uint, 0644);
MODULE_PARM_DESC(b_frames_reg,
		 "BAR5 candidate register offset for SDK property 411 B-frame experiments; 0xffffffff disables candidate writes");

static unsigned int b_frames_mask = ~0U;
module_param(b_frames_mask, uint, 0644);
MODULE_PARM_DESC(b_frames_mask,
		 "bit mask for the property-411 B-frame candidate field");

static unsigned int b_frames_shift;
module_param(b_frames_shift, uint, 0644);
MODULE_PARM_DESC(b_frames_shift,
		 "bit shift for the property-411 B-frame candidate field");

static unsigned int qp_step_reg = ~0U;
module_param(qp_step_reg, uint, 0644);
MODULE_PARM_DESC(qp_step_reg,
		 "BAR5 candidate register offset for SDK property 408 QP-step experiments; 0xffffffff disables candidate writes");

static unsigned int qp_step_mask = ~0U;
module_param(qp_step_mask, uint, 0644);
MODULE_PARM_DESC(qp_step_mask,
		 "bit mask for the property-408 QP-step candidate field");

static unsigned int qp_step_shift;
module_param(qp_step_shift, uint, 0644);
MODULE_PARM_DESC(qp_step_shift,
		 "bit shift for the property-408 QP-step candidate field");

static unsigned int record_mode_reg = ~0U;
module_param(record_mode_reg, uint, 0644);
MODULE_PARM_DESC(record_mode_reg,
		 "BAR5 candidate register offset for SDK property 407 record-mode experiments; 0xffffffff disables hardware writes");

static unsigned int record_mode_mask = 0x3;
module_param(record_mode_mask, uint, 0644);
MODULE_PARM_DESC(record_mode_mask,
		 "bit mask for the property-407 record-mode candidate field");

static unsigned int record_mode_shift;
module_param(record_mode_shift, uint, 0644);
MODULE_PARM_DESC(record_mode_shift,
		 "bit shift for the property-407 record-mode candidate field");

bool mz0380_enable_video;
module_param_named(enable_video, mz0380_enable_video, bool, 0444);
MODULE_PARM_DESC(enable_video,
		 "register the probe-safe V4L2 node; disabled by default to keep unloads simple during bring-up");

static unsigned int card[] = { [0 ... (MZ0380_MAXBOARDS - 1)] = UNSET };
module_param_array(card, int, NULL, 0444);
MODULE_PARM_DESC(card, "card type");

/*
 * Bring-up gates. All default OFF so existing probe-safe behaviour
 * survives. Flip them on incrementally as each phase is verified.
 *
 *   firmware_upload=1   load /lib/firmware/mz0380/MZ0380.HD.HEX
 *   enable_dma=1        alloc rings, enable bus master, request MSI
 *   enable_audio=1      register an ALSA snd_card alongside V4L2
 */
bool mz0380_firmware_upload_enabled;
module_param_named(firmware_upload, mz0380_firmware_upload_enabled,
		   bool, 0444);
MODULE_PARM_DESC(firmware_upload,
		 "load and upload the onboard ARM firmware (mz0380/MZ0380.HD.HEX); off by default until protocol is verified");

bool mz0380_enable_dma;
module_param_named(enable_dma, mz0380_enable_dma, bool, 0444);
MODULE_PARM_DESC(enable_dma,
		 "allocate ring buffers, request MSI, enable bus mastering; off by default");

/*
 * M4 diagnostic: enable bus mastering + MSI/ISR BEFORE firmware load, but do
 * NOT program any (still-unverified) ring addresses. Safe because the card has
 * no host DMA target to write to; tests whether the post-boot mailbox doorbell
 * only reaches the card once bus mastering is on.
 */
bool mz0380_dma_handshake;
module_param_named(dma_handshake, mz0380_dma_handshake, bool, 0444);
MODULE_PARM_DESC(dma_handshake,
		 "enable bus master + MSI (no ring programming) before firmware handshake; M4 diagnostic");

bool mz0380_enable_audio;
module_param_named(enable_audio, mz0380_enable_audio, bool, 0444);
MODULE_PARM_DESC(enable_audio,
		 "register an ALSA HDMI audio capture device; off by default");

unsigned int mz0380_video_ring_entries = 16;
module_param_named(video_ring_entries, mz0380_video_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entries,
		 "number of video DMA ring entries (default 16)");

unsigned int mz0380_video_ring_entry_size = (512 * 1024);
module_param_named(video_ring_entry_size, mz0380_video_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entry_size,
		 "size in bytes of each video DMA ring entry (default 512 KiB)");

unsigned int mz0380_audio_ring_entries = 8;
module_param_named(audio_ring_entries, mz0380_audio_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entries,
		 "number of audio DMA ring entries (default 8)");

unsigned int mz0380_audio_ring_entry_size = 32768;
module_param_named(audio_ring_entry_size, mz0380_audio_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entry_size,
		 "size in bytes of each audio DMA ring entry (default 32 KiB)");

#define dprintk(level, fmt, arg...) \
	do { if (debug >= level) \
		printk(KERN_DEBUG "%s: " fmt, dev->name, ##arg); \
	} while (0)

static unsigned int mz0380_devcount;
static DEFINE_MUTEX(devlist);
static LIST_HEAD(mz0380_devlist);

#define MZ0380_PROC_CMD_MAX 64

struct mz0380_snapshot_reg {
	const char *section;
	const char *name;
	int map;
	u32 reg;
	unsigned int min_profile;
};

static const struct mz0380_snapshot_reg mz0380_snapshot_regs[] = {
	{
		.section = "BAR0/MMIO",
		.name = "mmio_probe_0000",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x0000,
		.min_profile = 1,
	},
	{
		.section = "BAR0/MMIO",
		.name = "mmio_probe_0004",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x0004,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG",
		.name = "cfg_probe_0000",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0000,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG",
		.name = "cfg_probe_0004",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0004,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0008",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0008,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_000c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x000c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0010",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0010,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0014",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0014,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0018",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0018,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_001c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x001c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0020",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0020,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0024",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0024,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0028",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0028,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_002c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x002c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0030",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0030,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0034",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0034,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0038",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0038,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_003c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x003c,
		.min_profile = 2,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00a8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00a8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00ac",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00ac,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00c4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00c4,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00c8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00c8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d0",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d0,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d4,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00dc",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00dc,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00e4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00e4,
		.min_profile = 3,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0040",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0040,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0044",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0044,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0048",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0048,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_004c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x004c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0050",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0050,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0054",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0054,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0058",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0058,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_005c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x005c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0060",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0060,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0064",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0064,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0068",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0068,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_006c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x006c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0070",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0070,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0074",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0074,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0078",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0078,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_007c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x007c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0080",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0080,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0084",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0084,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0088",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0088,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_008c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x008c,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0090",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0090,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0094",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0094,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0098",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0098,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_009c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x009c,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a0",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a0,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a4",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a4,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a8",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a8,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00ac",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00ac,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b0",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b0,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b4",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b4,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b8",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b8,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00bc",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00bc,
		.min_profile = 5,
	},
	/*
	 * Profile 6 (M0 observability widen): mailbox command/status/param
	 * window (0x00c0..0x00e4), DMA ring window (0x0200..0x0234), and the
	 * firmware upload seq/ack/buffer window (0x0300..0x0310, 0x0400..0x0410).
	 * All BAR5, always mapped, read-only. Lets snapshot diffs reveal whether
	 * the CHECKME offsets in mz0380-reg.h respond to activity.
	 */
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c4, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c8", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c8, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00cc", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00cc, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d4, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d8", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d8, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00dc", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00dc, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00e0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00e0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00e4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00e4, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0200", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0200, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0204", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0204, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0208", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0208, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_020c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x020c, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0210", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0210, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0214", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0214, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0220", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0220, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0224", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0224, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0228", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0228, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_022c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x022c, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0230", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0230, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0234", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0234, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0300", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0300, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0304", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0304, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0308", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0308, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_030c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x030c, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0310", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0310, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0400", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0400, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0404", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0404, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0408", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0408, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_040c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x040c, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0410", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0410, .min_profile = 6, },
};

static unsigned int mz0380_snapshot_profile(void)
{
	if (snapshot_profile < 2)
		return 1;

	if (snapshot_profile < 3)
		return 2;

	if (snapshot_profile < 4)
		return 3;

	if (snapshot_profile < 5)
		return 4;

	if (snapshot_profile < 6)
		return 5;

	return 6;
}

static void mz0380_snapshot_note(struct mz0380_dev *dev, u32 value,
				 char *note, size_t note_len)
{
	int map;

	note[0] = '\0';

	for (map = 0; map < MZ0380_MAX_MAPS; map++) {
		resource_size_t start = dev->bar_start[map];
		resource_size_t end = start + dev->bar_len[map];
		unsigned long long offset;

		if (!dev->lmmio[map] || value < start || value >= end)
			continue;

		offset = (unsigned long long)(value - start);
		if (offset & 0x3)
			snprintf(note, note_len,
				 " -> bar%d+0x%llx (unaligned)",
				 dev->bar_nr[map], offset);
		else
			snprintf(note, note_len, " -> bar%d+0x%llx",
				 dev->bar_nr[map], offset);
		break;
	}
}

static void mz0380_dump_snapshot_reg(struct seq_file *m, struct mz0380_dev *dev,
				     const struct mz0380_snapshot_reg *snap)
{
	char note[48];
	u32 value;

	value = mz_read(dev, snap->map, snap->reg);
	mz0380_snapshot_note(dev, value, note, sizeof(note));

	seq_printf(m, "    %-16s [0x%04x] = %08x%s\n",
		   snap->name, snap->reg, value, note);
}

static void mz0380_dump_snapshot_range(struct seq_file *m, struct mz0380_dev *dev,
				       unsigned int profile, const char *title,
				       int map, u32 start, u32 end)
{
	unsigned int i;
	bool found = false;

	seq_printf(m, "  %s\n", title);

	for (i = 0; i < ARRAY_SIZE(mz0380_snapshot_regs); i++) {
		const struct mz0380_snapshot_reg *snap = &mz0380_snapshot_regs[i];

		if (profile < snap->min_profile)
			continue;
		if (snap->map != map)
			continue;
		if (snap->reg < start || snap->reg > end)
			continue;

		mz0380_dump_snapshot_reg(m, dev, snap);
		found = true;
	}

	if (!found)
		seq_printf(m,
			   "    unavailable at snapshot_profile=%u\n",
			   profile);
}

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg)
{
	return readl(dev->lmmio[map] + reg);
}

void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value)
{
	writel(value, dev->lmmio[map] + reg);
}

static u32 mz0380_cfg_trace_reg(unsigned int index)
{
	return MZ0380_CFG_TRACE_START + (index * sizeof(u32));
}

static void mz0380_capture_cfg_trace(struct mz0380_dev *dev, u32 *trace)
{
	unsigned int i;

	for (i = 0; i < MZ0380_CFG_TRACE_COUNT; i++)
		trace[i] = mz_read(dev, MZ0380_MAP_BAR_CFG,
				   mz0380_cfg_trace_reg(i));
}

static bool mz0380_candidate_config_valid(struct mz0380_dev *dev, u32 reg,
					  u32 mask, u32 shift)
{
	u64 field_mask;

	if (reg == ~0U || (reg & 0x3))
		return false;

	if ((resource_size_t)reg + sizeof(u32) >
	    dev->bar_len[MZ0380_MAP_BAR_CFG])
		return false;

	if (!mask || shift >= 32)
		return false;

	field_mask = (u64)mask << shift;
	if (field_mask > U32_MAX)
		return false;

	return true;
}

static bool mz0380_input_select_config_valid(struct mz0380_dev *dev)
{
	return mz0380_candidate_config_valid(dev, input_select_reg,
					     input_select_mask,
					     input_select_shift);
}

static u32 mz0380_extract_field(u32 word, u32 mask, u32 shift)
{
	return (word >> shift) & mask;
}

static bool mz0380_has_hw_cfg_field(struct mz0380_dev *dev, u32 reg,
				    u32 mask, u32 shift)
{
	return dev->board == MZ0380_BOARD_ELGATO_HD60_PRO &&
	       mz0380_candidate_config_valid(dev, reg, mask, shift);
}

static bool mz0380_read_hw_cfg_field(struct mz0380_dev *dev, u32 reg,
				     u32 mask, u32 shift, u32 *value,
				     u32 *raw_word, u32 *out_reg,
				     u32 *out_mask, u32 *out_shift)
{
	u32 word;

	if (!mz0380_has_hw_cfg_field(dev, reg, mask, shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	if (raw_word)
		*raw_word = word;
	if (value)
		*value = mz0380_extract_field(word, mask, shift);
	if (out_reg)
		*out_reg = reg;
	if (out_mask)
		*out_mask = mask;
	if (out_shift)
		*out_shift = shift;

	return true;
}

static bool mz0380_source_uses_candidate_override(const char *source)
{
	return source && !strcmp(source, "procfs");
}

static const char *
mz0380_property_experiment_result_name(enum mz0380_property_experiment_result result)
{
	switch (result) {
	case MZ0380_PROPERTY_EXPERIMENT_NONE:
		return "none";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED:
		return "intent-only (writes disabled)";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED:
		return "intent-only (candidate register not configured)";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG:
		return "intent-only (candidate register config invalid)";
	case MZ0380_PROPERTY_EXPERIMENT_CACHED_ONLY:
		return "cached-only (no hardware mapping confirmed)";
	case MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED:
		return "hardware write applied";
	case MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH:
		return "hardware write readback mismatch";
	}

	return "unknown";
}

static void mz0380_input_candidate_clear(void)
{
	input_select_reg = ~0U;
	input_select_mask = 0x7;
	input_select_shift = 0;
}

static void mz0380_input_candidate_set(u32 reg, u32 mask, u32 shift)
{
	input_select_reg = reg;
	input_select_mask = mask;
	input_select_shift = shift;
}

static void mz0380_bitrate_candidate_clear(void)
{
	bitrate_reg = ~0U;
	bitrate_mask = ~0U;
	bitrate_shift = 0;
}

static void mz0380_bitrate_candidate_set(u32 reg, u32 mask, u32 shift)
{
	bitrate_reg = reg;
	bitrate_mask = mask;
	bitrate_shift = shift;
}

static void mz0380_quality_candidate_clear(void)
{
	quality_reg = ~0U;
	quality_mask = ~0U;
	quality_shift = 0;
}

static void mz0380_quality_candidate_set(u32 reg, u32 mask, u32 shift)
{
	quality_reg = reg;
	quality_mask = mask;
	quality_shift = shift;
}

static void mz0380_gop_candidate_clear(void)
{
	gop_reg = ~0U;
	gop_mask = ~0U;
	gop_shift = 0;
}

static void mz0380_gop_candidate_set(u32 reg, u32 mask, u32 shift)
{
	gop_reg = reg;
	gop_mask = mask;
	gop_shift = shift;
}

static void mz0380_b_frames_candidate_clear(void)
{
	b_frames_reg = ~0U;
	b_frames_mask = ~0U;
	b_frames_shift = 0;
}

static void mz0380_b_frames_candidate_set(u32 reg, u32 mask, u32 shift)
{
	b_frames_reg = reg;
	b_frames_mask = mask;
	b_frames_shift = shift;
}

static void mz0380_qp_step_candidate_clear(void)
{
	qp_step_reg = ~0U;
	qp_step_mask = ~0U;
	qp_step_shift = 0;
}

static void mz0380_qp_step_candidate_set(u32 reg, u32 mask, u32 shift)
{
	qp_step_reg = reg;
	qp_step_mask = mask;
	qp_step_shift = shift;
}

static void mz0380_record_mode_candidate_clear(void)
{
	record_mode_reg = ~0U;
	record_mode_mask = 0x3;
	record_mode_shift = 0;
}

static void mz0380_record_mode_candidate_set(u32 reg, u32 mask, u32 shift)
{
	record_mode_reg = reg;
	record_mode_mask = mask;
	record_mode_shift = shift;
}

static int
mz0380_request_scalar_property_locked(struct mz0380_dev *dev,
				      struct mz0380_property_experiment *exp,
				      u32 property_id, u32 *cached_value,
				      u32 requested_value, u32 value_count,
				      u32 reg, u32 mask, u32 shift,
				      const char *source,
				      bool require_write_enable)
{
	u32 field_mask;
	int ret = 0;

	if (value_count && requested_value >= value_count)
		return -EINVAL;

	memset(exp, 0, sizeof(*exp));
	exp->valid = true;
	exp->property_id = property_id;
	exp->previous_value = *cached_value;
	exp->requested_value = requested_value;
	exp->reg = reg;
	exp->mask = mask;
	exp->shift = shift;
	strscpy(exp->source, source ?: "unknown", sizeof(exp->source));

	mz0380_capture_cfg_trace(dev, exp->before_cfg_trace);
	*cached_value = requested_value;

	if (require_write_enable && !allow_experimental_writes) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED;
		goto out_trace;
	}

	if (reg == ~0U) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED;
		ret = require_write_enable ? 0 : -EINVAL;
		goto out_trace;
	}

	if (!mz0380_candidate_config_valid(dev, reg, mask, shift)) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG;
		ret = require_write_enable ? 0 : -EINVAL;
		goto out_trace;
	}

	field_mask = mask << shift;
	exp->before_word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	exp->programmed_word =
		(exp->before_word & ~field_mask) |
		((requested_value & mask) << shift);
	mz_write(dev, MZ0380_MAP_BAR_CFG, reg, exp->programmed_word);
	exp->readback_word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	exp->hardware_write = true;
	if (exp->readback_word != exp->programmed_word)
		exp->result =
			MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH;
	else
		exp->result = MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED;
	ret = exp->result ==
	      MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH ?
		-EIO : 0;

out_trace:
	mz0380_capture_cfg_trace(dev, exp->after_cfg_trace);
	return ret;
}

bool mz0380_read_hw_input_select(struct mz0380_dev *dev, u32 *input, u32 *raw_word)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_INPUT_SELECT_HW_REG,
					MZ0380_INPUT_SELECT_HW_MASK,
					MZ0380_INPUT_SELECT_HW_SHIFT,
					input, raw_word, NULL, NULL, NULL);
}

bool mz0380_read_hw_bitrate(struct mz0380_dev *dev, u32 *bitrate,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_BITRATE_HW_REG,
					MZ0380_BITRATE_HW_MASK,
					MZ0380_BITRATE_HW_SHIFT,
					bitrate, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_quality(struct mz0380_dev *dev, u32 *quality,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_QUALITY_HW_REG,
					MZ0380_QUALITY_HW_MASK,
					MZ0380_QUALITY_HW_SHIFT,
					quality, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_record_mode(struct mz0380_dev *dev, u32 *mode,
				u32 *raw_word, u32 *reg,
				u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_RECORD_MODE_HW_REG,
					MZ0380_RECORD_MODE_HW_MASK,
					MZ0380_RECORD_MODE_HW_SHIFT,
					mode, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_gop(struct mz0380_dev *dev, u32 *gop,
			u32 *raw_word, u32 *reg,
			u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_GOP_HW_REG,
					MZ0380_GOP_HW_MASK,
					MZ0380_GOP_HW_SHIFT,
					gop, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_b_frames(struct mz0380_dev *dev, u32 *b_frames,
			     u32 *raw_word, u32 *reg,
			     u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_B_FRAMES_HW_REG,
					MZ0380_B_FRAMES_HW_MASK,
					MZ0380_B_FRAMES_HW_SHIFT,
					b_frames, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_qp_step(struct mz0380_dev *dev, u32 *qp_step,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_QP_STEP_HW_REG,
					MZ0380_QP_STEP_HW_MASK,
					MZ0380_QP_STEP_HW_SHIFT,
					qp_step, raw_word, reg, mask, shift);
}

bool mz0380_read_candidate_bitrate(struct mz0380_dev *dev, u32 *bitrate,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, bitrate_reg,
					   bitrate_mask,
					   bitrate_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, bitrate_reg);
	if (raw_word)
		*raw_word = word;
	if (bitrate)
		*bitrate = mz0380_extract_field(word, bitrate_mask,
						bitrate_shift);
	if (reg)
		*reg = bitrate_reg;
	if (mask)
		*mask = bitrate_mask;
	if (shift)
		*shift = bitrate_shift;

	return true;
}

bool mz0380_read_candidate_quality(struct mz0380_dev *dev, u32 *quality,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, quality_reg,
					   quality_mask,
					   quality_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, quality_reg);
	if (raw_word)
		*raw_word = word;
	if (quality)
		*quality = mz0380_extract_field(word, quality_mask,
						quality_shift);
	if (reg)
		*reg = quality_reg;
	if (mask)
		*mask = quality_mask;
	if (shift)
		*shift = quality_shift;

	return true;
}

bool mz0380_read_candidate_gop(struct mz0380_dev *dev, u32 *gop,
			       u32 *raw_word, u32 *reg,
			       u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, gop_reg,
					   gop_mask,
					   gop_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, gop_reg);
	if (raw_word)
		*raw_word = word;
	if (gop)
		*gop = mz0380_extract_field(word, gop_mask,
					    gop_shift);
	if (reg)
		*reg = gop_reg;
	if (mask)
		*mask = gop_mask;
	if (shift)
		*shift = gop_shift;

	return true;
}

bool mz0380_read_candidate_b_frames(struct mz0380_dev *dev, u32 *b_frames,
				    u32 *raw_word, u32 *reg,
				    u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, b_frames_reg,
					   b_frames_mask,
					   b_frames_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, b_frames_reg);
	if (raw_word)
		*raw_word = word;
	if (b_frames)
		*b_frames = mz0380_extract_field(word, b_frames_mask,
						 b_frames_shift);
	if (reg)
		*reg = b_frames_reg;
	if (mask)
		*mask = b_frames_mask;
	if (shift)
		*shift = b_frames_shift;

	return true;
}

bool mz0380_read_candidate_qp_step(struct mz0380_dev *dev, u32 *qp_step,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, qp_step_reg,
					   qp_step_mask,
					   qp_step_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, qp_step_reg);
	if (raw_word)
		*raw_word = word;
	if (qp_step)
		*qp_step = mz0380_extract_field(word, qp_step_mask,
						qp_step_shift);
	if (reg)
		*reg = qp_step_reg;
	if (mask)
		*mask = qp_step_mask;
	if (shift)
		*shift = qp_step_shift;

	return true;
}

bool mz0380_sync_candidate_bitrate(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 bitrate;
	u32 raw_word;

	if (!mz0380_read_candidate_bitrate(dev, &bitrate, &raw_word, NULL,
					   NULL, NULL))
		return false;

	dev->capture.bitrate = bitrate;
	printk(KERN_INFO
	       "%s: synced cached bitrate from candidate BAR5 field = %u (raw=%08x)%s%s\n",
	       dev->name, bitrate, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE)
		printk(KERN_WARNING
		       "%s: candidate bitrate field is outside current V4L2 range [%u..%u]\n",
		       dev->name, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE);

	return true;
}

bool mz0380_sync_hw_bitrate(struct mz0380_dev *dev, const char *reason)
{
	u32 bitrate;
	u32 raw_word;

	if (!mz0380_read_hw_bitrate(dev, &bitrate, &raw_word, NULL,
				    NULL, NULL))
		return false;

	dev->capture.bitrate = bitrate;
	printk(KERN_INFO
	       "%s: synced cached bitrate from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
	       dev->name, MZ0380_BITRATE_HW_REG, bitrate, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE)
		printk(KERN_WARNING
		       "%s: BAR5 bitrate field is outside current V4L2 range [%u..%u]\n",
		       dev->name, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE);

	return true;
}

bool mz0380_sync_candidate_quality(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 quality;
	u32 raw_word;

	if (!mz0380_read_candidate_quality(dev, &quality, &raw_word, NULL,
					   NULL, NULL))
		return false;

	dev->capture.quality = quality;
	printk(KERN_INFO
	       "%s: synced cached quality from candidate BAR5 field = %u (raw=%08x)%s%s\n",
	       dev->name, quality, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_quality(struct mz0380_dev *dev, const char *reason)
{
	u32 quality;
	u32 raw_word;

	if (!mz0380_read_hw_quality(dev, &quality, &raw_word, NULL,
				    NULL, NULL))
		return false;

	dev->capture.quality = quality;
	printk(KERN_INFO
	       "%s: synced cached quality from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
	       dev->name, MZ0380_QUALITY_HW_REG, quality, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	if (quality < MZ0380_MIN_QUALITY || quality > MZ0380_MAX_QUALITY)
		printk(KERN_WARNING
		       "%s: BAR5 quality field is outside current V4L2 range [%u..%u]\n",
		       dev->name, MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY);

	return true;
}

bool mz0380_sync_candidate_gop(struct mz0380_dev *dev,
			       const char *reason)
{
	u32 gop;
	u32 raw_word;

	if (!mz0380_read_candidate_gop(dev, &gop, &raw_word, NULL,
				       NULL, NULL))
		return false;

	dev->capture.gop_size = gop;
	printk(KERN_INFO
	       "%s: synced cached GOP from candidate BAR5 field = %u (raw=%08x)%s%s\n",
	       dev->name, gop, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_candidate_b_frames(struct mz0380_dev *dev,
				    const char *reason)
{
	u32 b_frames;
	u32 raw_word;

	if (!mz0380_read_candidate_b_frames(dev, &b_frames, &raw_word, NULL,
					    NULL, NULL))
		return false;

	if (b_frames > MZ0380_MAX_B_FRAMES) {
		printk(KERN_WARNING
		       "%s: candidate B-frame field returned out-of-range value %u (raw=%08x)%s%s\n",
		       dev->name, b_frames, raw_word,
		       reason && *reason ? " during " : "",
		       reason && *reason ? reason : "");
		return false;
	}

	dev->capture.b_frames = b_frames;
	printk(KERN_INFO
	       "%s: synced cached B-frames from candidate BAR5 field = %u (raw=%08x)%s%s\n",
	       dev->name, b_frames, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_b_frames(struct mz0380_dev *dev,
			     const char *reason)
{
	u32 b_frames;
	u32 raw_word;

	if (!mz0380_read_hw_b_frames(dev, &b_frames, &raw_word, NULL,
				     NULL, NULL))
		return false;

	if (b_frames > MZ0380_MAX_B_FRAMES) {
		printk(KERN_WARNING
		       "%s: BAR5 B-frame field returned out-of-range value %u (raw=%08x)%s%s\n",
		       dev->name, b_frames, raw_word,
		       reason && *reason ? " during " : "",
		       reason && *reason ? reason : "");
		return false;
	}

	dev->capture.b_frames = b_frames;
	printk(KERN_INFO
	       "%s: synced cached B-frames from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
	       dev->name, MZ0380_B_FRAMES_HW_REG, b_frames, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_candidate_qp_step(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 qp_step;
	u32 raw_word;

	if (!mz0380_read_candidate_qp_step(dev, &qp_step, &raw_word, NULL,
					   NULL, NULL))
		return false;

	dev->capture.qp_step = qp_step;
	printk(KERN_INFO
	       "%s: synced cached QP step from candidate BAR5 field = %u (raw=%08x)%s%s\n",
	       dev->name, qp_step, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_qp_step(struct mz0380_dev *dev, const char *reason)
{
	u32 qp_step;
	u32 raw_word;

	if (!mz0380_read_hw_qp_step(dev, &qp_step, &raw_word, NULL,
				    NULL, NULL))
		return false;

	dev->capture.qp_step = qp_step;
	printk(KERN_INFO
	       "%s: synced cached QP step from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
	       dev->name, MZ0380_QP_STEP_HW_REG, qp_step, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_gop(struct mz0380_dev *dev, const char *reason)
{
	u32 gop;
	u32 raw_word;

	if (!mz0380_read_hw_gop(dev, &gop, &raw_word, NULL,
				NULL, NULL))
		return false;

	dev->capture.gop_size = gop;
	printk(KERN_INFO
	       "%s: synced cached GOP from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
	       dev->name, MZ0380_GOP_HW_REG, gop, raw_word,
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_read_candidate_record_mode(struct mz0380_dev *dev, u32 *mode,
				       u32 *raw_word, u32 *reg,
				       u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, record_mode_reg,
					   record_mode_mask,
					   record_mode_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, record_mode_reg);
	if (raw_word)
		*raw_word = word;
	if (mode)
		*mode = mz0380_extract_field(word, record_mode_mask,
					       record_mode_shift);
	if (reg)
		*reg = record_mode_reg;
	if (mask)
		*mask = record_mode_mask;
	if (shift)
		*shift = record_mode_shift;

	return true;
}

bool mz0380_sync_candidate_record_mode(struct mz0380_dev *dev,
				       const char *reason)
{
	u32 mode;
	u32 raw_word;

	if (!mz0380_read_candidate_record_mode(dev, &mode, &raw_word, NULL,
					       NULL, NULL))
		return false;

	if (mode >= MZ0380_RECORD_MODE_COUNT) {
		printk(KERN_WARNING
		       "%s: candidate record-mode field returned out-of-range value %u (raw=%08x)%s%s\n",
		       dev->name, mode, raw_word,
		       reason && *reason ? " during " : "",
		       reason && *reason ? reason : "");
		return false;
	}

	dev->capture.record_mode = mode;
	printk(KERN_INFO
	       "%s: synced cached record mode from candidate BAR5 field = %u (%s)%s%s\n",
	       dev->name, mode, mz0380_record_mode_name(mode),
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_record_mode(struct mz0380_dev *dev,
				const char *reason)
{
	u32 mode;
	u32 raw_word;

	if (!mz0380_read_hw_record_mode(dev, &mode, &raw_word, NULL,
					NULL, NULL))
		return false;

	if (mode >= MZ0380_RECORD_MODE_COUNT) {
		printk(KERN_WARNING
		       "%s: BAR5 record-mode field returned out-of-range value %u (raw=%08x)%s%s\n",
		       dev->name, mode, raw_word,
		       reason && *reason ? " during " : "",
		       reason && *reason ? reason : "");
		return false;
	}

	dev->capture.record_mode = mode;
	printk(KERN_INFO
	       "%s: synced cached record mode from BAR5[0x%04x] = %u (%s)%s%s\n",
	       dev->name, MZ0380_RECORD_MODE_HW_REG, mode,
	       mz0380_record_mode_name(mode),
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_input_select(struct mz0380_dev *dev, const char *reason)
{
	u32 input;
	u32 raw_word;

	if (!mz0380_read_hw_input_select(dev, &input, &raw_word))
		return false;

	if (input >= MZ0380_INPUT_COUNT) {
		printk(KERN_WARNING
		       "%s: BAR5[0x%04x][2:0] returned out-of-range input %u (raw=%08x)%s%s\n",
		       dev->name, MZ0380_INPUT_SELECT_HW_REG, input, raw_word,
		       reason && *reason ? " during " : "",
		       reason && *reason ? reason : "");
		return false;
	}

	dev->capture.input = input;
	printk(KERN_INFO
	       "%s: synced cached input from BAR5[0x%04x][2:0] = %u (%s)%s%s\n",
	       dev->name, MZ0380_INPUT_SELECT_HW_REG, input,
	       mz0380_input_name(input),
	       reason && *reason ? " during " : "",
	       reason && *reason ? reason : "");

	return true;
}

int mz0380_request_input_select(struct mz0380_dev *dev, u32 input,
			       const char *source)
{
	struct mz0380_property_experiment *exp = &dev->input_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_input_select_config_valid(dev);
	int ret;

	if (input >= MZ0380_INPUT_COUNT)
		return -EINVAL;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_INPUT_SELECT_PROPERTY,
						    &dev->capture.input, input,
						    MZ0380_INPUT_COUNT,
						    use_candidate ?
						    input_select_reg :
						    MZ0380_INPUT_SELECT_HW_REG,
						    use_candidate ?
						    input_select_mask :
						    MZ0380_INPUT_SELECT_HW_MASK,
						    use_candidate ?
						    input_select_shift :
						    MZ0380_INPUT_SELECT_HW_SHIFT,
						    source, use_candidate);
	mutex_unlock(&dev->ctrl_lock);

	printk(KERN_INFO
	       "%s: property %u input-select request %u (%s) via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       mz0380_input_name(exp->requested_value), exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_bitrate_locked(struct mz0380_dev *dev, u32 bitrate,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->bitrate_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, bitrate_reg,
					      bitrate_mask,
					      bitrate_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_BITRATE_PROPERTY,
						    &dev->capture.bitrate,
						    bitrate, 0,
						    use_candidate ?
						    bitrate_reg :
						    MZ0380_BITRATE_HW_REG,
						    use_candidate ?
						    bitrate_mask :
						    MZ0380_BITRATE_HW_MASK,
						    use_candidate ?
						    bitrate_shift :
						    MZ0380_BITRATE_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u bitrate request %u via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_bitrate(struct mz0380_dev *dev, u32 bitrate,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_bitrate_locked(dev, bitrate, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_quality_locked(struct mz0380_dev *dev, u32 quality,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->quality_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, quality_reg,
					      quality_mask,
					      quality_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_QUALITY_PROPERTY,
						    &dev->capture.quality,
						    quality, 0,
						    use_candidate ?
						    quality_reg :
						    MZ0380_QUALITY_HW_REG,
						    use_candidate ?
						    quality_mask :
						    MZ0380_QUALITY_HW_MASK,
						    use_candidate ?
						    quality_shift :
						    MZ0380_QUALITY_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u quality request %u via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_quality(struct mz0380_dev *dev, u32 quality,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_quality_locked(dev, quality, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_gop_locked(struct mz0380_dev *dev, u32 gop,
			      const char *source)
{
	struct mz0380_property_experiment *exp = &dev->gop_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, gop_reg,
					      gop_mask,
					      gop_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_GOP_PROPERTY,
						    &dev->capture.gop_size,
						    gop, 0,
						    use_candidate ?
						    gop_reg :
						    MZ0380_GOP_HW_REG,
						    use_candidate ?
						    gop_mask :
						    MZ0380_GOP_HW_MASK,
						    use_candidate ?
						    gop_shift :
						    MZ0380_GOP_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u GOP request %u via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_gop(struct mz0380_dev *dev, u32 gop,
		       const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_gop_locked(dev, gop, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_b_frames_locked(struct mz0380_dev *dev, u32 b_frames,
				   const char *source)
{
	struct mz0380_property_experiment *exp = &dev->b_frames_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, b_frames_reg,
					      b_frames_mask,
					      b_frames_shift);
	int ret;

	if (b_frames > MZ0380_MAX_B_FRAMES)
		return -EINVAL;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_B_FRAMES_PROPERTY,
						    &dev->capture.b_frames,
						    b_frames,
						    MZ0380_MAX_B_FRAMES + 1,
						    use_candidate ?
						    b_frames_reg :
						    MZ0380_B_FRAMES_HW_REG,
						    use_candidate ?
						    b_frames_mask :
						    MZ0380_B_FRAMES_HW_MASK,
						    use_candidate ?
						    b_frames_shift :
						    MZ0380_B_FRAMES_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u B-frames request %u via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_b_frames(struct mz0380_dev *dev, u32 b_frames,
			    const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_b_frames_locked(dev, b_frames, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_qp_step_locked(struct mz0380_dev *dev, u32 qp_step,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->qp_step_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, qp_step_reg,
					      qp_step_mask,
					      qp_step_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_QP_STEP_PROPERTY,
						    &dev->capture.qp_step,
						    qp_step, 0,
						    use_candidate ?
						    qp_step_reg :
						    MZ0380_QP_STEP_HW_REG,
						    use_candidate ?
						    qp_step_mask :
						    MZ0380_QP_STEP_HW_MASK,
						    use_candidate ?
						    qp_step_shift :
						    MZ0380_QP_STEP_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u QP-step request %u via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_qp_step(struct mz0380_dev *dev, u32 qp_step,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_qp_step_locked(dev, qp_step, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_record_mode_locked(struct mz0380_dev *dev, u32 mode,
				      const char *source)
{
	struct mz0380_property_experiment *exp = &dev->record_mode_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, record_mode_reg,
					      record_mode_mask,
					      record_mode_shift);
	int ret;

	if (mode >= MZ0380_RECORD_MODE_COUNT)
		return -EINVAL;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_RECORD_MODE_PROPERTY,
						    &dev->capture.record_mode,
						    mode,
						    MZ0380_RECORD_MODE_COUNT,
						    use_candidate ?
						    record_mode_reg :
						    MZ0380_RECORD_MODE_HW_REG,
						    use_candidate ?
						    record_mode_mask :
						    MZ0380_RECORD_MODE_HW_MASK,
						    use_candidate ?
						    record_mode_shift :
						    MZ0380_RECORD_MODE_HW_SHIFT,
						    source, use_candidate);

	printk(KERN_INFO
	       "%s: property %u record-mode request %u (%s) via %s -> %s\n",
	       dev->name, exp->property_id, exp->requested_value,
	       mz0380_record_mode_name(exp->requested_value), exp->source,
	       mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		printk(KERN_INFO
		       "%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
		       dev->name, exp->property_id, exp->reg, exp->mask,
		       exp->shift, exp->before_word, exp->programmed_word,
		       exp->readback_word);

	return ret;
}

int mz0380_request_record_mode(struct mz0380_dev *dev, u32 mode,
			       const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_record_mode_locked(dev, mode, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

static int mz0380_request_bar(struct mz0380_dev *dev, int map, int bar)
{
	resource_size_t start = pci_resource_start(dev->pci, bar);
	resource_size_t len = pci_resource_len(dev->pci, bar);

	if (request_mem_region(start, len, dev->name) == 0) {
		printk(KERN_ERR "%s: can't get bar[%d] memory @ 0x%llx\n",
		       dev->name, bar, (unsigned long long)start);
		return -EBUSY;
	}

	dev->bar_nr[map] = bar;
	dev->bar_start[map] = start;
	dev->bar_len[map] = len;

	dev->lmmio[map] = ioremap(start, len);
	if (!dev->lmmio[map]) {
		release_mem_region(start, len);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
		printk(KERN_ERR "%s: failed to ioremap bar[%d]\n",
		       dev->name, bar);
		return -ENOMEM;
	}

	dev->bmmio[map] = (u8 __iomem *)dev->lmmio[map];
	return 0;
}

static void mz0380_release_bar(struct mz0380_dev *dev, int map)
{
	if (dev->lmmio[map]) {
		iounmap(dev->lmmio[map]);
		dev->lmmio[map] = NULL;
		dev->bmmio[map] = NULL;
	}

	if (dev->bar_nr[map] >= 0) {
		release_mem_region(dev->bar_start[map], dev->bar_len[map]);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
	}
}

static int mz0380_dev_setup(struct mz0380_dev *dev)
{
	int i;
	int ret;

	mutex_init(&dev->lock);
	mutex_init(&dev->ctrl_lock);
	mutex_init(&dev->fw_lock);
	mutex_init(&dev->cmd_lock);
	mutex_init(&dev->queue_lock);
	init_waitqueue_head(&dev->fw_wait);
	init_waitqueue_head(&dev->cmd_wait);
	spin_lock_init(&dev->buf_lock);
	INIT_LIST_HEAD(&dev->buf_list);
	dev->fw_state = MZ0380_FW_STATE_NONE;
	atomic_set(&dev->irq_count, 0);
	atomic_set(&dev->irq_video_count, 0);
	atomic_set(&dev->irq_audio_count, 0);
	atomic_set(&dev->irq_signal_count, 0);

	dev->nr = mz0380_devcount++;
	snprintf(dev->name, sizeof(dev->name), "mz0380[%u]", dev->nr);

	for (i = 0; i < MZ0380_MAX_MAPS; i++)
		dev->bar_nr[i] = -1;

	dev->board = UNSET;
	if (card[dev->nr] < mz0380_bcount)
		dev->board = card[dev->nr];
	for (i = 0; dev->board == UNSET && i < mz0380_idcount; i++) {
		if (dev->pci->subsystem_vendor == mz0380_subids[i].subvendor &&
		    dev->pci->subsystem_device == mz0380_subids[i].subdevice)
			dev->board = mz0380_subids[i].card;
	}
	if (dev->board == UNSET) {
		dev->board = MZ0380_BOARD_UNKNOWN;
		mz0380_card_list(dev);
	}

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_MMIO, MZ0380_BAR_MMIO);
	if (ret < 0)
		goto fail;

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_CFG, MZ0380_BAR_CFG);
	if (ret < 0)
		goto fail;

	if (dev->bar_len[MZ0380_MAP_BAR_MMIO] < MZ0380_BAR_MMIO_SIZE ||
	    dev->bar_len[MZ0380_MAP_BAR_CFG] < MZ0380_BAR_CFG_SIZE) {
		printk(KERN_ERR
		       "%s: unexpected BAR sizes mmio=0x%llx cfg=0x%llx\n",
		       dev->name,
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);
		ret = -ENODEV;
		goto fail;
	}

	printk(KERN_INFO
	       "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name,
	       dev->pci->subsystem_vendor,
	       dev->pci->subsystem_device,
	       mz0380_boards[dev->board].name,
	       dev->board,
	       card[dev->nr] == dev->board ? "insmod option" : "autodetected");
	printk(KERN_INFO
	       "%s: bar%d @ 0x%llx [0x%llx bytes], bar%d @ 0x%llx [0x%llx bytes]\n",
	       dev->name,
	       dev->bar_nr[MZ0380_MAP_BAR_MMIO],
	       (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO],
	       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
	       dev->bar_nr[MZ0380_MAP_BAR_CFG],
	       (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG],
	       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

	return 0;

fail:
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
	mz0380_devcount--;
	return ret;
}

static void mz0380_dev_unregister(struct mz0380_dev *dev)
{
	mz0380_card_cleanup(dev);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
}

static void mz0380_dump_pci_config(struct seq_file *m, struct mz0380_dev *dev)
{
	u32 reg;
	u32 val = 0;

	seq_puts(m, "  pci cfg dump:\n");
	for (reg = 0; reg < 0x100; reg += 0x10) {
		pci_read_config_dword(dev->pci, reg + 0x0, &val);
		seq_printf(m, "    %02x: %08x", reg + 0x0, val);
		pci_read_config_dword(dev->pci, reg + 0x4, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0x8, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0xc, &val);
		seq_printf(m, " %08x\n", val);
		}
}

static void mz0380_dump_pci_caps(struct seq_file *m, struct mz0380_dev *dev)
{
	int pm_cap;
	int msi_cap;
	int pcie_cap;

	pm_cap = pci_find_capability(dev->pci, PCI_CAP_ID_PM);
	msi_cap = pci_find_capability(dev->pci, PCI_CAP_ID_MSI);
	pcie_cap = pci_find_capability(dev->pci, PCI_CAP_ID_EXP);

	seq_printf(m, "  caps       : PM=%#x MSI=%#x PCIe=%#x\n",
		   pm_cap, msi_cap, pcie_cap);

	if (pm_cap) {
		u16 pmcsr = 0;

		pci_read_config_word(dev->pci, pm_cap + PCI_PM_CTRL, &pmcsr);
		seq_printf(m, "  pm csr     : %04x\n", pmcsr);
	}

	if (msi_cap) {
		u16 msi_flags = 0;

		pci_read_config_word(dev->pci, msi_cap + PCI_MSI_FLAGS, &msi_flags);
		seq_printf(m,
			   "  msi flags  : %04x (enable=%u 64bit=%u multimsg_cap=%u multimsg_en=%u)\n",
			   msi_flags,
			   !!(msi_flags & PCI_MSI_FLAGS_ENABLE),
			   !!(msi_flags & PCI_MSI_FLAGS_64BIT),
			   (msi_flags & PCI_MSI_FLAGS_QMASK) >> 1,
			   (msi_flags & PCI_MSI_FLAGS_QSIZE) >> 4);
	}

	if (pcie_cap) {
		u16 pcie_flags = 0;
		u16 devctl = 0;
		u16 devsta = 0;
		u32 lnkcap = 0;
		u16 lnksta = 0;

		pcie_capability_read_word(dev->pci, PCI_EXP_FLAGS, &pcie_flags);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVCTL, &devctl);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVSTA, &devsta);
		pcie_capability_read_dword(dev->pci, PCI_EXP_LNKCAP, &lnkcap);
		pcie_capability_read_word(dev->pci, PCI_EXP_LNKSTA, &lnksta);

		seq_printf(m, "  pcie flags : %04x (ver=%u type=%u)\n",
			   pcie_flags,
			   pcie_flags & PCI_EXP_FLAGS_VERS,
			   (pcie_flags & PCI_EXP_FLAGS_TYPE) >> 4);
		seq_printf(m, "  pcie dev   : ctl=%04x sta=%04x\n",
			   devctl, devsta);
		seq_printf(m,
			   "  pcie link  : cap=%08x speed_cap=%u width_cap=x%u status=%04x speed=%u width=x%u\n",
			   lnkcap,
			   lnkcap & PCI_EXP_LNKCAP_SLS,
			   (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4,
			   lnksta,
			   lnksta & PCI_EXP_LNKSTA_CLS,
			   (lnksta & PCI_EXP_LNKSTA_NLW) >> 4);
	}
}

static void mz0380_dump_snapshot(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int i;
	unsigned int profile = mz0380_snapshot_profile();
	const char *section = NULL;

	seq_puts(m, "  snapshot   : targeted read-only register snapshot\n");
	seq_puts(m, "  scope      : explicitly curated, known-safe offsets only\n");
	seq_puts(m, "  use        : compare repeated reads across HDMI cable or mode changes\n");
	seq_printf(m, "  profile    : %u\n", profile);
	if (profile >= 2)
		seq_puts(m, "  warning    : profile 2 adds experimental BAR5 low-offset reads beyond the vetted baseline\n");
	if (profile >= 3)
		seq_puts(m, "  warning    : profile 3 also adds SC0710-inspired BAR0 signal probes that are unvetted on MZ0380\n");
	if (profile >= 4)
		seq_puts(m, "  warning    : profile 4 also adds an extended BAR5 window and should be treated as exploratory only\n");
	if (profile >= 5)
		seq_puts(m, "  warning    : profile 5 extends BAR5 further and should be used only for deliberate correlation tests\n");

	for (i = 0; i < ARRAY_SIZE(mz0380_snapshot_regs); i++) {
		const struct mz0380_snapshot_reg *snap = &mz0380_snapshot_regs[i];

		if (profile < snap->min_profile)
			continue;

		if (!section || strcmp(section, snap->section)) {
			section = snap->section;
			seq_printf(m, "  %s\n", section);
		}

		mz0380_dump_snapshot_reg(m, dev, snap);
	}
}

static void mz0380_dump_control_path(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int profile = mz0380_snapshot_profile();

	seq_puts(m, "  control    : SDK-guided BAR5 correlation view\n");
	seq_printf(m, "  profile    : %u\n", profile);
	seq_puts(m, "  handles    : raw=\"MZ0380 PCI\"\n");
	seq_puts(m, "               enc=\"MZ0380 PCI, Analog Encoder\"\n");
	seq_puts(m, "               aud=\"MZ0380 PCI, Analog WaveIn\"\n");
	seq_puts(m, "  sequence   : query 0/8 -> query or set 201 -> raw format -> encoder format and properties -> optional audio -> RUN\n");
	seq_puts(m, "  priorities : keep property 201, 407, 403, 404, 405, 408, and 411 grounded, then move to the next syntax-family target\n");
	seq_puts(m, "  groups     : identity/capability = 0, 8\n");
	seq_puts(m, "               input/receiver = 201, 232, 234, 235\n");
	seq_puts(m, "               encoder rate = 403, 404, 407, 409, 410\n");
	seq_puts(m, "               encoder syntax = 405, 406, 408, 411, 412, 413, 414, 415, 422, 424\n");
	seq_puts(m, "  generic map: 0->405, 1->404, 3->407, 4->403, 5->408,\n");
	seq_puts(m, "               6->409, 7->410, 0x0a->411, 0x0d->422\n");
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 identity/capability candidates [0x0000..0x0018]",
				   MZ0380_MAP_BAR_CFG, 0x0000, 0x0018);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 BAR0-pointer or doorbell hints [0x0030..0x003c]",
				   MZ0380_MAP_BAR_CFG, 0x0030, 0x003c);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working input/receiver window [0x0040..0x0054]",
				   MZ0380_MAP_BAR_CFG, 0x0040, 0x0054);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working encoder-rate window [0x0058..0x007c]",
				   MZ0380_MAP_BAR_CFG, 0x0058, 0x007c);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working encoder-syntax window [0x0080..0x00bc]",
				   MZ0380_MAP_BAR_CFG, 0x0080, 0x00bc);
	seq_puts(m, "  experiment : write 'input <0..4>', 'bitrate <value>', 'quality <value>', 'gop <value>', 'bframes <0..2>', 'qpstep <value>', or 'recordmode <0..2>' to /proc/mz0380-experiment for property-201/403/404/405/407/408/411 runs\n");
	seq_puts(m, "  guardrail  : confirmed properties 201/403/404/405/407/408/411 use fixed BAR5 mappings in the normal driver path; allow_experimental_writes=1 is only required for candidate overrides\n");
}

static void mz0380_dump_cfg_trace_changes(struct seq_file *m,
					  const struct mz0380_property_experiment *exp)
{
	unsigned int i;
	bool found = false;

	for (i = 0; i < MZ0380_CFG_TRACE_COUNT; i++) {
		u32 reg = mz0380_cfg_trace_reg(i);
		u32 before = exp->before_cfg_trace[i];
		u32 after = exp->after_cfg_trace[i];

		if (before == after)
			continue;

		seq_printf(m, "    [0x%04x] %08x -> %08x\n", reg, before, after);
		found = true;
	}

	if (!found)
		seq_puts(m, "    no BAR5 delta across [0x0000..0x00bc]\n");
}

static void mz0380_dump_input_experiment(struct seq_file *m,
					 struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->input_experiment;

	seq_puts(m, "  experiment : SDK property-201 input-select helper\n");
	seq_puts(m, "  command    : echo 'input <0..4>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'candidate clear' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'writes on|off' > /proc/mz0380-experiment\n");
	seq_printf(m, "  writes     : %s\n",
		   allow_experimental_writes ? "enabled" : "disabled");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_INPUT_SELECT_HW_REG, MZ0380_INPUT_SELECT_HW_MASK,
		   MZ0380_INPUT_SELECT_HW_SHIFT);

	if (input_select_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   input_select_reg, input_select_mask,
			   input_select_shift,
			   mz0380_input_select_config_valid(dev) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %s (%u)\n",
		   mz0380_input_name(exp->previous_value),
		   exp->previous_value);
	seq_printf(m, "  requested  : %s (%u)\n",
		   mz0380_input_name(exp->requested_value),
		   exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_bitrate_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->bitrate_experiment;

	seq_puts(m, "  experiment : SDK property-403 bitrate helper\n");
	seq_puts(m, "  command    : echo 'bitrate <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bitrate-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bitrate-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_BITRATE_HW_REG, MZ0380_BITRATE_HW_MASK,
		   MZ0380_BITRATE_HW_SHIFT);

	if (bitrate_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   bitrate_reg, bitrate_mask, bitrate_shift,
			   mz0380_candidate_config_valid(dev, bitrate_reg,
							 bitrate_mask,
							 bitrate_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_quality_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->quality_experiment;

	seq_puts(m, "  experiment : SDK property-404 quality helper\n");
	seq_puts(m, "  command    : echo 'quality <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'quality-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'quality-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_QUALITY_HW_REG, MZ0380_QUALITY_HW_MASK,
		   MZ0380_QUALITY_HW_SHIFT);

	if (quality_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   quality_reg, quality_mask, quality_shift,
			   mz0380_candidate_config_valid(dev, quality_reg,
							 quality_mask,
							 quality_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_gop_experiment(struct seq_file *m,
				       struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->gop_experiment;

	seq_puts(m, "  experiment : SDK property-405 GOP helper\n");
	seq_puts(m, "  command    : echo 'gop <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'gop-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'gop-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_GOP_HW_REG, MZ0380_GOP_HW_MASK,
		   MZ0380_GOP_HW_SHIFT);

	if (gop_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   gop_reg, gop_mask, gop_shift,
			   mz0380_candidate_config_valid(dev, gop_reg,
							 gop_mask,
							 gop_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_b_frames_experiment(struct seq_file *m,
					    struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->b_frames_experiment;

	seq_puts(m, "  experiment : SDK property-411 B-frame helper\n");
	seq_puts(m, "  command    : echo 'bframes <0..2>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bframes-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bframes-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_B_FRAMES_HW_REG, MZ0380_B_FRAMES_HW_MASK,
		   MZ0380_B_FRAMES_HW_SHIFT);

	if (b_frames_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   b_frames_reg, b_frames_mask, b_frames_shift,
			   mz0380_candidate_config_valid(dev, b_frames_reg,
							 b_frames_mask,
							 b_frames_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_qp_step_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->qp_step_experiment;

	seq_puts(m, "  experiment : SDK property-408 QP-step helper\n");
	seq_puts(m, "  command    : echo 'qpstep <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'qpstep-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'qpstep-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_QP_STEP_HW_REG, MZ0380_QP_STEP_HW_MASK,
		   MZ0380_QP_STEP_HW_SHIFT);

	if (qp_step_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   qp_step_reg, qp_step_mask, qp_step_shift,
			   mz0380_candidate_config_valid(dev, qp_step_reg,
							 qp_step_mask,
							 qp_step_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static void mz0380_dump_record_mode_experiment(struct seq_file *m,
					       struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->record_mode_experiment;

	seq_puts(m, "  experiment : SDK property-407 record-mode helper\n");
	seq_puts(m, "  command    : echo 'recordmode <0..2>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'recordmode-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'recordmode-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_RECORD_MODE_HW_REG, MZ0380_RECORD_MODE_HW_MASK,
		   MZ0380_RECORD_MODE_HW_SHIFT);

	if (record_mode_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   record_mode_reg, record_mode_mask,
			   record_mode_shift,
			   mz0380_candidate_config_valid(dev, record_mode_reg,
							 record_mode_mask,
							 record_mode_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %s (%u)\n",
		   mz0380_record_mode_name(exp->previous_value),
		   exp->previous_value);
	seq_printf(m, "  requested  : %s (%u)\n",
		   mz0380_record_mode_name(exp->requested_value),
		   exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u (%s) -> %u (%s) -> %u (%s)\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->before_word,
						      exp->mask, exp->shift)),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->programmed_word,
						      exp->mask, exp->shift)),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->readback_word,
						      exp->mask, exp->shift)));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

static int mz0380_proc_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  pci id     : %04x:%04x\n",
			   dev->pci->vendor, dev->pci->device);
		seq_printf(m, "  subsystem  : %04x:%04x\n",
			   dev->pci->subsystem_vendor,
			   dev->pci->subsystem_device);
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		seq_printf(m, "  win driver : %s\n",
			   mz0380_boards[dev->board].windows_driver);
		if (dev->video_registered)
			seq_printf(m, "  video node : /dev/%s\n",
				   video_device_node_name(&dev->vdev));
		else if (!mz0380_enable_video)
			seq_puts(m,
				 "  video node : disabled (load with enable_video=1 to register /dev/video*)\n");
		seq_printf(m, "  firmware   : %s\n",
			   mz0380_boards[dev->board].firmware_name);
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

		if (procfs_verbosity > 1)
			seq_puts(m, "  BAR scan disabled: wide MMIO reads proved unsafe on this hardware\n");
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_snapshot_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_snapshot(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_control_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_control_path(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_experiment_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_input_experiment(m, dev);
		mz0380_dump_quality_experiment(m, dev);
		mz0380_dump_gop_experiment(m, dev);
		mz0380_dump_b_frames_experiment(m, dev);
		mz0380_dump_qp_step_experiment(m, dev);
		mz0380_dump_bitrate_experiment(m, dev);
		mz0380_dump_record_mode_experiment(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_state_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;
	u16 pci_command = 0;
	u16 pci_status = 0;
	u8 pci_irq_line = 0;
	u8 pci_irq_pin = 0;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		pci_read_config_word(dev->pci, PCI_COMMAND, &pci_command);
		pci_read_config_word(dev->pci, PCI_STATUS, &pci_status);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_LINE, &pci_irq_line);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_PIN, &pci_irq_pin);

		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  rev        : %u\n", dev->pci_rev);
		seq_printf(m, "  latency    : %u\n", dev->pci_lat);
		seq_printf(m, "  irq        : %u\n", dev->pci->irq);
		seq_printf(m, "  pci cmd    : %04x\n", pci_command);
		seq_printf(m, "  pci status : %04x\n", pci_status);
		seq_printf(m, "  bus master : %s\n",
			   pci_command & PCI_COMMAND_MASTER ? "enabled" : "disabled");
		seq_printf(m, "  probe mode : %s\n",
			   allow_bus_master ? "bus-master enabled" : "probe-safe");
		seq_printf(m, "  irq line   : %02x\n", pci_irq_line);
		seq_printf(m, "  irq pin    : %02x\n", pci_irq_pin);
		mz0380_dump_pci_caps(m, dev);
		seq_printf(m, "  mmio[0x0000] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0000));
		seq_printf(m, "  mmio[0x0004] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0004));
		seq_printf(m, "  cfg[0x0000]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0000));
		seq_printf(m, "  cfg[0x0004]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0004));
		mz0380_fw_info_dump(m, dev);
		mz0380_video_state_dump(m, dev);
		if (procfs_verbosity > 1)
			mz0380_dump_pci_config(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static ssize_t mz0380_proc_experiment_write(struct file *file,
					    const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	char *cmd;
	u32 reg;
	u32 mask;
	u32 shift;
	unsigned int input;
	unsigned int quality;
	unsigned int gop;
	unsigned int b_frames;
	unsigned int qp_step;
	unsigned int bitrate;
	unsigned int mode;
	ssize_t ret = -EINVAL;
	struct mz0380_dev *dev = NULL;

	if (!count)
		return 0;

	if (*ppos != 0)
		return -EINVAL;

	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	strim(cmd);
	if (!strcmp(cmd, "writes on")) {
		allow_experimental_writes = true;
		printk(KERN_INFO
		       "mz0380: experimental BAR5 writes enabled via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "writes off")) {
		allow_experimental_writes = false;
		printk(KERN_INFO
		       "mz0380: experimental BAR5 writes disabled via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "candidate clear")) {
		mz0380_input_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 201 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "recordmode-candidate clear")) {
		mz0380_record_mode_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 407 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "bitrate-candidate clear")) {
		mz0380_bitrate_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 403 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "quality-candidate clear")) {
		mz0380_quality_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 404 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "gop-candidate clear")) {
		mz0380_gop_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 405 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "bframes-candidate clear")) {
		mz0380_b_frames_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 411 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "qpstep-candidate clear")) {
		mz0380_qp_step_candidate_clear();
		printk(KERN_INFO
		       "mz0380: property 408 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_input_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 201 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       input_select_reg, input_select_mask,
		       input_select_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "bitrate-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_bitrate_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 403 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       bitrate_reg, bitrate_mask, bitrate_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "quality-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_quality_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 404 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       quality_reg, quality_mask, quality_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "gop-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_gop_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 405 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       gop_reg, gop_mask, gop_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "bframes-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_b_frames_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 411 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       b_frames_reg, b_frames_mask, b_frames_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "qpstep-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_qp_step_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 408 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       qp_step_reg, qp_step_mask, qp_step_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "recordmode-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_record_mode_candidate_set(reg, mask, shift);
		printk(KERN_INFO
		       "mz0380: property 407 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
		       record_mode_reg, record_mode_mask,
		       record_mode_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "input %u", &input) != 1)
		input = UINT_MAX;
	if (sscanf(cmd, "quality %u", &quality) != 1)
		quality = UINT_MAX;
	if (sscanf(cmd, "gop %u", &gop) != 1)
		gop = UINT_MAX;
	if (sscanf(cmd, "bframes %u", &b_frames) != 1)
		b_frames = UINT_MAX;
	if (sscanf(cmd, "qpstep %u", &qp_step) != 1)
		qp_step = UINT_MAX;
	if (sscanf(cmd, "bitrate %u", &bitrate) != 1)
		bitrate = UINT_MAX;
	if (sscanf(cmd, "recordmode %u", &mode) != 1)
		mode = UINT_MAX;
	if (input == UINT_MAX && quality == UINT_MAX && gop == UINT_MAX &&
	    b_frames == UINT_MAX &&
	    qp_step == UINT_MAX &&
	    bitrate == UINT_MAX && mode == UINT_MAX)
		goto out;

	mutex_lock(&devlist);
	if (!list_empty(&mz0380_devlist))
		dev = list_first_entry(&mz0380_devlist, struct mz0380_dev,
				       devlist);
	if (!dev) {
		ret = -ENODEV;
		mutex_unlock(&devlist);
		goto out;
	}

	if (input != UINT_MAX) {
		ret = mz0380_request_input_select(dev, input, "procfs");
	} else if (quality != UINT_MAX) {
		ret = mz0380_request_quality(dev, quality, "procfs");
	} else if (gop != UINT_MAX) {
		ret = mz0380_request_gop(dev, gop, "procfs");
	} else if (b_frames != UINT_MAX) {
		ret = mz0380_request_b_frames(dev, b_frames, "procfs");
	} else if (qp_step != UINT_MAX) {
		ret = mz0380_request_qp_step(dev, qp_step, "procfs");
	} else if (bitrate != UINT_MAX) {
		ret = mz0380_request_bitrate(dev, bitrate, "procfs");
	} else {
		ret = mz0380_request_record_mode(dev, mode, "procfs");
	}
	mutex_unlock(&devlist);
	if (!ret) {
		*ppos += count;
		ret = count;
	}

out:
	kfree(cmd);
	return ret;
}

static int mz0380_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_show, NULL);
}

static int mz0380_proc_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_state_show, NULL);
}

static int mz0380_proc_snapshot_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_snapshot_show, NULL);
}

static int mz0380_proc_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_control_show, NULL);
}

static int mz0380_proc_experiment_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_experiment_show, NULL);
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 0, 0)
static struct file_operations mz0380_proc_fops = {
	.open = mz0380_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_state_fops = {
	.open = mz0380_proc_state_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_snapshot_fops = {
	.open = mz0380_proc_snapshot_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_control_fops = {
	.open = mz0380_proc_control_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_experiment_fops = {
	.open = mz0380_proc_experiment_open,
	.read = seq_read,
	.write = mz0380_proc_experiment_write,
	.llseek = seq_lseek,
	.release = single_release,
};
#else
static struct proc_ops mz0380_proc_fops = {
	.proc_open = mz0380_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_state_fops = {
	.proc_open = mz0380_proc_state_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_snapshot_fops = {
	.proc_open = mz0380_proc_snapshot_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_control_fops = {
	.proc_open = mz0380_proc_control_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_experiment_fops = {
	.proc_open = mz0380_proc_experiment_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_experiment_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#endif

static void mz0380_proc_remove(void)
{
	remove_proc_entry("mz0380-experiment", NULL);
	remove_proc_entry("mz0380-control", NULL);
	remove_proc_entry("mz0380-snapshot", NULL);
	remove_proc_entry("mz0380-state", NULL);
	remove_proc_entry("mz0380", NULL);
}

static int mz0380_proc_create(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create("mz0380", 0444, NULL, &mz0380_proc_fops);
	if (!pe)
		return -ENOMEM;

	pe = proc_create("mz0380-state", 0444, NULL, &mz0380_proc_state_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-snapshot", 0444, NULL,
			 &mz0380_proc_snapshot_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-control", 0444, NULL,
			 &mz0380_proc_control_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-experiment", 0644, NULL,
			 &mz0380_proc_experiment_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	return 0;
}

/*
 * Mailbox command send.
 *
 * Protocol (inferred from the on-card ep.ko sysfs surface):
 *   1. host writes parameter words 0..nparams-1 into PARAM(i)
 *   2. host writes opcode into COMMAND
 *   3. host waits on cmd_wait until ISR sets cmd_complete on
 *      MZ0380_IRQ_CMD_COMPLETE, with timeout
 *   4. host reads STATUS and optionally PARAM(i) for return values
 *
 * Until the actual command/status registers are confirmed by .sys
 * disasm or runtime correlation, this function still pokes the
 * mailbox but treats absence of an IRQ-driven completion as a
 * timeout (NOT a hard error) so caller can decide.
 *
 * Caller must serialise via dev->cmd_lock.
 */
/*
 * Ack/rearm one card event, exact write order of the Windows event
 * thread (FUN_140284380): BAR5 flag first, then the BAR0 event word,
 * then the 0x400 doorbell. Also deasserts INTx.
 */
/*
 * M4 diagnostic: empirically locate the post-boot mailbox layout.
 *
 * ep.ko (card side) allocates a fresh dma_coherent(0x60) command buffer on boot
 * and maps host BAR0 to it via the inbound window based at dma_handle+4, so the
 * post-boot opcode/STATUS offsets differ from the bootloader's (which we use).
 * Rather than guess the offset, sweep candidate (opcode_off, doorbell_off)
 * layouts: for each, issue GET_BOARD_VERSION and see whether any word in the
 * 0x60 mailbox region changes away from the 0xdddddddd poison / our own writes.
 * Read-modify only within the 0x60 mailbox aperture - safe.
 */
void mz0380_mailbox_scan(struct mz0380_dev *dev)
{
	static const u32 op_off[]   = { 0x00, 0x04 };
	static const u32 bell_off[] = { 0x00, 0x04 };
	unsigned int oi, bi, w;

	pr_info("%s: mailbox scan: baseline region dump:\n", dev->name);
	for (w = 0; w < 0x60; w += 4)
		pr_info("%s:   base[0x%02x] = %08x\n", dev->name, w,
			mz_mmio_read(dev, w));

	for (oi = 0; oi < ARRAY_SIZE(op_off); oi++) {
		for (bi = 0; bi < ARRAY_SIZE(bell_off); bi++) {
			u32 o = op_off[oi], b = bell_off[bi];
			bool changed = false;

			/* clear the region we own, without touching the poison
			 * word until after, so we can spot a real card write */
			for (w = 0; w < 0x60; w += 4)
				if (w != MZ0380_MB_STATUS)
					mz_mmio_write(dev, w, 0);
			/* opcode GET_BOARD_VERSION at candidate offset */
			mz_mmio_write(dev, o, MZ0380_CMD_GET_BOARD_VERSION);
			wmb();
			mz_mmio_write(dev, b, MZ0380_MB_FIRE);
			msleep(50);

			for (w = 0; w < 0x60; w += 4) {
				u32 v = mz_mmio_read(dev, w);

				if (v != 0 && v != 0xdddddddd &&
				    v != MZ0380_CMD_GET_BOARD_VERSION) {
					pr_info("%s: scan op@0x%02x bell@0x%02x: base[0x%02x]=%08x (RESPONSE?)\n",
						dev->name, o, b, w, v);
					changed = true;
				}
			}
			if (!changed)
				pr_info("%s: scan op@0x%02x bell@0x%02x: no change\n",
					dev->name, o, b);
			mz0380_mb_ack_event(dev);
		}
	}
	pr_info("%s: mailbox scan done\n", dev->name);
}
EXPORT_SYMBOL_GPL(mz0380_mailbox_scan);

void mz0380_mb_ack_event(struct mz0380_dev *dev)
{
	mz_cfg_write(dev, MZ0380_CFG_INT_FLAG, MZ0380_CFG_INT_ACK_VAL);
	mz_mmio_write(dev, MZ0380_MB_EVENT, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_INT_ACK);
}

int mz0380_send_command(struct mz0380_dev *dev, u32 opcode,
			const u32 *params, unsigned int nparams,
			u32 *status_out, unsigned int timeout_ms)
{
	unsigned int i;
	long left;
	u32 status;
	int ret = 0;

	/*
	 * RE-confirmed mailbox model (BAR0). SEND_COMMAND writes the opcode to
	 * the PARAM0 slot (BAR0+0x04), the arguments to the following slots,
	 * then fires the doorbell (BAR0+0x00 = 0x800). Completion is signalled
	 * either by MSI (ISR sets cmd_complete) or by MZ0380_MB_STATUS bit0.
	 * The opcode occupies one param slot, so nparams args plus the opcode
	 * must fit the tracked slot count.
	 */
	if (nparams + 1 > MZ0380_REG_PARAM_MAX)
		return -EINVAL;

	mutex_lock(&dev->cmd_lock);

	/* drain a stale unacked event (e.g. from a previous module life) */
	{
		u32 stale = mz_mmio_read(dev, MZ0380_MB_EVENT);

		if (stale || mz_cfg_read(dev, MZ0380_CFG_INT_FLAG) == 1) {
			mz0380_mb_ack_event(dev);
			pr_info("%s: drained stale EVENT=0x%08x before command 0x%x\n",
				dev->name, stale, opcode);
		}
	}

	/* opcode -> PARAM0 (BAR0+0x04), args -> PARAM1.. (BAR0+0x08..) */
	mz_mmio_write(dev, MZ0380_MB_OPCODE, opcode);
	for (i = 0; i < nparams; i++)
		mz_mmio_write(dev, MZ0380_MB_PARAM(i + 1), params[i]);

	dev->cmd_complete = false;
	smp_wmb();

	/*
	 * Clear the status latch - but never stomp the firmware's stamp
	 * values (Windows only clears STATUS in its poll path; the IRQ path
	 * used for INIT/fw-download leaves it alone entirely).
	 */
	status = mz_mmio_read(dev, MZ0380_MB_STATUS);
	if (status != MZ0380_MB_STATUS_BOOT_STAMP &&
	    status != MZ0380_MB_STATUS_OK_STAMP)
		mz_mmio_write(dev, MZ0380_MB_STATUS, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_FIRE);

	/*
	 * timeout_ms == 0 means fire-and-forget (Windows SEND_COMMAND does
	 * the same): commands like COMMIT_FW reboot the card and never post
	 * a completion, so there is nothing to wait for here.
	 */
	if (timeout_ms == 0)
		goto out;

	if (dev->msi_enabled) {
		left = wait_event_interruptible_timeout(dev->cmd_wait,
			dev->cmd_complete,
			msecs_to_jiffies(timeout_ms));
		if (left == 0) {
			pr_warn("%s: command 0x%x timed out after %u ms\n",
				dev->name, opcode, timeout_ms);
			ret = -ETIMEDOUT;
			goto out;
		}
		if (left < 0) {
			ret = (int)left;
			goto out;
		}
		status = dev->cmd_last_status;
	} else {
		/*
		 * No MSI yet: emulate the Windows event thread
		 * (FUN_140284380). Completion is signalled either by
		 * MZ0380_MB_STATUS bit0 (short commands, polled by
		 * MZ0380_SEND_COMMAND itself) or by MZ0380_MB_EVENT bit11
		 * (command-complete event, normally semaphore-signalled via
		 * the interrupt). Every event must be acked with
		 * mz0380_mb_ack_event() or the card keeps INTx asserted and
		 * eventually stops signalling.
		 */
		unsigned int waited = 0;
		unsigned int max_wait = max(timeout_ms,
					    (unsigned int)MZ0380_MB_POLL_ITERS);
		bool done = false;
		u32 event;

		/*
		 * Poll QUIETLY: check STATUS and EVENT, and ack ONLY when a
		 * real event is pending. The earlier "ack every tick" scheme
		 * fired the 0x400 doorbell every 1 ms, which aborts a
		 * STATUS-completing command (0x01/0x0a) before the card
		 * finishes it - the mailbox scan proved a silent wait
		 * completes reliably where the ack-storm did not.
		 */
		do {
			status = mz_mmio_read(dev, MZ0380_MB_STATUS);
			/*
			 * Completion = bit0, or the firmware's 0xaaaaaaaa
			 * success stamp (GET_BOARD_VERSION/INIT). The
			 * 0xdddddddd boot stamp also has bit0 set but is NOT a
			 * completion.
			 */
			if (status == MZ0380_MB_STATUS_OK_STAMP ||
			    ((status & MZ0380_MB_STATUS_DONE) &&
			     status != MZ0380_MB_STATUS_BOOT_STAMP)) {
				done = true;
				break;
			}
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event) {
				bool cmd_done = event & MZ0380_MB_EVENT_CMD_DONE;

				mz0380_mb_ack_event(dev);
				pr_info("%s: EVENT=0x%08x during command 0x%x (%s), acked\n",
					dev->name, event, opcode,
					cmd_done ? "cmd-done" : "other");
				if (cmd_done) {
					done = true;
					break;
				}
			}
			usleep_range(900, 1100);
			waited += 1;
		} while (waited < max_wait);

		if (!done) {
			ret = -ETIMEDOUT;
			goto out;
		}

		/*
		 * A command that completed via STATUS bit0 may still post a
		 * trailing completion event a moment later; give it a few ms
		 * and ack it so the card is not left with INTx asserted.
		 */
		for (waited = 0; waited < 10; waited++) {
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event ||
			    mz_cfg_read(dev, MZ0380_CFG_INT_FLAG) == 1) {
				mz0380_mb_ack_event(dev);
				break;
			}
			usleep_range(300, 500);
		}

		/* capture the result/param slots for the caller */
		for (i = 0; i < MZ0380_REG_PARAM_MAX; i++)
			dev->cmd_last_param[i] =
				mz_mmio_read(dev, MZ0380_MB_PARAM(i));
	}

	if (status_out)
		*status_out = status;

out:
	mutex_unlock(&dev->cmd_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_send_command);

/*
 * Post-boot handshake (Windows FUN_140278bb0): program the BAR5 notify
 * pointers with the physical BAR0 mailbox addresses, ack, then send
 * CMD_INIT until the firmware answers. Follow up with GET_BOARD_VERSION
 * (success stamp 0xaaaaaaaa) whose result in PARAM 0x08/0x0c is the
 * RUNNING firmware version. The mailbox ignores most opcodes until this
 * dance is done.
 */
int mz0380_card_init(struct mz0380_dev *dev)
{
	resource_size_t bar0 = pci_resource_start(dev->pci, 0);
	unsigned int attempt;
	u32 status = 0;
	int ret = -ETIMEDOUT;

	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR0, lower_32_bits(bar0) + 0x04);
	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR1, lower_32_bits(bar0) + 0x5f);
	wmb();
	mz0380_mb_ack_event(dev);

	for (attempt = 0; attempt < 10 && ret; attempt++)
		ret = mz0380_send_command(dev, MZ0380_CMD_INIT, NULL, 0,
					  &status, 600);
	if (ret) {
		pr_warn("%s: CMD_INIT got no answer (%d), STATUS=%08x EVENT=%08x RESULT=%08x bar5[dc]=%08x bar5[30]=%08x bar5[38]=%08x\n",
			dev->name, ret,
			mz_mmio_read(dev, MZ0380_MB_STATUS),
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			mz_mmio_read(dev, MZ0380_MB_RESULT),
			mz_cfg_read(dev, MZ0380_CFG_INT_FLAG),
			mz_cfg_read(dev, MZ0380_CFG_NOTIFY_PTR0),
			mz_cfg_read(dev, MZ0380_CFG_NOTIFY_PTR1));
		if (mz0380_dma_handshake)
			mz0380_mailbox_scan(dev);
		return ret;
	}
	pr_info("%s: CMD_INIT answered on attempt %u (status=0x%08x)\n",
		dev->name, attempt, status);

	{
		u32 params[2] = { 0, 0 };

		ret = -ETIMEDOUT;
		for (attempt = 0; attempt < 10 && ret; attempt++)
			ret = mz0380_send_command(dev,
						  MZ0380_CMD_GET_BOARD_VERSION,
						  params, 2, &status, 5000);
	}
	if (ret) {
		pr_warn("%s: GET_BOARD_VERSION got no answer (%d)\n",
			dev->name, ret);
		return ret;
	}

	msleep(100);
	dev->fw_version_major = mz_mmio_read(dev, MZ0380_MB_PARAM(1));
	dev->fw_version_minor = mz_mmio_read(dev, MZ0380_MB_PARAM(2));
	pr_info("%s: board reports running firmware %u.%u (status=0x%08x)\n",
		dev->name, dev->fw_version_major, dev->fw_version_minor,
		status);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_card_init);

/*
 * Peripheral register file access via mailbox opcodes 0x1a/0x1b
 * (Windows FUN_1402777e4 / FUN_1402851cc). Read results land in the
 * PARAM3 slot (BAR0+0x10). Requires booted firmware.
 */
int mz0380_periph_read(struct mz0380_dev *dev, u8 chip, u8 reg, u32 *val)
{
	u32 params[3] = { chip, reg, 0 };
	int ret;

	ret = mz0380_send_command(dev, MZ0380_CMD_REG_READ, params, 3,
				  NULL, 1000);
	if (ret) {
		pr_info("%s: REG_READ chip=0x%02x reg=0x%02x failed (%d), STATUS=%08x EVENT=%08x RESULT=%08x P3=%08x\n",
			dev->name, chip, reg, ret,
			mz_mmio_read(dev, MZ0380_MB_STATUS),
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			mz_mmio_read(dev, MZ0380_MB_RESULT),
			mz_mmio_read(dev, MZ0380_MB_PARAM(3)));
		return ret;
	}
	if (val)
		*val = dev->cmd_last_param[3];
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_periph_read);

int mz0380_periph_write(struct mz0380_dev *dev, u8 chip, u8 reg, u32 val)
{
	u32 params[3] = { chip, reg, val };

	return mz0380_send_command(dev, MZ0380_CMD_REG_WRITE, params, 3,
				   NULL, 1000);
}
EXPORT_SYMBOL_GPL(mz0380_periph_write);

static int mz0380_initdev(struct pci_dev *pci_dev,
			  const struct pci_device_id *pci_id)
{
	struct mz0380_dev *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pci = pci_dev;

	err = pci_enable_device(pci_dev);
	if (err) {
		kfree(dev);
		return err;
	}

	pci_clear_master(pci_dev);
	/*
	 * We service the card by polling (like the Windows event thread);
	 * no IRQ handler is registered, so mask INTx at the PCI level.
	 * A pending assert on the (shared) line otherwise spins the kernel
	 * in unclaimed-interrupt handling between event and ack.
	 */
	pci_intx(pci_dev, 0);
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER, &dev->pci_lat);

	/*
	 * M4 diagnostic: bus mastering must be live BEFORE the firmware
	 * handshake (which happens inside mz0380_firmware_load below), not
	 * after. Enable it here so the CMD_INIT doorbell is issued with bus
	 * mastering on. No ring addresses are programmed (still unverified),
	 * so the card has no host DMA target - safe.
	 */
	if (mz0380_dma_handshake) {
		pci_set_master(pci_dev);
		pr_info("%s: dma_handshake: bus mastering enabled pre-firmware\n",
			pci_name(pci_dev));
	}

	printk(KERN_INFO
	       "mz0380 device found at %s, rev: %u, irq: %u, latency: %u\n",
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq, dev->pci_lat);
	printk(KERN_INFO "%s: probe mode: bus master initially disabled\n",
	       pci_name(pci_dev));

	err = mz0380_dev_setup(dev);
	if (err < 0)
		goto fail_disable;

	/*
	 * Phase 1: firmware load. request_firmware succeeds regardless
	 * of upload; the actual upload is gated by firmware_upload=1
	 * because the mailbox protocol still has CHECKME offsets.
	 */
	err = mz0380_firmware_load(dev);
	if (err) {
		pr_warn("%s: firmware load skipped/failed (%d) - continuing without it\n",
			dev->name, err);
		/* not fatal during bring-up */
	}

	/*
	 * Phase 2: request IRQ + alloc DMA rings, gated by enable_dma.
	 * Bus mastering is enabled only inside mz0380_dma_setup, AFTER
	 * the ring base addresses have been programmed.
	 */
	if (mz0380_enable_dma) {
		err = mz0380_irq_request(dev);
		if (err) {
			pr_warn("%s: IRQ request failed (%d) - continuing without DMA\n",
				dev->name, err);
		} else {
			err = mz0380_dma_setup(dev);
			if (err) {
				pr_warn("%s: DMA setup failed (%d) - continuing without streaming\n",
					dev->name, err);
				mz0380_irq_release(dev);
			}
		}
	} else if (allow_bus_master) {
		/* legacy escape hatch: bus-master without DMA setup */
		pci_set_master(pci_dev);
	}

	err = mz0380_card_setup(dev);
	if (err < 0)
		goto fail_dma;

	pci_set_drvdata(pci_dev, dev);

	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &mz0380_devlist);
	mutex_unlock(&devlist);

	dprintk(1, "probe complete\n");
	return 0;

fail_dma:
	mz0380_dma_teardown(dev);
	mz0380_irq_release(dev);
	mz0380_firmware_release(dev);
	mz0380_dev_unregister(dev);
fail_disable:
	pci_disable_device(pci_dev);
	kfree(dev);
	return err;
}

static void mz0380_finidev(struct pci_dev *pci_dev)
{
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return;

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	mz0380_dma_stop(dev);
	mz0380_dev_unregister(dev);
	mz0380_dma_teardown(dev);
	mz0380_irq_release(dev);
	mz0380_firmware_release(dev);
	pci_clear_master(pci_dev);
	pci_disable_device(pci_dev);
	kfree(dev);
}

static const struct pci_device_id mz0380_pci_tbl[] = {
	/* Rev. 1 (original)                       1cfa:0003 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0003 },
	/* Rev. 2 (never released)                 1cfa:0005 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0005 },
	/* Rev. 1 + Ryzen fix                      1cfa:0006 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0006 },
	/* Rev. 3 (device id 0x0381)               1cfa:0010 */
	{ .vendor = 0x12ab, .device = 0x0381,
	  .subvendor = 0x1cfa, .subdevice = 0x0010 },
	/* Prototype                               12ab:05cf */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x12ab, .subdevice = 0x05cf },
	{ }
};
MODULE_DEVICE_TABLE(pci, mz0380_pci_tbl);
MODULE_FIRMWARE("mz0380/MZ0380.HD.HEX");
MODULE_FIRMWARE("mz0380/MZ0381.HD.HEX");

static struct pci_driver mz0380_pci_driver = {
	.name = "mz0380",
	.id_table = mz0380_pci_tbl,
	.probe = mz0380_initdev,
	.remove = mz0380_finidev,
};

static int __init mz0380_init(void)
{
	int ret;

	printk(KERN_INFO "mz0380 driver version %d.%d.%d loaded\n",
	       (MZ0380_VERSION_CODE >> 16) & 0xff,
	       (MZ0380_VERSION_CODE >> 8) & 0xff,
	       MZ0380_VERSION_CODE & 0xff);

	ret = mz0380_proc_create();
	if (ret < 0)
		return ret;

	ret = pci_register_driver(&mz0380_pci_driver);
	if (ret < 0) {
		mz0380_proc_remove();
		return ret;
	}

	return 0;
}

static void __exit mz0380_fini(void)
{
	mz0380_proc_remove();
	pci_unregister_driver(&mz0380_pci_driver);
	printk(KERN_INFO "mz0380 driver unloaded\n");
}

module_init(mz0380_init);
module_exit(mz0380_fini);
