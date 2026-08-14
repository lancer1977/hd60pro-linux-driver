# NEXT SESSION START

_Last updated 2026-08-07. Full history in **RE_FINDINGS.md** (M0..M49b);
this file is only the handoff. Everything below was verified on hardware
unless it says otherwise._

---

## The one-paragraph state of the project

The host side of this driver works. The card takes firmware, answers the
mailbox, accepts commands, and DMAs a complete 1080p frame into host memory
within 450 ms of every stream start — contiguous, zero loss, zero IOMMU
faults, zero PCIe errors. Two things block real capture, and both are on the
card/analog side. **(1)** The fake-frame test path stalls inside the card
after exactly one frame, for a card-internal reason no host action can fix.
**(2)** The real capture path never starts because the HDMI source refuses to
transmit: it sees our hotplug signal, tries to read an EDID, finds none, and
gives up. Getting an EDID into the card is the single remaining blocker, and
static reverse engineering has run out of ways to find how.

---

## What is PROVEN — do not re-litigate these

1. **The whole host DMA/streaming stack is correct.** Addressing, 4 GiB-aligned
   IOVA remap, outbound aperture, per-chunk completion, interrupt credits,
   MSI, event ring. One complete `1920×1107×1.5` frame lands per stream start,
   contiguous end-to-end. (M35–M39)
2. **The fake path (`stream_nosg=1`) is a host-side dead end.** (M41) The
   card's DMA submit ioctl kicks the engine synchronously, so the frame
   landing proves the *engine* works and says nothing about its completion
   IRQ. That IRQ never arrives: the card's thread parks forever in an
   un-timed wait, a busy flag stays set so every later submit enqueues
   unkicked. `VPL_DMAC_Open` clears that state, which is exactly why each
   fresh spawn yields one more frame. Those fields are written **only** by the
   card's own ISR — no opcode, BAR write or doorbell reaches them.
3. **The MST3367 receiver is healthy.** Powered, out of reset, configured,
   answering I2C with our own written values read back. Only two devices exist
   on that bus (`0x9c`, `0x98`) — no EEPROM, no EDID MCU. (M43)
4. **Input select is not the gate.** `SET_VIC` now runs before receiver
   bring-up; detect stayed frozen. (M46)
5. **HPD WORKS.** (M48 — the most useful result of the session) The camera
   reacts to our hotplug edge and then falls back to its LCD: it sees the sink
   appear, fails to read an EDID, and reverts. So GPIO pin1 + `BANK0 0xb7` is
   genuinely the hotplug line and the source honours it.
6. **The receiver's per-mode config is NOT the current blocker.** The source
   isn't transmitting at all, so there is nothing yet to misconfigure.

## What is DISPROVEN (dead ends — don't retry)

- "The hardware discards the low half of the DMA target" → was a buffer
  overrun plus a mod-2³² wrap (M29).
- "There's a constant aperture offset" (M27/M28), "the encoder needs another
  outbound window" (M32), "the encoder waits on audio_ready" (M33), "the
  interrupt credit is dead" (M35), "the DMAC IRQ plumbing is broken" (M37 —
  the transfer completes at its intended size), "the thread exits after frame
  1" (M40 — it has exactly one exit and errors loop back).
- **EDID delivery**: 5 opcodes (`0x20/0x1f/0x1e/0x1b/0x1d`) × fire-and-forget ×
  HPD re-pulse — all negative (M47). No EEPROM on the bus (M43). **No
  EDID-sized RAM window in any bank** (M49b: longest contiguous writable run
  anywhere is 33 bytes; bank3's apparent long run was an artifact of the old
  0x80 scan ceiling).

---

## THE BLOCKER: how does the host get an EDID into this card?

Everything else is either working or provably not the current fault. The EDID
is also the one link no read-back can verify, because nothing answers at the
DDC address on this bus.

**This needs a live trace, not more static RE.** The opcode space is swept,
the bus is enumerated, every bank is dumped and writability-mapped — eight
consecutive negatives with no unexplored surface left. A capture of the
Windows driver doing `UpdateEDID` would show the opcode, frame layout, target
address and any unlock sequence in one go — and the same capture yields the
~175 per-mode receiver config writes (M10 step 2) needed immediately after.

**How to get it, in preference order:**
- **(a)** Boot Windows with the Elgato driver, run its capture app until a
  source locks, and capture the driver's mailbox traffic — DbgView/WPP on
  `e60MZ0380.X64.SYS` (it logs `[UPDATE.EDID]`, `send EDID data`,
  `[HOTPLUG %d]`), or an ETW/kernel trace of its BAR0 writes.
- **(b)** Probe the card's internal I2C (GPIO12/13) with a logic analyser
  while Windows brings it up — reads the wire directly, needs no software
  cooperation.
- **(c)** If neither is possible: treat the sink side as blocked and pivot —
  either to the other inputs, or to consuming the single raw frame the fake
  path does deliver.

---

## Driver state (all compile-verified)

New this session, all in-tree:

- `mz0380-edid.h` — the recovered 256-byte EDID, generated from
  `mz0380-edid-hd60pro.txt` with header + checksum validation.
- **EDID writer** — combo opcode, offset-led payload, fire-and-forget;
  opcode and timeout are live module params (`edid_opcode`,
  `edid_timeout_ms`) so variants can be swept without rebuilds.
- **Sink bring-up order** — input select → reset → init → EDID → HPD, matching
  the Windows sequence.
- **`enc_stat` handshake** (`BAR0+0x50+idx`) — the card sets it to 1 after
  each bitstream DMA and never clears it; the host must write 0 to ack or
  encoding stops after one frame. Acked at stream start and per delivered
  frame.
- **SET_VIC fps** — was always 0 (card forced 60 with an error); now sent from
  the detected timing.
- **streamon** re-detects live timing when `stream_nosg=0` and refuses to
  start without lock.
- **Diagnostics** — poison/extent watcher + hole map (`buf_poison`,
  `poison_byte`), live token watch + credit kick (`credit_kick_ms`),
  and `/proc/mz0380-hdmi` commands: `ramtest`, `wscan`, `watch [secs]`,
  `edid`, `hpd [count] [gap_ms]`.

## Test scripts (each has a decision table in its header)

`m35` live tokens · `m36` write extent + hole map · `m38` repoison ·
`m39` respawn · `m42` real-signal capture · `m43` sink read-back ·
`m45` detect watch · `m47` EDID opcode sweep · `m48` HPD isolation ·
`m49` writability scan.

## Note on the working tree

Everything is uncommitted (`git status` shows the modified sources plus ~15
untracked scripts, `mz0380-edid.h`, `ep-disasm.txt`, `re-dump/`). Worth a
commit before the next session — nothing here has been committed since
`6aaac66`.

---

## If you want to keep making progress without the Windows trace

Ranked by value:

1. **Commit the work** (above) — a lot of verified ground would be painful to
   lose.
2. **Pragmatic capture**: the fake path reliably delivers one complete raw
   1080p frame per stream start. A polling drain plus V4L2 NV12 would give
   working single-frame capture with no further RE.
3. **Decode opcode 0x50** in the card's *userspace* binaries (`vcm.txt`,
   `tinyvenc5.txt` — not `ep.ko`, which only doorbells it). Windows sends it
   with 32-byte payloads during streaming; it is the last host→card channel
   never decoded, and an outside chance for the EDID path.
4. **Try the other inputs** (SDI/component) — they may not need an EDID at
   all, which would exercise the whole capture path end-to-end and prove out
   the real-signal code independently of the HDMI sink problem.
