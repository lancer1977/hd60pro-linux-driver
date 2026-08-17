# NEXT SESSION START

_Last updated 2026-08-15. Full history in **RE_FINDINGS.md** (M0..M56);
this file is only the handoff. Everything below was verified on hardware
unless it says otherwise._

---

## The one-paragraph state of the project

Capture works. `stream_nosg=1` delivers real NV12 frames to userspace through
V4L2 (M50, verified: 4/4 frames, 0 faults) - the first working Linux capture
from this card. A source has finally been seen on the wire: a digital
microscope transmits and the MST3367 reached lock (M54), which retired the
old "nothing ever gets to the receiver" era. The remaining work is on the
real capture path: the receiver sees the source's clock but does not hold
frame lock, and the last change (a full port of hdcapm's init sequence)
is the untested fix for exactly that.

---

## What is PROVEN - do not re-litigate

1. **Host DMA/streaming stack is correct.** One complete frame per stream
   start, contiguous, zero loss, zero IOMMU faults. (M35-M39)
2. **Working capture, fake path.** `stream_nosg=1` -> NV12 **720x1080**
   polled capture (`mz0380_nosg_thread`). The frame is the card's own
   rendered "NO SIGNAL" splash: Y 720x1080 @0, UV 720x540 @0xbdd80 (all
   0x80), 0xff junk after. Renderer draws 720 wide regardless of SET_VIC.
   `start_delay_ms=500` works (~1.9 s/frame). (M50)
3. **HPD is genuinely driven.** pin1 read-back follows every assert/deassert,
   and the receiver's own 0xb7 bit follows. (M54)
4. **A source can lock.** Digital microscope: `detect 0x55=0xa3 LOCKED`,
   htot=0x898. It needs no EDID from us. **Use it as the standard test
   source.** The DSLR stays dark (htot=0000) - that is the EDID blocker
   and is source-specific, NOT a driver blocker. (M54)
5. **The receiver's detect map** is hdcapm's, now used verbatim: match on
   htotal/vtotal/hperiod/vperiod ranges + interlace (NOT hactive), with
   hperiod=1600000/raw, vperiod=1250000/raw, field masks applied. (M56)

## What is DISPROVEN - dead ends, do not retry

- **Bit-banged I2C over host GPIO (M51/M51b).** Pins reading HIGH at idle are
  driven high as OUTPUTS by our own bring-up; switched to input they drop
  low, so no pull-ups, not I2C lines. Pairs that toggled cleanly scanned
  0x08..0x77 with ZERO ACKs under both DIR polarities.
- **No host-reachable second I2C bus.** `ep.ko` (the mailbox server) contains
  no I2C code at all - only GPIO and streaming ops. The I2C helpers live in
  card userspace (fd-parameterised); the card opens `/dev/i2c-0` AND
  `/dev/i2c-1`, so bus 1 (the EEPROM bus per hdcapm) is card-side only.
- **EDID delivery, host side, is exhausted**: 5 opcodes negative (M47), no
  EEPROM on the receiver bus (M43), no EDID-sized writable window (M49b),
  no indirect address/data port (M53 - though see the caveat below).
- Older dead ends unchanged: low-32-DMA-loss (M29), aperture offset (M27/28),
  extra outbound window (M32), audio_ready gate (M33), dead credit (M35).

---

## Two known hardware gotchas that will waste a session if forgotten

1. **The card wedges after ~8-18 encoder spawns.** The mailbox stops ACKing
   everything - `CMD_INIT -110`, `BEGIN_FW_DL -110`, so firmware cannot even
   re-upload. `rmmod`/`insmod` does NOT recover it. **Only a mains-off cold
   boot does** (full shutdown, PSU switch off / cable out ~10 s; a warm
   reboot keeps PCIe aux power). Check `uptime` before believing any
   "everything fails" result. `mz0380-m52-card-recovery.sh` tries the PCI
   pm/bus/rescan resets first - it has never been run to completion.
2. **The scripts must always rmmod/insmod the fresh build.** A stale module
   silently lacks new /proc commands. The handler now rejects unknown
   commands and says so, instead of falling through to the numeric parser.

---

## THE CURRENT BLOCKER: the receiver will not hold frame lock

With the microscope attached, hardware showed:

    detect 55=81 no-lock | 5f=42 hper=1289 vper=1fff htot=0898

Read that as: bit 0x80 set = the receiver **sees the source's clock**;
hperiod scales to 337, which is exactly hdcapm's **1080p30** range; but the
lock bits (`0x55 & 0x3c`) stay clear and the vertical-period counter is
saturated at 0x1fff, i.e. **vsync never completes**. Lock has been achieved
exactly once, transiently, right at a power-cycle.

**The untested fix is already committed (7b69e64).** Our MST3367 init turned
out to be a small subset of hdcapm's `mst3367_init_setup`, missing exactly
the blocks that would explain this: `RxTmdsInit` (the TMDS equaliser/PLL -
BANK1 0x17/0x18/0x19/0x1a, 0x2a, BANK2 0x08), `RxVideoInit` (low-pass filter
0xad, 0xb2/0xb3/0xb4), the audio block, the patch tail (0xe2 auto-position
off, 0x1e/0x1f/0x73/0xb5) and the CSC table. All ported in hdcapm's order.

### Start here

```bash
sudo ./mz0380-m55-real-capture.sh        # power-cycle the microscope when prompted
```

- `detect ... LOCKED` **and** `-> htot=.. vtot=.. => MATCHED` -> detection
  works; the capture that follows is the real path. This is the goal.
- `=> no table entry` -> it locked but its mode is not in hdcapm's table.
  The line carries every field needed; add the entry to `mst3367_modes[]`.
- Still no lock -> next suspect is the **per-mode timing/PLL programming**
  Windows computes at runtime (~69 writes, `sub_14024dc28`, RE_FINDINGS
  "INIT (CAVEAT)"): addresses/order/commit-reg are recovered, the VALUES are
  dynamic. With a live locking source we can now iterate against hardware
  instead of needing a Windows trace.

Note the source only transmits around a plug/power event - the detect window
must be OPEN while it is cycled. `signal_poll_ms` (def 2000) bounds the poll;
detect is sampled, not read once, because lock walks 0x83->0xa3->0x83->0x03
within a second while a source settles.

---

## Driver state (all compile-verified, zero warnings)

- **nosg polling capture** - `mz0380_nosg_thread`, NV12 720x1080, one
  encoder spawn per frame, quiet op7 between frames.
- **Full hdcapm MST3367 init** (7b69e64) - UNTESTED on hardware.
- **hdcapm detect map + mode table** - `mst3367_measure()` /
  `mst3367_match_mode()`; `watch` prints the scaled fields and whether the
  table matched on every locked sample.
- **GPIO direction repair** - the reset sequence forces HPD/RX_ENABLE/
  RX_STRAP/RX_RESET to outputs first (the M51 probe left them as inputs and
  wedged the receiver through rmmod); bring-up verifies the bus afterwards
  and retries with the opposite `GPIO_DIR` polarity, which is still unproven.
- **HPD read-back** - reports `PIN DID NOT FOLLOW` if a write did not stick.
- Params added this session: `nosg_frame_timeout_ms`, `gpio_dir_invert`,
  `edidhunt_max_regs`, `signal_poll_ms`, `force_timings`.
- `/proc/mz0380-hdmi`: `ramtest wscan edidhunt gpiodump i2cscan edidburn
  hpd edid watch` + `<input> <w> <h> <fps>`.

## Test scripts (each has a decision table in its header)

`m35` live tokens · `m36` extent+holes · `m38` repoison · `m39` respawn ·
`m42` real-signal · `m43` sink read-back · `m45` detect watch · `m47` EDID
opcodes · `m48` HPD · `m49` writability · **`m50` nosg NV12 capture** ·
`m51` i2c bit-bang · `m51b` GPIO hunt · `m52` card recovery · `m53` edidhunt ·
`m54` source trigger · **`m55` real capture**.

---

## If the real path lands, the ranked follow-ups are

1. Decode the delivered frames (H.264 NALs expected: `00 00 00 01`).
2. Per-mode receiver config if the picture is wrong (M10 step 2).
3. Audio (ALSA path is scaffolded, never exercised).
4. The DSLR/EDID question - only a live Windows I2C trace or an inline EDID
   emulator/HDMI splitter will make that camera transmit. It no longer
   blocks driver development now that the microscope works.
