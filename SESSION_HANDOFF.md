# MZ0380 Linux driver — session handoff (2026-07-05)

## Project
Clean-room Linux V4L2 driver for the **Elgato Game Capture HD60 Pro** (PCIe,
12ab:0380, subsys 1cfa:0006). Own hardware, RE for interoperability — not
security. Repo `/home/wolffyx/Projects/sc0710`. Active driver = `mz0380-*.c`;
the `sc0710-*.c` files are untracked upstream reference (a sibling 4k60 mk.2
card) — read for ideas, don't build them.

Card identity: **Yuan MZ0380 / SC3C0 family, SoC = MStar SL6010**. The vendor
stack is Yuan QCAP (device name "MZ0380 PCI"). Firmware v01.11.

## The one-paragraph story
The card boots, the PCIe command mailbox works (GET_VERSION, GPIO, op41 all
complete). For weeks the blocker was "HDMI source (a camera) plugged into IN
never turns on / no passthrough on OUT / no signal." This session proved WHY and
found the fix: **the HDMI input receiver is a MStar MST3367 that must be brought
up by the HOST over I2C** (init registers + EDID load + HPD assert). Our driver
boots the (correct, retail-identical) firmware but never runs that bring-up, so
the card is a dead HDMI sink and the source stays dark. The complete bring-up
sequence has now been reverse-engineered from Elgato's Windows driver.

## What is SETTLED (don't re-litigate)
- **Firmware is correct.** `/lib/firmware/mz0380/MZ0380.HD.HEX` is byte-identical
  (md5 616643fb..) to the retail Elgato `MZ0380.HD.HEX` v01.11 = what the card
  reports. Not the problem. ("HD"/"SD" in the filenames != HDMI/SDI.)
- **Card cannot auto-detect the video standard, and ep.ko has NO card->host
  signal query.** (RE_FINDINGS M6; QCAP header: "MZ0380 PCI DON'T SUPPORT AUTO
  STANDARD DETECTION".) So V4L2 timings are host-SET, not queried — EXCEPT we can
  now read format directly from the MST3367 over I2C (see below), which
  resurrects real signal detection.
- **op41 (SET_VIC_PARAMS 0x29) is software-only** — sets a no_signal flag / sizes
  the encoder VIC / notifies; it touches NO receiver register and NO GPIO. It was
  never going to wake the source. GPIO all-high likewise did nothing.
- The firmware is a **register proxy**: the host drives the receiver via I2C over
  the mailbox. The init values live in the vendor host driver, not the firmware.

## THE FIX — MST3367 HDMI bring-up
(full detail in RE_FINDINGS.md "M10" + memories `mz0380-mst3367-i2c-abi`,
`mz0380-mst3367-edid-and-detect`)
Mailbox frame (host): dword +0x00=0x800 doorbell (write LAST), +0x04=opcode,
+0x08/+0x0c/+0x10 = params, poll +0x2c bit0. Ops: 0x1a I2C-read, 0x1b I2C-write,
0x1f bulk (EDID), 0x15 GPIO-set, 0x14 GPIO-read.
MST3367 primary I2C addr = **0x9C** (8-bit); EDID EEPROM = **0xA0**. Banked regs
paged by writing page# to reg 0x00 (pages 0/1/2).
Ordered checklist:
  1. Reset/power: write dev 0x9C page0 **reg 0x0F = 0x20**, then 0x0E/0x54 clears.
  2. Config sequence (addresses/order/pages recovered — see M10). !! the per-mode
     REGISTER VALUES for HDMI-1080p are computed at runtime and are the ONE piece
     not statically recoverable — capture them from a **live I2C trace** of a
     working Windows bring-up (or dump 0x9C regs via op 0x1a after Windows brings
     it up, or use an MST3367 datasheet).
  3. Commit strobe: reg 0x51 = 0x00 then 0x21.
  4. Load EDID: 8x32-byte bulk writes (op 0x1f) to I2C 0xA0 — bytes in
     `mz0380-edid-hd60pro.txt` (256B, checksums valid; any valid 1080p CEA EDID
     also works).
  5. Assert HPD: op 0x15, **GPIO pin 1** (mask 0x02, value 0x02) — AFTER EDID.
  6. Poll detect: op 0x1a dev 0x9C **reg 0x55** until (val & 0x3C)==0x3C = locked;
     then read regs 0x40-0x47 / 0x57-0x5F / page2 0x28-0x29 -> resolution + pixel
     clock (decode in M10). This is a REAL signal/format read.

## Tooling already in place (built, compiles clean)
- `/proc/mz0380-cmd` — generic mailbox sender: `echo "<opcode> [p0 p1 ...]" >`
  (hex or dec); dumps status + return slots to dmesg. USE THIS to replay the
  MST3367 sequence (op 0x1b writes, op 0x1a reads, op 0x1f EDID, op 0x15 HPD).
- `/proc/mz0380-hdmi` — fires op41 (kept as a convenience; now known insufficient).
- Real opcode map in `mz0380-reg.h` (SET_VIC=0x29, GPIO 0x14/0x15/0x17, input
  codes). `mz0380_query_signal()` rewritten honest (returns cached/host-set
  timings). Disproven `signal_from_bar0` + QUERY_SIGNAL dead code removed.
- Load line: `sudo insmod ./mz0380.ko firmware_upload=1 enable_video=1
  dma_handshake=1 procfs_verbosity=2` then wait `/proc/mz0380-state` = ready.

## NEXT SESSION — do this
1. **Verify the I2C path reaches the MST3367.** First confirm the op 0x1a packet
   framing (device addr in +0x08, reg in +0x0c, result back in +0x10) — read
   MST3367 reg 0x55 (dev 0x9C). If it ACKs with a plausible byte, the proxy
   works. NB fw 1.11 ep.ko may implement the I2C ops differently than the
   Windows-paired firmware — confirm empirically; if op 0x1a/0x1b are stubbed,
   try op 0x20 (MCU passthrough, slave 0x55->0xAA, sub-tag 0x66).
2. **Get the per-mode register VALUES** (the only missing piece): boot Windows on
   the dual-boot drive, run Elgato with the camera at 1080p, capture the I2C
   register writes — OR after Windows brings it up, dump dev 0x9C regs via op 0x1a
   from Linux. That yields the exact 1080p config values for step 2 of the fix.
3. **Wire the bring-up** into the driver (a real `mz0380_mst3367_bringup()` on
   input-select / stream-start) and retire the /proc experiments. Then the source
   should wake and capture can start (milestone C: cfg banks + XDMA).
4. **Re-wire `mz0380_query_signal()`** to read MST3367 reg 0x55 + timing regs over
   I2C — real VIDIOC_QUERY_DV_TIMINGS.

## Git / state
Branch `main`, ahead ~10 unpushed. This session is UNCOMMITTED (commit was offered
several times; user hadn't confirmed). Changed/added: `mz0380-core.c` (+/proc
hdmi & cmd, honest query_signal callers), `mz0380-video.c` (honest query_signal,
removed signal_from_bar0), `mz0380-reg.h` (real opcode map), `RE_FINDINGS.md`
(M5-M10), new `mz0380-edid-hd60pro.txt`, plus the earlier `mz0380-signal-hunt.sh`.
Do NOT stage `sc0710-*`. Memories added: signal-blob-layout, hdmi-activation,
mst3367-i2c-abi, mst3367-edid-and-detect (+ updates to bar0-no-signal-regs).
Suggested first commit: the opcode map + /proc tooling + honest signal path +
findings + EDID (a clean checkpoint before the MST3367 wiring).

## Reference material on disk
- Windows driver (the RE source): `/run/media/wolffyx/14CC4DC1CC4D9DBC/Program
  Files/Elgato/Game Capture HD60 Pro/e60MZ0380.X64.SYS` (+ `.AX` DirectShow
  filters). Also mirrored under the Steam Proton prefix compatdata/230410.
- Firmware extracted: `/tmp/mz0380-fw/` (ep.ko, yuan_ioctrl, disasm) — may be
  cleared on reboot; re-extract `tar xzf /lib/firmware/mz0380/MZ0380.HD.HEX`.
- QCAP Linux SDK samples (open "MZ0380 PCI"): `~/Downloads/test/SDK 1.1.0.202.0/
  .../QCAP/LINUX/qcap_linux_sdk_1_88_0/`. SC280/SC380 + Device-Custom-Property
  guides under `.../AMESDK/DOC/PRODUCTS/`.
- gchd USB driver (older HDMI capture, resolution-by-timing reference):
  `~/Downloads/test/elgato-gchd-master/`.
