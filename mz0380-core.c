/*
 *  Driver for MZ0380 based capture cards.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380.h"

/*
 * BAR0+0x04 is command word 0 (the opcode), followed by at most ten
 * argument words.  Word 10 / BAR0+0x2c is deliberately shared with the
 * completion latch.  A command which uses that tenth argument therefore owns
 * the latch and must use EVENT-only completion instead of STATUS polling.
 * BAR0+0x30 is the EVENT word and is never command payload.
 */
#define MZ0380_MB_MAX_ARGS		10U
#define MZ0380_MB_COMMAND_WORDS		(MZ0380_MB_MAX_ARGS + 1U)

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

static unsigned int scan_bar = MZ0380_MAP_BAR_MMIO;
module_param(scan_bar, uint, 0644);
MODULE_PARM_DESC(scan_bar,
		 "/proc/mz0380-scan target BAR: 0=BAR0/MMIO (default, HDMI signal window), 1=BAR5/CFG");

static unsigned int scan_start;
module_param(scan_start, uint, 0644);
MODULE_PARM_DESC(scan_start,
		 "/proc/mz0380-scan first byte offset (4-byte aligned); default 0x0000");

static unsigned int scan_len = 0x48;
module_param(scan_len, uint, 0644);
MODULE_PARM_DESC(scan_len,
		 "/proc/mz0380-scan window length in bytes; default 0x48 covers the sc0710-style HDMI status regs (0x00a8..0x00e4)");

static bool scan_unsafe;
module_param(scan_unsafe, bool, 0644);
MODULE_PARM_DESC(scan_unsafe,
		 "/proc/mz0380-scan: read ALL offsets in the window, not just proven-safe BAR0 ranges. DANGER: an un-backed post-boot BAR0 offset stalls the CPU on readl until the PCIe completion timeout (looks like a hard hang). Each read is logged to dmesg first so a stall's culprit offset is recoverable. Default false");

static unsigned int periph_chip = MZ0380_CHIP_BRIDGE;
module_param(periph_chip, uint, 0644);
MODULE_PARM_DESC(periph_chip,
		 "/proc/mz0380-periph-scan target chip id: 0x90=bridge/FPGA reg file (default, HDMI front-end), 0xb8=TVP5160 analog");

static unsigned int periph_start;
module_param(periph_start, uint, 0644);
MODULE_PARM_DESC(periph_start,
		 "/proc/mz0380-periph-scan first peripheral register index; default 0x00");

static unsigned int periph_count = 0x40;
module_param(periph_count, uint, 0644);
MODULE_PARM_DESC(periph_count,
		 "/proc/mz0380-periph-scan number of registers to read via REG_READ (0x1a); default 0x40");

static bool periph_probe;
module_param(periph_probe, bool, 0644);
MODULE_PARM_DESC(periph_probe,
		 "/proc/mz0380-periph-scan diagnostic: for the first few registers, issue REG_READ and dump EVERY mailbox return slot (STATUS/EVENT/PARAM/payload) so the slot that actually carries the read-back value can be located. Default false");

static unsigned int event_sample_us = 200;
module_param(event_sample_us, uint, 0644);
MODULE_PARM_DESC(event_sample_us,
		 "/proc/mz0380-events watcher: EVENT-word poll interval in microseconds; default 200");

static bool event_auto_ack = true;
module_param(event_auto_ack, bool, 0644);
MODULE_PARM_DESC(event_auto_ack,
		 "/proc/mz0380-events watcher: ack each recorded event (rearm the card for the next edge). Turn off to capture a single sticky event without acking. Default true");

/*
 * M35. store_channel_done writes the token words (BAR0+0x40/44/48/4c, and the
 * encoder status at 0x50) UNGATED, while EVENT[0x30]+MSI sit behind the
 * one-shot msi_enable credit that only our ack doorbell re-arms. So the token
 * words are the honest witness of card-side frame completion: the watcher
 * thread logs every change it sees in them (only meaningful while the watcher
 * runs - "echo start > /proc/mz0380-events").
 *
 * credit_kick_ms > 0 additionally fires the full ack sequence (BAR5[0xdc]=2,
 * EVENT=0, doorbell 0x400 -> card pciep_isr_clrint -> msi_enable=1) on that
 * period, re-arming the credit even when no MSI ever arrives. If frame tokens
 * only start moving (or MSIs appear) once this is on, the blocker was a dead
 * credit, not a parked encoder.
 */
static unsigned int credit_kick_ms;
module_param(credit_kick_ms, uint, 0644);
MODULE_PARM_DESC(credit_kick_ms,
		 "M35 watcher: period in ms for unconditional ack/credit-re-arm kicks (BAR5[0xdc]=2, EVENT=0, doorbell 0x400); 0 = off (default). Live-tunable via /sys/module/mz0380/parameters/");

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
 *   enable_dma=1        alloc rings, enable bus master, request MSI
 *   enable_audio=1      register an ALSA snd_card alongside V4L2
 *
 * There is no firmware_upload parameter and there must never be one again:
 * the card boots its own flash image and mz0380-fw.c only handshakes with it.
 * The old inert compatibility no-op was removed along with the scripts that
 * passed it.
 */
/*
 * GPIO pin 8, the companion reset/power strap. mz0380_mst3367_reset() has
 * always finished by driving it LOW and leaving it there.
 *
 * M109: pin 9 next door is the MST3367 reset and is documented ACTIVE-LOW - we
 * release it to 1. If pin 8 follows the same convention then driving it 0 holds
 * the companion device in reset for the entire session, which is a complete
 * explanation for the card's HDMI passthrough output being dead - no picture at
 * all on a monitor plugged into the card, not even a no-signal message.
 *
 * Nobody chose 0 deliberately; it came from the original bring-up sequence and
 * has never been varied. 0 and 1 are both real levels, so MZ0380_RX_STRAP_LEAVE
 * is the third value meaning "do not drive pin 8 at all".
 */
/*
 * M111: deliver frames the card has already written, without waiting for a
 * completion event that has never once fired.
 *
 * The real capture path delivers only from the MSI-driven drain. Every run this
 * project has ever made reports frame_events=0 and EVENT[0x30]=0 - the card
 * DMAs a complete 1920x1080 4:2:0 frame into buf0 (760 contiguous pages ending
 * at 0x2f7000 == 1920*1080*3/2) and then nothing tells the host it is there, so
 * vb2 is never fed and userspace reads 0 bytes. The nosg path was written as a
 * poller for exactly this reason (M41); the real path never got the same
 * treatment.
 *
 * Nonzero starts a kthread that scans the stream buffers on this interval and
 * hands any completed frame to the ORIGINAL drain path
 * (mz0380_drain_frame_snapshot), which infers the length from the poison
 * suffix, copies into vb2 and re-poisons - so a frame is delivered once and
 * only once, and the event-driven path stays byte-for-byte unchanged.
 *
 * Default 0 (off) so it bisects cleanly against every earlier result.
 */
/*
 * M118: fire the card's credit re-arm after a poll-drained frame.
 *
 * The completion path's ISR ends with BAR5[0xdc]=2, EVENT=0, doorbell 0x400 -
 * which drives the card's pciep_isr_clrint and sets msi_enable=1 again. That
 * one-shot credit is what lets the card raise the NEXT completion. It has never
 * run in this project, because it lives in an interrupt handler for an event
 * that never fires.
 *
 * The poll-drain acks enc_stat (M117) but that changed nothing, so the slot
 * handshake is not the gate. This is the other half of what a real completion
 * would have done, and it is the last piece of the ISR the poll path does not
 * reproduce.
 */
/*
 * M119: re-notify the on-card encoder, once per frame.
 *
 * ep.ko's op 0x06 handler (@0x1854, M22) does ONE thing: sysfs_notify() on
 * /sys/vpl_pciep/epint. tinyvenc5 blocks on that node, wakes, DMAs a frame, and
 * blocks again. So one op6 buys exactly one frame - which is precisely the
 * cadence measured all session, and why the nosg path respawns the encoder per
 * frame (M39) and burns the card's spawn budget doing it.
 *
 * op6 is fire-and-forget (no mailbox completion, M22) and does NOT fork an
 * encoder - SET_VIC does that. Re-sending it is therefore cheap and costs no
 * spawn budget. Every host-side ack has now been eliminated as the cadence gate
 * (op8/wency_ready M93+M116, enc_stat M117, completion credit M118); this is the
 * card-side wake-up those acks were standing in for.
 */
unsigned int mz0380_op6_kick_ms;
module_param_named(op6_kick_ms, mz0380_op6_kick_ms, uint, 0644);
MODULE_PARM_DESC(op6_kick_ms,
		 "M120: after each poll-drained frame, re-fire START_STREAMING (op 0x06) to re-notify /sys/vpl_pciep/epint and ask for the next one - tinyvenc5 delivers one frame per notify. Value is the MINIMUM spacing in ms, not a period; kicks never fire before the first frame (M119 fired free-running from stream start and got zero frames plus a lost lock) (def:0 = off; try 16)");

/*
 * M126: WHICH opcode the post-frame kick sends. M125 decoded ep.ko's
 * pciep_isr dispatcher:
 *
 *   op 0x06        notifies audio_ctrl, THEN epint / epint_1080p
 *   op 0x09, 0x2f  notify the same epint node, WITHOUT the audio_ctrl notify
 *
 * tinyvenc5 blocks on /sys/vpl_pciep/epint and delivers one frame per notify.
 * op 0x06 is "start", and re-sending start per frame drags the audio control
 * path along with it; 0x2f is the bare wake-up and is the natural per-frame
 * kick. The plumbing already exists - op6_kick_ms fires after each delivered
 * frame (M120) - so only this opcode changes.
 *
 * Default stays 0x06 so op6_kick_ms means exactly what it did before.
 */
unsigned int mz0380_kick_opcode = 0x06;
module_param_named(kick_opcode, mz0380_kick_opcode, uint, 0644);
MODULE_PARM_DESC(kick_opcode,
		 "M126: opcode fired by op6_kick_ms after each delivered frame - 0x06 (def, START, also notifies audio_ctrl) or 0x2f / 0x09 (bare epint wake-up)");

/*
 * M139: sentinel written into BAR0 0x40/0x44/0x48/0x4c just before
 * START_STREAMING so the producer watch can tell "the card reported one frame
 * from buffer 1" (nibble goes to 0, upper bits survive) from "the card never
 * reported at all" (sentinel intact).  Nothing card-side reads these
 * registers; store_channel_done() only read-modify-writes one nibble per
 * channel into them.  Set 0 to leave them alone.
 */
/*
 * M139: SET_VIC bytes 8..9 / 10..11 overrides.
 *
 * These are the bytes the card's cfg patcher (video_capture_mgr 0xa290)
 * rewrites every 1920- and 1080-valued cfg line with, which is where
 * EncodingGroup::Start reads m_vic_width from - and m_vic_width is the ONE
 * value libtkmf_video_source's img_handler compares the VIC's own measured
 * width against before it will publish a frame (M138).  A mismatch drops
 * every frame silently, which is exactly the state M139 measured.
 *
 * They have never been settable independently of the v4l2 capture geometry.
 * The point of splitting them out is the 8-bit double-rate reading: BANK0
 * 0xb0 = 0x21 is embedded-sync 8-bit BT1120, so a 1920-wide picture crosses
 * the bus as 3840 8-bit samples, and the receiver's own detect flaps between
 * hper=674 and hper=337 - a factor of two.  If the VIC measures 3840, 1920
 * can never match.
 *
 * Pair 3840 with height 540 to keep w*h*3/2 at 3110400 bytes: the cfg patcher
 * rewrites both, so the frame stays exactly the size of the existing 4 MiB
 * buffer and mz0380_infer_frame_length()'s hardcoded want. Changing width
 * alone would make the card write 6220800 bytes into a 4 MiB buffer, which is
 * the overrun that caused every IOMMU fault from M26 to M29.
 */
unsigned int mz0380_vic_out_w;
module_param_named(vic_out_w, mz0380_vic_out_w, uint, 0644);
MODULE_PARM_DESC(vic_out_w,
		 "M139: SET_VIC bytes 8..9 override - the width img_handler's frame gate compares the VIC's measurement against. 0 = use the v4l2 capture width (def:0). Pair 3840 with vic_out_h=540 to keep the frame 3110400 bytes.");

unsigned int mz0380_vic_out_h;
module_param_named(vic_out_h, mz0380_vic_out_h, uint, 0644);
MODULE_PARM_DESC(vic_out_h,
		 "M139: SET_VIC bytes 10..11 override. 0 = use the v4l2 capture height (def:0)");

unsigned int mz0380_token_seed = 0xa5a5a5a5;
module_param_named(token_seed, mz0380_token_seed, uint, 0644);
MODULE_PARM_DESC(token_seed,
		 "M139: sentinel seeded into the BAR0 frame-token registers before START so the producer watch can distinguish 'reported buffer 1' from 'never reported' (0 = do not seed, def:0xa5a5a5a5)");

/*
 * M126: with the kick confined to the post-delivery branch, a one-frame stream
 * fires exactly one kick - which is what the first 0x2f run measured, and it
 * cannot distinguish "the card ignored the wake-up" from "we only asked once".
 * With this set, the kick repeats every op6_kick_ms once at least one frame has
 * been delivered. Still never before the first frame: that is the M119/M120
 * race that cost a lock and a run.
 */
bool mz0380_kick_repeat;
module_param_named(kick_repeat, mz0380_kick_repeat, bool, 0644);
MODULE_PARM_DESC(kick_repeat,
		 "M126: after the FIRST delivered frame, repeat the kick every op6_kick_ms even with no new delivery (def:0)");

bool mz0380_poll_drain_credit;
module_param_named(poll_drain_credit, mz0380_poll_drain_credit, bool, 0644);
MODULE_PARM_DESC(poll_drain_credit,
		 "M118: after a poll-drained frame, fire the credit re-arm the completion ISR would have (BAR5[0xdc]=2, EVENT=0, doorbell 0x400) (def:0)");

unsigned int mz0380_poll_drain_ms;
module_param_named(poll_drain_ms, mz0380_poll_drain_ms, uint, 0644);
MODULE_PARM_DESC(poll_drain_ms,
		 "M111: poll the stream buffers every N ms and deliver any frame the card has already written, instead of waiting for a completion event that never arrives (def:0 = off; 20 is a reasonable value)");

unsigned int mz0380_rx_strap;
module_param_named(rx_strap, mz0380_rx_strap, uint, 0644);
MODULE_PARM_DESC(rx_strap,
		 "M109: GPIO8 companion reset/power strap level after the receiver reset (def:0, the level this driver has always driven; 1 = release; 0xffffffff = do not drive it at all)");

bool mz0380_enable_dma;
module_param_named(enable_dma, mz0380_enable_dma, bool, 0444);
MODULE_PARM_DESC(enable_dma,
		 "allocate ring buffers, request MSI, enable bus mastering; off by default");

/*
 * Gap between SET_VIC(0x29) and START_STREAMING(op 0x06). SET_VIC makes the
 * card's video_capture_mgr system()-fork tinyvenc5; op6 only kicks that
 * encoder once it has exec'd, opened /sys/vpl_pciep/epint and consumed the
 * SET_VIC command first (M22). Fire op6 too early and it either misses
 * tinyvenc5's poll or clobbers the SET_VIC it expects on its first read - the
 * encoder then sits idle (symptom: IRQ 164 stuck at 3, 0-byte capture).
 * Writable at runtime so the spawn window can be swept without a reload.
 */
unsigned int mz0380_start_delay_ms = 2000;
module_param_named(start_delay_ms, mz0380_start_delay_ms, uint, 0644);
MODULE_PARM_DESC(start_delay_ms,
		 "ms to wait after SET_VIC before firing START_STREAMING(op 0x06) so the card can fork+exec tinyvenc5 and read SET_VIC first (def:2000; too short = encoder idle at 3 IRQs)");

/*
 * Diagnostic bisection lever. SET_VIC byte[31]=is_nosg: when set, the card's
 * tinyvenc encoder spawns fake_frame_process, a black/color test-pattern
 * generator that pwrites channel_done on a timer with NO capture/SSM/BT1120
 * dependency (RE_FINDINGS.md M23). stream_nosg=1 => frames flowing proves the
 * whole START -> channel_done -> MSI -> outbound-ATU -> host-DMA path; frames
 * still absent then points downstream (channels[] host target unset). Off by
 * default so real capture is used. Writable at runtime.
 */
bool mz0380_stream_nosg;
module_param_named(stream_nosg, mz0380_stream_nosg, bool, 0644);
MODULE_PARM_DESC(stream_nosg,
		 "SET_VIC is_nosg flag: 1 = force the card's fake-frame (test-pattern) generator, bypassing real BT1120 capture; diagnostic bisection lever (def:0)");

/*
 * How long the stream_nosg polling capture waits for the raw frame's
 * contiguous burst to finish landing in buf0 before it gives up on this
 * spawn and respawns. Hardware lands the whole 0x30a5c0-byte frame within
 * 450 ms of START (M36), so 3 s is generous slack, not a tuned value.
 */
unsigned int mz0380_nosg_frame_timeout_ms = 3000;
module_param_named(nosg_frame_timeout_ms, mz0380_nosg_frame_timeout_ms,
		   uint, 0644);
MODULE_PARM_DESC(nosg_frame_timeout_ms,
		 "stream_nosg: ms to wait for the raw fake frame to land in buf0 before respawning the encoder (def:3000; hw lands in ~450)");

/*
 * M25 probe. op2 hands the card 8-byte channels[] slots; pcie_set_outbound
 * (ep.ko 0x5a8) then does ELBI[0x58] = slot word0, ELBI[0x54] = slot word1.
 * Default packing is {word0=high32, word1=low32}, which is what the firmware
 * intends (0x58=high, 0x54=low). M24 showed the resulting DMA target is
 * (word0 << 32) + aperture_offset with the low half always 0, i.e. word1 never
 * reaches the hardware. Setting this swaps the pair to {word0=low32,
 * word1=high32} so the fault address tells us which reading is true:
 *   fault at (low32 << 32)  -> only word0 lands; ELBI 0x54 is ignored by the
 *                              HW (the real outbound low target is elsewhere,
 *                              likely 0x84) -> take the 4GB-aligned-IOVA route.
 *   fault at the correct 64-bit address, or frames land -> both words land and
 *                              the earlier low32 loss was ours.
 * Diagnostic only; leave off for normal operation. Writable at runtime.
 */
bool mz0380_buf_pair_swap;
module_param_named(buf_pair_swap, mz0380_buf_pair_swap, bool, 0644);
MODULE_PARM_DESC(buf_pair_swap,
		 "M25 probe: swap the op2 channels[] slot word order to {low32, high32} (def:0 = {high32, low32})");

/*
 * M26. The M25 probe proved the card's outbound window takes only the HIGH
 * 32 bits of the host target: host_addr == (slot word0 << 32) + aperture
 * offset, with the low half (ELBI 0x54) dropped on the floor by the hardware.
 * The only way to make that address land in one of our buffers is to put the
 * buffer at an IOVA whose low 32 bits are zero, i.e. 4 GiB-aligned. So we
 * allocate the stream buffers as plain pages and iommu_map() each one at
 * dma_iova_base + (i << 32), then hand the card word0 = IOVA >> 32, word1 = 0.
 *
 * Requires a translating IOMMU domain (AMD-Vi/Intel VT-d in DMA mode - not
 * iommu=pt / iommu=off). Base must be 4 GiB-aligned; it is chosen low and far
 * from where the DMA-API's IOVA allocator hands out addresses (near the top of
 * the domain aperture), and every mapping is checked for a collision first.
 */
bool mz0380_dma_iova_remap = true;
module_param_named(dma_iova_remap, mz0380_dma_iova_remap, bool, 0644);
MODULE_PARM_DESC(dma_iova_remap,
		 "M26: place each stream buffer at its own 4GiB-aligned IOVA via iommu_map, so the card's high32-only outbound target lands in it (def:1)");

unsigned long long mz0380_dma_iova_base = 0x100000000ULL;
module_param_named(dma_iova_base, mz0380_dma_iova_base, ullong, 0644);
MODULE_PARM_DESC(dma_iova_base,
		 "M26: base IOVA for the remapped stream buffers, must be 4GiB-aligned; buffer i lands at base + (i << 32) (def:0x100000000)");

/*
 * M27. With the 4GiB-aligned IOVA in place the card's writes landed at
 * (word0 << 32) + 0x80000 - one SET_BUF stride past the window base, i.e.
 * host_addr = (word0 << 32) + bufindex * stride, with the card starting at
 * bufindex 2. Two runtime knobs to pin that down without a rebuild:
 *   set_buf_stride  - the stride word in SET_BUF (cmd[0x8]). If the offset is
 *                     bufindex*stride, 0 here should drag the writes down onto
 *                     the buffer base.
 *   dma_iova_offset - shifts every mapping up by this much, so the buffer sits
 *                     exactly where the card writes instead. Set it to the
 *                     observed offset (0x80000) if the stride knob does not
 *                     move the writes.
 * Exactly one of the two should be needed. Both are diagnostics until the hw
 * says which; the winner becomes the default.
 */
unsigned long long mz0380_dma_iova_offset;
module_param_named(dma_iova_offset, mz0380_dma_iova_offset, ullong, 0644);
MODULE_PARM_DESC(dma_iova_offset,
		 "M27: shift each remapped stream buffer up by this many bytes within its 4GiB slot, to meet the card's aperture offset (def:0)");

unsigned int mz0380_set_buf_stride = MZ0380_STREAM_BUF_STRIDE;
module_param_named(set_buf_stride, mz0380_set_buf_stride, uint, 0644);
MODULE_PARM_DESC(set_buf_stride,
		 "M27: the stride word sent in SET_BUF cmd[0x8]; the card appears to place buffer n at aperture offset n*stride (def:0x80000)");

/*
 * M29. Final model, and every hw data point fits it:
 *
 *     host_addr = ((word0 << 32) | word1) + aperture_offset
 *
 * where aperture_offset starts at 0 and the low half adds MOD 2^32 (no carry
 * into word0). The "0x80000 offset" chased in M27/M28 was not an offset at
 * all: it was the card running off the END of a 512 KiB buffer. Writes inside
 * the mapping succeed silently, so the first fault appears one buffer-size in,
 * which looked exactly like a constant offset. M28 advertised base while
 * mapping base+0x80000, left the advertised address unmapped, and faulted at
 * offset 0 - which is what exposed the mistake.
 *
 * So: advertise the buffer base as-is (offset 0), and make the buffers big
 * enough for whatever the card streams. This knob stays as a runtime escape
 * hatch, defaulting to 0.
 */
unsigned int mz0380_card_frame_offset;
module_param_named(card_frame_offset, mz0380_card_frame_offset, uint, 0644);
MODULE_PARM_DESC(card_frame_offset,
		 "M29: bytes the card adds to the advertised target before writing a frame; buffers are mapped this far above the address sent in SET_BUF. Measured 0 on hw (def:0)");

/*
 * M32. A raw frame lands in window0 buf0, but the card never signals
 * channel_done. ep.ko's delivery path is not the gate: command-done and
 * frame-done share one re-arm token (msi.constprop.1 / store_channel_done both
 * bail when it is 0, and pciep_isr_clrint sets it), and our command MSIs prove
 * that token cycles correctly. So the card's userspace is not declaring a
 * finished frame - most likely the encoder's bitstream destination is one of
 * the outbound windows we have never programmed (op2 only fills window0).
 * This sends the other windows' buffer-setter opcodes too, pointed at the three
 * buffers the card is currently ignoring, to find which window wakes up.
 */
/*
 * M92: Windows never sends 0x02 without 0x08 right behind it (0x14027b62d
 * then 0x14027b752, same channel, same size, four more address pairs).
 * ep.ko: op2 fills window0 slots 1..4 and clears host_ready; op8 fills
 * slots 5..8 and sets wency_ready = 8.
 */
/*
 * M94: write the receiver's output stage the way the Windows driver does -
 * 0xb0/0xae/0xad/0xb1/0xb2/0xb3/0xb4 as one block, not 0xb0 alone. See
 * mst3367_commit_digital_output(). Windows sends 0xb0 = 0x14, so pair this
 * with vic_b0=0x14.
 */
bool mz0380_mst_win_output;
module_param_named(mst_win_output, mz0380_mst_win_output, bool, 0644);
MODULE_PARM_DESC(mst_win_output,
		 "M94: write the full Windows MST3367 output-stage block (def:0; pair with vic_b0=0x14)");

/*
 * M94: BANK0 0xad. Windows computes it as (context_flag > 0) & 5, i.e. 0 or 1,
 * from a field we cannot identify from the host. Exposed so both can be tried.
 */
unsigned int mz0380_mst_ad;
module_param_named(mst_ad, mz0380_mst_ad, uint, 0644);
MODULE_PARM_DESC(mst_ad, "M94: MST3367 BANK0 0xad value - Windows sends 0 or 1 (def:0)");

bool mz0380_set_buf_op8;
module_param_named(set_buf_op8, mz0380_set_buf_op8, bool, 0644);
MODULE_PARM_DESC(set_buf_op8,
		 "M92: also send SET_BUF op 0x08 after op 0x02, as Windows always does (def:0)");

bool mz0380_probe_windows;
module_param_named(probe_windows, mz0380_probe_windows, bool, 0644);
MODULE_PARM_DESC(probe_windows,
		 "M32 probe: also program outbound windows 1-3 (op 0x04/0x05/0x03) pointing at stream bufs 1/2/3, to find where the encoder writes its bitstream (def:0)");

/*
 * M36. The fake frame's payload is a constant 0x11 fill followed by ZERO
 * padding, and the old buffer sampling only looked for non-zero bytes - so
 * neither repeated frames nor the true end of the card's write burst were
 * visible. Poisoning the buffers with 0xAA before START makes every
 * card-written byte (including zeros) detectable; a kthread in mz0380-dma.c
 * then tracks the write extent per buffer over time (crawl vs stall, exact
 * stop offset - M35 measured the visible stop at 0x30a000 = 1556 x 0x800
 * DMAC chunks, but zeros beyond it were invisible).
 */
/*
 * EDID command framing is confirmed by both sides of the shipped stack:
 * Windows sends opcode 0x1f with nine parameters and waits for STATUS; the
 * card handler interprets byte 5 as the EEPROM offset and writes the payload
 * in retrying <=8-byte I2C chunks.  Keep the knobs visible for diagnostics,
 * but use that known-good transaction by default.
 */
/*
 * M51b. Data-bit polarity of GPIO_DIR (op 0x17): 0 = data bit 1 means
 * OUTPUT (our reading), 1 = inverted. Unverified against the card, and a
 * wrong guess makes every bit-banged line look stuck low, so it is a
 * runtime switch rather than a constant.
 */
/*
 * M53. Candidate registers per bank for the indirect-port hunt. Cost is
 * quadratic (~20 ms per ordered pair over the mailbox), so 48 candidates is
 * ~45 s per bank; raise it if the hunt comes back empty and the writable
 * count printed per bank was clipped.
 */
unsigned int mz0380_edidhunt_max_regs = 48;
module_param_named(edidhunt_max_regs, mz0380_edidhunt_max_regs, uint, 0644);
MODULE_PARM_DESC(edidhunt_max_regs,
		 "M53: max writable registers per bank used as indirect address/data candidates (def:48; cost is quadratic)");

/*
 * M54. A real source walks in and out of lock while it settles (observed:
 * detect 0x83 -> 0xa3 locked -> 0x83 -> 0x03 inside a second), so a
 * single-shot detect read calls a transmitting source "no signal". How
 * long to sample before giving up.
 */
unsigned int mz0380_signal_poll_ms = 2000;
module_param_named(signal_poll_ms, mz0380_signal_poll_ms, uint, 0644);
MODULE_PARM_DESC(signal_poll_ms,
		 "M54: how long to sample the MST3367 detect register for a lock before reporting no signal (def:2000)");

/*
 * M54. Stream a locked-but-unmatched signal as 1080p60 when the geometry
 * says 1080p (htotal 2200). The first source that ever locked reports a
 * saturated vperiod counter, so the frame rate cannot be derived and no
 * table entry matches - without this the whole real-signal path stays
 * blocked on one unreadable register.
 */
/*
 * M113: arm the real capture path with no source connected at all.
 *
 * Windows streams the card's own "no signal" splash into OBS with nothing
 * plugged in, so a source is NOT required to get pixels out of this card - and
 * running without one removes the last uncontrolled variable from a test (the
 * operator power-cycling a source mid-window is what produced the M105
 * measurement artefact).
 *
 * mz0380_force_timings only rescues a signal that IS locked but matches no
 * table entry. With nothing connected there is no lock to rescue, so streamon
 * bails before the encoder is ever armed. This makes it arm anyway, at the
 * geometry below, exactly as if a 1080p60 source had been detected.
 */
bool mz0380_stream_without_signal;
module_param_named(stream_without_signal, mz0380_stream_without_signal,
		   bool, 0644);
MODULE_PARM_DESC(stream_without_signal,
		 "M113: arm the real capture path even when no HDMI signal is locked, using 1920x1080p60 - for reproducing what Windows shows with no source connected (def:0)");

bool mz0380_force_timings;
module_param_named(force_timings, mz0380_force_timings, bool, 0644);
MODULE_PARM_DESC(force_timings,
		 "M54: on a locked but unmatched signal with htotal=2200, stream as 1920x1080p60 (def:0)");

bool mz0380_gpio_dir_invert;
module_param_named(gpio_dir_invert, mz0380_gpio_dir_invert, bool, 0644);
MODULE_PARM_DESC(gpio_dir_invert,
		 "M51: invert the GPIO_DIR (op 0x17) data-bit sense used by the bit-banged I2C (def:0 = 1 means output)");

unsigned int mz0380_edid_opcode = MZ0380_CMD_I2C_WRITE_S;
module_param_named(edid_opcode, mz0380_edid_opcode, uint, 0644);
MODULE_PARM_DESC(edid_opcode,
		 "mailbox opcode used to push EDID (def: 0x1f, confirmed Windows/card bulk-write handler)");

unsigned int mz0380_edid_timeout_ms = 100;
module_param_named(edid_timeout_ms, mz0380_edid_timeout_ms, uint, 0644);
MODULE_PARM_DESC(edid_timeout_ms,
		 "ms to wait for each synchronous EDID chunk (def:100; 0 is unsafe fire-and-forget)");

bool mz0380_buf_poison = true;
module_param_named(buf_poison, mz0380_buf_poison, bool, 0644);
MODULE_PARM_DESC(buf_poison,
		 "M36: fill stream buffers with a poison byte before START and track the card's write extent over time (def:1)");

/*
 * M37 follow-up: the hole map found EXACTLY one 4-byte hole at 0x6d384,
 * reproducible at the same offset across runs. A deterministic single-dword
 * hole is at least as likely to be the frame DATA containing the poison
 * word (collision) as a lost write. Running twice with two different poison
 * bytes discriminates: a real un-written hole stays put; a collision moves
 * or disappears.
 */
unsigned int mz0380_poison_byte = 0xaa;
module_param_named(poison_byte, mz0380_poison_byte, uint, 0644);
MODULE_PARM_DESC(poison_byte,
		 "M37: poison byte value for buf_poison (def 0xaa; re-run with e.g. 0x55 to tell a real write hole from a data-equals-poison collision)");

/*
 * M33. The card's encoder will not complete a frame until audio is declared
 * ready. tinyvenc5's is_nosg path waits on /sys/audio_status/audio_ready, and
 * the only writer of that file is video_capture_mgr's SET_AIC_PARAMS (op 0x2a)
 * handler, which runs "echo '1' > /sys/audio_status/audio_ready" when the
 * command's on byte is set. So we send SET_AIC(on=1) just before START.
 * The rest of the fields are ordinary PCM geometry; they are parameters rather
 * than constants because nothing yet tells us the card validates them, and a
 * mismatch with the real I2S setup is a plausible future suspect.
 */
bool mz0380_aic_on = true;
module_param_named(aic_on, mz0380_aic_on, bool, 0644);
MODULE_PARM_DESC(aic_on,
		 "M33: send SET_AIC_PARAMS(on=1) before START, releasing the encoder's audio_ready gate (def:1)");

/*
 * M60: the nosg path costs one encoder spawn per frame (M39: the card's
 * encoder parks after exactly one frame, and only a fresh spawn unparks it),
 * and the card wedges for good after roughly 8-18 of them - a wedge no PCIe
 * reset clears (M52), only removing slot power. Every spawn also re-sent
 * SET_AIC(on=1). audio_ready is a sysfs flag on the card, so re-arming it per
 * frame is most likely pure waste; sending it once per streaming session may
 * stretch the budget. Set aic_every_frame=1 to go back if frames stop landing.
 */
/*
 * M62: require a second agreeing measurement pass before trusting a mode.
 *
 * Off by default, because it costs more time than the source gives us. One
 * pass is ~17 mailbox round-trips (~80ms) and demanding two agreeing passes
 * 10ms apart needs ~170ms of unbroken lock, while the only source available
 * transmits in bursts of roughly 300ms starting at an arbitrary moment - so
 * the pair rarely fits and the run reports "no coherent timing snapshot
 * survived" despite a real full lock.
 *
 * A single pass is not an unchecked one: it re-reads detect and vtotal_lo
 * after the block and rejects the sample if either moved, then cross-checks
 * vperiod against hperiod/(vtotal+1) within 0.5Hz. Set this when a source
 * holds lock continuously and the extra redundancy is free.
 */
/*
 * M65: how long a successful HDMI detection may be reused to arm the encoder
 * after the source has stopped transmitting. The only source available bursts
 * for roughly 300ms per power-cycle, so requiring a live lock at STREAMON
 * makes capture unreachable even though the mode is known. 0 disables the
 * fallback and restores "live lock or nothing".
 */
unsigned int mz0380_signal_cache_ms = 30000;
module_param_named(signal_cache_ms, mz0380_signal_cache_ms, uint, 0644);
MODULE_PARM_DESC(signal_cache_ms,
		 "M65: reuse the last good HDMI detection for this long when arming the stream (def:30000, 0=require a live lock)");

/*
 * M71: SET_VIC byte6 = "fw", the card's ENCODER SELECTOR, not a pixel format.
 * video_capture_mgr compares it to 7 and 8 to launch ./tinyvenc7 / ./tinyvenc8
 * and otherwise falls through to ./tinyvenc5; the value is also handed to the
 * encoder in its own argv. We had been sending 2 or 3 here (a misread of the
 * field as "2=progressive, 3=interlaced"), which reached tinyvenc5 only by
 * fallthrough. 5 is the H.264 encoder this card uses. Exposed as a parameter
 * so the alternative can be bisected on hardware without a rebuild.
 *
 * M82 supersedes the value, not the meaning. The Windows driver's own
 * [CH00] log prints byte6 as "fw" and byte7 as "vi", and across all four
 * live traces it sends fw = 6 for 1080p30 / 1080p29.97 and fw = 7 for
 * 1080p60. It never sends 5. Since vcm derives the capture cfg's "output
 * format" from this byte (2/YUY2 when fw == 6, else 1/YV12) and spawns
 * ./tinyvenc7 when it is 7, 5 and 6 differ in the pixel format the capture
 * stage is told to produce - and 5 is a value the retail driver has never
 * exercised.
 *
 * M88 (hardware): and yet **only fw = 5 works on this card.** Bisected against
 * the splash oracle with everything else held at its M82 value - the Windows
 * ordering, the new color_info/fast_kill/nosg/aic_int_mode, INTx - flipping
 * this one byte is the whole difference:
 *
 *     fw = 5  -> tinyvenc5, cfg output format 1/YV12  -> splash renders
 *     fw = 6  -> tinyvenc5, cfg output format 2/YUY2  -> nothing written
 *     fw = 7  -> tinyvenc7, cfg output format 1/YV12  -> nothing written
 *
 * 5 and 6 differ ONLY in the cfg's output format, so YUY2 by itself stops the
 * capture loop before it writes anything; 7 fails separately, through a
 * different encoder binary. Windows sends 6 or 7 and captures, so something
 * about the card's state differs from ours and this byte is where it surfaces
 * - but a value that demonstrably kills the capture loop is not one to ship
 * for the sake of parity. 0 selects the Windows rule (6 at <=30 fps, 7 above)
 * for anyone re-testing that contradiction.
 */
unsigned int mz0380_vic_fw = 5;
module_param_named(vic_fw, mz0380_vic_fw, uint, 0644);
MODULE_PARM_DESC(vic_fw,
		 "SET_VIC byte6 'fw' - 5=tinyvenc5/YV12, the only value that works here (M88); 0=Windows rule (6 at <=30fps, 7 above); 6, 7, 8 (def:5)");

/*
 * M72: SET_VIC byte12 ("m" in the card's log) is VideoCap's OUTPUT FORMAT.
 * The SDK's own capture config, re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg,
 * documents the enum inline: "1:YUV420, 2:YUV422" - and 0, which this driver
 * has always sent, is not a legal value. H.264 encodes from YUV420.
 *
 * M79 then found that byte12 never reaches the cfg at all - vcm passes it on
 * the tinyvenc argv instead, and the cfg's output format comes from byte6.
 * M82 settles the value: Windows sends m = 0 for both 1080p30 and 1080p60 and
 * m = 1 only for the 29.97 DSLR, so it looks like a fractional-rate flag, and
 * 0 is what this card is given for an ordinary integer-rate source.
 */
unsigned int mz0380_vic_out_format;
module_param_named(vic_out_format, mz0380_vic_out_format, uint, 0644);
MODULE_PARM_DESC(vic_out_format,
		 "SET_VIC byte12 'm' - Windows sends 0 at integer frame rates, 1 at 29.97 (def:0)");

/*
 * M72: SET_VIC bytes 16..19 ("color_info") line up with the same config file's
 * brightness / contrast / saturation / field-invert block, whose documented
 * neutral values are 0 / 0 / 128 / 0 - "saturation adjustment (0~255, 128:off,
 * 0:mono)". We have always sent 0,0,0,0, i.e. saturation pinned to mono.
 *
 * M82 retires that reading: the four bytes are color_info[0..3] and Windows
 * sends a fixed 1,1,1,2 on every SET_VIC, never a picture control. vic_saturation
 * now only overrides byte18 of vic_color_info, and does nothing unless set.
 */
unsigned int mz0380_vic_saturation = MZ0380_VIC_SATURATION_UNSET;
module_param_named(vic_saturation, mz0380_vic_saturation, uint, 0644);
MODULE_PARM_DESC(vic_saturation,
		 "override SET_VIC byte18 only; unset leaves vic_color_info alone (def:unset)");

/*
 * M82: SET_VIC bytes 16..19, exactly as the Windows driver builds them at
 * 0x14028bcb9 - [16]=1 [17]=1 [18]=1 [19]=2, i.e. 0x02010101 little-endian.
 * Identical in every trace and for every source.
 */
unsigned int mz0380_vic_color_info = 0x02010101;
module_param_named(vic_color_info, mz0380_vic_color_info, uint, 0644);
MODULE_PARM_DESC(vic_color_info,
		 "M82: SET_VIC bytes 16..19 color_info (def:0x02010101, what Windows always sends)");

/* M82: SET_VIC byte33 fast_kill. Windows logs fk=1 unconditionally. */
unsigned int mz0380_vic_fast_kill = 1;
module_param_named(vic_fast_kill, mz0380_vic_fast_kill, uint, 0644);
MODULE_PARM_DESC(vic_fast_kill, "M82: SET_VIC byte33 fast_kill (def:1)");

/*
 * M82: SET_VIC bytes 36..39 - no-signal fill colour, back.Y.U.V. Windows sends
 * 0.00.80.80, i.e. neutral grey; we sent all zeros, which is green.
 */
/*
 * M136/M138: SET_VIC byte 34 - NOT the frame-completion interrupt enable.
 *
 * Found in ep.ko, not guessed. pciep_isr's cmd-41 arm (0x171c) ends with:
 *
 *   1744: ldrb r1, [r4, #0x22]     @ SET_VIC payload byte 34
 *   1748: adds r3, r1, #0
 *   174c: movne r3, #1
 *   1754: strb r3, [r5, #0x630]    @ state[0x630] = (byte34 != 0)
 *
 * and state[0x630] IS read by store_channel_done() - the sysfs attribute the
 * CARD's userspace writes when a channel finishes a frame:
 *
 *   e88: ldrb r3, [r1, #0x630]
 *   e8c: cmp  r3, #0
 *   e94: beq  0xefc                @ int_mode == 0: the OTHER branch
 *
 * M138 CORRECTION - this byte is NOT the EVENT gate. Both branches of that
 * test raise the EVENT the same way, and both are gated on the same thing: the
 * one-shot completion credit at ep.ko's .data[0].
 *
 *   e98: ldr r0, [credit] ; beq 0xed8   @ int_mode != 0 branch: no credit ->
 *   f08: ldr r8, [credit] ; beq 0xf34   @ int_mode == 0 branch:  accumulate
 *   f54: ldr r0, [credit] ; beq 0xfb4   @ ditto, aic_int_mode != 1
 *
 * The only difference between the branches is the shift used for the AUDIO
 * channel bit (r0+15 vs r4+16). For video the code is identical. So byte 34
 * cannot change frame reporting, which is exactly what M137 measured.
 *
 * The credit itself is fine: it starts at 1, every raised event consumes it,
 * and card IRQ 42 (pciep_isr_clrint) restores it - reached from the host by
 * BAR0[0x00] = MZ0380_MB_INT_ACK (0x400, bit 10; 0x800/bit 11 is IRQ 43, the
 * command dispatcher). M133's 1224 completed commands prove that path re-arms
 * it 1000+ times per run. See mz0380_credit_rearm().
 *
 * Kept as a knob only because it is free and the AIC twin (SET_AIC byte 17 ->
 * state[0x63c]) is set to 1; nothing depends on it.
 */
unsigned int mz0380_vic_int_mode;
module_param_named(vic_int_mode, mz0380_vic_int_mode, uint, 0644);
MODULE_PARM_DESC(vic_int_mode,
		 "SET_VIC byte 34 -> ep.ko state[0x630]. M138: NOT the frame-completion EVENT gate (the one-shot credit is, and it is re-armed correctly); byte 34 only shifts the audio bit. Tried in M137, changes nothing (def:0)");

unsigned int mz0380_vic_nosg = 0x80800000;
module_param_named(vic_nosg, mz0380_vic_nosg, uint, 0644);
MODULE_PARM_DESC(vic_nosg,
		 "M82: SET_VIC bytes 36..39 nosg back/Y/U/V (def:0x80800000 = 0.00.80.80)");

/*
 * M73: BANK0 0xb0 output format/clock select for ordinary (non-720p30) modes.
 * We write 0x21; the GPL hdcapm driver ends its init with 0xb0 = 0x20,
 * commented "YUV422 / 8-bit output". Bit0 is the difference and it is
 * unidentified. Tunable so the two can be compared on hardware while the
 * receiver's output stage is under suspicion for producing no BT1120 clock.
 */
unsigned int mz0380_vic_b0 = 0x21;
module_param_named(vic_b0, mz0380_vic_b0, uint, 0644);
MODULE_PARM_DESC(vic_b0,
		 "M73: MST3367 BANK0 0xb0 output select for ordinary modes (def:0x21, hdcapm uses 0x20)");

/*
 * M126: BANK0 0xb1 / 0xb2 / 0xb5 - the receiver output-stage registers that
 * every sibling driver disagrees with us about, and that have never been
 * tunable.
 *
 *            ours    hdcapm   gchd (1080p)   Windows 0x9c trace
 *   0xb1     0xc0    0xe0     0x0c           0xc0  (confirmed)
 *   0xb2     0x00    0x08     0xc4           value never resolved
 *   0xb5     0x0c    -        0xd0           not seen
 *
 * The Windows trace pins 0xb1 and leaves 0xb2 as "?", so 0xb2 is the one
 * register with no reference value at all - and it is the one both siblings
 * write non-zero while we write zero. gchd is the more interesting witness
 * despite writing different values: it selects BOTH 0xb2 and 0xb5 from the
 * INPUT RESOLUTION (configure_hdmi.cpp: 0xb2 = c4/cc/cf and 0xb5 = d0/cc/cc
 * for 1080 / 720 / SD), where ours are two fixed constants. Structure
 * transfers even when values do not.
 *
 * Defaults are the current hard-coded values, so leaving these unset changes
 * nothing. Sweep ONE at a time (method rule 2), with fw=5, win_seq=0 and
 * vic_b0=0x21 - the only combination known to reach the splash.
 */
unsigned int mz0380_mst_b1 = 0xc0;
module_param_named(mst_b1, mz0380_mst_b1, uint, 0644);
MODULE_PARM_DESC(mst_b1,
		 "M126: MST3367 BANK0 0xb1 output config (def:0xc0 = ours+Windows; hdcapm 0xe0, gchd 0x0c)");

unsigned int mz0380_mst_b2;
module_param_named(mst_b2, mz0380_mst_b2, uint, 0644);
MODULE_PARM_DESC(mst_b2,
		 "M126: MST3367 BANK0 0xb2 output config (def:0x00 = ours; hdcapm 0x08, gchd 0xc4 for 1080p)");

/*
 * M130: MST3367 BANK0 0x92 - the CSC control byte.
 *
 * The 31-byte table we inherit from hdcapm spans 0x92..0xB0, and only the
 * first byte is not a coefficient:
 *
 *   0x92        0x40                 <- this byte: control
 *   0x93..0x98  M11 M12 M13          (2 bytes each, big-endian)
 *   0x99..0x9E  M21 M22 M23
 *   0x9F..0xA4  M31 M32 M33
 *   0xA5..0xAA  A1  A2  A3           (offsets)
 *   0xAB..0xB0  15 95 05 20 C0 08    (colour range + output stage; 0xB0 is
 *                                     rewritten straight afterwards)
 *
 * Why it matters (M129/M130): the captured frame's luma is perfect and its
 * chroma is 96% ANTI-correlated with luma - the signature of a matrix
 * subtracting luma from channels that are already luma-free. HDMI YCbCr 4:4:4
 * puts Cb on the blue channel, Y on green and Cr on red, so feeding it to an
 * RGB->YCbCr matrix gives Cb_out = -0.291*Y + ..., exactly what we measure.
 * Inverting that model on the captured frame turns the picture from
 * magenta/green mush into a coherent image, which confirms the mechanism.
 *
 * hdcapm writes this table unconditionally. Its MST3367_HdmiGetPacketColor()
 * reads the input colour space from BANK2 0x48 bits 6:5 and caches it in
 * regb2r48_cached - and then never uses it to pick a matrix. That is fine on
 * hdcapm's board, whose EDID makes sources send RGB. We push no EDID at all
 * (M127) and this source picked YUV444, which our own link log reports on
 * every run: "48=d2 ... input colorspace YUV444".
 *
 * With YCbCr in and YCbCr 4:2:2 out over BT1120, no conversion is wanted. If
 * 0x40 is the CSC enable, 0 should bypass it and the chroma should come out
 * right. That is one spawn and it is the cheapest thing that could work; the
 * fixed-point format of the coefficients has NOT been cracked, so do not try
 * to hand-write a matrix yet.
 */
unsigned int mz0380_mst_csc_ctl = MZ0380_MST_CSC_CTL_AUTO;
module_param_named(mst_csc_ctl, mz0380_mst_csc_ctl, uint, 0644);
MODULE_PARM_DESC(mst_csc_ctl,
		 "M130: MST3367 BANK0 0x92, the CSC control byte. Default AUTO (0xffffffff) = pick from the detected input colorspace: 0x00 (no conversion) for YUV422/YUV444, 0x40 (hdcapm's RGB->YCbCr) for RGB. Any other value forces that byte");

unsigned int mz0380_mst_b5 = 0x0c;
module_param_named(mst_b5, mz0380_mst_b5, uint, 0644);
MODULE_PARM_DESC(mst_b5,
		 "M126: MST3367 BANK0 0xb5 (def:0x0c = ours; gchd sends 0xd0 for 1080p, 0xcc otherwise)");

/*
 * M126: gchd commits 0xb0 in TWO writes - the configuration value with bit0
 * (embedded sync) CLEAR, then the same value with bit0 SET as the very last
 * register write of the whole HDMI bring-up (0xe8 then 0xe9). We write 0xb0
 * once, with bit0 already set, inside the 0xab bit7 freeze bracket.
 *
 * This is NOT the same experiment as M96, which measured 0x20 as a terminal
 * value and got nothing written. If bit0 is an output enable rather than a
 * mode select, the config must land before it is asserted, and a one-shot
 * write with bit0 already high never gives the retimer a clean edge.
 */
bool mz0380_mst_b0_late;
module_param_named(mst_b0_late, mz0380_mst_b0_late, bool, 0644);
MODULE_PARM_DESC(mst_b0_late,
		 "M126: commit 0xb0 as (value & ~1) inside the freeze, then set bit0 after unfreezing - gchd's order (def:0)");

/*
 * M75: which opcode programs the encoder's DMA destination buffers.
 *
 * We have always sent 0x02. The Windows driver builds the SAME 12-word command
 * (doorbell, opcode, channel, stride, then {hi,lo} address pairs) with opcodes
 * 0x04 and 0x05 - win64.txt 0x140279219 and 0x140279663, both with r8d=0xc
 * words and the payload pulled from an address array in dword pairs.
 *
 * 0x02 demonstrably works for the card's synthetic raw-NV12 path, so it does
 * reach a channel; but the H.264 bitstream may DMA from a different channel
 * whose addresses 0x02 never programs, which would leave the encoder running
 * with no destination and the host buffers untouched - exactly what the poison
 * scan reports (0/1024 pages) while the receiver holds a clean lock.
 */
unsigned int mz0380_set_buf_opcode = 0x02;
module_param_named(set_buf_opcode, mz0380_set_buf_opcode, uint, 0644);
MODULE_PARM_DESC(set_buf_opcode,
		 "M75: opcode that programs encoder DMA buffers - 2 (def), or 4/5/8 as the Windows driver uses");

/*
 * M76: SET_VIC bytes 24..27 ("input_frame_width/height") are the ONLY host
 * input that reaches the VIC's own width register. video_capture_mgr patches
 * them straight into the cfg lines "input frame width"/"input frame height"
 * (vcm FUN_0000a290, args 9 and 10), separately from the capture width, and
 * libvideocap writes that value to VIC channel register +0x68. vpl_vic's ISR
 * compares the incoming line against it and reports
 * "(CCIR or width(%lu) chck fail)" on a mismatch - one of the three status
 * bits behind "No signal !!".
 *
 * If the MST3367 is emitting 8-bit double-rate samples rather than true
 * 16-bit BT1120, the VIC counts 3840 samples per line where we declared 1920.
 * This is the only way to test that from the host without a scope. 0 keeps
 * the detected geometry.
 */
/*
 * M76: how many bytes of the card's /mnt/flash/PIC_ENC /proc/mz0380-cardlog
 * pages back. One mailbox round-trip per 16 bytes, so keep it small until the
 * transport is proven.
 */
unsigned int mz0380_cardlog_bytes = 256;
bool mz0380_cardlog_probe_enabled;
module_param_named(cardlog_probe, mz0380_cardlog_probe_enabled, bool, 0644);
MODULE_PARM_DESC(cardlog_probe,
		 "M76: WRITES 16 bytes to the card's /mnt/flash/PIC_ENC and reads them back, to prove vcm services op 0x6e (def:0)");
module_param_named(cardlog_bytes, mz0380_cardlog_bytes, uint, 0644);
MODULE_PARM_DESC(cardlog_bytes,
		 "M76: bytes of the card's PIC_ENC to page via /proc/mz0380-cardlog (def:256, 16 B per mailbox command)");

unsigned int mz0380_vic_in_w;
module_param_named(vic_in_w, mz0380_vic_in_w, uint, 0644);
MODULE_PARM_DESC(vic_in_w,
		 "M76: override SET_VIC bytes 24..25 input_frame_width (0=detected; try 3840 for 8-bit double-rate)");

unsigned int mz0380_vic_in_h;
module_param_named(vic_in_h, mz0380_vic_in_h, uint, 0644);
MODULE_PARM_DESC(vic_in_h,
		 "M76: override SET_VIC bytes 26..27 input_frame_height (0=detected)");

/*
 * SET_VIC byte7: the capture INPUT FORMAT enum (1=8-bits Raw, 2=CCIR656i,
 * 3=CCIR656p, 4=Bayer, 5=16-bits Raw, 6=BT1120p, 7=BT1120i), from the SDK
 * capture config. We derive 6/7 from the detected scan.
 *
 * M103 briefly reclassified this as the interlace flag, on the strength of the
 * card's own printf calling it "interlace" and M71's disassembly of the op-41
 * handler. M104 settled it on hardware and the enum reading won: 3, 6 and 7 all
 * reach the card's NOSG splash and are indistinguishable, while 0 - not in the
 * enum - produces nothing at all, which is VideoCap failing to open and
 * tinyvenc exiting before it can draw. The printf label is loose, not wrong:
 * 6 vs 7 is BT1120p vs BT1120i.
 *
 * The same run PROVED the byte is consumed at all, which three
 * indistinguishable values never could.
 *
 * MZ0380_VIC_IN_FMT_AUTO is the "derive it" sentinel. It exists because the old
 * `vic_in_fmt ?: derived` idiom could not express 0, so this field's own sweep
 * knob could not reach part of its own range - which is why the one informative
 * value went untried for so long. The derived default is unchanged.
 */
unsigned int mz0380_vic_in_fmt = MZ0380_VIC_IN_FMT_AUTO;
module_param_named(vic_in_fmt, mz0380_vic_in_fmt, uint, 0644);
MODULE_PARM_DESC(vic_in_fmt,
		 "SET_VIC byte7 input format (2=CCIR656i, 3=CCIR656p, 6=BT1120p, 7=BT1120i). Unset = derive 6/7 from the scan. M104: 0 is NOT in the enum and stops the card writing anything - it is the proof the byte is consumed, not a usable setting");

bool mz0380_signal_confirm;
module_param_named(signal_confirm, mz0380_signal_confirm, bool, 0644);
MODULE_PARM_DESC(signal_confirm,
		 "M62: require two agreeing measurement passes per mode detect (def:0 - a short-burst source cannot supply the window)");

bool mz0380_aic_every_frame;
module_param_named(aic_every_frame, mz0380_aic_every_frame, bool, 0644);
MODULE_PARM_DESC(aic_every_frame,
		 "M60: re-send SET_AIC(on=1) on every encoder spawn instead of once per session (def:0)");

unsigned int mz0380_aic_channels = 2;
module_param_named(aic_channels, mz0380_aic_channels, uint, 0644);
MODULE_PARM_DESC(aic_channels, "M33: SET_AIC channel_num (def:2)");

unsigned int mz0380_aic_bits = 16;
module_param_named(aic_bits, mz0380_aic_bits, uint, 0644);
MODULE_PARM_DESC(aic_bits, "M33: SET_AIC bits per sample (def:16)");

unsigned int mz0380_aic_freq = 48000;
module_param_named(aic_freq, mz0380_aic_freq, uint, 0644);
MODULE_PARM_DESC(aic_freq, "M33: SET_AIC sample rate (def:48000)");

unsigned int mz0380_aic_period_frames = 256;
module_param_named(aic_period_frames, mz0380_aic_period_frames, uint, 0644);
MODULE_PARM_DESC(aic_period_frames,
		 "SET_AIC frames per period (def:256; Windows/card default, 256x4=1024-frame mover)");

unsigned int mz0380_aic_periods = 4;
module_param_named(aic_periods, mz0380_aic_periods, uint, 0644);
MODULE_PARM_DESC(aic_periods, "M33: SET_AIC period_num_of_buffer (def:4)");

/*
 * M82: SET_AIC byte17. ep.ko stores it as G[0x63c] ("aic_int_mode") and the
 * Windows driver logs aic_int_mode=1 in every trace; we have always sent 0.
 * Its value is board-derived there and resolves to 1 for board id 0xFA.
 */
unsigned int mz0380_aic_int_mode = 1;
module_param_named(aic_int_mode, mz0380_aic_int_mode, uint, 0644);
MODULE_PARM_DESC(aic_int_mode, "M82: SET_AIC byte17 aic_int_mode (def:1, what Windows sends)");

/* ---- M82: the Windows capture-start sequence ---------------------------- */

/*
 * The retail driver reconfigures with
 *   0x07(all channels) -> ~1.9 s -> 0x29 -> 0x2a -> 0x2d(main) -> 0x2d(sub)
 *   -> 0x31
 * and never sends START_STREAMING (0x06) on the capture path: 0x2d and 0x31
 * are themselves bare sysfs_notify("epint") doorbells in ep.ko, which is the
 * same wake 0x06 performs. Buffers are registered before the reconfiguration,
 * when the capture pin opens.
 *
 * 1 selects that order. 0 is the pre-M82 order
 * (0x29 -> 0x2d -> SET_BUF -> 0x2a -> 0x06), which is what every run up to and
 * including M81 used.
 *
 * DEFAULT IS 0, deliberately. M90 showed on hardware that win_seq=1 renders
 * nothing at all - not even the card's NO SIGNAL splash - while win_seq=0 with
 * fw=5 reliably renders it (760/1024 pages, M88). Both variables are necessary
 * and they interact. A default that is known not to reach the splash is a trap:
 * it silently confounds every experiment run "with defaults", which is exactly
 * how the vic_b0=0x20 test was wasted. Defaults track the best known-working
 * configuration; set win_seq=1 explicitly to work on the Windows ordering.
 */
bool mz0380_win_seq;
module_param_named(win_seq, mz0380_win_seq, bool, 0644);
MODULE_PARM_DESC(win_seq,
		 "M82: use the Windows capture-start order and opcode set. DEFAULT 0 - win_seq=1 does not reach the splash (M90); 0 is the known-good baseline");

/*
 * Belt and braces: send 0x06 as well at the end of the Windows sequence. Off
 * because Windows does not, and an extra epint notify could re-enter the
 * encoder's read loop out of turn.
 */
bool mz0380_win_start_op6;
module_param_named(win_start_op6, mz0380_win_start_op6, bool, 0644);
MODULE_PARM_DESC(win_start_op6,
		 "M82: also fire START_STREAMING(0x06) after the Windows sequence (def:0)");

/*
 * The measured gap between "[FIRMWARE RESET]" and the [CH00] that follows it
 * was 1.84-1.91 s in every one of the six observed reconfigurations. Only a
 * few hundred ms of that is accounted for by sleeps inside the config
 * function, so treat the rest as the card settling and do not shorten it
 * without evidence.
 */
unsigned int mz0380_stop_settle_ms = 1900;
module_param_named(stop_settle_ms, mz0380_stop_settle_ms, uint, 0644);
MODULE_PARM_DESC(stop_settle_ms,
		 "M82: delay between the pre-STOP and SET_VIC (def:1900, Windows measures 1840-1910)");

/*
 * M84: SET_BUF placement, split out so it can be bisected without touching
 * win_seq. Windows registers its buffers when the capture pin opens, i.e.
 * before the reconfiguration, which is what 1 does. But M23 established on
 * hardware that op6 is what makes vpl_dmac latch channels[] into the outbound
 * iATU, and put SET_BUF *after* SET_VIC so our addresses are the ones latched.
 * Those two orderings only agree if op6 is absent; set 0 to keep the M23
 * placement while the rest of the Windows sequence stays.
 */
bool mz0380_win_bufs_first = true;
module_param_named(win_bufs_first, mz0380_win_bufs_first, bool, 0644);
MODULE_PARM_DESC(win_bufs_first,
		 "M84: with win_seq, program SET_BUF before SET_VIC as Windows does (def:1; 0 = M23 placement, after SET_VIC)");

/* M82: Windows configures a second (sub) encoder stream on every start. */
bool mz0380_enc_sub = true;
module_param_named(enc_sub, mz0380_enc_sub, bool, 0644);
MODULE_PARM_DESC(enc_sub,
		 "M82: also send SET_ENC_PARAMS for the sub stream (main_or_sub=1) (def:1)");

/*
 * SET_ENC_PARAMS validity mask. We send bits 0/1/6 (fps, gop, bitrate) and
 * leave the rest masked out because their enums were never verified; Windows
 * sends 0x3FFF with every field populated. 0 keeps the conservative mask.
 */
unsigned int mz0380_enc_mask;
module_param_named(enc_mask, mz0380_enc_mask, uint, 0644);
MODULE_PARM_DESC(enc_mask,
		 "SET_ENC_PARAMS validity mask - 0=conservative fps/gop/bitrate, 0x3fff=Windows (def:0)");

/* M82: POST_PROC (0x31). Windows sends mask 0x1F with di=1, everything else 0. */
/*
 * M128d/M131: DEFAULT CHANGED 0x1f -> 0. The mask gates three stores in
 * tinyvenc5's SET_PREVIEW_PARAMS handler - bit 0 skip, bit 1 avg, bit 4 die_en
 * - and with 0x1f one of them TRUNCATES THE DMA to a single 16-byte burst.
 * Proven on hardware: same command, mask 0x1f -> 16 bytes, mask 0 -> a whole
 * 3110400-byte frame. It is also the entire reason win_seq=1 "renders nothing"
 * (M90/M91), since that path sends 0x31 with this mask.
 *
 * Which of the three bits does it is still unbisected: post_mask=0x10 (die_en
 * alone) against 0x01 (skip alone), one spawn each. Prior is die_en - a
 * de-interlacer switched on for a progressive source, set only because Windows
 * sends it.
 */
unsigned int mz0380_post_mask;
module_param_named(post_mask, mz0380_post_mask, uint, 0644);
MODULE_PARM_DESC(post_mask,
		 "SET_PREVIEW_PARAMS(0x31) validity mask (def:0 - M128d: 0x1f truncates the DMA to 16 bytes; 0x1f was the old Windows-parity default)");

unsigned int mz0380_post_di = 1;
module_param_named(post_di, mz0380_post_di, uint, 0644);
MODULE_PARM_DESC(post_di, "M82: POST_PROC(0x31) deinterlace flag (def:1, what Windows sends)");

/*
 * M128: op 0x31 is not "POST_PROC", it is SET_PREVIEW_PARAMS, and its payload
 * byte 0x0e is `fake_frame_off`. Decoded statically from tinyvenc5's dispatch
 * table (main+0x804, index = cmd - 6) and its verbose printf at 0xf570:
 *
 *   [4..7]=mask(u32, sticky-OR'd)  [8]=ch  [9]=fps  [0x0a]=skip  [0x0b]=avg
 *   [0x0c]=die_en  [0x0d]=preview_off  [0x0e]=fake_frame_off
 *   [0x0f]=preview_no_osd  [0x10]=mirror  [0x11]=flip  [0x12]=hw_d
 *
 * The handler at 0xef24 stores byte 0x0e to
 * EncodingGroup::preview_params_settings[ch].byte[0x0a] UNCONDITIONALLY - the
 * validity mask gates only byte 0x0c (die_en, mask bit 4). That is the exact
 * byte M127 identified as the standby-splash gate: EncodingGroup::init_func
 * (0x10ac0) does
 *
 *   if (preview_params_settings[ch].byte[0x0a] == 0)
 *           pthread_create(&t, NULL, EncodingGroup::fake_frame_process, this);
 *
 * and main zeroes it at startup (0xe400). So on our baseline the standby
 * thread that draws NOSG_LOGO_Y is created because we have never told the card
 * otherwise. The second spawn site, in EncodingGroup::Start (0x10f30), is
 * gated by is_nosg instead, which we already send as 0.
 *
 * The guard runs inside the op-0x06 handler (which news the EncodingGroups,
 * then pthread_create's on_start_thread -> Start -> init_func), so 0x31 only
 * has an effect if it lands BEFORE START_STREAMING.
 *
 * Why it is worth a spawn: the "real frames are rejected in
 * libtkmf_video_source.so.0" mechanism is static RE only - the card's console
 * is not reachable, so we have never observed the drop. With the standby
 * thread suppressed there is nothing left to emit a frame except the real
 * encode path, which splits the two remaining stories cleanly:
 *   NO FRAME   -> the rejection is real; nothing ever reaches the encoder.
 *   real frame -> the standby thread was winning the race and masking it.
 * M131: DEFAULT CHANGED 0 -> 1. With it at 0 the card draws NOSG_LOGO_Y over a
 * perfectly good capture, which is what hid real video from this project for
 * its entire life (M129). Set it to 0 only to reproduce a pre-M129 result.
 */
bool mz0380_fake_frame_off = true;
module_param_named(fake_frame_off, mz0380_fake_frame_off, bool, 0644);
/*
 * M128b: the control for the above. The first fake_frame_off run changed TWO
 * things at once relative to the baseline - it sent op 0x31, which win_seq=0
 * never does, AND it set byte 0x0e in it - and it came back with M91's exact
 * 16-byte DMA stall. M91 saw that stall under win_seq=1, whose sequence also
 * contains 0x31, so "0x31 itself truncates the transfer" is a live and
 * parsimonious explanation that the run cannot separate from "the standby
 * thread is gone and the real path stalls".
 *
 * post_proc=1 sends 0x31 on the baseline with fake_frame_off left at 0, i.e.
 * one variable from that run. Method rule 1: every other variable is at a
 * value already known to permit a full frame, so it is informative either way.
 *   full splash (3110400 B) -> 0x31 is harmless; the stall belongs to
 *                              fake_frame_off, i.e. to the real path
 *   16 bytes of 0x11        -> 0x31 truncates; that is also M91's answer
 */
bool mz0380_post_proc = true;
module_param_named(post_proc, mz0380_post_proc, bool, 0644);
/*
 * M128b: how long to wait between op 0x31 and op 0x06.
 *
 * The control run pinned the truncation on 0x31 but NOT on anything 0x31
 * means. The handler is inert on this card: for ch 0 it falls through to
 * fopen("/tmp/PIC_INSERT","rb"), which cannot exist, so it sets
 * g_insert_pic = 0 and bare-ACKs (tinyvenc5 0xf068 -> 0xffac). The MemBroker
 * allocation that would have followed is never reached. Nothing it writes
 * touches capture, DMA or the VIC.
 *
 * What DID change is the mailbox cadence in front of START_STREAMING. ep.ko
 * serves one command at a time out of the mailbox and pokes it at the card as
 * a bare sysfs_notify("epint"); tinyvenc5 wakes, reads 44 bytes, and services
 * it. Measured on the two runs:
 *
 *   baseline   SET_AIC -> [156 ms, spent in the output-stage diag] -> 0x06
 *   with 0x31  SET_AIC -> [156 ms diag] -> 0x31 -> [9 us] -> 0x06
 *
 * Nine microseconds. 0x06 is fire-and-forget (timeout_ms = 0), so it does not
 * wait for anything, and it lands on the doorbell while tinyvenc5 is still
 * inside the 0x31 handler. Every other command in the sequence is tens to
 * hundreds of ms apart. That is the one thing 0x31 changes that could plausibly
 * truncate a transfer, and it also explains M91 without needing win_bufs_first.
 *
 * A gap is one variable from the run that produced the stall. Nonzero and the
 * frame comes back whole => cadence, and 0x31 is usable, which is what
 * fake_frame_off needs. Still 16 bytes at, say, 200 ms => the race is not it
 * and the 0x31 handler needs another read.
 */
unsigned int mz0380_post_proc_gap_ms;
module_param_named(post_proc_gap_ms, mz0380_post_proc_gap_ms, uint, 0644);
/*
 * M128c: which opcode goes into the pre-START slot. Default 0x31, i.e. no
 * change. The point of making it a variable is the control that partitions the
 * remaining space: op 0x09 is provably inert on BOTH sides - ep.ko routes it to
 * the same 0x1824 arm of pciep_isr that 0x2f/0x50/0x51/0x52/0x62 use, and
 * tinyvenc5's handler (0xe93c) is a bare pwrite(epint, payload, 44) with no
 * side effect whatsoever.
 *
 *   0x09 also truncates -> the fault is "an extra epint command immediately
 *                          before START", not anything 0x31 means. Structural,
 *                          and it would apply to the whole win_seq ordering.
 *   0x09 is clean        -> 0x31's own writes into tinyvenc5 are the cause,
 *                          and post_mask / post_di split them further.
 */
unsigned int mz0380_post_proc_opcode = MZ0380_CMD_POST_PROC;
module_param_named(post_proc_opcode, mz0380_post_proc_opcode, uint, 0644);
MODULE_PARM_DESC(post_proc_opcode,
		 "M128c: opcode for the pre-START slot (def:0x31). 0x09 is the inert control - same ep.ko arm, and tinyvenc5's handler is a bare ACK");
MODULE_PARM_DESC(post_proc_gap_ms,
		 "M128b: ms to wait between SET_PREVIEW_PARAMS(0x31) and START_STREAMING(0x06). 0 reproduces the 16-byte stall exactly; try 200 (def:0)");
MODULE_PARM_DESC(post_proc,
		 "send SET_PREVIEW_PARAMS(0x31) on the win_seq=0 baseline (def:1 since M131 - it is what carries fake_frame_off). 0 restores the pre-M129 sequence");
MODULE_PARM_DESC(fake_frame_off,
		 "M128: SET_PREVIEW_PARAMS(0x31) byte 0x0e - 1 suppresses the card's standby NOSG_LOGO_Y thread, so a delivered frame is a real one (def:1 since M131; 0 restores the splash)");

/*
 * M4 diagnostic: enable bus mastering + MSI/ISR BEFORE firmware load, but do
 * NOT program any (still-unverified) ring addresses. Safe because the card has
 * no host DMA target to write to; tests whether the post-boot mailbox doorbell
 * only reaches the card once bus mastering is on.
 */
/*
 * M82: run the card on legacy INTx, which is the only interrupt path the
 * Windows driver has ever used on this device. See mz0380_irq_request().
 */
bool mz0380_irq_intx = true;
module_param_named(irq_intx, mz0380_irq_intx, bool, 0444);
MODULE_PARM_DESC(irq_intx,
		 "M82: force legacy INTx instead of MSI, as the Windows driver does (def:1)");

bool mz0380_dma_handshake;
module_param_named(dma_handshake, mz0380_dma_handshake, bool, 0444);
MODULE_PARM_DESC(dma_handshake,
		 "enable bus master + MSI (no ring programming) before firmware handshake; M4 diagnostic");

bool mz0380_enable_audio;
module_param_named(enable_audio, mz0380_enable_audio, bool, 0444);
MODULE_PARM_DESC(enable_audio,
		 "register the experimental ALSA scaffold (PCM DMA is not implemented; def:0)");

unsigned int mz0380_video_ring_entries = 16;
module_param_named(video_ring_entries, mz0380_video_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entries,
		 "reserved legacy ring knob; real video uses four SET_BUF slots (default 16)");

unsigned int mz0380_video_ring_entry_size = (512 * 1024);
module_param_named(video_ring_entry_size, mz0380_video_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entry_size,
		 "reserved legacy ring knob, not used by SET_BUF video (default 512 KiB)");

unsigned int mz0380_audio_ring_entries = 8;
module_param_named(audio_ring_entries, mz0380_audio_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entries,
		 "reserved until the audio DMA ABI is implemented (default 8)");

unsigned int mz0380_audio_ring_entry_size = 32768;
module_param_named(audio_ring_entry_size, mz0380_audio_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entry_size,
		 "reserved until the audio DMA ABI is implemented (default 32 KiB)");

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
	spin_lock_init(&dev->event_lock);
	spin_lock_init(&dev->frame_event_lock);
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
		seq_puts(m,
			 "  firmware   : card's own flash image (host never uploads)\n");
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

/*
 * BAR0 offsets proven to complete a readl without stalling the PCIe link:
 * these are already read live by mz0380_mailbox_scan() (0x00..0x60) and
 * mz0380_signal_from_bar0() (0xa8..0xe4). A blind read of an un-backed BAR0
 * offset post-boot does NOT return 0xffffffff - it stalls the CPU on the readl
 * until the host completion timeout, which presents as a hard hang. So the scan
 * only reads inside these ranges unless the operator opts in with scan_unsafe=1.
 */
struct mz0380_safe_range {
	u32 start;	/* inclusive */
	u32 end;	/* exclusive */
};

static const struct mz0380_safe_range mz0380_scan_safe_mmio[] = {
	{ 0x0000, 0x0060 },	/* bootloader / mailbox aperture */
	{ 0x00a0, 0x00e8 },	/* sc0710-style HDMI status block  */
};

static bool mz0380_scan_offset_safe(unsigned int map, u32 reg)
{
	unsigned int i;

	/* BAR5/CFG is a small fully-backed 4K window; sweeping it is safe. */
	if (map != MZ0380_MAP_BAR_MMIO)
		return true;

	for (i = 0; i < ARRAY_SIZE(mz0380_scan_safe_mmio); i++)
		if (reg >= mz0380_scan_safe_mmio[i].start &&
		    reg < mz0380_scan_safe_mmio[i].end)
			return true;

	return false;
}

/*
 * Raw contiguous register-window dump for signal-source discovery.
 *
 * Emits one 4-byte register per line in a diff-stable format:
 *     bar0[0x00a8] = 12345678
 * so two captures (HDMI source plugged vs unplugged) can be compared with
 * plain diff(1) to see exactly which register(s) track cable/lock state.
 * The window (BAR, start, length) is retunable live via the scan_bar /
 * scan_start / scan_len module params under /sys/module/mz0380/parameters/.
 * Read-only and bounded to the mapped BAR length - never touches the card.
 */
static void mz0380_dump_scan_window(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int map = scan_bar;
	u32 start = scan_start & ~0x3u;
	resource_size_t bar_len;
	u64 end;
	u32 reg;

	if (map >= MZ0380_MAX_MAPS || !dev->lmmio[map]) {
		seq_printf(m, "  scan       : bar index %u unmapped\n", map);
		return;
	}

	bar_len = dev->bar_len[map];
	end = (u64)start + (scan_len ? scan_len : 0x48);
	if (end > bar_len)
		end = bar_len;

	seq_printf(m,
		   "  scan bar%d 0x%04x..0x%04llx (len 0x%x, bar_len 0x%llx, mode %s)\n",
		   dev->bar_nr[map], start, end, scan_len,
		   (unsigned long long)bar_len,
		   scan_unsafe ? "UNSAFE-all-offsets" : "safe-ranges-only");

	if ((u64)start >= end) {
		seq_puts(m, "  scan       : empty window (start past bar_len)\n");
		return;
	}

	for (reg = start; reg < end; reg += 4) {
		if (!scan_unsafe && !mz0380_scan_offset_safe(map, reg)) {
			seq_printf(m,
				   "  bar%d[0x%04x] = ........ (skipped: outside safe range; scan_unsafe=1 to read)\n",
				   dev->bar_nr[map], reg);
			continue;
		}

		/*
		 * Log the target before the readl so that if an un-backed
		 * offset stalls the link, the last line in dmesg after the
		 * (possibly forced) recovery pinpoints the culprit.
		 */
		if (scan_unsafe)
			pr_info("%s: scan probing bar%d[0x%04x]\n",
				dev->name, dev->bar_nr[map], reg);

		seq_printf(m, "  bar%d[0x%04x] = %08x\n",
			   dev->bar_nr[map], reg, mz_read(dev, map, reg));
	}
}

static int mz0380_proc_scan_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_scan_window(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

/*
 * Peripheral register-file scan for signal-source discovery.
 *
 * Unlike /proc/mz0380-scan (raw BAR0 reads, most of which are un-backed
 * post-boot), this walks a chip's registers through the proven mailbox
 * REG_READ (0x1a) command path - the same route mz0380_periph_read() uses to
 * reach the HDMI bridge (chip 0x90) and the TVP5160 (0xb8). Each register is
 * emitted diff-stably as:
 *     periph[0x90][0x12] = 00000001
 * so a plugged-vs-unplugged diff reveals which bridge register actually tracks
 * HDMI signal lock (the old 0x12 "bit0 = signal present" guess is unverified).
 * A short per-read timeout bounds the cost of a non-responding register.
 */
static void mz0380_dump_periph_scan(struct seq_file *m, struct mz0380_dev *dev)
{
	u8 chip = periph_chip & 0xff;
	unsigned int reg;
	unsigned int start = periph_start & 0xff;
	unsigned int end = start + periph_count;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		seq_puts(m, "  periph     : card handshake not complete\n");
		return;
	}
	if (end > 0x100)
		end = 0x100;

	/*
	 * Diagnostic: the read-back result slot for REG_READ is a guess
	 * (periph_read() reads PARAM3). If a bridge scan returns all-zero, the
	 * card may be writing the value into a different slot. Issue REG_READ
	 * for the first few registers and dump every candidate return word so
	 * the real result slot is visible. The transaction helper snapshots the
	 * opcode plus the ten real command words and copies them while cmd_lock is
	 * still held. EVENT/payload are re-read separately (all within the safe
	 * mailbox aperture).
	 */
	if (periph_probe) {
		unsigned int n = min(periph_count, 4u);
		unsigned int i;

		seq_printf(m,
			   "  periph PROBE: REG_READ(0x1a) chip 0x%02x, full slot dump for %u reg(s)\n",
			   chip, n ? n : 1);
		for (reg = start; reg < start + (n ? n : 1); reg++) {
			u32 params[3] = { chip, reg, 0 };
			u32 reply[MZ0380_MB_COMMAND_WORDS] = { 0 };
			u32 status = 0;
			int ret = mz0380_send_command_reply(
				dev, MZ0380_CMD_REG_READ, params, 3,
				&status, 500, reply, ARRAY_SIZE(reply));

			seq_printf(m, "  --- reg 0x%02x: send ret=%d STATUS[0x2c]=%08x ---\n",
				   reg, ret, status);
			for (i = 0; i < MZ0380_MB_COMMAND_WORDS; i++)
				seq_printf(m, "    PARAM(%2u)[bar0+0x%02x] = %08x%s\n",
					   i, MZ0380_MB_PARAM(i), reply[i],
					   i == 0 ? "  <- opcode echo" :
					   i == 1 ? "  <- RESULT slot"  :
					   i == 3 ? "  <- periph_read() reads here" : "");
			seq_printf(m, "    EVENT[0x30]      = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVENT));
			seq_printf(m, "    PAYLOAD0[0x40]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0));
			seq_printf(m, "    PAYLOAD1[0x44]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1));
			seq_printf(m, "    PAYLOAD2[0x48]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2));
			seq_printf(m, "    PAYLOAD3[0x4c]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3));
		}
		return;
	}

	seq_printf(m,
		   "  periph chip 0x%02x regs 0x%02x..0x%02x via REG_READ(0x1a)%s\n",
		   chip, start, end ? end - 1 : start,
		   dev->dma_armed ? "" :
		   " [dma not armed - reads may fail; load dma_handshake=1]");

	for (reg = start; reg < end; reg++) {
		u32 value = 0;
		int ret = mz0380_periph_read(dev, chip, reg, &value);

		if (ret)
			seq_printf(m,
				   "  periph[0x%02x][0x%02x] = ........ (err %d)\n",
				   chip, reg, ret);
		else
			seq_printf(m, "  periph[0x%02x][0x%02x] = %08x\n",
				   chip, reg, value);
	}
}

static int mz0380_proc_periph_scan_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_periph_scan(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

/* ===== live card-event watcher ==========================================
 *
 * The card notifies signal/format changes as edge events on the BAR0 EVENT
 * word (0x30), carrying data in the payload words at 0x40..0x4c, rather than
 * exposing a pollable status register. A kthread samples EVENT at high rate and
 * records each edge into a per-device ring; /proc/mz0380-events dumps the ring
 * (read) and starts/stops/clears the watcher (write). Snapshot the payloads
 * before acking, since the ack clears the event.
 */
static void mz0380_event_record(struct mz0380_dev *dev, u32 event)
{
	struct mz0380_event_rec snap;
	unsigned long flags;

	snap.t_ns       = local_clock();
	snap.event      = event;
	snap.payload[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0);
	snap.payload[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
	snap.payload[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);
	snap.payload[3] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	snap.status     = mz_mmio_read(dev, MZ0380_MB_STATUS);
	snap.intflag    = mz_cfg_read(dev, MZ0380_CFG_INT_FLAG);

	spin_lock_irqsave(&dev->event_lock, flags);
	dev->event_ring[dev->event_head] = snap;
	dev->event_head = (dev->event_head + 1) % MZ0380_EVENT_RING_SIZE;
	if (dev->event_count < MZ0380_EVENT_RING_SIZE)
		dev->event_count++;
	dev->event_seen++;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static int mz0380_event_thread(void *data)
{
	static const u16 tok_reg[5] = { 0x40, 0x44, 0x48, 0x4c, 0x50 };
	struct mz0380_dev *dev = data;
	u64 tok_changes[5] = { 0 };
	u32 tok_last[5];
	bool tok_valid = false;
	unsigned long next_print = jiffies;
	unsigned long next_kick = jiffies;
	unsigned int kicks = 0;
	u32 last = 0;
	unsigned int i;

	while (!kthread_should_stop()) {
		u32 tok[5];
		bool tok_diff = false;
		u32 event = mz_mmio_read(dev, MZ0380_MB_EVENT);

		/* M35: token words are written ungated by store_channel_done */
		for (i = 0; i < ARRAY_SIZE(tok_reg); i++) {
			tok[i] = mz_mmio_read(dev, tok_reg[i]);
			if (tok_valid && tok[i] != tok_last[i]) {
				tok_diff = true;
				tok_changes[i]++;
			}
			tok_last[i] = tok[i];
		}
		if (tok_diff && time_after_eq(jiffies, next_print)) {
			pr_info("%s: live token EVENT=%08x tok40=%08x 44=%08x 48=%08x 4c=%08x enc50=%08x\n",
				dev->name, event, tok[0], tok[1], tok[2],
				tok[3], tok[4]);
			next_print = jiffies + HZ / 20;
		}
		tok_valid = true;

		if (credit_kick_ms &&
		    time_after_eq(jiffies, next_kick)) {
			mutex_lock(&dev->cmd_lock);
			mz0380_mb_ack_event(dev);
			mutex_unlock(&dev->cmd_lock);
			kicks++;
			pr_info("%s: credit kick #%u (ack sequence fired)\n",
				dev->name, kicks);
			next_kick = jiffies + msecs_to_jiffies(credit_kick_ms);
		}

		if (event && event != last) {
			/*
			 * Serialise the ack (which rings the 0x400 doorbell)
			 * with the command channel so an in-flight command is
			 * never aborted mid-flight.
			 */
			mutex_lock(&dev->cmd_lock);
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event) {
				mz0380_event_record(dev, event);
				if (event_auto_ack)
					mz0380_mb_ack_event(dev);
			}
			mutex_unlock(&dev->cmd_lock);
			last = event_auto_ack ? 0 : event;
		} else if (!event) {
			last = 0;
		}
		usleep_range(event_sample_us, event_sample_us + 50);
	}
	pr_info("%s: token watch summary: changes tok40=%llu 44=%llu 48=%llu 4c=%llu enc50=%llu, kicks=%u\n",
		dev->name, tok_changes[0], tok_changes[1], tok_changes[2],
		tok_changes[3], tok_changes[4], kicks);
	return 0;
}

static void mz0380_event_watch_start(struct mz0380_dev *dev)
{
	if (dev->event_watching)
		return;
	dev->event_kthread = kthread_run(mz0380_event_thread, dev,
					 "mz0380-events/%u", dev->nr);
	if (IS_ERR(dev->event_kthread)) {
		dev->event_kthread = NULL;
		return;
	}
	dev->event_watching = true;
}

static void mz0380_event_watch_stop(struct mz0380_dev *dev)
{
	if (dev->event_kthread) {
		kthread_stop(dev->event_kthread);
		dev->event_kthread = NULL;
	}
	dev->event_watching = false;
}

static void mz0380_event_ring_clear(struct mz0380_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	dev->event_head = 0;
	dev->event_count = 0;
	dev->event_seen = 0;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static void mz0380_dump_events(struct seq_file *m, struct mz0380_dev *dev)
{
	struct mz0380_event_rec *snap;
	unsigned int count, head, i, idx;
	u64 seen, t0;
	unsigned long flags;

	seq_printf(m, "  watcher    : %s (sample %u us, auto_ack %d)\n",
		   dev->event_watching ? "running" : "stopped",
		   event_sample_us, event_auto_ack);
	seq_printf(m,
		   "  live       : EVENT[0x30]=%08x STATUS[0x2c]=%08x intflag=%08x\n",
		   mz_mmio_read(dev, MZ0380_MB_EVENT),
		   mz_mmio_read(dev, MZ0380_MB_STATUS),
		   mz_cfg_read(dev, MZ0380_CFG_INT_FLAG));

	snap = kmalloc_array(MZ0380_EVENT_RING_SIZE, sizeof(*snap), GFP_KERNEL);
	if (!snap) {
		seq_puts(m, "  (out of memory)\n");
		return;
	}

	spin_lock_irqsave(&dev->event_lock, flags);
	count = dev->event_count;
	head = dev->event_head;
	seen = dev->event_seen;
	for (i = 0; i < count; i++) {
		idx = (head - count + i + MZ0380_EVENT_RING_SIZE) %
		      MZ0380_EVENT_RING_SIZE;
		snap[i] = dev->event_ring[idx];
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);

	seq_printf(m, "  recorded   : %u shown of %llu total edges%s\n",
		   count, seen,
		   seen > count ? " (ring wrapped, oldest lost)" : "");
	if (!count) {
		seq_puts(m,
			 "  (none; 'echo start > /proc/mz0380-events', toggle the HDMI source, then re-read)\n");
		kfree(snap);
		return;
	}

	t0 = snap[0].t_ns;
	for (i = 0; i < count; i++)
		seq_printf(m,
			   "  +%9llu us  EVENT=%08x  p=%08x %08x %08x %08x  STATUS=%08x intflag=%08x\n",
			   (unsigned long long)(snap[i].t_ns - t0) / 1000,
			   snap[i].event,
			   snap[i].payload[0], snap[i].payload[1],
			   snap[i].payload[2], snap[i].payload[3],
			   snap[i].status, snap[i].intflag);

	kfree(snap);
}

static int mz0380_proc_events_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_events(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static ssize_t mz0380_proc_events_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	char *cmd;
	ssize_t ret = -EINVAL;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		if (!strcmp(cmd, "start")) {
			mz0380_event_watch_start(dev);
			ret = count;
		} else if (!strcmp(cmd, "stop")) {
			mz0380_event_watch_stop(dev);
			ret = count;
		} else if (!strcmp(cmd, "clear")) {
			mz0380_event_ring_clear(dev);
			ret = count;
		} else if (!strcmp(cmd, "repoison")) {
			/* M38: re-fill the stream bufs with poison mid-stream */
			mz0380_extent_repoison(dev);
			ret = count;
		}
	}
	mutex_unlock(&devlist);

	if (ret > 0)
		*ppos += count;
	kfree(cmd);
	return ret;
}

/*
 * Select the real HDMI front end without spawning the encoder.  The old proc
 * command packed an unverified raw input code and geometry into SET_VIC; that
 * opcode configures/spawns tinyvenc and belongs only in stream start.  Property
 * 201 uses the V4L2 input index instead, where HDMI is confirmed as index 0.
 * Refreshing the MST3367 sink then drives HPD low, reloads EDID, and raises HPD
 * so the source sees a coherent sink transition.
 */
static int mz0380_activate_hdmi_sink(struct mz0380_dev *dev)
{
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: HDMI sink refresh skipped - card handshake not complete (load dma_handshake=1)\n",
			dev->name);
		return -ENODEV;
	}

	ret = mz0380_request_input_select(dev, 0, "hdmi-proc");
	if (ret) {
		pr_warn("%s: HDMI property-%u index-0 select failed (%d)\n",
			dev->name, MZ0380_INPUT_SELECT_PROPERTY, ret);
		return ret;
	}

	ret = mz0380_mst3367_reload_edid(dev);
	pr_info("%s: HDMI property-%u index 0 selected; MST3367 EDID/HPD refresh ret=%d (SET_VIC deferred to stream start)\n",
		dev->name, MZ0380_INPUT_SELECT_PROPERTY, ret);
	return ret;
}

static int mz0380_proc_hdmi_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	seq_puts(m, "usage: echo hdmi > /proc/mz0380-hdmi\n");
	seq_puts(m, "  selects confirmed V4L2/property-201 HDMI index 0, reloads EDID, and pulses HPD.\n");
	seq_puts(m, "  It does not send SET_VIC or start the encoder; those happen only at stream start.\n");
	seq_puts(m, "  Legacy numeric forms accept property index 0 or raw HDMI code 2 only; geometry is ignored.\n");
	seq_puts(m, "  Raw DVI/component/SDI/auto input codes are rejected. Other commands: edid, hpd, watch, ramtest, wscan, edidhunt, gpiodump, i2cscan, edidburn.\n");
	seq_puts(m, "\nsink chain read-back (M43: write returns prove nothing - the\n"
		    "firmware forces the I2C result to 0 on a NAK, so verify by reading):\n");
	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		mz0380_mst3367_diag(dev, m);
	mutex_unlock(&devlist);
	return 0;
}

static ssize_t mz0380_proc_hdmi_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	u32 input = 0, width = 1920, height = 1080, fps = 60;
	char *cmd;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);
	if (!strcmp(cmd, "hdmi"))
		goto activate_hdmi;

	/* M44: "ramtest" probes whether MST3367 BANK3 is writable RAM */
	if (!strcmp(cmd, "ramtest")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_ramtest(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M49: "wscan" hunts writable RAM windows (EDID store) */
	if (!strcmp(cmd, "wscan")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_wscan(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M48: "hpd [count] [gap_ms]" pulses HPD only, nothing else */
	if (!strncmp(cmd, "hpd", 3)) {
		unsigned int n = 5, gap = 4000;

		sscanf(cmd, "hpd %u %u", &n, &gap);
		n = clamp(n, 1u, 20u);
		gap = clamp(gap, 200u, 10000u);
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_hpd_pulse(dev, n, gap);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M47: "edid" re-pushes the EDID and re-pulses HPD */
	if (!strcmp(cmd, "edid")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_reload_edid(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M53: "edidhunt" - hunt an indirect address/data port into EDID RAM */
	if (!strcmp(cmd, "edidhunt")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_edidhunt(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M51b: "gpiodump" - idle level of every GPIO pin (pull-up hunt) */
	if (!strcmp(cmd, "gpiodump")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_gpio_dump(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M51: "i2cscan [sda] [scl]" bit-bang scan of a spare-GPIO I2C bus */
	if (!strncmp(cmd, "i2cscan", 7)) {
		unsigned int sda = 13, scl = 12;

		sscanf(cmd, "i2cscan %u %u", &sda, &scl);
		kfree(cmd);
		if (sda > 31 || scl > 31 || sda == scl)
			return -EINVAL;
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_i2cbb_scan(dev, sda, scl);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M51: "edidburn [sda] [scl] [addr7]" burn+verify EDID into the EEPROM */
	if (!strncmp(cmd, "edidburn", 8)) {
		unsigned int sda = 13, scl = 12, addr = 0x50;

		sscanf(cmd, "edidburn %u %u %x", &sda, &scl, &addr);
		kfree(cmd);
		if (sda > 31 || scl > 31 || sda == scl || addr > 0x7f)
			return -EINVAL;
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_i2cbb_edid_burn(dev, sda, scl, addr);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M45: "watch [secs]" logs the detect block live to dmesg */
	if (!strncmp(cmd, "watch", 5)) {
		unsigned int secs = 20;

		sscanf(cmd, "watch %u", &secs);
		secs = clamp(secs, 1u, 120u);
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_watch(dev, secs);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/*
	 * Anything left must be the deprecated numeric "input width height fps"
	 * form. A word that matched no command above is a typo - or, more usefully,
	 * a
	 * command this module is too old to know, which is what a stale
	 * insmod looks like. Falling through would silently run an HDMI
	 * activation with default geometry and log nothing about the real
	 * request, so reject it and name the vocabulary instead.
	 */
	if (*cmd && !(*cmd >= '0' && *cmd <= '9')) {
		pr_info("mz0380: unknown /proc/mz0380-hdmi command '%s' - known: hdmi, ramtest, wscan, edidhunt, gpiodump, i2cscan, edidburn, hpd, edid, watch, or legacy '0|2 [w h fps]'\n",
			cmd);
		kfree(cmd);
		return -EINVAL;
	}

	{
		u32 values[4];
		unsigned int n = 0;
		char *p = cmd;
		char *tok;

		while ((tok = strsep(&p, " \t")) != NULL) {
			if (!*tok)
				continue;
			if (n == ARRAY_SIZE(values) ||
			    kstrtou32(tok, 0, &values[n])) {
				pr_warn("mz0380: malformed legacy HDMI request; use 'hdmi'\n");
				kfree(cmd);
				return -EINVAL;
			}
			n++;
		}
		if (!n) {
			kfree(cmd);
			return -EINVAL;
		}

		input = values[0];
		if (n > 1)
			width = values[1];
		if (n > 2)
			height = values[2];
		if (n > 3)
			fps = values[3];
	}

	/* 0 is property-201 HDMI; 2 is accepted only as the old raw HDMI code. */
	if (input != 0 && input != MZ0380_INPUT_CODE_HDMI) {
		pr_warn("mz0380: rejected non-HDMI raw input code %u; use 'hdmi' (property-201 index 0)\n",
			input);
		kfree(cmd);
		return -EINVAL;
	}
	pr_warn("mz0380: legacy HDMI request '%u %u %u %u': geometry is ignored; selecting property-201 index 0 and refreshing EDID/HPD\n",
		input, width, height, fps);

activate_hdmi:
	kfree(cmd);

	{
		ssize_t ret = count;
		bool found = false;

		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist) {
			int err;

			found = true;
			err = mz0380_activate_hdmi_sink(dev);
			if (err && ret > 0)
				ret = err;
		}
		mutex_unlock(&devlist);
		if (!found)
			return -ENODEV;
		if (ret < 0)
			return ret;
	}

	*ppos += count;
	return count;
}

/*
 * Generic mailbox command passthrough for RE (RE_FINDINGS.md M7). Sends an
 * arbitrary opcode + params and dumps the returned status/param slots, so the
 * front-end/HPD bring-up can be probed without a recompile per opcode - e.g.
 * GPIO direction/data (op 0x17/0x15, SL6010 GPIO props 940/941), config banks
 * (op 0x02/0x04/0x08). Root-only (0644) and gated on firmware-ready.
 */
static int mz0380_raw_command_locked(struct mz0380_dev *dev, u32 opcode,
				     const u32 *params, unsigned int nparams)
{
	u32 reply[4] = { 0 };
	u32 status = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: raw cmd skipped - firmware not ready\n", dev->name);
		return -ENODEV;
	}
	if (nparams > MZ0380_MB_MAX_ARGS)
		return -E2BIG;

	ret = mz0380_send_command_reply(dev, opcode, params, nparams,
					   &status, 500, reply,
					   ARRAY_SIZE(reply));
	pr_info("%s: raw cmd op=0x%02x n=%u ret=%d status=0x%08x out=%08x %08x %08x %08x\n",
		dev->name, opcode, nparams, ret, status,
		reply[0], reply[1], reply[2], reply[3]);
	return ret;
}

static int mz0380_proc_cmd_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	seq_puts(m, "usage: echo \"<opcode> [p0 p1 ...]\" > /proc/mz0380-cmd  (hex 0x.. or dec)\n");
	seq_puts(m, "  sends one mailbox command; results go to dmesg (status + first 4 return slots).\n");
	seq_puts(m, "  e.g. GPIO all-out: '0x17 0xffff' then all-high: '0x15 0xffff'; read GPIO: '0x14'.\n");
	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		seq_printf(m, "%s: fw %s\n", dev->name,
			   mz0380_fw_state_name(dev->fw_state));
	mutex_unlock(&devlist);
	return 0;
}

static ssize_t mz0380_proc_cmd_write(struct file *file,
				     const char __user *buffer,
				     size_t count, loff_t *ppos)
{
	u32 vals[MZ0380_MB_COMMAND_WORDS] = { 0 };
	struct mz0380_dev *dev;
	unsigned int n = 0;
	char *cmd, *p, *tok;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);

	p = cmd;
	while ((tok = strsep(&p, " \t")) != NULL) {
		if (!*tok)
			continue;
		if (n == ARRAY_SIZE(vals)) {
			kfree(cmd);
			return -E2BIG;
		}
		if (kstrtou32(tok, 0, &vals[n])) {
			kfree(cmd);
			return -EINVAL;
		}
		n++;
	}
	kfree(cmd);
	if (n < 1)
		return -EINVAL;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		mz0380_raw_command_locked(dev, vals[0], &vals[1], n - 1);
	mutex_unlock(&devlist);

	*ppos += count;
	return count;
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

static int mz0380_proc_scan_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_scan_show, NULL);
}

static int mz0380_proc_periph_scan_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_periph_scan_show, NULL);
}

static int mz0380_proc_events_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_events_show, NULL);
}

static int mz0380_proc_hdmi_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_hdmi_show, NULL);
}

static int mz0380_proc_cmd_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_cmd_show, NULL);
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

static struct file_operations mz0380_proc_scan_fops = {
	.open = mz0380_proc_scan_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_periph_scan_fops = {
	.open = mz0380_proc_periph_scan_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_events_fops = {
	.open = mz0380_proc_events_open,
	.read = seq_read,
	.write = mz0380_proc_events_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_hdmi_fops = {
	.open = mz0380_proc_hdmi_open,
	.read = seq_read,
	.write = mz0380_proc_hdmi_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_cmd_fops = {
	.open = mz0380_proc_cmd_open,
	.read = seq_read,
	.write = mz0380_proc_cmd_write,
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

static struct proc_ops mz0380_proc_scan_fops = {
	.proc_open = mz0380_proc_scan_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_periph_scan_fops = {
	.proc_open = mz0380_proc_periph_scan_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_events_fops = {
	.proc_open = mz0380_proc_events_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_events_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_hdmi_fops = {
	.proc_open = mz0380_proc_hdmi_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_hdmi_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_cmd_fops = {
	.proc_open = mz0380_proc_cmd_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_cmd_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#endif

/*
 * M76: read 16 bytes from the card's /mnt/flash/PIC_ENC via LOAD_FILES.
 *
 * The card echoes the whole 44-byte command back after servicing it, so the
 * data lands in PARAM1..PARAM4. Costs one mailbox round-trip per chunk.
 */
static int mz0380_load_files_read(struct mz0380_dev *dev, u16 offset,
				  u8 out[MZ0380_LOAD_FILES_CHUNK])
{
	u32 reply[MZ0380_MB_COMMAND_WORDS] = { 0 };
	u32 params[5];
	unsigned int i;
	int ret;

	params[0] = (u32)offset << 16;	/* is_write = 0 in the low half */
	/*
	 * Sentinel, not zero-fill. If the card cannot open PIC_ENC it prints
	 * "[LOGO_LOAD] cannot open %s" and returns WITHOUT touching the
	 * payload, so the command still completes and whatever we sent comes
	 * straight back. A zero payload therefore cannot distinguish "the file
	 * is 256 zero bytes" from "the file does not exist". 0xA5 can.
	 */
	for (i = 1; i < ARRAY_SIZE(params); i++)
		params[i] = 0xa5a5a5a5;

	ret = mz0380_send_command_reply(dev, MZ0380_CMD_LOAD_FILES,
					params, ARRAY_SIZE(params), NULL, 2000,
					reply, ARRAY_SIZE(reply));
	if (ret)
		return ret;

	/*
	 * reply[0] is the PARAM0 slot, which is the OPCODE word (reg.h:
	 * MZ0380_MB_OPCODE == MZ0380_MB_PARAM(0) == 0x04). Our params[0]
	 * therefore comes back in reply[1], and the 16 data bytes - card
	 * struct[8..23] - in reply[2..5].
	 */
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		out[i] = reply[2 + (i / 4)] >> ((i % 4) * 8);

	return 0;
}

/*
 * M76: write 16 bytes to the card's /mnt/flash/PIC_ENC via LOAD_FILES.
 *
 * vcm opens "wb+" when the offset is 0 (create/truncate) and "ab" otherwise,
 * so offset 0 is the only one that reliably lands where we asked. Used solely
 * by the cardlog_probe self-test: an untouched read sentinel cannot tell
 * "the file is missing" from "vcm never serviced the command", and a
 * write-then-read-back can.
 *
 * This is the only thing in-tree that writes to the card's flash. PIC_ENC is
 * the encoder's logo scratch file, not boot-critical, and the probe truncates
 * it to the 16 bytes it writes.
 */
static int mz0380_load_files_write(struct mz0380_dev *dev, u16 offset,
				   const u8 in[MZ0380_LOAD_FILES_CHUNK])
{
	u32 params[5] = { 0 };
	unsigned int i;

	params[0] = 1u | ((u32)offset << 16);	/* is_write = 1 */
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		params[1 + (i / 4)] |= (u32)in[i] << ((i % 4) * 8);

	return mz0380_send_command(dev, MZ0380_CMD_LOAD_FILES, params,
				   ARRAY_SIZE(params), NULL, 2000);
}

static void mz0380_cardlog_probe(struct seq_file *m, struct mz0380_dev *dev)
{
	static const u8 pattern[MZ0380_LOAD_FILES_CHUNK] = {
		'M', 'Z', '0', '3', '8', '0', '-', 'M',
		'7', '7', '-', 'P', 'R', 'O', 'B', 'E',
	};
	u8 back[MZ0380_LOAD_FILES_CHUNK];
	unsigned int i;
	int ret;

	seq_puts(m, "probe: writing a 16-byte pattern to PIC_ENC offset 0, then reading it back\n");

	ret = mz0380_load_files_write(dev, 0, pattern);
	if (ret) {
		seq_printf(m, "probe: write failed: %d\n", ret);
		return;
	}

	ret = mz0380_load_files_read(dev, 0, back);
	if (ret) {
		seq_printf(m, "probe: read-back failed: %d\n", ret);
		return;
	}

	seq_puts(m, "probe: read back |");
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		seq_putc(m, (back[i] >= 0x20 && back[i] < 0x7f) ? back[i] : '.');
	seq_puts(m, "|\n");

	if (!memcmp(back, pattern, sizeof(pattern)))
		seq_puts(m, "probe: PASS - vcm services op 0x6e and PIC_ENC is readable and writable\n");
	else if (back[0] == 0xa5 && !memchr_inv(back, 0xa5, sizeof(back)))
		seq_puts(m, "probe: FAIL - sentinel untouched, vcm never serviced the command\n");
	else
		seq_puts(m, "probe: MISMATCH - the command was serviced but the bytes differ\n");
}

static int mz0380_proc_cardlog_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		unsigned int off;

		seq_printf(m, "%s: /mnt/flash/PIC_ENC via LOAD_FILES(0x6e), %u bytes\n",
			   dev->name, mz0380_cardlog_bytes);

		if (mz0380_cardlog_probe_enabled)
			mz0380_cardlog_probe(m, dev);

		for (off = 0; off < mz0380_cardlog_bytes;
		     off += MZ0380_LOAD_FILES_CHUNK) {
			u8 buf[MZ0380_LOAD_FILES_CHUNK];
			unsigned int i;
			int ret;

			if (off > U16_MAX)
				break;

			ret = mz0380_load_files_read(dev, off, buf);
			if (ret) {
				seq_printf(m, "%04x: <read failed: %d>\n",
					   off, ret);
				break;
			}

			if (buf[0] == 0xa5 && !memchr_inv(buf, 0xa5,
							  sizeof(buf))) {
				seq_printf(m, "%04x: <untouched sentinel - the card did not read the file>\n",
					   off);
				break;
			}

			seq_printf(m, "%04x:", off);
			for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
				seq_printf(m, " %02x", buf[i]);
			seq_puts(m, "  |");
			for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
				seq_putc(m, (buf[i] >= 0x20 && buf[i] < 0x7f) ?
					 buf[i] : '.');
			seq_puts(m, "|\n");
		}
	}
	mutex_unlock(&devlist);
	return 0;
}

static int mz0380_proc_cardlog_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz0380_proc_cardlog_show, NULL);
}

static struct proc_ops mz0380_proc_cardlog_fops = {
	.proc_open = mz0380_proc_cardlog_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*
 * M76: raw dump of stream buffer 0.
 *
 * The poison scan proved the card writes ~3.1 MB into buf0 on the REAL
 * (is_nosg=0) path while the receiver holds lock - head bytes 0x11, i.e. the
 * card's own NO-SIGNAL splash, not H.264. Identifying that content offline is
 * the difference between "the encoder produced nothing" and "the encoder
 * produced the wrong picture", so expose the buffer verbatim.
 */
static ssize_t mz0380_proc_buf0_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	ssize_t ret = 0;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		if (!dev->stream_bufs[0].va)
			continue;
		ret = simple_read_from_buffer(buf, count, ppos,
					      dev->stream_bufs[0].va,
					      MZ0380_STREAM_BUF_SIZE);
		break;
	}
	mutex_unlock(&devlist);
	return ret;
}

static struct proc_ops mz0380_proc_buf0_fops = {
	.proc_read = mz0380_proc_buf0_read,
	.proc_lseek = default_llseek,
};

static void mz0380_proc_remove(void)
{
	remove_proc_entry("mz0380-cardlog", NULL);
	remove_proc_entry("mz0380-buf0", NULL);
	remove_proc_entry("mz0380-cmd", NULL);
	remove_proc_entry("mz0380-hdmi", NULL);
	remove_proc_entry("mz0380-events", NULL);
	remove_proc_entry("mz0380-periph-scan", NULL);
	remove_proc_entry("mz0380-scan", NULL);
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

	pe = proc_create("mz0380-cardlog", 0400, NULL,
			 &mz0380_proc_cardlog_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-buf0", 0400, NULL, &mz0380_proc_buf0_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-scan", 0444, NULL, &mz0380_proc_scan_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-periph-scan", 0444, NULL,
			 &mz0380_proc_periph_scan_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-events", 0644, NULL, &mz0380_proc_events_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-hdmi", 0644, NULL, &mz0380_proc_hdmi_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-cmd", 0644, NULL, &mz0380_proc_cmd_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	return 0;
}

/*
 * Mailbox command send.
 *
 * Protocol confirmed against both the Windows transport and ep.ko:
 *   1. clear the shared PARAM10/STATUS word before installing payload
 *   2. write opcode to PARAM0 and up to ten arguments to PARAM1..PARAM10
 *   3. ring BAR0+0x00 with 0x800
 *   4. unless explicitly asynchronous, wait for STATUS or EVENT completion;
 *      a full-width command can wait only for EVENT because STATUS is payload
 *
 * The public helper serialises transactions with dev->cmd_lock. Peripheral
 * reads use the internal locked form so the lock also covers their late result.
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

static void mz0380_mb_snapshot_reply(struct mz0380_dev *dev)
{
	unsigned int i;

	BUILD_BUG_ON(MZ0380_MB_COMMAND_WORDS > MZ0380_REG_PARAM_MAX);
	memset(dev->cmd_last_param, 0, sizeof(dev->cmd_last_param));
	for (i = 0; i < MZ0380_MB_COMMAND_WORDS; i++)
		dev->cmd_last_param[i] =
			mz_mmio_read(dev, MZ0380_MB_PARAM(i));
}

void mz0380_mb_ack_event(struct mz0380_dev *dev)
{
	unsigned long flags;
	u32 event;

	/*
	 * ACK re-arms the endpoint and permits it to overwrite TOKEN/PAYLOAD.
	 * Capture their frame-bearing state first, regardless of whether the ACK
	 * came from the MSI ISR, command poller, boot waiter, or stale-event drain.
	 * The poller and ISR can observe the same one-shot EVENT concurrently, so
	 * serialize the complete snapshot/rearm sequence.  Otherwise one context
	 * could clear and rearm the mailbox while the other is still reading the
	 * old event's payload.
	 */
	spin_lock_irqsave(&dev->event_lock, flags);
	event = mz_mmio_read(dev, MZ0380_MB_EVENT);
	if (event && event != U32_MAX) {
		/*
		 * Keep the command reply paired with the same EVENT as the frame
		 * payload.  Reading these in the ISR before taking event_lock lets a
		 * polling CPU ACK/rearm between EVENT and the reply snapshot.
		 */
		if (event & MZ0380_MB_EVENT_CMD_DONE) {
			dev->cmd_last_status =
				mz_mmio_read(dev, MZ0380_MB_STATUS);
			mz0380_mb_snapshot_reply(dev);
			smp_store_release(&dev->cmd_complete, true);
			wake_up_all(&dev->cmd_wait);
		}
		mz0380_handle_event_snapshot(dev, event);
	}
	mz_cfg_write(dev, MZ0380_CFG_INT_FLAG, MZ0380_CFG_INT_ACK_VAL);
	mz_mmio_write(dev, MZ0380_MB_EVENT, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_INT_ACK);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/*
 * M118: the credit re-arm, lifted out of the ISR so the poll-drain can fire the
 * same sequence. The card's completion channel is a one-shot: msi_enable is
 * consumed when it posts an event and only this doorbell restores it. On the
 * poll path no event ever posts, so nothing has ever re-armed it.
 */
void mz0380_credit_rearm(struct mz0380_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	mz_cfg_write(dev, MZ0380_CFG_INT_FLAG, MZ0380_CFG_INT_ACK_VAL);
	mz_mmio_write(dev, MZ0380_MB_EVENT, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_INT_ACK);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}
EXPORT_SYMBOL_GPL(mz0380_credit_rearm);

/* dev->cmd_lock must remain held until any opcode-specific late reply is read. */
static int mz0380_send_command_locked(struct mz0380_dev *dev, u32 opcode,
				       const u32 *params,
				       unsigned int nparams,
				       u32 *status_out,
				       unsigned int timeout_ms)
{
	unsigned int i;
	u32 status = 0;
	bool ack_slot_is_payload;
	int ret = 0;

	/*
	 * RE-confirmed mailbox model (BAR0). SEND_COMMAND writes the opcode to
	 * the PARAM0 slot (BAR0+0x04), the arguments to the following slots,
	 * then fires the doorbell (BAR0+0x00 = 0x800). BAR0+0x2c is both PARAM10
	 * and STATUS; Windows sends commands which use all ten arguments through
	 * its EVENT-wait path, which skips only the overlapping STATUS poll.
	 * PARAM11 would be EVENT and must never be written as payload.
	 */
	BUILD_BUG_ON(MZ0380_MB_PARAM(MZ0380_MB_MAX_ARGS) !=
		     MZ0380_MB_STATUS);
	BUILD_BUG_ON(MZ0380_MB_PARAM(MZ0380_MB_MAX_ARGS + 1) !=
		     MZ0380_MB_EVENT);

	lockdep_assert_held(&dev->cmd_lock);
	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;
	ack_slot_is_payload = nparams == MZ0380_MB_MAX_ARGS;
	if (status_out)
		*status_out = 0;

	/*
	 * Drain a stale unacked event (e.g. from a previous module life) before
	 * assigning a new command to cmd_complete. mz0380_mb_ack_event() takes
	 * the frame/token snapshot before it clears EVENT, so this cannot discard
	 * a late frame completion.
	 */
	{
		u32 stale = mz_mmio_read(dev, MZ0380_MB_EVENT);

		WRITE_ONCE(dev->cmd_complete, false);
		if ((stale && stale != U32_MAX) ||
		    mz_cfg_read(dev, MZ0380_CFG_INT_FLAG) == 1) {
			mz0380_mb_ack_event(dev);
			pr_info("%s: drained stale EVENT=0x%08x before command 0x%x\n",
				dev->name, stale, opcode);
		}
		/* Retire an ISR which observed the old EVENT before the drain. */
		if (dev->irq_requested)
			synchronize_irq(dev->irq);
	}

	/*
	 * Clear the completion latch before any command words are installed.
	 * In particular, do not clear it after PARAM10: for a full-width command
	 * that same write would destroy its final payload word. The firmware's
	 * 0xaaaaaaaa/0xdddddddd values are replies/initial state, not immutable
	 * stamps; retaining an old one would falsely complete the next command.
	 */
	mz_mmio_write(dev, MZ0380_MB_STATUS, 0);
	WRITE_ONCE(dev->cmd_complete, false);
	smp_wmb();

	/* opcode -> PARAM0 (BAR0+0x04), args -> PARAM1..PARAM10 */
	mz_mmio_write(dev, MZ0380_MB_OPCODE, opcode);
	for (i = 0; i < nparams; i++)
		mz_mmio_write(dev, MZ0380_MB_PARAM(i + 1), params[i]);

	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_FIRE);

	/*
	 * timeout_ms == 0 is the only fire-and-forget form. A command occupying
	 * PARAM10 cannot poll that shared word, but a non-zero timeout still waits
	 * for EVENT CMD_DONE, matching Windows' semaphore path for SET_BUF.
	 */
	if (!timeout_ms)
		return 0;

	{
		/*
		 * Short commands poll STATUS even when MSI is enabled: they can finish
		 * through bit0 without an interrupt. Full-width commands skip STATUS
		 * because it is PARAM10 and wait exclusively for EVENT CMD_DONE.
		 * dev->cmd_complete covers an event consumed first by the ISR; this
		 * loop also consumes live events so it works without IRQ delivery.
		 * Ack only a real event—the old ack-every-tick scheme could abort a
		 * STATUS-completing command.
		 */
		unsigned int waited = 0;
		unsigned int max_wait = max(timeout_ms,
					    (unsigned int)MZ0380_MB_POLL_ITERS);
		bool done = false;
		u32 event;

		do {
			if (smp_load_acquire(&dev->cmd_complete)) {
				if (!ack_slot_is_payload)
					status = dev->cmd_last_status;
				done = true;
				break;
			}
			if (!ack_slot_is_payload)
				status = mz_mmio_read(dev, MZ0380_MB_STATUS);
			/*
			 * Completion = bit0, or the firmware's 0xaaaaaaaa
			 * success stamp (GET_BOARD_VERSION/INIT). The
			 * 0xdddddddd boot stamp also has bit0 set but is NOT a
			 * completion.
			 */
			if (!ack_slot_is_payload &&
			    (status == MZ0380_MB_STATUS_OK_STAMP ||
			     ((status & MZ0380_MB_STATUS_DONE) &&
			      status != MZ0380_MB_STATUS_BOOT_STAMP))) {
				done = true;
				break;
			}
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event && event != U32_MAX) {
				bool cmd_done = event & MZ0380_MB_EVENT_CMD_DONE;

				/* Snapshot frame-bearing lanes before the ACK clears EVENT. */
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

		if (!done)
			ret = -ETIMEDOUT;

		/*
		 * A command that completed via STATUS bit0 may still post a
		 * trailing completion event a moment later. Give it a few ms and
		 * snapshot every frame-bearing event before ACKing; a frame-only
		 * event does not end this short trailing-command-event window.
		 */
		if (done) {
			for (waited = 0; waited < 10; waited++) {
				bool cmd_done;

				event = mz_mmio_read(dev, MZ0380_MB_EVENT);
				cmd_done = event && event != U32_MAX &&
					   (event & MZ0380_MB_EVENT_CMD_DONE);
				if (event && event != U32_MAX) {
					mz0380_mb_ack_event(dev);
					if (cmd_done)
						break;
				} else if (mz_cfg_read(dev,
						       MZ0380_CFG_INT_FLAG) == 1) {
					/* No EVENT payload is live; only rearm INTx. */
					mz0380_mb_ack_event(dev);
					break;
				}
				usleep_range(300, 500);
			}
		}

		/*
		 * Preserve a reply already paired with CMD_DONE by
		 * mz0380_mb_ack_event().  Once that ACK rearms the endpoint, a live
		 * reread is no longer tied to the event we just consumed.  STATUS-only
		 * completions have no such snapshot, so take one while excluding the
		 * ISR's EVENT snapshot/ACK sequence.
		 */
		{
			unsigned long flags;

			spin_lock_irqsave(&dev->event_lock, flags);
			if (!smp_load_acquire(&dev->cmd_complete))
				mz0380_mb_snapshot_reply(dev);
			spin_unlock_irqrestore(&dev->event_lock, flags);
		}
	}

	if (status_out)
		*status_out = status;
	return ret;
}

/* Copy shared reply storage before cmd_lock permits another transaction. */
int mz0380_send_command_reply(struct mz0380_dev *dev, u32 opcode,
			       const u32 *params, unsigned int nparams,
			       u32 *status_out, unsigned int timeout_ms,
			       u32 *reply, unsigned int reply_words)
{
	int ret;

	if (reply_words > MZ0380_MB_COMMAND_WORDS ||
	    (reply_words && !reply))
		return -EINVAL;
	if (status_out)
		*status_out = 0;
	if (reply_words)
		memset(reply, 0, reply_words * sizeof(*reply));
	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;

	mutex_lock(&dev->cmd_lock);
	ret = mz0380_send_command_locked(dev, opcode, params, nparams,
					 status_out, timeout_ms);
	if (reply_words) {
		if (timeout_ms && nparams != MZ0380_MB_MAX_ARGS)
			memcpy(reply, dev->cmd_last_param,
			       reply_words * sizeof(*reply));
	}
	mutex_unlock(&dev->cmd_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_send_command_reply);

int mz0380_send_command(struct mz0380_dev *dev, u32 opcode,
			const u32 *params, unsigned int nparams,
			u32 *status_out, unsigned int timeout_ms)
{
	int ret;

	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;

	mutex_lock(&dev->cmd_lock);
	ret = mz0380_send_command_locked(dev, opcode, params, nparams,
					 status_out, timeout_ms);
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
	u32 init_reply[2] = { 0 };
	u32 status = 0;
	int ret = -ETIMEDOUT;

	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR0, lower_32_bits(bar0) + 0x04);
	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR1, lower_32_bits(bar0) + 0x5f);
	wmb();
	mz0380_mb_ack_event(dev);

	for (attempt = 0; attempt < 10 && ret; attempt++)
		ret = mz0380_send_command_reply(
			dev, MZ0380_CMD_INIT, NULL, 0, &status, 600,
			init_reply, ARRAY_SIZE(init_reply));
	if (ret) {
		pr_warn("%s: CMD_INIT got no answer (%d), STATUS=%08x EVENT=%08x RESULT=%08x bar5[dc]=%08x bar5[30]=%08x bar5[38]=%08x\n",
			dev->name, ret,
			status,
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			init_reply[1],
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

		/*
		 * The version words can settle after command completion. Keep the
		 * mailbox transaction locked through that delay and extraction so a
		 * concurrent GPIO/I2C command cannot replace PARAM1/PARAM2.
		 */
		mutex_lock(&dev->cmd_lock);
		ret = -ETIMEDOUT;
		for (attempt = 0; attempt < 10 && ret; attempt++)
			ret = mz0380_send_command_locked(
				dev, MZ0380_CMD_GET_BOARD_VERSION, params, 2,
				&status, 5000);
		if (!ret) {
			msleep(100);
			dev->fw_version_major =
				mz_mmio_read(dev, MZ0380_MB_PARAM(1));
			dev->fw_version_minor =
				mz_mmio_read(dev, MZ0380_MB_PARAM(2));
			dev->cmd_last_param[1] = dev->fw_version_major;
			dev->cmd_last_param[2] = dev->fw_version_minor;
		}
		mutex_unlock(&dev->cmd_lock);
	}
	if (ret) {
		pr_warn("%s: GET_BOARD_VERSION got no answer (%d)\n",
			dev->name, ret);
		return ret;
	}

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
/*
 * The card's userspace I2C proxy (yuan_ioctrl) writes the read result into the
 * PARAM3 slot (BAR0+0x10) *after* it posts command completion, so the snapshot
 * send_command() takes at the completion edge can still hold the previous
 * command's result (off-by-one on back-to-back reads). The firmware writes only
 * the low BYTE of that slot, so we pre-load a distinctive sentinel byte and
 * poll the low byte until the firmware replaces it (NAK -> 0x00, ACK -> value).
 * A real register may itself equal one sentinel.  Retry once with its inverse;
 * no byte can equal both values, so a legitimate 0xa5/0x5a reply is not
 * misreported as a timeout.
 */
#define MZ0380_PERIPH_READ_SENTINEL0	0xa5u
#define MZ0380_PERIPH_READ_SENTINEL1	0x5au
#define MZ0380_PERIPH_READ_POLL_US	500
#define MZ0380_PERIPH_READ_POLL_ITERS	60	/* ~30 ms budget for the result */

int mz0380_periph_read(struct mz0380_dev *dev, u8 chip, u8 reg, u32 *val)
{
	static const u8 sentinels[] = {
		MZ0380_PERIPH_READ_SENTINEL0,
		MZ0380_PERIPH_READ_SENTINEL1,
	};
	u32 params[3] = { chip, reg, 0 };
	u32 result = 0;
	unsigned int attempt, i;
	int ret = -ETIMEDOUT;

	/*
	 * Keep cmd_lock across both command completion and the proxy's late
	 * low-byte store. Otherwise a second mailbox command may replace PARAM3
	 * between send_command() unlocking and this result poll.
	 */
	mutex_lock(&dev->cmd_lock);
	for (attempt = 0; attempt < ARRAY_SIZE(sentinels); attempt++) {
		params[2] = sentinels[attempt];
		ret = mz0380_send_command_locked(dev, MZ0380_CMD_REG_READ,
						 params, ARRAY_SIZE(params),
						 NULL, 1000);
		if (ret) {
			pr_info("%s: REG_READ chip=0x%02x reg=0x%02x failed (%d), STATUS=%08x EVENT=%08x RESULT=%08x P3=%08x\n",
				dev->name, chip, reg, ret,
				mz_mmio_read(dev, MZ0380_MB_STATUS),
				mz_mmio_read(dev, MZ0380_MB_EVENT),
				mz_mmio_read(dev, MZ0380_MB_RESULT),
				mz_mmio_read(dev, MZ0380_MB_PARAM(3)));
			goto out_unlock;
		}

		for (i = 0; i < MZ0380_PERIPH_READ_POLL_ITERS; i++) {
			result = mz_mmio_read(dev, MZ0380_MB_PARAM(3));
			if ((result & 0xff) != sentinels[attempt]) {
				ret = 0;
				goto have_result;
			}
			usleep_range(MZ0380_PERIPH_READ_POLL_US,
				     MZ0380_PERIPH_READ_POLL_US * 2);
		}
	}

	ret = -ETIMEDOUT;
	pr_warn("%s: REG_READ chip=0x%02x reg=0x%02x result timed out with both sentinels after %u polls each (P3=%08x STATUS=%08x EVENT=%08x)\n",
		dev->name, chip, reg, MZ0380_PERIPH_READ_POLL_ITERS,
		result, mz_mmio_read(dev, MZ0380_MB_STATUS),
		mz_mmio_read(dev, MZ0380_MB_EVENT));
	goto out_unlock;

have_result:
	dev->cmd_last_param[3] = result;
	if (val)
		*val = result & 0xff;

out_unlock:
	mutex_unlock(&dev->cmd_lock);
	return ret;
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
	 * Mask INTx for the duration of probe: nothing is registered to service
	 * the line yet, and a pending assert on a shared line would spin the
	 * kernel in unclaimed-interrupt handling between event and ack.
	 * mz0380_irq_request() unmasks it again once an ISR exists, which on
	 * the default INTx path (M82) is required for any interrupt at all.
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
	 * Phase 1: card handshake. Nothing is uploaded - the card boots
	 * its own flash image; we only shake hands and read its version.
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
	mz0380_irq_release(dev);
	mz0380_dma_teardown(dev);
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

	/* stop the event watcher before any MMIO mapping is torn down */
	mz0380_event_watch_stop(dev);

	mz0380_nosg_capture_stop(dev);	/* join before the bufs it polls die */
	mz0380_dma_stop(dev);
	/*
	 * M84: free the IRQ BEFORE any MMIO mapping goes away. This was the
	 * other way round, which was survivable only because we ran on MSI: an
	 * MSI source stops signalling once the device is quiesced, so the
	 * handler was never entered after the unmap. On the shared INTx line
	 * the Windows driver uses, every other device on the line enters our
	 * handler, and the first one to do so between mz0380_dev_unregister()
	 * and free_irq() dereferenced a NULL bmmio and oopsed inside rmmod -
	 * leaving the module wedged in MODULE_STATE_GOING (refcnt -1), which
	 * only a reboot clears.
	 */
	mz0380_irq_release(dev);
	mz0380_dev_unregister(dev);
	mz0380_dma_teardown(dev);
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
/*
 * The only file this driver ever asks the firmware loader for: the 6-byte
 * ASCII version sidecar, read so a version mismatch can be reported. No image
 * is ever sent to the card.
 */
MODULE_FIRMWARE("mz0380/MZ0380.FW.TXT");

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
