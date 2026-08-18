# NEXT SESSION START

_Last updated 2026-08-18. Full history in **RE_FINDINGS.md**; this file is
only the handoff. Everything below was verified on hardware unless it says
otherwise._

---

## The one-paragraph state of the project

HDMI detection is **finished**. From a card that had never seen a signal, the
driver now brings up the receiver, holds lock on a live source continuously,
measures it coherently, matches it against the mode table and reports a
correct V4L2 preset - for both modes the test source produces (1080p60 VIC 16
and 1080p30 VIC 34). `STREAMON` arms the real BT1120 -> H.264 path with the
right geometry. The single remaining blocker is that **the card's own capture
library never delivers a frame to the encoder**: with the receiver locked for
59 seconds and every host-side parameter verified or swept, the host buffers
stay untouched (`0/1024 pages`), `enc_stat` never sets. The host-side search
space is exhausted; the card knows why and prints it to a console we cannot
read.

---

## The two root causes fixed this session (both were long-standing)

1. **HPD polarity was inverted for the entire life of the project.**
   `FUN_14024eeb8` in the Windows driver (Elgato branch, board id 0x1c/0xfa)
   computes `pin1 = ~(arg>>4) & 1` - the board HPD pin is **ACTIVE-LOW**, so
   HPD_ON drives it LOW. We drove it high to "assert". This single bug
   produced every source symptom on record: the camera reacting to an edge
   then falling back to its LCD, the microscope transmitting only a ~300 ms
   burst at its own power-up, hotplug pulses doing nothing, and plausibly the
   0xA0 EDID NAKs (HPD was asserted during our EDID writes, leaving the source
   free to master the DDC lines). Fixed -> the source now stays locked
   indefinitely. See `MZ0380_GPIO_HPD` in `mz0380-reg.h`.

2. **The SET_VIC field map was wrong in three fields.** Decoded from the
   card's own log string (`video_capture_mgr` .rodata 0xb734) argument-by-
   argument under AAPCS, cross-checked against the SDK capture config
   `re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg`, which documents every
   enum inline. Authoritative map now in `mz0380_dma_start()`.

---

## What is PROVEN - do not re-litigate

1. **Host DMA/streaming stack is correct.** The card's synthetic path
   (`stream_nosg=1`) DMAs real frames into our buffers through `SET_BUF`
   opcode 0x02. Buffers, IOVA remap, addressing all work.
2. **NOSG means NO-SIGNAL, not no-scatter-gather.** `is_nosg` selects the
   card's synthetic black/logo frame generator (`NOSG_LOGO_YUV422`,
   `/tmp/PIC_NOSG`). It never touches live HDMI. Do not use it to test capture.
3. **Detection is complete and correct.** Lock gate is R55 core bits `0x1c`
   (bit 0x20 flaps independently and rejected good samples for three straight
   runs); vtotal stability is a full-value delta, not the old low-byte XOR
   mask; interlace comes from geometry (`lines` vs `vtotal` vs `vtotal/2`),
   because R5F bits proved unreliable in both directions; the coherence check
   is lines-vs-vtotal, valid for both field orders. A self-consistent sample
   is kept even if the source stops during the trailing reads.
4. **The receiver's output stage is equivalent to Windows.** Read back live:
   `ab=15` (bit7 clear, not frozen), `b0=21`, `b1=c0`, `b7=00`, `51=89`,
   BANK2 `01=61 02=f5 07=04`. The Windows commit sequence is byte-matched
   (`win64.txt` 0x14024ed8a: `orb $0x21` on 0xb0, then 0xab; the BANK1
   0x01/0x34 accesses that follow are READS computing a return status).
5. **The host->card video control surface is COMPLETE.** `video_capture_mgr`
   dispatches only opcodes **7, 41 (SET_VIC), 42 (SET_AIC), 96, 97, 110**;
   `yuan_ioctrl` only **0x18-0x22** (I2C/SPI). There is no video command we
   are failing to send. Our `SET_ENC_PARAMS(0x2d)` is silently dropped by the
   card and is *not* a gate.
6. **The DMA destination opcode is not the blocker.** Windows builds the same
   12-word SET_BUF command with 0x04/0x05 (`win64.txt` 0x140279219,
   0x140279663). Swept 2/4/5/8 against a live locked source: all `0/1024`.
7. **Our own diagnostic is not the blocker.** The watch used to re-arm
   AUTO_POSITION mid-capture (now suppressed while streaming). A control run
   with `WATCH=0` - zero receiver I2C during capture - behaves identically.

---

## The blocker, stated precisely

`tinyvenc5` opens the VIC itself and its capture loop calls
`VideoCap_GetBuf` -> `VideoCap_GetBufVIC`; on failure it prints
`[VIDEOCAP][ERROR]: No signal !!`. So the SoC's video input controller reports
no signal on BT1120 while the MST3367 is locked and configured to drive it.
`VideoCap_CheckVIC` is only a driver-version check, not signal detection.

Observable state at failure, every run:
```
receiver:  R55=7f LOCKED before START, after START, and 59s later at stop
encoder:   SET_VIC(fw=5 in_fmt=6 out_fmt=1) ret=0, SET_AIC ret=0, START fired
card:      EVENT=0 token=0 enc_stat=0 frame_events=0 fifo_drops=0
buffers:   0/1024 sampled pages touched on all four
```

---

## Start here

**Recommended: get the card's serial console.** Every remaining question is
answered by one line of the card's own log. These Yuan/Mozart boards bring the
SoC UART to a header or test pads; a 3.3 V USB-TTL adapter at 115200 8N1 gives
the boot log plus live output from `video_capture_mgr` and `tinyvenc5`,
including which VideoCap call fails and with what parameters. No bootargs or
console device were found in the firmware image, so the port has to be located
on the board.

**Software-only alternatives, in order of value:**

1. Disassemble `re-dump/fw/yuan_demo_sdi/libvideocap.so.13` - what
   `VideoCap_GetBufVIC` requires, and which vpl_vic ioctl reports no-signal.
   This is the only unexplored binary in the chain.
2. Sweep the remaining guessed SET_VIC values: `m` / `vic_out_format` (0/1/2),
   `vanc_lines`, `is_slave`, `fast_kill`. Low yield - `m=0` and `m=1` both
   already produce nothing.
3. The unexplored I2C device: our bus scan finds **0x98** answering (besides
   the MST3367 at 0x9c), and Windows bulk-writes 18 bytes to it at sub-address
   0x76 plus a byte at 0x73 (`win64.txt` 0x140260cec/0x140260d12, payloads at
   0x14033e938 / 0x140340b5c). We have never written it. Its single call site
   sits in what looks like an audio/DSP path, so this is a long shot.

Run:
```bash
sudo ./mz0380-m55-real-capture.sh          # real path, prompts for power-cycle
sudo WATCH=0 ./mz0380-m55-real-capture.sh  # control: no receiver I2C at all
sudo ./mz0380-m75-setbuf-sweep.sh          # sweep SET_BUF opcodes 2/4/5/8
```

---

## Gotchas that cost time

- **The shell is fish.** `for ... do ... done` hangs waiting for input. Put
  loops in a script.
- **The card wedges after ~8-18 encoder spawns** and only a **mains-off cold
  boot** recovers it (M52 proved pm/bus reset and remove+rescan all fail; slot
  standby power keeps the SoC alive through a soft-off). A wall of
  `SET_VIC ret=-110` is the wedge, not a capture bug - the M55 script detects
  and announces it. Keep `FRAMES` small (default 6).
- **The script runs `make` as root**, leaving root-owned `.o`/`.ko`/
  `modules.order`/`.module-common.o` that break the next non-root build with
  `Operation not permitted`. Delete them or build as root.
- **Always reload the module** - a stale one silently lacks new params.
- **`dmesg -C` between script steps** erases earlier evidence; the EDID
  verdict and output-stage lines are surfaced deliberately for this reason.

---

## Module parameters added this session

| param | default | purpose |
|---|---|---|
| `signal_cache_ms` | 30000 | reuse last good detection to arm STREAMON (a burst source can never be locked at the exact STREAMON instant) |
| `signal_confirm` | 0 | require two agreeing measurement passes (costs ~170 ms of lock; off because a bursty source cannot supply it) |
| `vic_fw` | 5 | SET_VIC byte6 encoder selector (5=tinyvenc5, 7, 8) |
| `vic_out_format` | 1 | SET_VIC byte12 `m` - 1:YUV420, 2:YUV422 (0 is illegal and was sent for years) |
| `vic_saturation` | 128 | SET_VIC byte18 - 128 neutral, 0 mono |
| `vic_b0` | 0x21 | MST3367 BANK0 0xb0 output select (hdcapm uses 0x20) |
| `set_buf_opcode` | 2 | which opcode programs DMA destinations (Windows uses 4/5) |
| `aic_every_frame` | 0 | re-send SET_AIC per spawn instead of once per session |

---

## Method notes

Several dead ends this session were mine, not the hardware's: an interlace
flag changed on inference that the SDK config later contradicted (the cfg file
was in the repo the whole time and should have been the first stop), a
suggestion to test capture with NOSG when NOSG means no-signal, and three
consecutive "strongest lead" fixes that came back negative. The eliminations
are real progress and each was cheap, but the hit rate says the remaining
hypothesis space is no longer well-constrained by host-side evidence - which
is the argument for the serial console over more sweeping.
