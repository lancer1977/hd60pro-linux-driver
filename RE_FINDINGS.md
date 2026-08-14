# MZ0380 register map — reverse-engineered from `e60MZ0380.X64.SYS`

Ground truth extracted via Ghidra headless decompilation of the Windows driver
(`e60MZ0380.X64.SYS`, 3.7 MB, from the Elgato HD60 Pro Windows install). These
replace the earlier `CHECKME` guesses in `mz0380-reg.h`, which were structurally
wrong (they assumed a BAR5 mailbox + chunked firmware protocol).

## BAR identity (CONFIRMED)

The device context holds two mapped MMIO pointers:
- `ctx + 0x108` → **BAR0** (1 MiB) — command mailbox + firmware download buffer.
- `ctx + 0x110` → **BAR5** (4 KiB) — DMA-address pointer window + SDK property cache.

Proof: init routine `FUN_140278bb0` writes `*(ctx+0x110) + 0x30 = BAR0phys + 4`
and `*(ctx+0x110) + 0x38 = BAR0phys + 0x5f`. On this host `lspci` shows BAR0 at
`fc200000`, and PLAN.md's live BAR5 snapshot observed `0x30 = fc200004` and
`0x38 = fc20005f` — an exact match. So the window written at ctx+0x110 IS BAR5,
and its 0x30/0x38 registers are programmed with BAR0-relative physical addresses.

## Command mailbox — BAR0 (from `MZ0380_SEND_COMMAND` = `FUN_140285074`)

Layout, relative to BAR0 base:
| offset | role |
|--------|------|
| `0x00` | doorbell / command trigger. Write `0x800` to fire a command; `0x400` seen in reset/init |
| `0x04` | PARAM0 = opcode (`param_2[1]`) |
| `0x08` | PARAM1 / result readback (`== 0` means success after firmware commit) |
| `0x0c` | PARAM2 |
| `0x10…` | further params (PARAM3…) |
| `0x2c` | STATUS. **bit0 = command done/ready**. Cleared to 0 before issuing |
| `0x30` | written 0 during init (misc control) |
| `0x60` | firmware download buffer aperture (blob written here word-by-word, ascending) |

`SEND_COMMAND(ctx, params[], count, [wait])`:
1. If `count >= 2`: for i in 1..count-1, write `params[i]` to `BAR0 + i*4`
   (so opcode `params[1]` → `BAR0+0x04`, next → `+0x08`, …).
2. Write `0x800` to `BAR0 + 0x00` (doorbell).
3. If `wait` arg set: block on a semaphore (async completion via ISR).
   Else poll `BAR0 + 0x2c` up to 50× (`0x32`), success when `& 1`.

## Firmware download (from `MZ0380_DownloadFirmware` = `FUN_1402762d4`)

Main (HD) firmware:
1. `SEND_COMMAND(op=0x0b, size)` — BEGIN_FIRMWARE_DOWNLOAD, param = blob byte count.
2. Copy the **entire** blob into `BAR0 + 0x60`, ascending 32-bit writes
   (`(size+3)>>2` words). Not chunked, no per-chunk ack.
3. `SEND_COMMAND(op=0x0c)` — commit / execute (card reboots into new firmware).
4. Delay, then success iff `*(BAR0 + 0x08) == 0`.

Base firmware (`MZ0380_DownloadBaseFirmware` = `FUN_140275f64`) is identical with
opcodes `0x0e` (begin) / `0x0f` (commit) instead of `0x0b` / `0x0c`.

## Confirmed opcodes
| op | meaning |
|----|---------|
| `0x0b` | BEGIN_FIRMWARE_DOWNLOAD (param = size) |
| `0x0c` | COMMIT/EXECUTE firmware |
| `0x0e` | BEGIN_BASE_FIRMWARE_DOWNLOAD (param = size) |
| `0x0f` | COMMIT/EXECUTE base firmware |

## BAR5 (ctx+0x110) — confirmed uses
- `0x30` / `0x38`: BAR0-relative physical pointers programmed at init (0x30 = base+4,
  0x38 = base+0x5f) — the card's notify/mailbox address handshake.
- `0x40`…`0x88`: SDK property cache (7 correlation-confirmed props — unchanged).
- `0xdc`: written `2` during init (purpose TBD).

## Still to resolve (next RE pass)
- IRQ status/mask/ack registers + bit assignments (decompile `Interrupt_Handler`
  = `FUN_14024ba60`).
- DMA ring / XDMA descriptor programming (the `0x30/0x38` BAR5 pointers plus BAR0
  channel regs — decompile the streaming setup around the `[FIRMWARE RESET]` path
  `FUN_1402829a0`).
- Raw-vs-encoded output format selection (SET_VIC / SET_AIC opcodes).

Source logs: `scratchpad/re/ghidra.log` (string xrefs), `scratchpad/re/decomp.log`
(decompiled functions).

## Interrupt/event protocol (RE pass 2 — FUN_140284380, the Windows "event thread")

Windows does NOT service the card from a real ISR for the mailbox: a priority-31
kernel thread loops every ~1 ms doing:

```c
event = BAR0[0x30];              // interrupt/event status word
BAR5[0xdc] = 2;                  // ack/rearm the card-side flag (card sets it to 1)
BAR0[0x30] = 0;                  // clear the event word
BAR0[0x00] = 0x400;              // doorbell ack (0x400 = event ack, not "reset")
if (event & BIT(11))             // bit11 = COMMAND COMPLETE
    KeReleaseSemaphore(cmd_sem); // wakes MZ0380_SEND_COMMAND's IRQ-wait path
// event[7:0]  -> video/stream DPCs, payload in BAR0[0x40/0x44/0x48]
// event[23:16]-> other DPC class,  payload in BAR0[0x4c]
```

Confirmed against hardware: after our un-acked BEGIN_FW_DL, BAR5[0xdc] read 1 and
PCI INTx stayed pending until this exact 3-write ack sequence.

## MZ0380_SEND_COMMAND (FUN_140285074) — fully decoded

- cmd struct words[1..n] are written to BAR0+0x04,0x08,0x0c,0x10 (word1 = opcode).
- doorbell = write 0x800 to BAR0+0x00.
- Poll path: clear BAR0+0x2c, poll bit0, 50 x 1 ms (matches our driver).
- IRQ path (long commands, incl. firmware download): wait on semaphore released by
  the event thread on EVENT bit11 — STATUS bit0 alone is NOT sufficient for these.
- After >=100 consecutive timeouts the driver stops clearing STATUS (failure latch).

## Peripheral register file = mailbox opcodes 0x1a/0x1b

- `FUN_1402777e4(dev, 0, chip, reg)`  = mailbox cmd {0x800, 0x1a, chip, reg, 0},
  result read back from BAR0+0x10. (register READ)
- `FUN_1402851cc(dev, 0, chip, reg, val)` = mailbox cmd {0x800, 0x1b, chip, reg, val}.
  (register WRITE)
- chip ids seen: 0x90 (bridge/FPGA register file — the ISR-status regs 0x10..0x19,
  0x8b live here), 0xb8 (TVP5160 analog front-end), 0x50 (encoder?).
- So the "Interrupt_Handler" (FUN_14024ba60, actually a DPC) reads its per-source
  status registers THROUGH the mailbox, indices 0x10/0x13/0x14/0x15/0x8b of chip
  0x90; final re-reads of 0x13/0x14/0x15/0x10 act as read-to-clear.

## Firmware upload — VERIFIED ON HARDWARE (M2 PASS)

Working sequence (mz0380-fw.c):
1. BEGIN_FW_DL (0x0b, param = byte count) — completes via EVENT bit11 (~1 ms).
2. Whole blob word-wise to BAR0+0x60.
3. COMMIT_FW (0x0c) fire-and-forget — this opcode never posts a completion.
4. Boot wait ~21 s with 1 ms unconditional ack heartbeat (mirror the Windows
   event thread); boot-done arrives as EVENT=0x800 and RESULT (BAR0+0x08)
   transitions size-echo -> 0.
5. Post-boot: STATUS (BAR0+0x2c) reads 0xdddddddd, GET_FW_VERSION (0x01)
   answers (fw 0.1), mailbox remains functional.

Every event MUST be acked (BAR5[0xdc]=2, BAR0[0x30]=0, doorbell 0x400) or the
card wedges with INTx asserted and later commands time out.

## DMA buffer allocation (RE pass 3 — FUN_14028d254, generic YUAN SDK init)

One SDK function sizes and allocates all DMA memory. MZ0380 matches via
`(devid - 0x370) & 0xffee == 0` (0x370/0x371/0x380/0x381) at LAB_14028dcfb:

- ctx[0x18] = 0        -> allocation mode 0: MmAllocateContiguousMemorySpecifyCache,
                          retry loop walking 128 MB windows upward below 4 GB
- ctx[0x1c] = common buffer size. HD60 Pro (subsys 1cfa:0006 takes the
  "else" branch) = 0x400000 (4 MB); other MZ038x flavors 0x200000.
- stream buffer sizes for our flavor: 0x655000 (or 0x466000/0x34bd00 variant),
  DbgPrint tag "[MEMORY] [%08X] [%08X] [%08X]".
- allocated VA stored at ctx[0x28], physical address (MmGetPhysicalAddress)
  at ctx[0x27]. WHO writes ctx[0x27] into a BAR register = still open —
  next RE target (consumers of ctx offset 0x138/0x140 region).

Note: BAR5[0x30]=bar0+0x4 and BAR5[0x38]=bar0+0x5f exist pre-boot already,
so those two are card-programmed defaults, not host DMA pointers.

## THE ORACLE: firmware blob is a full Linux SDK; card-side ep.ko serves the mailbox

MZ0380.HD.HEX = gzip -> GNU tar "yuan_demo_sdi/" = complete ARM Linux userland
for the card SoC (project SC5C0). rc.local loads drivers then runs
video_capture_mgr / audio_capture_mgr / yuan_ioctrl. The host-facing PCIe
mailbox is implemented by drivers/ep.ko (ARM ELF, not stripped) - this is the
authoritative post-boot command spec, no more guessing from the Windows side.

Extracted: scratchpad/fw/yuan_demo_sdi/. Key funcs in ep.ko:
  pciep_isr @ 0x11234       - host command dispatcher (the real ISR)
  pciep_isr_clrint @ 0x10d68
  command_store/show        - sysfs command attr

### ep.ko command model (RE-confirmed)
- Command buffer base = inbound_mem0_start + 4 (== host BAR0 phys + 4), recomputed
  every doorbell from *(ep_regs + 0x10). Matches BAR5[0x30]=bar0+4, BAR5[0x38]=bar0+0x5f.
- Layout at that base: word0 = opcode, word1.. = params, word10 (= bar0+0x2c,
  our STATUS) = completion slot.
- Completion is per-opcode, TWO mechanisms:
  * opcode 0x01 (INIT), 0x0a (GET_VERSION), 0x02/0x03/0x04 (SET banks): write
    STATUS word10. GET_VERSION writes version to word1/word2 (bar0+8/0x0c) and
    STATUS = 0xaaaaaaaa (DAT_00011978) when ready, else DAT_0001197c.
  * opcode 0x0b/0x0e (FW download), 0x2a/0x29 (signal): sysfs_notify -> raises the
    host EVENT 0x800 instead of writing STATUS. (Explains why fw upload completes
    via EVENT, not STATUS.)
- Unknown opcode -> prints "$$$command: 0x%08x" + 10 params and returns WITHOUT
  writing STATUS (host sees a timeout).

### Why the live command path (INIT/version/signal) is still deaf post-boot
Pre-boot the bootloader answers 0x0b with a fixed inbound mapping. Post-boot ep.ko
brings up the full inbound/DMA window; until the host establishes that window
(bus-master DMA, currently disabled by enable_dma=0), the doorbell IRQ fetches the
command from the wrong card-RAM address and STATUS stays at ep.ko's 0xdddddddd
poison value. => the post-boot control path is gated on DMA/bus-master (M4), not on
opcode correctness. mz0380_card_init() is Windows-accurate and should light up once
the inbound window is programmed.

## M4 CONTROL PATH WORKING (verified on hardware)

Post-boot mailbox is fully operational. Sequence (mz0380_card_init):
1. BAR5[0x30]=bar0_phys+4, BAR5[0x38]=bar0_phys+0x5f (already latched at boot).
2. CMD_INIT (0x01): opcode -> BAR0+0x04, doorbell BAR0+0x00 = 0x800.
   Completion = STATUS(BAR0+0x2c) reads 0xaaaaaaaa (NOT bit0; the fw success
   stamp). Answered on attempt 1.
3. GET_BOARD_VERSION (0x0a): running fw version in PARAM1/PARAM2 (BAR0+0x08/0x0c).
   Reports 1.11, matching MZ0380.FW.TXT "01.11".
4. Version-skip: if the card already runs the shipped version, the ~21 s
   upload+boot is skipped entirely.
5. Peripheral read (mz0380_periph_read, opcode 0x1a) of bridge chip 0x90 reg
   0x12 returns live HDMI signal presence (bit0). Confirmed responding.

THE decisive bug (found via mz0380_mailbox_scan): the poll loop was firing the
0x400 ack doorbell every 1 ms unconditionally, which ABORTS a STATUS-completing
command (0x01/0x0a) before the card finishes. Fix: poll silently, ack only when
EVENT(0x30) != 0. Offsets (op@0x04, bell@0x00) were correct throughout.

Bus mastering (dma_handshake=1) was enabled for the successful runs; whether the
quiet poll alone suffices without it is untested.

Remaining for full capture (M4 second half): program the frame DMA ring (real
ring-register offsets still a guess in mz0380-dma.c) and drive SET_VIC /
START_STREAMING over the now-working command channel.

## M4b SIGNAL-DETECT: BAR0 dead end, pivot to bridge-chip command path (2026-07-04)

Hardware plug/unplug diff via the new `/proc/mz0380-scan` (BAR0 window,
per-read `pr_info` trace):

- BAR0/MMIO is only backed at **0x04..0x5c** (the command-mailbox aperture).
  0x00 reads 0xffffffff. Every offset >= 0x60 eats a ~200 ms PCIe completion
  timeout then returns 0xffffffff -- NOT a hard hang, just ~0.2 s/read, so a
  blind wide BAR0 sweep looks frozen (that was the earlier "freeze").
- The sc0710-ported HDMI window (0xa8..0xe4 in `mz0380_signal_from_bar0()`)
  sits in that dead zone => reads never change on plug/unplug.
  **The sc0710 register model does not apply here; HDMI signal is NOT a BAR0
  register.** Those guesses should be dropped from `mz0380_signal_from_bar0()`.

Correct surface = the HDMI **bridge chip (0x90)** via the proven mailbox
`REG_READ (0x1a)` path (`mz0380_periph_read`). New `/proc/mz0380-periph-scan`
walks a chip's register file diff-stably. The old `MZ0380_BRIDGE_SIGNAL 0x12
bit0` label is unverified -- the hunt (`PERIPH=1 ./mz0380-signal-hunt.sh`)
finds which bridge register really tracks lock. Needs `dma_handshake=1`.

Tooling: `/proc/mz0380-scan` (raw BAR, safe-range gated), `/proc/mz0380-periph-scan`
(bridge/TVP5160 via 0x1a), `mz0380-signal-hunt.sh` (capture + diff analyzer,
LOCK-BIT vs ACTIVITY).

## M4c EP.KO COMMAND ORACLE — signal is NOT a register, and 0x1a is bogus (2026-07-04)

Re-extracted the card firmware (`tar xzf /lib/firmware/mz0380/MZ0380.HD.HEX`,
gzip+tar rooted at `yuan_demo_sdi/`) and disassembled the card's PCIe-endpoint
mailbox server `drivers/ep.ko` (ARM, not stripped) with llvm-objdump. The
command dispatcher is `pciep_isr`: it reads the opcode with `ldr r6,[r4]`
(r4 = inbound command buffer; [r4+0]=host BAR0+0x04 opcode, [r4+4]=host 0x08
RESULT, [r4+8]=host 0x0c, ...) and branches on a fixed opcode set.

Opcodes actually dispatched by fw 1.11:
  1,2,4,6,7,8,9,10,11,14,15,20,21,23,34,41,42,45,47,49,80,82,96,97,98,100,110,123,124
Identified so far:
  - 10 (0x0a) GET_BOARD_VERSION  -> writes version to [r4+4]/[r4+8] (host PARAM1/2). MATCHES.
  -  1 INIT, 11 fw-download BEGIN, 14 fw related (debug-printed in tail).
  -  2 / 8  copy a block of host words into card config state (SET_* setters).
  - 41 (0x29) SET_VIC_PARAMS: reads width/height FROM the host command, prints
       "SET_VIC_PARAMS(fw %d) size(%dx%d) fps(%d)"; if width==0||height==0 it
       sets the card `no_signal` global (data sym `no_signal` @0x71c) and prints
       "cmd(%d) => no signal". So the HOST supplies resolution; the card derives
       no-signal from a zero size.

KEY CORRECTIONS (previous guesses were wrong):
  - 0x1a REG_READ / 0x1b REG_WRITE are NOT card commands. Opcode 26 (0x1a)
    dispatches to a sysfs_notify stub, not a peripheral read. The earlier
    "bridge chip 0x90 reg read confirmed" was the mailbox returning a constant
    0x48 (the RESULT slot), never real register data. `mz0380_periph_read()` and
    the whole bridge-register model are built on a bogus opcode.
  - Guessed opcodes SET_VIC_PARAMS=0x10, QUERY_SIGNAL=0x20, START_STREAMING=0x12
    are absent from the dispatcher; SET_VIC is really 0x29 (41).
  - There is NO mailbox getter that returns input width/height/signal. HDMI
    signal presence is therefore NOT pollable via a command in this firmware.

SIGNAL MODEL (reconciles the QCAP SDK): QCAP exposes signal via CALLBACKS
(QCAP_REGISTER_NO_SIGNAL_DETECTED_CALLBACK / SIGNAL_REMOVED) and the result code
NO_SIGNAL_DETECTED=0x9, not a poll. The card detects format itself (video_capture_mgr
+ libvideocap + the front-end receiver) into `g_stream_info` (44B @0x640) and
raises a host EVENT. reg.h already reserves EVT_PAYLOAD0..3 at BAR0+0x40..0x4c
("DPC args"). => the real signal/format path is the card->host EVENT with the
format carried in the 0x40..0x4c payload words, decoded on the interrupt.

NEXT (correct milestone B): capture EVENT(0x30)+payloads(0x40..0x4c) LIVE across
a plug/unplug (events are edge-triggered, so sample continuously without reload
and without acking), and RE which pciep_isr path / card event writes those
payloads. The two-static-snapshot register diff cannot see an edge event.

================================================================================
M5  CROSS-REF: sibling 4k60 Pro mk.2 (sc0710) + USB gchd give the EXACT signal
    blob layout. (Reviewed ~/Downloads/test/elgato-gchd-master + untracked
    sc0710-i2c.c / sc0710-video.c in this repo.) 2026-07-04.
================================================================================

The status blob our ARM MCU serves is now known byte-for-byte, from the sibling
card that reads the SAME ARM MCU over a different transport.

sc0710 (4k60 Pro mk.2) reads HDMI status via an AXI-IIC master at BAR0 0x3100:
  - ARM MCU is an I2C slave: I2C_DEV__ARM_MCU = (0x32<<1). Host does write-read.
  - AXI-IIC regs: 0x3100 ctrl (0x2=TX reset,0x1=enable), 0x3104 status
    (0x44=addr ack, 0xc4=subaddr ack, 0xc8=read done), 0x3108 TX_FIFO
    (bit8=start, bit9=stop), 0x310C RX_FIFO.
  - THREE status pages by subaddress: 0x00 -> 26 bytes (HDMI status),
    0x1a -> 16 bytes (status2), 0x2a -> 16 bytes (status3).

HDMI status blob (subaddr 0x00, 26 bytes) decode (sc0710_i2c_read_hdmi_status):
    rbuf[0x08]            != 0  => LOCKED (signal present)     <-- the flag
    rbuf[0x0a..0x0b] LE16      => width            (e.g. 1920)
    rbuf[0x08..0x09] LE16      => height/field     (e.g. 540; x2 if interlaced)
    rbuf[0x04..0x05] LE16      => pixelLineV/V-total (e.g. 562)
    rbuf[0x06..0x07] LE16      => pixelLineH/H-total (e.g. 2200)
    rbuf[0x0d] & 0x01          => interlaced
    (rbuf[0x0d] & 0x30) >> 4   => colorimetry 1=BT709 2=BT601 3=BT2020
    rbuf[0x0f]                 => colorspace 0=YUV422/420 1=YUV444 2=RGB444
  Captured example (1080i59.94): 1920 / Htot 2200 / Vtot 562 / interlaced.
  (H,V) -> format via sc0710_format_find_by_timing() table (sc0710-video.c:185+):
    {1650, 750,1280, 720,0,...720p}, {2200,562,1920,540,1,...1080i}, etc.
  NO-signal branch just zeroes everything and sets locked=0. There is no
  distinct "no signal" register - locked byte == 0 IS the no-signal state.
  (Matches gchd's USB path: it derives the same by SAMPLING a line-period
  counter and pattern-matching magic sums, and treats a sentinel read
  0xad4d as "no signal". Colorspace autodetect unreliable there too.)

WHY THIS NAILS MZ0380 (HD60 Pro): same ARM MCU, different transport. sc0710's
AXI-IIC block (BAR0 0x3100..0x3120) lands in OUR dead zone (BAR0 backed only
0x04..0x5c) -> HD60 Pro has no AXI-IIC path; the host<->ARM-MCU channel here IS
the BAR0 mailbox. So the firmware serves this same locked/width/height/H/V blob
(our g_stream_info 44B@0x640) over the mailbox/EVENT instead of I2C. The sibling
polls it (sc0710-core.c:230,450) - so on MZ0380 a poll-able form may also exist.

REVISED NEXT (supersedes the blind diff above):
  1. Run the live event watcher across a cable toggle and decode payload words
     0x40..0x4c with the template above (hunt for the locked byte + LE16
     1920/1080/2200/1125 - not "which word flips").
  2. New probe: read the firmware buffer at BAR0+0x60 for a 26/44-byte blob =
     the mailbox mirror of sc0710's "I2C read subaddr 0" (g_stream_info@0x640).
  3. Wire mz0380_signal_from_bar0() to those exact offsets + reuse the
     sc0710-video.c timing table for fps/scan-mode. Delete remaining guesses.

M5 EMPIRICAL RESULT (2026-07-04, fw 1.11): the idle card reports NO signal to
the host, two independent ways:
  a. Live event watcher across a real HDMI plug/unplug: 0 edges. EVENT(0x30)
     stayed 0, STATUS(0x2c) steady 0x00000001. The card raised no host edge.
  b. Static two-state diff of the backed mailbox region BAR0 0x00..0x5c
     (6 unplugged + 6 plugged samples): no word changed with plug state.
  => In idle (front-end not armed, no input selected, not streaming), there is
     neither a pollable signal word NOR a pushed event in the host-visible BAR0
     region. This reconciles everything: fw 1.11 follows the host-forces-format
     model (SET_VIC_PARAMS 0x29 takes WxH FROM the host); the card does not
     autodetect-and-report to the host while idle. Matches gchd/elgato ("no
     continuous autodetect") and the QCAP SET_VIC path.
  => Signal/format reporting, IF it exists at all, is gated behind arming the
     pipeline (input-select + start-streaming = milestone C). The sibling
     sc0710 can read a status blob because its ARM MCU front-end is always
     powered over AXI-IIC; on HD60 Pro that path is absent (dead zone) and the
     front-end appears powered down until capture starts.
  NOTE: mz0380-reg.h still carries STALE sc0710-derived guesses disproven by RE
     - CMD opcodes 0x10/0x11/0x12/0x13/0x20 (real SET_VIC=0x29; 0x12/0x20 absent
     from dispatcher), AXI_IIC block @0x3100 (in BAR0 dead zone, no HW), HDMI
     regs 0xa8..0xe4 (un-backed). Clean these when wiring the real path.
  OPEN (dispatched to firmware disasm): does a get-stream-info/get-status opcode
     exist among the real set {6,7,9,11,14,15,20,21,23,34,42,45,47,49,80,82,96,
     97,98,100,110,123,124}, and which are the real START_STREAMING + INPUT_SELECT?

================================================================================
M6  DISASM CLOSURE (2026-07-04): ep.ko command set has NO card->host format
    path. Milestone B is unsupported by fw 1.11 firmware. Evidence-locked.
================================================================================

Disassembled ep.ko (yuan_demo_sdi variant, ARM EABI5, not stripped; dispatcher
pciep_isr @.text 0x1210). Full opcode->handler map recovered. Conclusions:

Q1 GET-STATUS OPCODE: NONE (HIGH). The ISR (0x1210..0x1944) never references
   g_stream_info (.bss+0x640). No handler copies video-format/resolution/fps/
   signal into the host reply words. The richest reply is GET_VERSION (op10):
   2 words (fw_version[0],[1]) at reply [4],[8]. GPIO ops 20/21/23 return a bit
   map at [2]/[8]. That is the entire card->host data surface.

Q3 WHO FILLS g_stream_info: ONLY the card's local userspace, via the on-card
   char-device ioctl livectrl_ioctl (LIVECTRL_IOCTL_GET_INFO 0x40047005 /
   SET_INFO 0xC0047005, 44B, copy_to/from_user of .bss+0x640). ep.ko has NO
   HDMI-detect routine; it never measures format. The front-end (Gennum GV7601
   SDI/HDMI receiver via spi_gv7601, + vpl_vic / gv7601_audio) lives in sibling
   .ko modules and is NOT reachable over the PCIe mailbox.

Q4 EVENT EMIT: sysfs_notify + the msi finalizer (0x115c) which sets EVENT bit11
   (orr #2048) at DBI+0x30 and rings a doorbell = the sole host-notify path.
   It carries ONLY the event bit; NO format payload. Several opcodes
   (6,9,15,42,45,47,49,80,96,97,98,110) gate their notify on no_signal
   (.bss+0x71c) but write no data. BAR0 0x40..0x4c is populated only by
   config-bank echoes (op2/4/6/8) and op10/21/23 replies - never format.

=> DEFINITIVE MODEL: format is strictly HOST->CARD via SET_VIC_PARAMS (op41,
   0x29). Correct field layout of the command buffer (cmd[] = mailbox param
   slots, byte offsets from opcode buffer base):
     cmd[6]     = VIC / input code (special value 7)
     cmd[5]     = fps
     cmd[8..9]  = Width  (u16)
     cmd[10..11]= Height (u16)
     cmd[0x22]  = int_reduce flag -> g_interrupt_reduce_enable (.bss+0x630)
   Width==0 || Height==0  => sets no_signal=1 (.bss+0x71c), prints
   "cmd(%d) => no signal". Else no_signal=0, stores input code to .data+0x04
   (windows_select_fw), sysfs_notify. There is NO reverse (card->host) format
   command. Any host "query signal" design must be abandoned. The host is the
   SOURCE of format (assert via op41), and gets only a no_signal-change nudge
   (EVENT bit11) back - exactly the gchd/elgato "no autodetect" model.

REAL OPCODE MAP (replaces the stale CHECKME guesses in mz0380-reg.h):
   1 INIT | 2 set-cfg-bankA | 4 set-cfg-bankB | 6 enc-status-bank+notify |
   8 set-enc-params(via host ptr) | 10 GET_VERSION | 11 BEGIN_FW_DL |
   14 BEGIN_BASE_FW_DL | 20 GPIO-read | 21 GPIO-set | 23 GPIO-dir |
   34 STOP_STREAMING-area notify | 41 SET_VIC_PARAMS | 45 SET_AIC_INT_MODE |
   100 PREVIEW_BUF_EX | 123/124 fw-dl variants | 7/9/15/42/47/49/80/82/96/97/98/
   110 = signal-gated sysfs notifies (no data). START_STREAMING is NOT an ep.ko
   opcode - the encoder/XDMA lives in the vpl_*/vma_* modules; "start capture"
   from the host = program cfg banks (op2/4/8) + assert VIC (op41) + arm XDMA
   host-side. Stale/bogus: 0x10/0x11/0x12/0x13/0x20 (not in dispatch), AXI_IIC
   @0x3100 (BAR0 dead zone), HDMI regs 0xa8..0xe4 (un-backed).

MILESTONE B: CLOSED - not achievable with this firmware. Proceed to the
host-forces-format design (V4L2 dv_timings SET by app/EDID, asserted via op41;
no S_DV_TIMINGS query-from-card, no VIDIOC_QUERY_DV_TIMINGS backed by hardware).

================================================================================
M7  HDMI ACTIVATION (2026-07-04): the source stays asleep because the card
    never presents itself as a sink (HPD/EDID). Fix = run the SDK bring-up.
================================================================================

USER SYMPTOM: HDMI source (camera) plugged into the card's IN port does NOT
enter HDMI mode / outputs nothing (OUT passthrough dark). Same camera on a real
monitor DOES wake. => classic HDMI sink-handshake gap: a sink must assert HPD
+ serve a valid EDID over DDC for the source to output. The idle card asserts
neither, so the source never starts. This is WHY every idle signal test this
session saw nothing - there was never a signal on the wire to detect.

REFERENCE (Yuan QCAP Linux SDK, ~/Downloads/test/SDK 1.1.0.202.0/.../QCAP/LINUX/
qcap_linux_sdk_1_88_0). The samples open our exact device by name:
  QCAP_CREATE("MZ0380 PCI", 0, win, &dev, TRUE, TRUE);
  // header: "MZ0380 PCI" IS FOR SC350, SC3C0, SC550, SC560, SC5C0
  // header: SC3C0N4/N8/N16 (MZ0380 PCI) DON'T SUPPORT AUTO STANDARD DETECTION
Card identity: Yuan SC3C0-family, SoC = SL6010 (per SC280/SC380 guide). The
"no auto standard detection" line officially confirms M6: host must supply the
video standard; only signal lock (present/absent) is reported back.

MINIMAL BRING-UP that wakes the source (sc5c0n1.c initialize_capture):
  QCAP_CREATE(...); register NO_SIGNAL/SIGNAL_REMOVED/FORMAT_CHANGED callbacks;
  QCAP_SET_VIDEO_INPUT(dev, QCAP_INPUT_TYPE_HDMI /* =2 */);
  QCAP_SET_AUDIO_INPUT(dev, EMBEDDED_AUDIO /* =0 */);
  QCAP_RUN(dev);
No explicit EDID or HPD call - the front-end asserts HPD + serves EDID as a
side effect of input-select + RUN. Our driver boots the firmware but never
selects the input or runs the pipeline, so the front-end stays powered down
and HPD is never asserted. THAT is the gap to close.

INPUT CODES (qcap.h): COMPOSITE0 SVIDEO1 HDMI2 DVI_D3 COMPONENT4 RGB5 SDI6
  AUTO7 DP8. HDMI = 2. This is the value for op41 SET_VIC_PARAMS cmd[6].

GPIO PATH (SC280/SC380 guide, "access SL6010's GPIO"): property 940 =
  direction (1=output), 941 = data (1=high). These map to ep.ko GPIO opcodes
  23 (direction) and 21 (set). HPD to the source is plausibly one of these
  pins ("GPIO controlled by the first chipset in one board"). Doc example even
  shows SET(940,0xFFFF);SET(941,0xFFFF) = all 16 pins output-high.
SIGNAL LOCK IS READABLE after all (AMESDK guide): AMESDK_GET_LOCK(dev[ch],
  &status) returns a 4-bit value, bit N = channel N lock. So lock (present/
  absent) is host-visible in the official stack - reconcile with M6: it is the
  STANDARD (WxH/fps) that is not auto-detected, not the lock bit.

CURRENT DRIVER GAP: input_select is wired as a BAR register poke
  (input_select_reg module param) - wrong model. Input-select is the op41
  mailbox command (cmd[6]=input code), not a register write. Rewire to op41.

NEXT (activation, in cheap->involved order):
  1. Issue op41 SET_VIC_PARAMS(input=HDMI=2, W=1920,H=1080,fps=60) over the
     mailbox after firmware-ready, then check: does the source wake (OUT lights
     up / monitor on OUT shows it) and does a no_signal/lock event fire?
  2. If not, assert the HPD GPIO: op23 set pin(s) to output, op21 drive high
     (start from the documented all-high, then narrow). Re-check source wake.
  3. Full RUN = program cfg banks (op2/4/8) + op41 + arm host XDMA. Milestone C.

EXPERIMENT RESULTS (2026-07-05, driver builds; new /proc/mz0380-hdmi op41 +
/proc/mz0380-cmd generic mailbox sender). Command path CONFIRMED WORKING:
  - op41 SET_VIC_PARAMS(input=2, various WxH/fps): ret=0, card echoes 0x29 in
    the result slot (opcode ack). First send after load times out (-110) as a
    warmup/stale-event drain; subsequent sends complete. => op41 completes but
    does NOT wake the source and does NOT light OUT. Consistent with the disasm
    (op41 only stores input + sets no_signal + sysfs_notify; no front-end power).
  - GPIO fully controllable: op20 GPIO_READ returns 0x48 (bits 3,6) at idle;
    op23 dir=0xffff then op21 data=0xffff -> re-read op20 returns 0xffff (write
    verified stuck). Driving ALL GPIO high did NOT wake the source. => GPIO is
    r/w-able but is NOT the HPD lever (or HPD alone is insufficient w/o EDID).
  CONCLUSION: neither op41 (declare format) nor GPIO (all-high) makes the card
  present itself as an HDMI sink. The blocker is the deeper front-end bring-up
  (receiver power + EDID load + HPD), which lives in the ARM-side modules/app,
  not ep.ko. Dispatched to a focused firmware RE (front-end chip, boot-vs-
  on-demand power, HPD mechanism, default-EDID presence, host trigger chain).
  NB: op41 result slot convention - cmd_last_param[0] = opcode echo, not data.
  NB: after all-high poke the card GPIO is left 0xffff; reload to restore 0x48.

================================================================================
M8  ROOT CAUSE (2026-07-05, firmware RE #2): the flashed firmware is the SDI
    demo build; it has NO HDMI receiver driver. Front-end mismatch.
================================================================================

Disassembled the whole firmware image (ep.ko + yuan_ioctrl app + front-end .ko's).
/lib/firmware/mz0380/MZ0380.HD.HEX (914KB gzip, a 2020 Yuan build) extracts to a
SINGLE tree: yuan_demo_sdi/. Front-end modules shipped: spi_gv7601.ko (Gennum
GV7601 = 3G-SDI receiver, SPI, /dev/gv7601), NULLSensor.ko, vpl_vic.ko, i2c-gpio.
The HDMI receiver (ITE IT6604) driver is NOT in the image - IT6604 appears only
as an uncompiled build option (drivers/Kconfig "HDMI audio (for IT6604)",
Kbuild IPCam/ + IT6604_Audio.ko under CONFIG_IT6604). => this is the SDI variant
firmware; nothing in it powers/initialises an HDMI receiver, asserts HDMI HPD, or
serves EDID. Byte-scan for an EDID header (00 FF FF FF FF FF FF 00) across the
whole tree = zero. No "hpd"/"edid"/"ddc" strings anywhere.

ARCHITECTURE (front-end agnostic SoC): the external receiver (GV7601 for SDI,
IT6604 for HDMI) converts the input to BT.1120 16-bit parallel video and feeds
the SL6010 SoC VIC (vpl_vic.ko, /dev/vpl_vic0; libvideocap.so.13 = the "No
signal" source). The PCIe/encoder path (ep.ko + vma_* encoders) is identical
regardless of front-end. So on an HDMI card running THIS firmware, spi_gv7601
tries SPI to a GV7601 that isn't present, while the card's actual IT6604 (I2C) is
never initialised -> no HPD/EDID -> the source stays dark. This is the root cause
of every "source won't wake" result above.

FIRMWARE IS A REGISTER PROXY: yuan_ioctrl (the on-card app) hardcodes NO receiver
register values. It boots, opens /dev/gv7601 + /dev/i2c-0,1 + /sys/vpl_pciep/
command, clears the "dency" gate, and blocks in poll() waiting for host commands.
The HOST supplies every receiver register write over the mailbox:
  op24 (0x18) GV7601 SPI read      op25 (0x19) GV7601 SPI write
  op33 (0x21) SPI group read       op34 (0x22) SPI group write
  op26 (0x1a) I2C read             op27 (0x1b) I2C write
  (ep.ko forwards opcode<=34,!=41 via sysfs_notify "command" to yuan_ioctrl,
   which reads a 44B packet, packet[0]=opcode, and dispatches - main switch
   @0x97c4. GV7601 SPI = 16-bit addr/16-bit data; ioctl(fd,1)=write ioctl(fd,125)
   =read.) So the receiver init register list lives in the VENDOR HOST driver
   (QCAP/libvideocap/Windows), NOT the firmware - that is the missing piece.

CONFIRMS op41 is software-only (pciep_isr@0x171c): packet[5]=input, [8]=W u16,
[0xa]=H u16, [0x22]=int_reduce; W==0||H==0 -> no_signal flag; else set VIC code +
notify encoder. Touches no receiver reg, no GPIO. Also: i2cdetect/i2cget binaries
ship in the tarball (they run on the card ARM) documenting the I2C proxy ABI.

IMPLICATION / FORK:
  A. This is the wrong-front-end firmware. If a retail Elgato (HDMI) firmware or a
     Yuan HDMI build (with the IT6604 driver + init) exists, flashing THAT brings
     HDMI up natively. Provenance of MZ0380.HD.HEX (a 2020 Yuan SDI demo) is
     suspect - it is almost certainly not the retail HD60 Pro firmware.
  B. Stay on this firmware and host-drive the receiver via the I2C proxy (op26/27):
     probe /dev/i2c-0,1 for the IT6604, then replay its full power+EDID+HPD init
     register sequence from the host. Hard - needs the IT6604 register script,
     which is not in this blob.
  NEXT (cheap, decisive): probe the front-end. op24 SPI-read a GV7601 chip-ID
  reg (is an SDI rx even there?) and op26 I2C-scan i2c-0/1 for the IT6604 - see
  what ACKs. That confirms the physical front-end + whether the proxy can reach
  it, before committing to A or B. (Need the op24/op26 packet param layout first.)

================================================================================
M9  FRONT-END IDENTIFIED + FIRMWARE IS CORRECT (2026-07-05, retail Windows
    driver RE). HDMI receiver = MStar MST3367, host-driven over I2C.
================================================================================

CORRECTION to M8's "wrong firmware": the flashed /lib/firmware/mz0380/
MZ0380.HD.HEX is BYTE-IDENTICAL (md5 616643fb..) to the retail Elgato
MZ0380.HD.HEX, version 01.11 = exactly what the card reports. So the firmware is
CORRECT. Retail set (Elgato HD60 Pro install): MZ0380.HD/SD.HEX + MZ0381.HD/SD
.HEX + FW.TXT(01.11). "HD"/"SD" != HDMI/SDI (the .HD blob ships the GV7601 *SDI*
driver, .SD ships neither) - they are board/version variants, not front-ends.

FRONT-END (from the Windows kernel driver e60MZ0380.X64.SYS, at the dual-boot
mount /run/media/.../Program Files/Elgato/Game Capture HD60 Pro/): the HDMI
input receiver is a **MStar MST3367**, driven ENTIRELY by the host over I2C via
the firmware's MCU I2C proxy (op26/27; "mcu_i2c_access", "i2c_write_bytes"). The
firmware's spi_gv7601 is for the SDI variant and is vestigial on the HDMI board.
The .SYS also supports SA7160 / TW2968 (analog SD) and GV7601 (SDI) front-ends -
Elgato picks MST3367 for HD60 Pro HDMI. There is NO IT6604 here (M8's Kconfig
IT6604 was a red herring).

The .SYS carries the complete HDMI bring-up (debug strings as anchors):
  - MST3367 init/power: "MST3367 DELAY", "MST3367_ADC_AUTO_PHASE",
    "MST3367_HDMI_MODE_DETECT( 0x55 = 0x%x )" (mode/timing read from reg 0x55).
  - EDID: host BUILDS + LOADS an EDID into the receiver - "CDevice::Enter
    UpdateEDID", "send EDID data", "update EDID checksum", "[UPDATE.EDID] ELGATO
    BOARD SC5C0N1 1080P". No raw EDID header (00 FF..00) in the binary, so it is
    constructed in code or written straight into MST3367 EDID RAM via I2C. For
    waking the source, ANY valid 1080p CEA EDID should suffice.
  - HPD: "[HOTPLUG %d]" - an HPD assert path exists (MST3367 register or GPIO).
  - Mode detect reg 0x55 => this is ALSO a potential real signal/format read
    (contradicts nothing in M6: that was ep.ko; here the host reads MST3367 regs
    over I2C directly). GetHDMIDotClock derives pixel clock + audio sample rate.

=> ROOT CAUSE (final): our driver boots the (correct) firmware but never runs the
MST3367 HDMI-receiver bring-up that the Windows driver does over I2C. No MST3367
init + no EDID + no HPD => the source sees a dead sink => stays dark. op41/GPIO
were always irrelevant to this.

NEXT: RE e60MZ0380.X64.SYS to extract (a) the MST3367 power-up/init I2C register
sequence, (b) EDID load sequence (or use a generic 1080p EDID), (c) HPD/[HOTPLUG]
assert, (d) MST3367 I2C slave address + which bus, (e) the op26/27 I2C mailbox
packet framing - then replay it from the Linux driver via /proc/mz0380-cmd.
Dispatched to a focused Windows-driver RE agent.

================================================================================
M10 MST3367 BRING-UP RECOVERED (2026-07-05, e60MZ0380.X64.SYS RE). The exact
    replayable HDMI-receiver init/EDID/HPD/detect sequence. THE unblocker.
================================================================================
Full detail in memories [[mz0380-mst3367-i2c-abi]] + [[mz0380-mst3367-edid-and-detect]].
Windows x64 driver, image base 0x140000000; working copy + disasm in the session
scratchpad. All verified against call sites.

MAILBOX TRANSPORT (host side, confirms our BAR0 model): single BAR writer
sub_140285074. Frame = dwords, DOORBELL LAST:
  +0x00 = 0x800 (doorbell/go, written last)   +0x04 = op-number (verbatim, no shift)
  +0x08 = word2   +0x0c = word3 (read result returned here for some ops)
  +0x10 = word4 (I2C write value / I2C read result)   +0x2c = completion, poll bit0
Ops: 0x1a=I2C read, 0x1b=I2C write, 0x1f=bulk (EDID), 0x15=GPIO-set, 0x14=GPIO-read,
0x20=MCU raw-I2C passthrough. (These match /proc/mz0380-cmd: opcode->+0x04,
params->+0x08.. . NB our fw 1.11 ep.ko may stub some; the Windows-paired
yuan_ioctrl implements them - verify each empirically.)

MST3367 I2C ADDRESSES (8-bit): PRIMARY bank = 0x9C (HDMI core/timing/detect,
this is the one to use); others 0x88/0x98/0x60/0x90/0x94 (aux banks);
EDID DDC EEPROM = 0xA0. Paged: to touch a banked reg, first write the page# to
reg 0x00 (op 0x1b) - pages 0,1,2. Helpers: reg-write sub_14028658c (op0x1b,
args dev=0x9C,page,reg,val); reg-read sub_140277884 (op0x1a). MCU passthrough
slave 0x55->0xAA via op 0x20 sub-tag 0x66 (sub_140275804) - separate from banks.

INIT (CAVEAT): NO static {reg,val} table exists. Two runtime megafuncs compute
values from the detected/forced timing: config sub_14024efc8 (~106 writes, gated
on input-type @this+0x73a8) + timing/PLL sub_14024dc28 (~69 writes). ADDRESSES/
ORDER/PAGES/commit-reg are recovered; per-mode VALUES are dynamic. Skeleton
(dev 0x9C p0): FIRST write reg 0x0F=0x20 (soft-reset/power); then clears 0x54=0,
0x0E=0, input-select 0x04/0x01, window 0x05/0x06/0x07/0x1F, PLL 0xB2, analog/EQ
0xE2-0xE4/0xAB, output BT.1120 window 0x18-0x1A/0x2D-0x2F/0x3A/0x3B/0x39/0x2C,
output timing gen 0x80-0x86; const from sub_14024dc28: 0xB1=0xC0, 0xB4=0x55 then
RMW &0xFC, page2 0x61. COMMIT/strobe = reg 0x51 (write 0x00 then 0x21 to latch).
=> the HDMI-1080p data VALUES need a LIVE I2C TRACE of a working Windows bring-up
(or an MST3367 datasheet). This is the one remaining unknown.

EDID: fully recovered, 256 bytes, checksums valid -> saved verbatim to
mz0380-edid-hd60pro.txt (from .data @0x140350a20). Write = 8x 32-byte bulk (op
0x1f) to I2C 0xA0; frame word2 = (len<<16)|(block<<8)|0xA0 then 32 bytes; loop
0..0x100 step 0x20. Orchestrator CDevice::UpdateEDID sub_14024678c validates
header + checksum first. Any valid 1080p CEA EDID substitutes.

HPD = a GPIO write (NOT an MST3367 reg): op 0x15, word2=(1<<pin) mask, word3=
(state<<pin). HPD pin = 1 (mask 0x02, value 0x02). Asserted AFTER EDID load
(UpdateEDID loads EDID -> logs "[HOTPLUG %d]" -> raises pin, sub_140287d00).
Plug read-back: op 0x14 GPIO-read, word2=(1<<pin), result bit at +0x0c.

MODE/FORMAT DETECT (real signal read! resurrects milestone B): all op0x1a dev
0x9C. reg 0x55 = signal status, LOCKED/HDMI when (val & 0x3C)==0x3C (gates
detect). Timing from regs 0x40-0x45 (dot-clock + H/V total counters, (hi<<8)|lo),
0x47 (color-depth/pixel-repeat: low nibble 5/6/7 => /5,/3,/2), 0x57&0x3F/0x58/
0x59&0x3F/0x5A/0x5B/0x5C/0x5F (Htot/Vtot/Hact/Vact/sync), page2 0x28/0x29 (counter
hi bits), page1 0x01 bit2 (lock/interlace), 0x4C p2 (audio rate). Dot-clock =
divide 1600000/1250000 by counters; funcs sub_14024d5e0 (detect) + sub_14024a174
(GetHDMIDotClock).

REPLAYABLE CHECKLIST:
  1. reset/power: write dev0x9C p0 reg 0x0F=0x20, then 0x0E/0x54 clears.
  2. run config seq (addresses/order above); HDMI-1080p VALUES from live trace.
  3. commit: reg 0x51 = 0x00 then 0x21.
  4. load EDID: 8x32B bulk (op0x1f) to 0xA0 (bytes in mz0380-edid-hd60pro.txt).
  5. assert HPD: op0x15 pin1 (mask 0x02, val 0x02) - AFTER step 4.
  6. poll reg 0x55 until (v&0x3C)==0x3C; read timing regs, decode per above.
ONLY remaining unknown = the per-mode MST3367 register VALUES (step 2). Get via
a live I2C capture on Windows, or by an op0x1a dump of 0x9C regs after a working
Windows bring-up, or from the MST3367 datasheet.

================================================================================
M11 OP26/27 ARE LIVE (yuan_ioctrl, 2026-07-05 am) — but every I2C read NAKed
================================================================================

Disassembled the on-card `yuan_ioctrl` (ARM ELF from the firmware tar). It
services the I2C-proxy opcodes ep.ko merely sysfs-notifies about — so M4c's
"0x1a is a stub" was wrong at the SYSTEM level: ep.ko's sysfs_notify hands the
command buffer to the userspace daemon, which does the real work. Dispatch is
jump-table on (opcode - 24) @0x97d8; idx8 combo handler @0x9d98.

Mailbox slot map for op 0x1a/0x1b (host BAR0 offsets): dev8 @0x08 (card does
addr7 = dev8 >> 1), reg @0x0c, val/result @0x10. Doorbell/framing identical to
the proven SEND_COMMAND path.

EMPIRICAL RESULT (sentinel in the result slot): every read attempt at every
address on "both buses" left the sentinel untouched — read as NAK-everywhere,
"not even the EDID EEPROM answers". See M12 for why that conclusion was partly
an artifact.

================================================================================
M12 WINDOWS-SIDE RE COMPLETE (2026-07-05 pm) — authoritative I2C protocol,
    GPL MST3367 driver found, corrections to M10/M11. Docs vendored at
    docs/re-2026-07-05/ (HD60-PRO-LINUX-DRIVER.md is the reference).
================================================================================

A Windows-side session disassembled BOTH the retail e60MZ0380.X64.SYS
(v1.1.0.194; loaded v195 byte-compatible) AND the on-card firmware, and found
that an existing GPL-v2 driver (stoth68000/hdcapm, Startech USB2HDCAPM — same
Vatics Mozart 395s + MST3367) already contains the complete MST3367 register
logic RE'd from the same Windows driver. Cross-validated against our disasm.

VERIFIED PROTOCOL (both sides agree; supersedes M11's bus map):
  - READ = op 0x1a, WRITE = op 0x1b, dev = 8-BIT 0x9C in the command word;
    the card computes addr7 = addr8 >> 1 (0x9C -> 0x4E). We already sent 0x9C.
  - 0x1e/0x1f = multi-byte read_s/write_s on BUS0 (NOT bus1 — M11's bus map
    was wrong). 0x1c/0x1d = HDCP-encrypted ops, the ONLY bus1 users.
    0x20 = combo (addr8 @byte0, rw @byte1, len @bytes2-3 of word +0x08;
    payload from +0x0c; card chunks 16B internally).
  - Registers are BANKED: write reg 0x00 = bank (0..3) first; Windows caches
    the current bank (ctx+0x2090). Reg 0x55 etc. are BANK0.
  - No separate MCU: "mcu version = 0" / "NO MCU" — the SoC is the I2C master.
    mcu_i2c_access passthrough theory is dead.
  - Card boots itself from flash at slot power; fw 1.11.1.11 confirmed live.

HARDWARE CONFIRMED (teardown-corroborated): MST3367CMK-LF-170 receiver,
Vatics Mozart 395s encoder SoC (NOT MStar SL6010), ITE IT6621FN HDMI front-end.

WHY M11 SAW "NAK EVERYWHERE" — reconciliation with the 0xEE-sentinel data:
  - The bus0 NAK was REAL, not an artifact: fw forces result=0x00 on I2C
    failure, and every bus0 probe came back 0x00 (sentinel 0xEE overwritten).
    yuan_ioctrl ran, the kernel I2C_RDWR failed. Result-race excluded for bus0.
  - But the pin-map theory ("demo firmware's gpio-i2c doesn't reach the
    MST3367") is now DEAD: the same card, same flashed fw 1.11.1.11, works
    under Windows on this machine. Bus0 GPIO12/13 does reach the receiver.
  - => The receiver I2C domain is GATED until something Windows does first.
    Candidates, in test order: (1) input select / SET_VIC (AUTO.INPUT runs in
    Windows D0 init before receiver access); (2) MZ0380_HwInitialize-era
    config setters (op 0x02/0x08 blocks) / windows_select_fw; (3) a GPIO
    power/reset pin via op 0x15 (HPD is already known = pin1; another pin may
    be receiver reset — read the bitmap with op 0x14 before guessing);
    (4) SA7160_HARDWARE_I2C_RESET-equivalent bus recovery.
  - DISASM CONFLICT to resolve when it matters: our yuan_ioctrl RE put
    0x1e/0x1f (read_s/write_s) on BUS1; the Windows-side RE of the same binary
    puts them on BUS0 (only HDCP 0x1c/0x1d on bus1). Either way the MST3367 is
    bus0 via 0x1a/0x1b; the EDID 0xA0 target of the bulk op 0x1f follows
    whichever bus 0x1f really uses. Bus1 probes were inconclusive (op 0x1e
    does NOT force 0 on NAK; sentinel survived = no info).

GPL DRIVER GIVES THE MISSING VALUES (M10's "one remaining unknown" CLOSED):
hdcapm mst3367-drv.c has init_setup, HPD (BANK0 0xB7 bit1), HDMI reset (BANK2
0x07 f4->04), HDCP reset (BANK0 0xb8 10->00), mode detect (BANK0 0x55 & 0x3c;
htotal 0x6a/0x6b, vtotal 0x5b/0x5c, hperiod 1600000/(0x57<<8|0x58), vperiod
1250000/(0x59<<8|0x5a), interlaced 0x5f bit1, hactive BANK2 0x29<<8|0x28) and
the full video-standards timing table. Local copy:
docs/re-2026-07-05/mst3367-reference-from-gpl-driver.md.
Architecture: register an i2c_adapter whose master_xfer tunnels through the
mailbox, then reuse mst3367-drv.c as a V4L2 i2c subdev nearly unmodified.

256-byte EDID recovered (valid checksums, "SC530-N1", 1080p60 preferred):
docs/re-2026-07-05/elgato-hd60pro-EDID.bin — push verbatim.

BRING-UP ORDER (Windows): reset -> write EDID -> assert HPD -> mode detect.
A source outputs NOTHING until EDID+HPD are presented; reg 0x55 reads
no-signal even with I2C working — do not conflate with NAK.

NEXT: run ./mz0380-mst3367-test.sh (root, module loaded) — baseline 0x9C read
(nonzero result = first-ever ACK), GPIO bitmap read, then the gating
candidates: --with-vic (SET_VIC first), GPL init/reset sequence. First real
ACK unblocks the whole hdcapm reuse path.

================================================================================
M13 HDCAPM BRING-UP REPLAYED ON HW — DECISIVE: addressing correct, mailbox
    result path proven, MST3367 STILL NAKs after input-select + full init.
    Gate = a host-driven receiver reset/power GPIO. (2026-07-05 evening)
================================================================================

Ran mz0380-mst3367-test.sh --with-vic on the card (fw 1.11.1.11 live). Results:

ADDRESSING CONFIRMED CORRECT (no double-shift): we send dev 0x9c; the card
echoes 0x4e back in the dev slot (0x9c >> 1 = 0x4e = 7-bit MST3367). So the
8-bit-address convention is right and the card shifts exactly once.

MAILBOX RESULT PATH PROVEN WORKING: GPIO read (op 0x14, mask 0xee) returned a
REAL value 0x20 in the result slot (BAR0+0x0c) — nonzero data comes back when a
handler produces it. So a 0x00 read result is a true NAK, not a transport bug.

SET_VIC (input select) COMPLETED: ret was -110 only because our wait polls
STATUS; the card actually signalled completion via EVENT bit11 (=0x800, drained
before the next command). So input=HDMI(2) 1920x1080@60 WAS selected.

MST3367 STILL DEAD: after --with-vic + the full GPL hdcapm init_setup + HDMI
reset (B2 0x07 f4/04) + HDCP reset (B0 0xb8 10/00), EVERY read (0x55, 0x6a/0x6b,
0x5b/0x5c, 0x57-0x5a, 0x5f, 0xb7 in BANK0; 0x28/0x29 in BANK2) returns 0x00 =
NAK. Since a write to a NAKing device fails silently (status still "done"), the
init WRITES almost certainly never landed either → the bus is DEAD/GATED, not
merely unconfigured.

WHY (firmware role clarified): the card firmware is a DUMB I2C PROXY. Disasm of
the on-card yuan_ioctrl shows NO MST3367 / reset / GPIO / HDMI logic at all —
only i2c_read/i2c_write/spi. ALL receiver bring-up (SA7160_HARDWARE_I2C_RESET,
mode detect, etc.) lives in the Windows host driver and is replayed over the
mailbox. ep.ko owns the GPIO (gpio_direction_output / gpio_set_value, symbol
gpio_dir_settings), reachable from the host via op 0x14 (read) / 0x15 (set) /
0x17 (dir). So the missing step is a HOST-driven receiver power/reset GPIO that
Windows asserts (its SA7160_HARDWARE_I2C_RESET) BEFORE any receiver I2C. HPD is
GPIO pin1; the receiver reset/power pin is a different, still-unidentified pin.

drivers.sh note (Mozart-SDK generic): bus map `i2c-gpio bus_num=2 scl0=12
sda0=13 scl1=4 sda1=5`; a comment calls gpio12/13 the SSM2603 audio-codec bus
and gpio6/7 the "VIC control pins ... from sensor or video chip" (commented-out
alt). Windows proves /dev/i2c-0 does reach the MST3367 on this same firmware, so
the pin pair is fine — the blocker is receiver POWER/RESET state, not routing.

NEXT (M14): identify the receiver-reset GPIO and pulse it before I2C.
  1. Read the FULL GPIO bitmap (op 0x14 mask 0xffffffff) as a baseline.
  2. Bus-liveness proof: write reg0=0x02 then read reg0 back; 0x02 = bus alive,
     0x00 = confirmed dead. (Distinguishes gate-from-reset vs wrong-result-slot.)
  3. Get the exact reset pin from ep.ko's gpio_dir_settings init (disasm in
     progress) and/or the Windows SA7160_HARDWARE_I2C_RESET's 3 GPIO words
     (dir/data/mask). Then op 0x17 dir-out + op 0x15 pulse low->high on that
     pin, re-probe 0x55. CAUTION: some Mozart GPIOs gate PCIe/DMA — use the
     identified pin, not a blind sweep; reload the module after.

================================================================================
M14 RECEIVER-RESET GPIO IDENTIFIED (Windows e60MZ0380 disasm, 2026-07-05 eve).
    The dead I2C bus is because pin9 (MST3367 reset, active-low) is asserted.
================================================================================

Disassembled the Windows driver's GPIO paths. Two protocol facts + the pin map:

op 0x15 (GPIO SET) is SINGLE-PIN mask+data, NOT a full-bitmap write (corrects an
ep.ko-disasm guess): the only builder (Win helper 0x140287d00, args dl=pin,
r8b=level) emits word2 = mask = (1<<pin), word3 = data = (level<<pin). So it
drives ONLY the masked pin. There is NO op 0x17 (GPIO DIR) builder anywhere —
Windows never sets direction; pins are pre-configured as outputs by the card
firmware. op 0x14 (GPIO READ) builder = 0x140277f98.

SA7160_HARDWARE_I2C_RESET (fn 0x140293408) drives NO GPIO — it is pure MMIO to
the SoC's on-chip I2C-master block (base [ctx+0x108], regs 0xb008 cmd / 0xb00c
status / 0xbfd8..0xbfe8 clock dividers). It resets the I2C CONTROLLER, not the
receiver. (So that string was a red herring for the reset pin.)

GPIO PIN MAP (op 0x15, pin = bit index, mask = 1<<pin):
  pin 1  (0x02)  = HPD (confirmed). assert/deassert around EDID re-read.
  pin 3  (0x08)  = RX / mux enable. driven =1 once at bring-up (0x14028574f).
  pin 8  (0x100) = companion reset/power strap, driven low alongside pin9 low.
  pin 9  (0x200) = *** MST3367 RECEIVER RESET, ACTIVE-LOW ***. Canonical pulse
                   1->0->1 in 5+ routines (0x14027755c etc). RELEASE = drive
                   HIGH (1); assert = drive LOW (0).
  pin 10 (0x400) / pin 11 (0x800) = alt-board-variant resets (guarded branches).
MST3367 bring-up orchestrator = 0x14028548c: interleaves pin9 reset pulses with
I2C writes to slaves 0x9a/0x88/0x9c/0xb8 (0x9c = MST3367; the others = front-end
/ HDCP / companion chips).

=> ROOT CAUSE of the dead bus (M11-M13): the card powers up with pin9 LOW =
MST3367 held in reset, and nothing on the Linux side ever releases it. Every
I2C txn NAKs because the chip is in reset. Windows releases pin9 (+ pin3 enable)
during D0 bring-up before touching the receiver.

FIX (mailbox commands, no direction needed):
  RX enable:      {0x800, 0x15, 0x08,  0x08}        (pin3 = 1)
  reset pulse:    {..,0x15,0x200,0x200} high ->
                  {..,0x15,0x200,0x000} low  (delay) ->
                  {..,0x15,0x200,0x200} high (release)   (pin9 1->0->1)
  companion:      pin8 low during the low phase, high on release.
Then run the hdcapm init + probe reg 0x55.

TEST: ./mz0380-mst3367-test.sh --gpio-reset   (P3g runs exactly this sequence,
then re-probes 0x55 + a write/read-back bus-liveness check). First nonzero read
= MST3367 out of reset = whole bring-up unblocked.

================================================================================
M15  *** SUCCESS *** MST3367 OUT OF RESET, LOCKED TO 1080i. First working HDMI
     signal detection on Linux. (2026-07-05 evening, ran --gpio-reset)
================================================================================

Releasing GPIO pin9 (receiver reset) was the whole blocker. Ran
mz0380-mst3367-test.sh --gpio-reset. Sequence pin3=1 (RX enable), pin9 1->0->1
(release reset), pin8 companion. Results:

- Immediately after the pin9 pulse (before any register init): reg 0x55 read
  0x03 (was 0x00), and a write reg0=0x02 read back 0x02 => the I2C bus is LIVE,
  writes AND reads now ACK. The dead bus was purely pin9 held low.
- After the hdcapm init_setup + HDMI/HDCP resets, the mode-detect block reads a
  real locked signal:
    0x55 = 0x7b   -> & 0x3c = 0x38 (nonzero) = LOCKED / signal present
    0x6a,0x6b = 08,98 -> Htotal = 0x0898 = 2200
    0x5b,0x5c = 04,38 -> (vtotal-ish) 0x0438 = 1080
    0x57,0x58 = 12,8f -> hperiod_raw 0x128f=4751 -> 1600000/4751 ~= 337
    0x59,0x5a = 10,4f -> vperiod_raw 0x104f=4175 -> 1250000/4175 ~= 300 (=>~30fps)
    0x5f = 47 -> bit1 set = INTERLACED
    BANK2 0x28,0x29 = 80,07 -> Hactive = 0x0780 = 1920
  => 1920 active, Htotal 2200, interlaced, ~30 frame/s = 1080i59.94. Matches the
  sibling sc0710's known {htot 2200, 1920, interlaced} 1080i signature exactly.
- User observed: the source (camera) entered HDMI mode and STAYED in it after we
  ran this — the bring-up genuinely activates the input.

PROVEN END-TO-END BRING-UP RECIPE (host, over the mailbox):
  1. RX enable:      op 0x15 pin3 = 1        {0x800,0x15,0x08,0x08}
  2. reset pulse:    op 0x15 pin9 1 -> 0 -> 1 (delay each) ; pin8 low then high
  3. hdcapm init_setup (BANK writes, docs/re-2026-07-05/mst3367-reference-*.md)
  4. HDMI reset B2 0x07 f4->04 ; HDCP reset B0 0xb8 10->00
  5. poll BANK0 0x55; locked when (0x55 & 0x3c) != 0
  6. read timing: Htot 0x6a/0x6b, Vtot 0x5b/0x5c, hperiod 0x57/0x58,
     vperiod 0x59/0x5a, interlaced 0x5f&2, Hact BANK2 0x29/0x28; match table.
  (EDID push + HPD pin1 assert still to be added for sources that gate on EDID;
   this source locked without it.)

NEXT: fold steps 1-6 into the driver as mz0380_mst3367_bringup() +
query_signal, called from card_setup / on an input-select. Then the endgame:
an i2c_adapter tunneling the mailbox so hdcapm mst3367-drv.c runs as a V4L2
i2c subdev. Milestone B (real signal/format to host) is now OPEN and WORKING.

================================================================================
M16 DRIVER INTEGRATION — live VIDIOC_QUERY_DV_TIMINGS working. (2026-07-05 eve)
================================================================================

Folded the bring-up + detect into the driver (mz0380-mst3367.c):
  mz0380_mst3367_bringup()     - pin3=1, pin9 1->0->1, pin8 strap, hdcapm init.
  mz0380_mst3367_read_signal() - poll BANK0 0x55; if locked, read the timing
                                 regs and map to v4l2_dv_timings via a CEA preset
                                 table. Wired into mz0380_query_signal() and
                                 called once at card_setup (dev->mst3367_ready).

RESULT: v4l2-ctl --query-dv-timings returns a full valid timing, e.g.
  1920x1080, total 2200x1125, pixelclock 74.25 MHz, CTA-861 VIC 34 (1080p30).
The interlace bit / fps now decode correctly with stable reads; the M15
"interlaced" read was a racy artifact (see the read-race gotcha below).

*** CRITICAL GOTCHA - mailbox read result race (root of two integration bugs) ***
The card's userspace proxy (yuan_ioctrl) writes the read result into the PARAM3
slot (BAR0+0x10) *after* ep.ko posts command completion, and it writes only the
LOW BYTE of that slot. Two consequences the driver must handle:
  1. Reading dev->cmd_last_param[3] at the send_command() completion edge returns
     the PREVIOUS command's byte on back-to-back reads (off-by-one). The manual
     probe script hid this with a 200 ms "late re-read"; the driver did not.
  2. A full-word sentinel (0xffffffff) never clears in the high bytes (firmware
     touches only byte0), so a "high bytes == 0" settle check times out forever.
FIX (mz0380_periph_read): preload a distinctive LOW-BYTE sentinel (0xa5) into
the result slot, fire, then poll (slot & 0xff) until it != sentinel (~30 ms
budget); return the low byte. NAK -> 0x00, ACK -> value, both != 0xa5.

Milestone B (host reads HDMI format off the receiver) is DONE and in the driver.
Remaining: EDID push + HPD for sources that gate on EDID; then the DMA/stream
path (milestone C) and the i2c_adapter-tunnel refactor to reuse hdcapm verbatim.

================================================================================
M17 STREAMING/DMA PROTOCOL — both-sides RE (ep.ko + e60MZ0380.X64.SYS).
    The current mz0380-dma.c ring model is FICTIONAL; real path below.
    (2026-07-05 evening; milestone C)
================================================================================

Two independent disassembly passes (on-card ep.ko + Windows host driver) plus
our own empirical opcode knowledge converge on this. The existing DMA code
(BAR5 ring base/size/head/tail regs, START/STOP 0x12/0x13, IRQ regs at BAR0
0x100/0x104/0x108) is entirely invented and must be replaced.

REAL MODEL = card-programmed iATU outbound window + BAR0 status window, NOT a
host-programmed ring:
- Host allocates contiguous host buffers and DMAs frame data NOWHERE itself;
  the card's encoder DMAs frames INTO those host buffers via a PCIe iATU
  outbound window (15 MiB / 0xF00000 per channel) that the card programs from a
  host-supplied physaddr.
- Host hands the card the buffer physical addresses via mailbox CONFIG opcodes
  op 0x02/0x03/0x04/0x05/0x08 (host-side proven). Each is a 12-word command:
    word0=0x800, word1=opcode, word2=channel, word3=stride(0x2000),
    word4..=4x (phys_hi, phys_lo) pairs.   (loop channel 0..7)
  Card-side ep.ko confirms: op2/4/8 ISR handlers bulk-copy host cmd words into
  the channels[] array; pcie_set_outbound(idx) then copies the {lo,hi} pair into
  the iATU (elbi_base+0x58/+0x54), region ctrl +0x74, size +0xd4=0xF00000.
- ARM = mailbox op 0x29 SET_VIC_PARAMS (width/height/fps/interlace/color...).
  We already send this and it COMPLETES (M13). size==0 -> card sets no_signal;
  size!=0 -> encoder armed. The card's own video_capture_mgr reacts by setting
  its internal sysfs flags (hready/epint/dency..wency) - these are CARD-SIDE
  plumbing, NOT host-facing, so the host does pure mailbox.
- STOP = mailbox op 0x2a STOP_STREAMING(fw). (Old 0x12/0x13 are bogus.)

FRAME COMPLETION (host-side proven; matches our existing event-ack):
- IRQ cause = EVENT word BAR0+0x30. bit11 = command-done; byte-lanes bits0..23
  = per-channel frame-done. 0xEEEEEEEE=error, 0xAAAAAAAA=ready sentinel.
- Frame token = BAR0+0x40: (token & 7) = completed buffer/descriptor index,
  (token & ~7) = frame pointer/token. Host xchgs it into its own producer ring,
  then clears BAR0+0x50 = 0.
- Per-channel encode status byte = BAR0+0x50+N (== enc_statN sysfs). Packed
  done-count nibbles at BAR0+0x40/0x44/0x48/0x4c (card-side).
- ACK (both sides agree, == our working event ack): BAR5(ctx+0x110)+0xDC = 2 ->
  BAR0+0x30 = 0 -> doorbell BAR0+0x00 = 0x400.
- Buffer geometry: 8 channels x 0x20 (32) descriptors = 256 slots; per-buffer
  stride 0x2000; per-descriptor written-length flags in a host array
  (Windows ctx+0x2590). One full buffer per descriptor index.

OPCODE-NUMBERING CAVEAT (host-side agent error, corrected by our empirics):
- The Windows-side pass claimed "no op 0x29" and "op 0x15/0x17 = stream enable".
  WRONG per hardware: op 0x29 SET_VIC completes on our card (M13); op 0x15 is
  GPIO set (pin9 reset woke the receiver, M15). So op 0x15/0x17 are GPIO, and
  SET_VIC(0x29) is the real arm. Trust the CARD-SIDE ep.ko opcode map + our
  empirics; use the host-side pass only for the buffer-address command SHAPE and
  the completion/token/ack mechanism (which it proved cleanly).

OPEN ITEMS to resolve on hardware (can't be settled statically):
1. Whether SET_VIC alone starts frames once buffers are set, or a separate
   enable/kick mailbox op is needed. (Card userspace does epint internally;
   host may need nothing beyond SET_VIC.)
2. Exact frame BYTE-LENGTH source: a BAR0 status reg (card-side suggests
   +0x28..+0x44 window / +0x50+N byte) vs a card-written host-memory field.
   Read candidates on the completion IRQ and see which tracks frame size.
3. Which of op2/3/4/5/8 maps to which stream (H.264 vs preview vs audio); start
   with op2 = the encoded-video buffer set.

DRIVER REWRITE PLAN (mz0380-dma.c), keeping the vb2/IRQ scaffolding:
  setup:  alloc N contiguous buffers (start 1 channel, 4-8 descriptors of
          0x2000+ each; H.264 frames are small); send op2 (+3/4/5/8) with their
          phys pairs; keep the existing MSI request.
  arm:    on vb2 start_streaming: send SET_VIC_PARAMS(0x29) with detected WxH/fps
          (reuse mz0380_activate_hdmi_locked packing).
  isr:    read EVENT BAR0+0x30; if a frame-done lane, read token BAR0+0x40,
          idx=token&7, copy that buffer to the head vb2 buffer (length from the
          candidate length reg), vb2_buffer_done; ack (0xDC=2,0x30=0,doorbell
          0x400) via the existing mz0380_mb_ack_event.
  stop:   op 0x2a STOP_STREAMING.
  Replace all CHECKME ring/IRQ reg offsets in reg.h with the BAR0 status-window
  offsets above; delete the base/size/head/tail ring regs and the xdma plan-B.

Primary evidence: ep.ko pcie_set_outbound@0x5a8, store_channel_done@0xdc8,
pciep_isr@0x1210, msi.constprop@0x115c, epint_store@0x19b8; Windows arm routine
0x140278ce0, buffer-addr ops 0x14027957b.., ISR 0x14028ec70 (EVENT@0x30,
token@0x40), ack 0x14028eca4.

================================================================================
M18 STREAMING FIRST-CUT TEST — hang fixed; SET_VIC alone does NOT start frames.
    (2026-07-05 late)
================================================================================

Ran the first-cut streaming path (enable_dma=1 enable_video=1). Findings:

BUG FIXED (regression from enabling the MSI ISR): mz0380_send_command took the
MSI-wait branch whenever msi_enabled, but this card completes short commands via
STATUS bit0 with NO interrupt (only ~3 MSI ever fire). So every command
(GPIO 0x15 in bring-up, 0x1b register writes) timed out 500-1000 ms each -> the
MST3367 bring-up alone stalled insmod ~30 s ("stuck"). Fix: send_command now
ALWAYS polls STATUS for completion and additionally honours dev->cmd_complete
(set by the ISR if it catches an EVENT-bit11 command-done). insmod is instant
again, no timeouts.

EMPIRICAL (open item #1 resolved): with buffers programmed via op 0x02 + arm via
SET_VIC_PARAMS(0x29, ret=0), NO frames flow: IRQ count stays 3, zero frame-token
EVENTs, v4l2 capture blocks, /tmp/o.h264 = 0 bytes. => SET_VIC is necessary but
NOT sufficient; a separate START KICK is required, and/or op 0x02 is not the
right buffer-address opcode.

STILL UNKNOWN (both RE passes were fuzzy here): the exact HOST mailbox opcode
that makes the card's video_capture_mgr start a channel. Card-side said the kick
is an internal `epint` sysfs (set by video_capture_mgr, not host-facing);
host-side said op 0x17/0x15 "enable" - but op 0x15 is GPIO on our card (M14/M15),
so that mapping is suspect. Need: RE video_capture_mgr's mailbox-command
handling to find which host opcode -> channel start (and confirm the
buffer-address opcode), rather than guessing on hardware. Frame length reg
(item #2) and op->stream map (#3) remain open behind this.

================================================================================
M19 STREAMING START = SET_VIC spawns tinyvenc (video_capture_mgr RE). Remaining
    gaps need tinyvenc RE or a live Windows trace. (2026-07-05 late)
================================================================================

RE'd video_capture_mgr (ARM, stripped; resolved via PLT + literal pool). It is a
command DISPATCHER daemon, not the encoder:
- opens /sys/vpl_pciep/epint O_RDWR, poll()s it, on event pread()s the 44-byte
  mailbox command struct, dispatches on cmdbuf[0]=opcode (@0x8df4+).
- op 41 (SET_VIC, @0x8ea8): populates a per-channel channels[] config from the
  command bytes, sprintf's a `./tinyvenc5 -D ... -w %d` (or venc7) command line,
  and **system()-spawns tinyvenc @0x9348**. THIS is the start - there is NO
  separate epint/ency/hready mailbox kick the host must send. SET_VIC with valid
  fields IS the "go".
- op 42 (STOP): killall -9 the encoders, then writes hready=0.
- BAIL branches in the op-41 handler (why tinyvenc may never spawn):
    @0x8ec4: if cmdbuf[5] == 0  -> jumps to the no-signal/no-bitstream path.
    @0x8eb4: if table[cmdbuf[5]] == 8 -> reject.
  cmdbuf[5] = mailbox byte 0x09 = params[0] bits[15:8]. Our current packing puts
  fps there (nonzero), so we likely pass 0x8ec4 - but the exact field semantics
  (channel/format/codec at cmdbuf+4/+5/+6, W/H at +8/+10) are not fully pinned,
  and a wrong codec/format byte still mis-launches or picks the wrong tinyvenc.

- The *ency sysfs (dency/qency/hency/wency) are written by yuan_ioctrl = the SDI
  (GV7601) path, NOT the HDMI/MST3367 path. Irrelevant here.

BUFFER-ADDRESS OPCODE (ep.ko pciep_isr): op2 copies host cmd words -> BAR0
+0x08..+0x24; op4 -> +0x48; op8 (PREVIEW_BUF_EX) -> +0x28. None call
pcie_set_outbound directly; the exported pcie_set_outbound(idx,lo,hi) is called
by TINYVENC, reading the address the host left at BAR0+0x08 (op2). So op2 is the
best candidate for the encoded-video buffer physaddr - confirm the words land at
BAR0+0x08.

FRAME LENGTH: NOT in ep.ko. store_channel_done writes only 4-bit per-channel
status nibbles (BAR0 +0x40/44/48/4c) + the done token (+0x30) + MSI. The encoded
byte length is written by tinyvenc into its own output descriptor. => need to RE
tinyvenc5/7 (now extracted to scratchpad fw/yuan_demo_sdi/) for the length field
+ the exact channels[] struct it consumes.

WHY NO FRAMES (hypotheses, unresolved): (1) SET_VIC struct fields mis-packed so
tinyvenc launches wrong / not at all; (2) hready gate not asserted by the host
(host mechanism unknown - card sysfs, host must trigger via a mailbox op/BAR
write; op1/INIT may or may not set it); (3) buffer physaddr not reaching BAR0+0x08.

RECOMMENDED NEXT (most efficient): a LIVE Windows mailbox trace - breakpoint
SEND_COMMAND @0x140285074 (or the arm routine @0x140278ce0) in WinDbg during a
working 1080p capture and dump every {opcode, args} in order. That resolves ALL
remaining unknowns at once: the exact buffer opcode+physaddr args, the exact
SET_VIC 44-byte struct, whether/how hready is asserted, and the start ordering.
Static alternative: RE tinyvenc5 for the channels[] struct + length descriptor.

================================================================================
M20 STREAMING CONTRACT NAILED (tinyvenc5 + video_capture_mgr RE). SET_VIC bug
    found + fixed; frame-length located. (2026-07-05 late)
================================================================================

SET_VIC (op 0x29) 44-byte struct - EXACT field map (video_capture_mgr op-41,
@0x8ea8; channels[] base 0x141d4 stride 60; tinyvenc argv reads this record):
  struct byte (=param word: params[0]=struct[4..7], params[i]=struct[4+4i..]):
    [4]     channel (0)
    [5]     fw/index
    [6]     FORMAT/CODEC: {2,3}=tinyvenc5 H.264 (2 prog, 3 interlaced - inferred),
            7=tinyvenc7. MUST be in {2,3} for venc5.
    [8..9]  width  (LE, MUST be >127)
    [10..11]height (LE, MUST be >127)
    [22..23]input_frame_width
    [24..25]input_frame_height
    [26..27]bitstream_num (MUST be >=1)
    [30]    is_nosg
The op-41 bail branches (0x8eb4 cmp #8; 0x8ec0) are argv/boot-fixed
(product_type, debug flag), NOT host-controllable, and BOTH still spawn tinyvenc.
So SET_VIC always launches the encoder; the host-side musts are: width>127,
height>127, byte6 in {2,3}, bitstream_num>=1.

THE BUG (why the first cut spawned tinyvenc but produced nothing): our SET_VIC
sent bitstream_num=0 and input_frame_w/h=0 (params[2..7] were 0). FIXED in
mz0380_dma_start:
    params[0] = fmt << 16;                    // struct[6] format 2/3
    params[1] = (h<<16) | w;                  // width, height
    params[4] = w << 16;                       // struct[22..23] input_w
    params[5] = (1<<16) | h;                   // struct[26..27]=1, [24..25]=input_h
(1080p example struct: 29 00 00 00 00 00 02 00 80 07 38 04 00*10 80 07 38 04
 01 00 ...; 0x780=1920, 0x438=1080.)

FRAME DELIVERY (tinyvenc5, not stripped):
- Per frame: DMA via the SoC MMA engine (TK_MMA_SetOptions/ProcessOneFrame,
  PCIEtOptions @0x7ed8c) from a physically-contiguous MemBroker buffer; ep.ko's
  ATU outbound window (programmed from the host addr at BAR0+0x08 via
  pcie_set_outbound) relays it into host DRAM. tinyvenc never reads BAR0 itself.
- Notify: pwrite(/sys/class/vpl_pciep/channel_done, record, 24) once per frame.
  record = 6xu32; word0 = channel. ep.ko store_channel_done turns this into the
  host MSI + the BAR0+0x40/44/48/4c status nibbles + BAR0+0x30 EVENT.
- FRAME BYTE LENGTH = binary u32 at **enc_stat struct +0x08** (from PB_GetEncBytes
  delta), also mirrored in the channel_done record. keyframe flag at result+0x94.

RISK / OPEN: tinyvenc's EncodingGroup::enable_dma gate (@0x7ed88) is set 1 only
when input-type [cfg+0x02] == 4 or 9 in this yuan_demo_sdi build; our HDMI format
is 2/3. The retail Windows driver clearly DMAs HDMI frames with this same fw, so
either [cfg+0x02] is a different (input-select) value than byte6, or the gate is
bypassed on the HDMI path. Watch for it: if the SET_VIC fix still yields no host
IRQ, this gate (input-type) is the next thing to resolve.

Host buffer address: goes to the card via op 0x02 (lands at BAR0+0x08, feeds
pcie_set_outbound). hready is NOT a hard gate; op2-before-SET_VIC is the real
requirement.

================================================================================
M21 hready is NOT the blocker (ep.ko RE). Confirms enable_dma is the sole gate.
    (2026-07-05 late)
================================================================================

RE of ep.ko host_ready/channel_done path:
- /sys/class/vpl_pciep/channel_done: .show=show_host_ready (returns host_ready
  @.bss+0), .store=store_channel_done. Separate writable node `hready`
  (hready_store -> kstrtoint -> writes .bss+0 = host_ready).
- host_ready is a STATUS word, NOT a hard interlock: store_channel_done and the
  MSI path (msi.constprop @0x18a0) branch on pirq_base != 0, not on host_ready.
  No `if(host_ready) deliver` test in the frame path.
- NO host mailbox opcode sets host_ready (op1/INIT included - no such store).
  It's set by on-card userspace via the hready sysfs node.
- Re-confirmed op2 (@0x1458): copies mailbox [ep_command+0xc..+0x28] ->
  ep_regs+0x08..+0x24 = the DMA-target address staging. Matches our op2.

=> hready is a RED HERRING for "no frames". The sole remaining blocker is
tinyvenc's enable_dma input-type gate (M20: [cfg+2] must be 4/9; HDMI=2/3).
Next session: don't re-chase hready. Resolve enable_dma via the live Windows
trace (what the retail driver sends to turn on host-DMA for HDMI) or by fully
tracing tinyvenc5 main @0xded0 ([r5+2] source, literals @0xef04/0xedbc).

================================================================================
M22 THE REAL BLOCKER: missing START_STREAMING (mailbox op 0x06). enable_dma gate
    was a MISREAD. Fix implemented. (2026-07-06, full on-card static RE)
================================================================================

Path B (static RE of the on-card yuan_demo_sdi userspace) resolved the gate
definitively. M20's "enable_dma input-type 4/9 gate" was WRONG - it conflated
three different globals that happen to be tested near each other in tinyvenc5.

GROUND TRUTH (symbols from the not-stripped tinyvenc5 ELF; all addrs its .text):
  0x7ed88 = EncodingGroup::enable_dma        (byte)
  0x7dd3c = EncodingGroup::total_channel_num (word)  <- "%d channels"
  0x7dd40 = EncodingGroup::preview_settings  (struct; +2 = input-format byte)
- enable_dma is WRITE-ONLY in tinyvenc5: set 0 @0xe350 (STOP path), set 1
  UNCONDITIONALLY @0xe97c inside the START_STREAMING handler. Nothing in .text
  reads it to gate frames. So it never blocked host DMA. (It is a status byte
  other code/ep.ko may observe; not a host-controllable gate.)
- The `==4 || ==9` compare @0xe968 (that M20 read as the enable_dma gate) really
  sets total_channel_num=4 (SDI multi-channel override). total_channel_num is
  otherwise loaded from argv via atoi @0xe2e0 in the getopt loop, so the
  video_capture_mgr-spawned path already gets a valid channel count (>=1) from
  the SET_VIC bitstream_num field. Not a blocker either.

ACTUAL START PROTOCOL (tinyvenc5 + video_capture_mgr + ep.ko, all cross-checked):
1. Both tinyvenc5 and video_capture_mgr read 44-byte commands from
   /sys/vpl_pciep/epint (O_RDWR). ep.ko's epint_show copies the raw host mailbox
   command (incl. opcode word) to readers; epint_show only passes the command
   set {6,7,9,40,41,42,45,47,49,80,81,82,96,97,98,110}.
2. Host SET_VIC_PARAMS (op 41 = 0x29): video_capture_mgr dispatches it (@0x8ea8),
   builds the long argv (-a..-w = fps/res/interlace/input_w/h/bitstream_num/...)
   and system()-spawns `./tinyvenc5 -D -a.. -w..`. ep.ko op41 also sets
   no_signal=0 when WxH!=0. video_capture_mgr does NOT handle cmd 6 and does NOT
   self-issue any start - it only spawns.
3. tinyvenc5 boots, opens epint, its FIRST read expects SET_VIC(41) (acks with a
   44-byte pwrite @0xf264), then enters a poll/pread loop.
4. Host START_STREAMING (op 6): tinyvenc5's dispatch is a jump table indexed by
   (cmd-6); table[0] (cmd 6) = the start block @0xe954, which spawns the encoder
   channels and begins the per-frame path (TK_MMA + pwrite
   /sys/class/vpl_pciep/channel_done -> ep.ko MSI + outbound ATU into host DRAM).
   ep.ko op6 ISR handler (@0x1854): if no_signal==0, sysfs_notify(epint) -> wakes
   tinyvenc5's poll. Op6 carries NO payload.
=> Frames need op 0x29 THEN op 0x06. Our driver only ever sent 0x29 (+ SET_BUF
   0x02). That is why IRQ count stayed at 3 with a 0-byte file, through every
   SET_VIC field fix - the encoder was spawned and idle, never told to start.

M6 RECONCILED: M6 read op6 from the ep.ko ISR only and labelled it
"enc-status-bank+notify", concluding "START_STREAMING is NOT an ep.ko opcode."
The ISR view was right (op6 = a gated sysfs_notify) but missed the tinyvenc5
consumer for which that notify IS the start kick. No contradiction; M6 was
half the picture.

FIX (implemented this session):
- mz0380-reg.h: MZ0380_CMD_START_STREAMING 0x12(bogus) -> 0x06.
- mz0380-dma.c mz0380_dma_start(): after SET_VIC succeeds, msleep(500) then
  send op 0x06 (no params). The delay lets the freshly system()-forked tinyvenc5
  exec + consume its first-read SET_VIC before our op6 lands as the next command
  (avoids racing tinyvenc5 start-up; tune on hw). Builds clean.
- STOP path unchanged: op 0x2a = 42 is handled by video_capture_mgr (@0x9358,
  killall tinyvenc) - correct as-is.

NEXT (hardware test): load, start capture, expect two acks (SET_VIC then
START_STREAMING) then a rising IRQ/token count + nonzero-length frames. If op6
acks but tokens stay flat: (a) increase the msleep (tinyvenc5 not up yet), or
(b) confirm no_signal==0 at op6 time (needs live locked source + real WxH in
SET_VIC). Frame LENGTH still at enc_stat+0x08 (M20) - wire once frames flow.

================================================================================
M23 THE OFFSET BUG + is_nosg BISECTION. Encoder was spawned-but-silent from a
    +2-byte SET_VIC packing error (M20/M22 were wrong); downstream DMA targets
    host 0 because SET_BUF(op2) never lands our buffer addr in channels[]. Full
    HD60 Pro fw-1.11 on-card RE via llvm-objdump. (2026-07-07)
================================================================================

FIRMWARE SOURCE: the HD60 Pro's own blob /lib/firmware .../MZ0380.HD.HEX
(gzip + GNU tar, ~2.5 MB) extracts to yuan_demo_sdi/ - the SAME package M6/M20/
M22 RE'd. So tinyvenc5 / video_capture_mgr / ep.ko ARE this card's fw-1.11
runtime, not a foreign SDI SDK. Boot rc.local: yuan_ioctrl + audio_capture_mgr
+ video_capture_mgr (latter spawns tinyvenc on SET_VIC). Un-stripped (tinyvenc5
~1162 syms). Disasm: llvm-objdump --triple=armv7-linux-gnueabi (system objdump
has no ARM target). Tree: scratchpad/fw/yuan_demo_sdi/.

no_signal (ep.ko, RE-CONFIRMED - kills the M20 "SoC signal-detect" idea):
  written ONLY in the op41 handler, purely from W/H:
    no_signal = (width==0 || height==0) ? 1 : 0   // width=ep_command[8],H=[0xA]
  No hardware/BT1120 read touches it; host fully controls it via op41 W/H. Gate:
  many opcodes incl. op6 hit `if(no_signal){printk("$$$ cmd(%d) => no signal");
  MSI; return;}`. Our W/H@8/10 were always correct => gate was OPEN the whole
  time; no_signal was never the blocker.

op6 (START) is FIRE-AND-FORGET (ep.ko handler .text 0x1854): if no_signal==0 it
  sysfs_notify(epint) [epint_1080p iff op41 byte[6]==7]. It posts NO mailbox
  cmd-done -> host MUST send op6 with timeout_ms=0. Waiting for a completion =
  the false -ETIMEDOUT(-110) we hit. epint_show returns the LIVE ep_command
  mailbox (len = ep_cmds_size[opcode]); op6 relies on its command still sitting
  in the mailbox, doesn't copy.

tinyvenc5: opens /sys/vpl_pciep/epint O_RDWR, poll()s. FIRST blocking read must
  be SET_VIC(0x29) (acks by pwrite'ing 44B back); later cmds dispatch via jump
  table index (opcode-6). op6 -> on_start_thread -> EncodingGroup::Start ->
  init_func: TKMF_VideoSrc_Init (fail => "Can't create video capture ->exit!!!")
  + spawn encode_handler (always) + fake_frame_process (iff is_nosg!=0). NO
  signal-lock check in tinyvenc: it pulls frames from an SSM shared-mem ring the
  CAPTURE side fills from BT1120; empty ring => usleep(1000) loop, silent.

SET_VIC 44-byte struct - byte offsets VERIFIED 3 ways in video_capture_mgr's
op41 handler (THE FIX):
  [4]ch [5]fps [6]fw/format(2 prog|3 int, ==7 alt) [7]interlace
  [8:9]width [10:11]height [12]m [13]flip [14]mirror [16:19]color_info
  [20:21]x_start [22:23]y_start [24:25]input_frame_width [26:27]input_frame_height
  [28]bitstream_num(MUST>=1) [29]osd_en [30]osd_size [31]is_nosg
  [35]is_slave [36:39]nosg back/y/u/v.  Spawn GATE: width<=127||height<=127 =>
  "Wrong resolustion", no spawn (our W/H pass).
  BUG: M20/M22 put input_w@22,input_h@24,bitstream_num@26 - all +2 short. FW
  then read bitstream_num=0 (byte28 unset) + junk input dims => tinyvenc spawned
  but emitted nothing. This (NOT timing, NOT no_signal) was the silence. Proof
  it wasn't timing: start_delay sweep 2000/4000ms = flat 3 IRQ.
  CORRECTED packing (params[i]->struct[4+4i]): params[0]=fmt<<16;
  params[1]=params[5]=(h<<16)|w; params[6]=1|((is_nosg?1:0)<<24).

is_nosg BISECTION (on-hw, decisive): stream_nosg=1 sets byte[31] -> card spawns
  fake_frame_process (black-frame gen, memset buf, timer channel_done, NO
  capture/SSM dep). Result: IRQ 164 3->7 AND a burst of IOMMU IO_PAGE_FAULTs at
  host 0x0,0x80,0x100..0x480 (128B stride). => the encode -> channel_done -> MSI
  -> outbound-ATU -> host-DMA path is ALIVE, but the ATU host target is ~0, not
  our buf 0xfff80000. Per ep.ko pcie_set_outbound the target comes from
  channels[] (window*24+field; ATU low@0x58/high@0x54), populated by op2/4/8
  copying ep_command[0xc..0x28]. So SET_BUF(op2) isn't landing our physaddr
  (likely hi/lo order or channel-entry offset; our buf hi=0 read as target-low
  => DMA to host 0). ATU aperture base 0x90000000 is hardcoded card-side (=the
  probe-time fault).

TWO REMAINING BUGS, cleanly separated by is_nosg:
  (1) DOWNSTREAM: SET_BUF(op2) channels[] host-addr layout wrong -> ATU hits
      host 0. Fix = correct op2 command layout (exact offsets being RE'd).
  (2) UPSTREAM: real BT1120->SSM capture not delivering -> real encoder idle
      (TEST1 nosg=0 stayed at 3 IRQ, no faults). Needs VIC / MST3367 BT1120
      output routing on the host-init side.

HOST FIXES THIS SESSION (built, hw-tested): op6 fire-and-forget (send_command
  timeout_ms=0); SET_VIC offsets corrected (bitstream_num@28=1, input dims@24/
  26); module params start_delay_ms (timing - now known irrelevant) and
  stream_nosg (is_nosg bisection lever, 0644). NEXT: fix SET_BUF layout, re-test
  nosg=1 (expect frames into our buffer, no fault), then upstream BT1120.

================================================================================
M24 DMA TARGET: engine works, but host target low32 is stuck 0 - frames land at
    (cmd[0xc] << 32) + aperture_offset. SET_BUF/channels[]/ELBI fully traced.
    Open blocker (paused). (2026-07-08)
================================================================================

is_nosg=1 (fake_frame_process) proved the encode->channel_done->MSI->outbound-
ATU->host-DMA chain is ALIVE: the card actively DMAs frames. The only defect is
the DESTINATION address. Empirically nailed (three hw runs):
  cmd[0xc]=0          -> IOMMU faults at host 0x0,0x80,0x100..0x480 (128B stride)
  cmd[0xc]=0xfff80000 -> faults at 0xfff8000000000000,..80,..100
  => host_target = (cmd[0xc] << 32) + aperture_offset;  LOW32 == 0 ALWAYS,
     independent of cmd[0x10]. The 0x80 stride is the frame walking the aperture.

CARD-SIDE TRACE (ep.ko + vpl_dmac.ko, llvm-objdump + capstone):
- channels[] entry = 192 bytes = 24 x 8-byte {hi,lo} slots per channel (stride
  192 = 0xC0 * channel). SIX buffer-load opcodes each memcpy ep_command[0xc..0x28]
  (8 words) into a DIFFERENT window region, each skipping slot0:
    handler@0x1458 -> entry+0x08 (also zeros entry+0), @0x14b8 -> +0x28 (sets
    [0x61c]=opcode, the SET_BUF_EX kick), @0x1588 -> +0x48, @0x12b0 -> +0x68,
    @0x1408 -> +0x88, @0x1518 -> +0xa8 (sets [0x620]=1). (The earlier "op2->+0x00"
    was WRONG; our op2 buf0 hi/lo land at entry+0x08/+0x0c, i.e. slot1.)
- pcie_set_outbound(window,ch,bufidx) (ep.ko @0x5a8) is the SOLE writer of the
  ELBI outbound-target regs. entry = channels + 8*(24*ch + (bufidx-1) + wfield).
  window0 wfield=0. It writes: ELBI[0x74]=0x90000000 (aperture base), [0x7c]=
  0x91FFFFFF (limit, 32MB), [0x54]=entry+4 (intended LOW), [0x58]=entry+0
  (HIGH, empirically effective), [0xd4]=0xf00000 (15MB). No ldrd; plain ldr/str.
- ELBI is a Vatics-PROPRIETARY glue block (ioremap phys 0x82040000, size 256),
  NOT DesignWare iATU (that's dbi_base 0x82000000). Init programs the INBOUND
  window on the same block: base@0x30, limit@0x38, target-LOW@0x40, target-HIGH@
  0x48 (spacing +8 from base+0x10). Init also ZEROES ELBI 0x54..0xCC at probe.
- INFERENCE (unproven): by inbound's field spacing, the OUTBOUND effective low-
  target is likely ELBI 0x84 (base 0x74 + 0x10), and pcie_set_outbound writes the
  low to 0x54 - a register the HW ignores (init-zeroed, never re-read). That is
  exactly "HIGH tracks channels+0, LOW always 0". So no op2/op4/op8 payload can
  set low32 via this path (only pcie_set_outbound touches ELBI, and it targets
  the wrong reg). vpl_dmac VPL_DMAC_StartTail/ISRTail copy profile[0x8..0x34] ->
  DMA-engine MMR (the real per-frame dst descriptor); profile[0x38/39/3a/3b] =
  window/channel/bufindex/outbound-enable, set at runtime by the card DMAC mgr.

CAVEAT (why the "unreachable" read is probably incomplete): the HD60 Pro works
normally with Elgato's driver on <4GB-RAM PCs, so low32 MUST be reachable by some
host path. We are likely mis-driving the buffer setup (wrong opcode, or the DMAC
manager builds profile[dst] from a channels[] slot we populate wrong), NOT hitting
a true hw dead-end. Unresolved.

PAUSED here (user choice). Two ways forward next session:
  (1) RIGHT WAY: RE how the WORKING path gets the host address into the DMA
      descriptor - trace vpl_dmac.ko profile[dst] source + the Windows driver's
      frame-buffer setup (our op2 came from M17, may be the wrong command).
  (2) WORKAROUND (guaranteed by the confirmed model): put the DMA buffer at a
      4GB-aligned IOVA (low32=0) via an explicit iommu_map at IOVA 0x1_0000_0000,
      set cmd[0xc]=high32(=1), cmd[0x10]=0 -> frames land in-buffer. Needs IOMMU
      remap or a reserved-mem boot param; a hack, but works if (1) stalls.
Fault flags: 0x20 (read-ish) vs 0x30 seen across runs. Disasm cached:
scratchpad/disasm.txt (ep.ko), scratchpad/dmac.txt (vpl_dmac.ko); fw tree at
scratchpad/fw/yuan_demo_sdi/.

================================================================================
M25 DMA TARGET, ROUND 2: host->card command aliasing PROVEN CORRECT (no off-by-N
    in op2); opcode->window/slot map completed; ELBI outbound target is the sole
    host-address input in the entire card firmware. (2026-08-07)
================================================================================

Re-extracted MZ0380.HD.HEX (gzip+tar -> yuan_demo_sdi/), re-disassembled ep.ko +
vpl_dmac.ko, and disassembled the WINDOWS driver (MZ0380.X64.SYS, PE32+ x86-64,
extracted from Game_Capture_HD60_Pro_1.1.0.177 via 7z).

1) MAILBOX ALIASING - our op2 packing is CORRECT (this was the top suspect after
   the SET_VIC +2 bug; it is now ruled out).
   ep.ko init (mozart_module_init):
     g[0x4]  = dma_alloc_coherent(0x60 bytes), bus addr -> g[0x724]
     g[0x624]= that buffer + 4                          <- "ep_command" base (r4)
     ELBI[0x08] = g[0x724] + 4                          <- card target of window0
     ELBI[0x30] = DBI[0x10] + 4    (host BAR0 base + 4) <- host base  of window0
     ELBI[0x38] = DBI[0x10] + 0x5f (host BAR0 + 95)     <- host limit of window0
   pciep_isr reads the opcode at [r4] == host BAR0+0x04. Therefore
        ep_command[i]  ==  host BAR0 offset (4 + i).
   So ep_command[0xc] == BAR0+0x10 == our PARAM(3), ep_command[0x10] == BAR0+0x14.
   Our mz0380_stream_program_bufs writes {channel, stride, hi, lo, ...} to
   PARAM(1..) i.e. BAR0 0x08,0x0c,0x10,0x14 -> lands exactly on ep_command
   [0x4]=channel, [0x8]=stride, [0xc]=hi, [0x10]=lo. VERIFIED CORRECT.
   (DBI = ioremap(0x82000000,4096) = DesignWare dbi; DBI[0x10]/[0x14] = the BAR0/
   BAR1 addresses the HOST BIOS assigned - that is how the card learns them.)

2) COMPLETE buffer-load opcode -> channels[] map (pciep_isr dispatch @0x1258):
     op 0x02 -> channels[ch] + 0x08   (also zeroes +0x00), window0 slots 1..4
     op 0x08 -> +0x28                 window0 slots 5..8
     op 0x04 -> +0x48                 window1 slots 1..4
     op 0x05 -> +0x68                 window2 slots 1..4
     op 0x03 -> +0x88                 window3 slots 1..4
     op 0x64 -> +0xa8                 window4 slots 1..4
   Each copies ep_command[0xc..0x28] (8 words = 4 x 8-byte slots), channel index
   from ep_command[0x4], stride 192 (0xC0) per channel. pcie_set_outbound's
   window->wfield add is {w0:+0, w1:+8, w2:+12, w3:+16, w4:+20} slots, which is
   exactly the region each opcode fills, starting at bufidx 2 (slot0 unused).
   => op2 IS the right command for window0. Not the M17 guess it was feared to be.

3) vpl_dmac.ko VPL_DMAC_StartTail (0xb5c) - definitive:
     if (profile[0x3b]) pcie_set_outbound(profile[0x38], profile[0x39],
                                          profile[0x3a]);   /* win, ch, bufidx */
     then copies profile[0x08..0x34] -> DMA MMR, MMR[0x8] |= 6 (go).
   profile[0x08..0x34] are CARD-LOCAL addresses (the 0x90000000 outbound
   aperture). No host address exists anywhere in the DMA descriptor.
   => the ELBI outbound target written by pcie_set_outbound is the ONLY host
   address input in the whole card firmware. There is no second path to RE.
   pcie_set_outbound is EXPORT_SYMBOL_GPL'd and vpl_dmac.ko is its only caller.

4) pcie_set_outbound (ep.ko 0x5a8), exact:
     entry = channels + 8*(24*ch + (bufidx-1) + wfield)
     ELBI[0x50] = 1              ELBI[0x74] = 0x90000000   ELBI[0x7c] = 0x91FFFFFF
     ELBI[0x54] = *(entry+4)     ELBI[0x58] = *(entry+0)   ELBI[0xd4] = 0xF00000
   Our slot layout {word0=hi, word1=lo} therefore feeds 0x58=hi, 0x54=lo - which
   is what the firmware intends. Hardware honours 0x58 and drops 0x54 (M24).

5) ELBI window-field spacing, now with two independent samples (fw_store and
   logo_store @0xc98/0xd00 program a SECOND inbound window at runtime):
     window0: host base @0x30, host limit @0x38, card target @0x08
     window1: host base @0x40, host limit @0x48, card target @0x10
     (init also writes ELBI[0x18]=0xc7000030 -> implies a window2 whose base/limit
      would be @0x50/@0x58 - exactly the registers pcie_set_outbound scribbles.)
   Two readings remain, and they are mutually exclusive:
     (a) 0x50=enable, 0x54/0x58 = outbound target lo/hi, 0x74/0x7c = card-side
         base/limit. Firmware is right, HW should honour 0x54 -> then our low32
         is being lost for some other reason.
     (b) 0x50/0x58 are inbound-window2 base/limit and the real OUTBOUND target
         low/high live elsewhere (0x84/0x8c by the +0x10 base spacing). Firmware
         writes the low to a register the HW ignores -> M24's "low always 0".
   The hw evidence (host_target == (word0 << 32) + aperture_offset, exactly)
   fits (b). Nothing in the extracted firmware writes ELBI 0x84.

6) WINDOWS DRIVER (MZ0380.X64.SYS) - what it confirms and what it does not:
   - Mailbox is identical: ctx[0x108] = BAR0 MMIO; opcode -> +0x04, params ->
     +0x08.., doorbell = write 0x800 to +0x00, completion = poll +0x2c bit0,
     500 x 1ms, then "MZ0380 COMMAND TIMEOUT". Matches mz0380-reg.h exactly.
   - Frame buffers: default path (ctx[0xc0]==0) is
     DmaOperations->AllocateCommonBuffer -> logical addr in ctx[0xc8], VA in
     ctx[0xd0]; fallback path is MmAllocateContiguousMemorySpecifyCache with
     HighestAcceptableAddress walked 0x07FFFFFF -> 0xFFFFFFFF in 0x8000000 steps.
     Both are <4GB, i.e. the Windows path always has high32 == 0 and a NON-ZERO
     low32 - the exact case our hardware says is impossible. So either the HW does
     honour a low-target register (and reading (a) is right), or the Windows
     driver reaches it by a command we have not yet located.
   - Opcodes >= 0x50 are NOT handled by ep.ko (dispatch sends 80/81/82 to the
     bare notify path @0x1824); they are consumed by card userspace
     (video_capture_mgr / tinyvenc5) reading the same command buffer. The
     Windows driver does send op 0x50 with 32-byte payloads. Not yet decoded.

NEXT (in order, cheapest decisive first):
  A. HW PROBE, ~20 min, settles (a) vs (b) for good: send op2 with the pair
     order SWAPPED (word0=low32, word1=high32) and with a deliberately
     recognisable low (e.g. buffer at a known IOVA). Watch the AMD-Vi fault
     addresses.
       fault at (low32 << 32)          -> word0 is the only word that reaches the
                                          HW -> reading (b) -> use workaround #2.
       fault at correct 64-bit address -> reading (a); the earlier low32 loss was
                                          a driver-side artifact, and we are done.
  B. If (b): 4GB-aligned IOVA workaround (M24 item 2) - iommu_map at
     0x1_0000_0000, cmd[0xc]=1, cmd[0x10]=0.
  C. Decode op 0x50 in the Windows driver + video_capture_mgr; it is the only
     remaining host->card channel we have not read.
Artifacts this session: scratchpad/fw/yuan_demo_sdi/ (re-extracted),
scratchpad/disasm.txt (ep.ko), scratchpad/dmac.txt (vpl_dmac.ko),
scratchpad/win/ (Windows driver package), scratchpad/win64.txt (MZ0380.X64.SYS).

================================================================================
M26 SWAP PROBE: CLOSED. The card latches ONLY the high 32 bits of the host DMA
    target; ELBI 0x54 is ignored by the hardware. Fix = 4GiB-aligned IOVA.
    (2026-08-07, hardware-verified)
================================================================================

PROBE (mz0380-m25-swap-probe.sh, buf_pair_swap module param, stream_nosg=1).
Same buffers both runs, dma=0xfff80000 / 0xfff00000 / 0xffe80000 / 0xffe00000
(all high32 == 0):

  buf_pair_swap=1  slot {word0=fff80000, word1=00000000}
                   -> IO_PAGE_FAULT at 0xfff8000000000000, +0x80, +0x100 .. +0x480
  buf_pair_swap=0  slot {word0=00000000, word1=fff80000}   (default packing)
                   -> IO_PAGE_FAULT at 0x0, 0x80, 0x100 .. 0x480

The target followed word0 in BOTH runs and word1 in NEITHER. With word1 =
0xfff80000 the low half of the target was still exactly 0. Therefore:

    host_target = (slot word0 << 32) + aperture_offset      [CONFIRMED]
    slot word1 -> ELBI[0x54] -> DISCARDED BY THE HARDWARE   [CONFIRMED]

This settles M24/M25 reading (b): pcie_set_outbound writes the host LOW target
to a register the Vatics glue block does not implement (init zeroes 0x54..0xCC;
by the inbound field spacing the live one is probably 0x84, which no code in the
extracted firmware ever writes). No mailbox opcode can reach it - op2/3/4/5/8/
0x64 only fill channels[], and channels[] is consumed solely by
pcie_set_outbound. Option A is dead.

Also observed on the swap=1 run: one fault at address=0x90000000, the card's own
outbound aperture base leaking through as a host address - an access made before
the window target was programmed. Harmless, but it confirms the aperture base.

Both runs: 4 MSIs, /tmp/cap-swap{0,1}.h264 = 0 bytes. Encoder alive, frames lost.

FIX IMPLEMENTED (M26): give the card a target whose low 32 bits are zero, i.e.
put each stream buffer at its own 4 GiB-aligned IOVA.
  - dma_alloc_coherent cannot be steered to a chosen IOVA, so the buffers are
    now alloc_pages() + an explicit iommu_map() into the device's IOMMU domain
    at dma_iova_base + (i << 32) (mz0380_stream_bufs_alloc_iova, mz0380-dma.c).
  - SET_BUF then sends word0 = IOVA >> 32 (1,2,3,4 with the default base) and
    word1 = 0, so host_target == the buffer base exactly, and the card's
    offset walk (0x0, 0x80, ...) writes straight into it.
  - New params: dma_iova_remap (bool, def 1), dma_iova_base (ullong, def
    0x100000000, must be 4GiB-aligned). buf_pair_swap must stay 0.
  - Requires a translating IOMMU domain: DMA / DMA_FQ / UNMANAGED. Identity or
    pass-through (iommu=pt, iommu=off) is rejected with a clear message; the
    code then falls back to dma_alloc_coherent and warns that frames will not
    land. Collisions with the DMA-API's own IOVA allocator are checked for with
    iommu_iova_to_phys() before every map (that allocator works down from the
    top of the aperture, so a low base is safe in practice).
RESULT (hardware, mz0380-m26-iova-test.sh): the high32 half now works exactly as
the model predicts. Domain type DMA_FQ, all four buffers mapped:
  buf0 phys 0x97b900000 -> IOVA 0x100000000 (high32=1)   SET_BUF slot {1, 0}
  buf1 phys 0x96ba00000 -> IOVA 0x200000000 (high32=2)   SET_BUF slot {2, 0}
  buf2 phys 0x97cc80000 -> IOVA 0x300000000 (high32=3)   SET_BUF slot {3, 0}
  buf3 phys 0x97b880000 -> IOVA 0x400000000 (high32=4)   SET_BUF slot {4, 0}
Faults are now at 0x100080000, +0x80, +0x100 .. +0x480 - i.e. inside buf0's
4GiB window (high32 == 1, as commanded), but at aperture offset 0x80000, one
MZ0380_STREAM_BUF_STRIDE past the base. Our 512 KiB mapping covers
0x100000000..0x10007FFFF, so the writes miss it by exactly one stride.

    host_target = (word0 << 32) + bufindex * stride     [refined, M27]

with the card starting at bufindex 2 (slot0 unused, M25 item 2) -> 1 * 0x80000.
That also explains the earlier runs: every buffer had high32 == 0 then, so all
four windows aliased onto 0 and the offset was masked by the noise.

STATUS: M26 high32 fix CONFIRMED on hardware. Aperture offset open -> M27.

================================================================================
M27 APERTURE OFFSET: two runtime knobs to pin down where inside the window the
    card writes. (2026-08-07, written, hw run pending)
================================================================================

New params (mz0380-core.c), both diagnostic until the hw picks a winner:
  set_buf_stride  (uint, def 0x80000) - the stride word in SET_BUF cmd[0x8].
        If the offset really is bufindex*stride, 0 drags the writes down onto
        the buffer base.
  dma_iova_offset (ullong, def 0)     - shifts every mapping up inside its 4GiB
        slot, so the buffer sits where the card already writes instead. Low 32
        bits of the IOVA no longer have to be zero: the card supplies them from
        the aperture offset, we only have to agree on the value.
Test: mz0380-m27-offset-test.sh - run A sends stride=0, run B shifts the
mappings by 0x80000. Exactly one should give zero faults; that becomes default.

RESULT (hardware): NEITHER passed, and the pair of results is more informative
than a pass would have been.
  run A  set_buf_stride=0      buffer base 0x100000000, slot {1, 00000000}
                               -> faults at 0x100080000 + 0x80 ..
  run B  dma_iova_offset=0x80000
                               buffer base 0x100080000, slot {1, 00080000}
                               -> faults at 0x100100000 + 0x80 ..
Both are exactly (buffer base + 0x80000). Two conclusions:
  - stride is NOT the source of the offset: sending 0 moved nothing. 0x80000 is
    a card-side constant (the frame area starts that far into the aperture).
  - run B advertised word1 = 0x80000 and the write moved up by 0x80000, so
    **word1 DOES reach the hardware**. M25/M26's "ELBI 0x54 is discarded" is
    WRONG and is retracted here.

CORRECTED MODEL (supersedes M24/M25/M26):

    host_addr = ((word0 << 32) | word1) + 0x80000,  low half adds mod 2^32

The 32-bit truncation is what produced the original M25 evidence: in the
buf_pair_swap=0 run word1 was 0xfff80000, and 0xfff80000 + 0x80000 = 0x1_0000_0000
truncates to 0 - hence faults at 0x0, which looked exactly like "the low word is
ignored". It was a wrap, not a discard.
STILL UNEXPLAINED: the buf_pair_swap=1 run (word0=0xfff80000, word1=0) faulted at
0xfff8000000000000, not 0xfff8000000080000 as the +0x80000 constant predicts.
Noted, not papered over; it does not block the fix.

================================================================================
M28 FIX: map each buffer card_frame_offset ABOVE the address advertised in
    SET_BUF. (2026-08-07, written, hw run pending)
================================================================================

  advertise: base + (i << 32)                     -> slot {i+1, 00000000}
  map at:    base + (i << 32) + card_frame_offset
  card writes advertised + 0x80000 == the buffer base.
word1 stays 0, so the mod-2^32 wrap is unreachable by construction; a wrap that
could still happen (non-default params) is warned about in the log.
New param card_frame_offset (uint, def 0x80000). set_buf_stride keeps its
default and is now known to be inert. Test: mz0380-m28-landing-test.sh.

RESULT (hardware): faults moved to 0x100000000 - the ADVERTISED address, offset
0 - which disproves the +0x80000 constant and exposes what was really going on.

================================================================================
M29 THE OFFSET WAS A BUFFER OVERRUN. Final model; every data point from M24 on
    fits it. (2026-08-07, hardware)
================================================================================

    host_addr = ((word0 << 32) | word1) + aperture_offset
    aperture_offset starts at 0; the low half adds MOD 2^32 (no carry to word0)

An IOMMU only faults on UNMAPPED addresses. Writes that land inside our mapping
are silent. So the first fault address is not where the card starts writing - it
is where the card leaves the buffer. Every "offset" we measured was a buffer
end, not an offset:
  M26     map 0x100000000 +512K, advertise 0x100000000
          -> faults at 0x100080000 == buffer END. Data WAS landing.
  M27 A   identical mapping, stride=0 -> same end -> same faults. Stride inert.
  M27 B   map 0x100080000 +512K, advertise 0x100080000
          -> faults at 0x100100000 == that buffer's END. (This is the run that
             "proved" word1 works - it does, but the +0x80000 was the overrun.)
  M28     map 0x100080000, advertise 0x100000000 -> the advertised address is
          UNMAPPED, so the very first write faults -> 0x100000000, offset 0.
And the two older ones, which now also fit:
  M25 swap=1  target 0xfff80000_00000000, nothing mapped there -> faults from
              offset 0 -> 0xfff8000000000000. Fits.
  M25 swap=0  target 0x00000000fff80000, which IS where dma_alloc_coherent put
              the buffer -> writes landed until the overrun at 0xfff80000 +
              0x80000 = 0x1_00000000, which wraps mod 2^32 -> faults at 0x0.
              Fits, and explains the "low word is discarded" reading exactly.
M24/M25/M26's "ELBI 0x54 is ignored by the hardware" is RETRACTED in full. The
low word always worked. The blocker was never the address protocol: it was a
512 KiB buffer the card streams straight past, plus one 32-bit wrap.

FIX (M29):
  - card_frame_offset default 0: advertise the buffer base as-is.
  - MZ0380_STREAM_BUF_SIZE 0x80000 -> 0x400000 (4 MiB, one order-10 block, over
    an uncompressed 1080p NV12 frame; 4 x 4 MiB fits the 32 MiB aperture).
  - The 4GiB-aligned IOVA remap (M26) is KEPT: it is no longer strictly
    required, but it pins word1 to 0, which puts the mod-2^32 wrap out of reach
    by construction. dma_iova_remap=0 falls back to plain dma_alloc_coherent.
Test: mz0380-m29-frames-test.sh. If faults now stop at a 4 MiB boundary the
card wants an even bigger buffer; if they stop entirely, read the "frame token"
lines for the payload head bytes.

RESULT (hardware): FAULT COUNT 0 - the first clean run in the whole M24..M29
sequence. All four buffers mapped and advertised cleanly:
  buf[i] phys 0xa26c00000 / 0xa21c00000 / 0xa28c00000 / 0xa31000000
         -> IOVA 0x100000000 / 0x200000000 / 0x300000000 / 0x400000000
  SET_BUF[i] target == buf == slot {i+1, 00000000}
The host->card DMA address protocol is SOLVED.

Not solved: no "frame token" lines, IRQ 164 still at 4 (all command-done),
/tmp/cap-m29.h264 still 0 bytes.

================================================================================
M30 IS THE CARD WRITING? Zero faults is ambiguous by construction. (2026-08-07,
    written, hw run pending)
================================================================================

An IOMMU reports only the writes that MISS. "No faults" therefore means either
"every write landed in our buffers" or "no writes happened", and the completion
path is silent either way, so it cannot break the tie. The buffer contents can.
Added mz0380_stream_bufs_dump() (mz0380-dma.c), called from mz0380_dma_stop()
before STOP_STREAMING, which logs per buffer: the first 16 bytes, how many
4 KiB-sampled pages are non-zero, and the highest non-zero offset - plus the
card's EVENT/token/payload/enc-status words and the IRQ counters.
  head 00 00 00 01 + non-zero pages -> card IS writing H.264 into our memory;
        the remaining bug is the frame-done MSI / token path, NOT the address.
  all zero                          -> the card writes nothing now; the target
        it holds is one it declines to use. Compare status words against M29.
Test: mz0380-m30-landed-test.sh.

RESULT (hardware): **THE CARD IS WRITING INTO HOST MEMORY.** Milestone.
  stop buf[0] @0x100000000 head=11 11 11 11 11 11 10 11 11 11 11 11 11 11 11 11
              | 779/1024 sampled pages non-zero, last @0x30a000
  stop buf[1] @0x200000000 all zero        (same for buf[2], buf[3])
  stream stop: EVENT=0 token=0 0x44=0 0x48=0 0x4c=0 enc=0 irqs=3/0
  fault count 0
Readings:
- ~3.19 MiB written (last non-zero sample at 0x30a000). A 1920x1080 4:2:0 frame
  is 3110400 == 0x2F7600. So what lands is an UNCOMPRESSED raw frame, NOT an
  H.264 bitstream - there is no 00 00 00 01 start code, the fill is a constant
  0x11 (one 0x10 in the head), which is what the card's fake_frame_process test
  pattern generator emits under stream_nosg=1.
- Only buffer 0 is written. The card is not rotating bufindex over our four
  slots, at least not in the fake-frame path.
- This also confirms the M29 diagnosis exactly: a 3.19 MiB write into a 512 KiB
  buffer overruns at 0x80000, which is precisely where every fault from M26
  onward began.
So the host<-card DATA path is now proven end to end. What is NOT working is the
COMPLETION path: no frame-done MSI (irq_video_count == 0), EVENT/token/enc all
zero, so mz0380_dma_drain_video never runs and nothing reaches vb2.

================================================================================
M31 OPEN: frame-done completion. The data arrives; nothing announces it.
================================================================================
Two independent questions, in priority order:
  1. Why no frame-done MSI? ep.ko has store_channel_done + encode_status0..15
     sysfs attrs, and the card's userspace pwrites channel_done when a frame is
     finished. Our MSI fires 3 times (all command-done, EVENT bit11) and never
     for a frame. Either the card needs an explicit enable we have not sent, or
     the fake-frame path skips the channel_done write that raises it.
  2. Raw vs encoded. What lands is a raw frame in the buffer set we programmed
     with op2 (window0). The H.264 bitstream, if it is produced at all, must go
     to one of the other windows (op3/4/5/8/0x64), whose targets we have never
     programmed - they would be 0, yet there are no faults, which suggests the
     encoder is not running or not DMAing in this path. Worth re-testing with
     stream_nosg=0 now that the address path works.
Note that a raw 1080p frame path is itself a usable capture driver (V4L2 NV12),
so option 2 is a fork in the road, not just a diagnosis.

CONTROL RUN (M25 swap probe re-run on the M30 build) - the landing is causal,
not a fluke:
  buf_pair_swap=1 (target deliberately wrong, {0,i+1}) -> 10 faults, ALL FOUR
                  buffers all-zero. Nothing lands.
  buf_pair_swap=0 (correct, {i+1,0})                   -> 0 faults, buf[0] 0x11
                  fill, 779/1024 pages, last @0x30a000. Reproduces M30 exactly.

================================================================================
M32 THE EVENT TOKEN IS NOT THE GATE. ep.ko delivery RE'd; the card's userspace
    is not declaring a finished frame. (2026-08-07)
================================================================================

ep.ko RE (from windowsDriver/hd60-trace/artifacts/oncard-binaries/ep.ko, which
the Windows session already extracted - disasm in ep-disasm.txt):

store_channel_done (0xdc8) - the sysfs attr the card's userspace writes when a
frame is finished. It takes 6 words (channel, then per-plane/per-stage values),
packs (val-1) into a 4-bit field per channel in the mailbox payload words at
0x40 / 0x44 / 0x48 - which is exactly our MZ0380_MB_FRAME_TOKEN and its
neighbours, and confirms the (token & 7) buffer-index reading - and then:
    if (G[0x630])   ... path A     (G[0x630] = SET_VIC cmd[0x22] != 0, the
    else            ... path B      "int_reduce" flag; path B also consults
                                     G[0x63c], set from another command's [0x11])
  BOTH paths end in the same delivery sequence, and both require *flag != 0:
    mailbox[0x30] = G[0x634] | bit ;  G[0x634] = 0 ;  *flag = 0 ;  ELBI[0xdc] = 1
  and when *flag == 0 they only OR the bit into G[0x634] and return - the event
  is accumulated, never delivered.

msi.constprop.1 (0x115c) - the COMMAND-done path - is gated on the SAME flag,
with the same consume-and-clear:
    r0 = G[0x634] | 0x800 ; if (*flag == 0) return ; mailbox[0x30] = r0 ;
    G[0x634] = 0 ; *flag = 0 ; ELBI[0xdc] = 1
pciep_isr_clrint (0xd44) sets *flag = 1 (and clears mailbox[0x30]).

So the card->host event channel is a strict one-shot ping-pong: one event is
delivered, the token is consumed, and the host's ack re-arms it. CRUCIALLY this
means the token CANNOT be our blocker: our command-done MSIs work and cycle that
exact token (3 of them per run), so it was armed while frames were flowing. Had
store_channel_done run at all, we would have gotten an MSI.
=> The card's userspace never writes channel_done for these frames.

HYPOTHESIS (M32 probe): op2 programs window0, which is where the RAW frame goes.
The ENCODER's bitstream destination is a different outbound window, one we have
never programmed - so tinyvenc has nowhere to write its output and never
completes a frame. Probe sends op 0x04 / 0x05 / 0x03 (windows 1/2/3) pointed at
stream bufs 1/2/3, which the card currently ignores, so it costs no memory.
New param probe_windows (def 0). Test: mz0380-m32-windows-test.sh.
If no window wakes up, the remaining unread host->card channel is opcode 0x50,
which ep.ko forwards to card userspace and which the Windows driver does send.

RESULT (hardware): HYPOTHESIS REJECTED, cleanly.
  probe window1 (op 0x04) -> buf[1] 0x200000000 ret=0
  probe window2 (op 0x05) -> buf[2] 0x300000000 ret=0
  probe window3 (op 0x03) -> buf[3] 0x400000000 ret=0
  stop buf[0] 0x11 fill, 779/1024 pages (unchanged)
  stop buf[1], buf[2], buf[3] ALL ZERO
  irqs 6/0 (3 commands + 3 probes, all acked), 0 faults, no frame token
All three opcodes were accepted, so the card took the addresses; nothing was
ever written to them, and no new fault addresses appeared either (so the
encoder is not writing to some OTHER unmapped target). The encoder is not
producing a bitstream at all, to any window. An unprogrammed destination was
not the blocker.

REMAINING LEADS, in order:
  1. Opcode 0x50 - the only host->card channel never decoded. ep.ko does not
     handle it (dispatch sends 80/81/82 to a bare notify); it is consumed by
     card USERSPACE, and the Windows driver sends it with 32-byte payloads.
     This is where a bitstream-buffer / encoder-config handoff would live.
     Needs: video_capture_mgr + tinyvenc5 disasm (they read the same command
     buffer), and the matching Windows-side call site.
  2. What makes video_capture_mgr call channel_done at all - RE it directly
     rather than inferring from the outside.
  3. stream_nosg=0 (real BT1120 capture) now that the address path works - the
     fake-frame generator may simply not drive the encoder end of the pipe.
  4. Fallback that needs no further RE: the raw ~3.1 MiB frame in buf0 is
     already a usable V4L2 NV12 capture path if driven by polling instead of
     the missing completion interrupt.

================================================================================
M33 THE ENCODER IS WAITING FOR AUDIO. Also: STOP_STREAMING is op 7, not 0x2a.
    (2026-08-07, from the card's own binaries; hw run pending)
================================================================================

Who writes channel_done: tinyvenc5, to /sys/class/vpl_pciep/channel_done. Not
video_capture_mgr. And tinyvenc5's is_nosg path blocks on AUDIO first:
    [tiny5]is_nosg cannot open /sys/audio_status/audio_ready
    [tiny5]is_nosg wait audio timeout, i2s_num=%d, audio ready[%d]
    [tiny5]is_nosg %d audio channels (wait %d times pass)
    [tiny5][ch %d] START_STREAMING -------------> is_nosg ACK
The only writer of that file is video_capture_mgr's SET_AIC_PARAMS handler:
    echo '1' > /sys/audio_status/audio_ready      (when the command's on byte
    echo '0' > /sys/audio_status/audio_ready       is set / clear)
We have never sent any audio command. That fits M30-M32 exactly: a raw frame
lands in our buffer, and channel_done is never written.

video_capture_mgr's dispatch, which also corrects an M17 guess:
    pread(fd, cmd, 44, 0) ; switch (cmd[0]):
       7  -> STOP_STREAMING        41 (0x29) -> SET_VIC
       42 (0x2a) -> SET_AIC_PARAMS    96/97/110 -> others
STOP is op 7, adjacent to START (op 6). 0x2a is the AUDIO command, so our
"STOP_STREAMING" at every streamoff was really SET_AIC with an all-zero payload
- i.e. "audio off" - and no stop was ever sent. Both fixed in mz0380-reg.h.

SET_AIC_PARAMS payload, recovered from video_capture_mgr's printf vararg order
(handler at vcm.txt 0x9358, format at 0xb57c) and cross-checked against the
Windows driver string "ai=%d, chs=%d, bits=%d, freq=%d, period=%d.%d,
is_aic_on=%d, aic_int_mode=%d". cmd+N == BAR0+4+N:
    cmd+4  u8  channel_num          cmd+5  u8  mono
    cmd+6  u16 bits                 cmd+8  u32 freq
    cmd+12 u16 frame_num_of_period  cmd+14 u16 period_num_of_buffer
    cmd+16 u8  on                   cmd+17 u8  aic_int_mode -> ep.ko G[0x63c]
(ep.ko's own op-0x2a handler reads cmd[0x11] into G[0x63c], the second gate in
store_channel_done - so both sides of this command are now accounted for.)

FIX: send SET_AIC(on=1) between SET_BUF and START. New params aic_on (def 1),
aic_channels/bits/freq/period_frames/periods. Test: mz0380-m33-audio-gate-test.sh.
Watch for "frame token" lines - that is the completion path we have never once
seen fire.

RESULT (hardware): SET_AIC(on=1, 2 ch, 16 bit, 48000 Hz, 1024 x 4) ret=0 -
accepted - and NOTHING ELSE CHANGED. No frame token, irqs 3/0, buf0 still gets
the raw 0x11 frame, bufs 1-3 empty, 0 faults. Audio was not the gate, or not the
only one. Note the timing: SET_VIC at t=7418.58 spawns tinyvenc5, SET_AIC lands
at t=7420.65 - two seconds later, because of start_delay_ms - so if tinyvenc5
samples audio_ready early and times out, we set it too late to matter. Worth one
retry with the AIC sent BEFORE SET_VIC before writing the idea off entirely.

WHAT THIS RUN DOES ESTABLISH: the 0x11 fill is EncodingGroup::fake_frame_process,
which is a tinyvenc5 symbol. So tinyvenc5 IS running, IS in its frame loop, and
IS reaching the DMA - it simply never completes a frame or writes channel_done.
The remaining unknown is inside tinyvenc5's per-frame path, between generating
the frame and the channel_done write.

HYPOTHESES TESTED AND REJECTED SO FAR (each one narrowed the search):
  M25/M26  "the hardware discards the low half of the DMA target"  -> retracted
           in M29; it was a buffer overrun plus a mod-2^32 wrap.
  M27/M28  "there is a constant aperture offset"                   -> no; same
           overrun seen from the other side.
  M32      "the encoder's bitstream needs another outbound window" -> no; all
           three extra windows accepted the addresses, none was ever written.
  M33      "the encoder is blocked on audio_ready"                 -> no visible
           change (but see the timing caveat above).

NEXT, in order of expected value:
  1. RE tinyvenc5's channel_done write site directly (re-dump/tinyvenc5.txt,
     string /sys/class/vpl_pciep/channel_done) and walk backwards: what must be
     true for it to be reached? This is the only place left that can answer it,
     and it stops the guess-and-check loop.
  2. Opcode 0x50 - still the only host->card channel never decoded, and the
     Windows driver does send it.
  3. AIC before SET_VIC (cheap retry of M33 with the ordering fixed).
  4. stream_nosg=0 real capture, now that the address path works.
  5. PRAGMATIC: the raw ~3.1 MiB 1080p frame in buf0 is a complete picture. A
     polling drain plus V4L2 NV12 would give working capture without decoding
     the encoder's completion path at all. This does not need any further RE.

================================================================================
M34 THE COMPLETION WRITE SITE, AND WHY IT IS NEVER REACHED. (2026-08-07)
================================================================================

tinyvenc5 opens /sys/class/vpl_pciep/channel_done in BOTH
EncodingGroup::encode_handler (0x12c60) and EncodingGroup::fake_frame_process
(0x15d58) - the latter is the one running in our stream_nosg=1 path, and the fd
is kept at sp+0x84. The write, at 0x168c8:

    16838: bl TK_MMA_ProcessOneFrame       <- the MassMemAccess DMA to the host
    1683c: r2 = *[0x16bfc] ; r4++          <- frame counter vs a limit
    1684c: cmp r2, r4 ; bgt 0x167cc        <- loop while more frames remain
    1686c: r2 = [r3+0xe98] ; bne 0x170d8   <- alternate exit, also a
    16878: r3 = [r3+0xe9c] ; bne 0x170d8      channel_done pwrite (0x170e8)
    168c8: r0 = fd ; r1 = r4-88 ; r2 = 24 ; r3 = 0
    168d8: bl pwrite                       <- CHANNEL_DONE, 24 bytes

24 bytes == the 6 words store_channel_done unpacks (ldm {r4,r8}, +8, +0xc,
+0x10, +0x14). Both sides of the completion protocol now reconcile exactly.

THE KEY ORDERING: channel_done is written only AFTER TK_MMA_ProcessOneFrame
RETURNS. Our data did land in the host buffer, so that call was entered and did
transfer. The state that fits every observation is that it NEVER RETURNS -
MassMemAccess_WaitDMAC blocking on a DMA-completion that never arrives:
  - exactly ONE frame's worth of data in buf0 (last non-zero at 0x30a000,
    against a 1080p 4:2:0 frame of 0x2F7600) and never more, across many runs;
  - tinyvenc5 demonstrably alive (its fake_frame_process produced that frame);
  - no channel_done, therefore no EVENT, no MSI, no token - all consistent with
    the thread being parked inside one call rather than looping.
So the question is no longer "why does the card not tell us" but "why does the
card's own DMA engine not report its transfer complete". That is a card-internal
signal (vpl_dmac's ISR / VPL_DMAC_ISRTail), and it is the next thing to read.

NEXT: VPL_DMAC_ISRTail + MassMemAccess_WaitDMAC (re-dump/vpl_dmac.txt, and
libmassmemaccess.so.9 which is in re-dump/fw/yuan_demo_sdi/). Specifically: what
does the DMAC ISR require before it marks a transfer done, and does the outbound
path need something from the host (a credit, a read pointer, an ack) that we are
not providing. Note ep.ko exposes hready/dency sysfs attrs that card userspace
reads - vcm does "echo '0' > /sys/vpl_pciep/hready" on stop, and tinyvenc5 opens
/sys/vpl_pciep/dency - so a host-readiness handshake exists and we have never
driven it. That is the strongest remaining suspect.

================================================================================
M35 THE WHOLE NOTIFY/CREDIT ARCHITECTURE, AND WHY "EXACTLY ONE FRAME" WAS NEVER
    PROVEN. (2026-08-07, static RE; hw discriminator pending)
================================================================================

ep.ko re-disassembled WITH relocations (llvm-objdump -dr) - every literal that
was an opaque `.word 0x0` in ep.txt now resolves. Regenerate via `-dr`; the old
dump hid all of the following.

1. THE CARD IS SYSFS-NOTIFY DRIVEN. pciep_isr does almost no work itself; it
   parks data in globals and calls sysfs_notify(pciep_kobj, NULL, <attr>).
   Card userspace poll()s those attrs. Full dispatch (r6 = opcode):
     1            -> print FIRMWARE Ready, set flag
     2 / 8        -> window0 slots 1-4 / 5-8 into channels[ch] (+ op2 CLEARS
                     host_ready=G[0], op8 sets wency_ready=8)
     3 / 4 / 5    -> windows 3 / 1 / 2 slot writes (per-channel struct 0xc0 apart)
     100 (0x64)   -> window4 slots + pre_uv_ready=1
     10           -> query: writes G[0x66c],G[0x670] back into mailbox cmd+4/+8
     20 / 21 / 23 -> GPIO (dir clear / single pin / 20-bit mask loop)
     41 SET_VIC   -> no_signal=G[0x71c] from (W==0||H==0); G[0x630]=int_reduce
                     from cmd[0x22]; windows_select_fw(.data+4) = cmd[6]==7 ? 7:5
                     (fw 7 = tinyvenc7/full-HD path, else 5 = tinyvenc5);
                     notify "epint"
     42 SET_AIC   -> G[0x63c]=cmd[0x11] (aic_int_mode); notify "epint" (or
                     "epint_1080p" if fw==7), then notify "audio_ctrl"
     6 START      -> notify "audio_ctrl", then "epint"/"epint_1080p". No MSI,
                     no mailbox touch - confirms M22 fire-and-forget.
     7 STOP       -> print STOP_STREAMING(fw), notify "epint"
     9,45,47,49,80-82(0x50-0x52),96,97,98,110,123,124
                  -> NOTHING but notify "epint"/"epint_1080p": pure doorbells.
                     These opcodes are handled ENTIRELY by card userspace, which
                     wakes and pread()s the 44-byte mailbox itself. This is how
                     op 0x50 (the "undecoded channel") works - decode it in
                     vcm/tinyvenc, not ep.ko.
   Simple ops (2,3,4,5,8,10,20,21,23,100) end with msi.constprop.1 = the
   cmd-done MSI. Userspace-routed ops get their cmd-done later, from the app.

2. THE INTERRUPT CREDIT (confirms + sharpens M32's note). store_channel_done:
     - token words are written UNCONDITIONALLY and UNGATED:
         w5==0: BAR0[0x40]=(w1-1), [0x44]=(w2-1), [0x48]=(w3-1)  (nibble<<ch*4)
         w5!=0: BAR0[0x4c]=(w4-1)
     - EVENT[0x30] + MSI are gated on msi_enable (.data[0]): if 0, the
       completion bit is parked in pending G[0x634] and NOTHING reaches the
       host. Every delivered interrupt CLEARS msi_enable (one-shot credit).
     - pciep_isr_clrint (our ack: BAR5[0xdc]=2 + doorbell 0x400) = BAR0[0x30]=0
       and msi_enable=1. The ONLY re-arm.
     - int_reduce (G[0x630], from SET_VIC cmd[0x22]) and aic_int_mode (G[0x63c])
       only select which pending-accumulate flavour runs; both flavours still
       need the credit to deliver.

3. "EXACTLY ONE FRAME" (M30/M34) WAS NEVER PROVABLE from our evidence:
     - fake_frame fill is constant 0x11 -> frame N overwriting buf0 with the
       same bytes is invisible;
     - if the card reuses slot 1 every frame (host never advances the ring),
       every channel_done writes token nibble (1-1)=0 -> BAR0[0x40] reads 0
       forever, INCLUDING at stop;
     - we only ever sampled at STOP.
   So "channel_done never fires" (M34) vs "channel_done fires repeatedly into
   slot 1 but EVENT/MSI delivery is dead" are BOTH consistent with every run so
   far. M34's WaitDMAC-park inference stands as the favourite (the credit
   provably cycles during the command phase, and START consumes none), but it
   is not proven.

4. WaitDMAC MECHANICS (libmassmemaccess.so.9 + vpl_dmac.ko):
     - MassMemAccess_WaitDMAC = `while (ioctl(fd, 0xde01)) ;` - an INFINITE
       retry loop; the ioctl only errors on signal_pending.
     - vpl_dmac Ioctl 0xde01: sleeps (prepare_to_wait/schedule, NO timeout) on
       flag[slot] at dev+((slot+130)<<2), set only by the DMAC hw ISR, which
       also wake_up()s dev+(slot+1)*8 and calls ISRTail.
     - VPL_DMAC_ISRTail = ring consumer: advances tail (mod 64), programs the
       next queued descriptor into the hw regs and kicks it (reg[8]|=6); when
       ring empty clears busy (reg2[0]&=~0x400, dev[0x138]=0).
     - So a missing DMAC completion IRQ parks tinyvenc5 forever, exactly M34's
       shape. It also STALLS THE WHOLE DESCRIPTOR RING - nothing else the card
       queues on that engine will run either.
     - pcie_set_outbound (ep.ko, now readable): ELBI[0x74]=?, [0x7c]=0x91FFFFFF
       (window limit -> 0x90000000..0x91FFFFFF = 32 MiB aperture), [0x54]=low32,
       [0x58]=high32, [0xd4]=0xF00000. 32 MiB >> one frame; window size is not
       the stall.

5. tinyvenc5 EncodingGroup::Start (0x10ebc..): spawns the frame thread
   (pthread_create -> 0x15c60 wrapper -> fake_frame_process), then for the LAST
   channel pread()s the 44-byte mailbox and, if cmd[0]==6 (START), polls
   /sys/vpl_pciep/dency at 1 ms up to 100 tries until atoi(dency) == a
   per-group expected value, then pwrite()s a 44-byte ACK. dency_ready lives in
   ep.ko .bss (init 0); its ONLY writers are the sysfs store (yuan_ioctrl's
   debug `echo '0' > dency` - "Disable ENCY") - nothing in vcm sets it. If the
   expected value is 0 the gate is open by default; RE of the expected-value
   field still open. hready/host_ready: G[0], cleared by op2 SET_BUF, read back
   only by hready_show; vcm reads the attr and writes '0' on stop. It is a
   vcm-side state flag, NOT an ep.ko DMA gate - the M34 "hready handshake"
   suspicion is DOWNGRADED (nothing in ep.ko or vpl_dmac consumes it).

DISCRIMINATOR (mz0380-m35-live-token-test.sh): watch BAR0 0x40-0x50 DURING the
stream (phase A, 12 s), then add a 1 Hz ack-kick (BAR5[0xdc]=2, BAR0[0x30]=0,
doorbell 0x400 - each kick re-arms the credit) for 12 s more:
   - edges in phase A            -> channel_done fires; blocker is MSI delivery
   - quiet A, "frame token" in B -> dead credit; fix ack/re-arm in driver
   - both quiet                  -> M34 confirmed; go RE vpl_dmac ISRHead/
                                    IntrEnable + the DMAC IRQ plumbing.
Implementation note: userspace mmap of /sys/.../resource0 fails EINVAL - the
driver's request_mem_region marks the BAR busy and CONFIG_IO_STRICT_DEVMEM
makes busy regions exclusive - so the poller lives in the driver's event-watch
kthread (mz0380-core.c): "live token" change lines (20/s cap) + exact change
counters on watcher stop, and a live-tunable credit_kick_ms param for the
kicks. Watcher armed via "echo start > /proc/mz0380-events".

RESULT (hardware, 2026-08-07): BOTH PHASES QUIET. Token change counters
tok40=44=48=4c=enc50=0 over the whole 24 s stream, 19 credit kicks changed
nothing, event ring recorded 0 edges, frame still lands in buf0 (779 pages),
0 faults, irqs 3/0.
  ==> M34 CONFIRMED, now by direct ungated evidence: store_channel_done is
  NEVER CALLED. Not a credit problem, not an MSI-delivery problem. tinyvenc5
  is parked inside TK_MMA_ProcessOneFrame -> MassMemAccess_WaitDMAC: the
  card's own DMAC completion IRQ never fires, even though the full ~3.19 MiB
  did reach host RAM. The dead/ambiguous branches (delivery, credit, ring
  reuse) are all closed; the only open question is inside vpl_dmac:
  Start/Wait slot protocol, ISRHead slot attribution, IntrEnable, and why a
  transfer whose data demonstrably arrived never reports completion.
  (Side observation: even the SET_VIC/SET_AIC command completions produced no
  EVENT edges while the watcher ran - cmd-done during streamon is evidently
  seen via the STATUS-bit0 poll path, another hint the card-side interrupt
  raise machinery is not doing what M17 assumed.)

================================================================================
M36 THE TRANSFER IS CHUNKED - AND OUR EXTENT EVIDENCE WAS HALF-BLIND.
    (2026-08-07, static RE + instrumentation; hw run pending)
================================================================================

MassMemAccess RE (libmassmemaccess.so.9, scratchpad mma.txt):
- StartDMAC builds a 60-byte descriptor (ctrl@+8 with mode bits x100/x200/x300,
  src@+0x10, dst@+0x14, 2D geometry @+0x18..0x34), MemMgr_CacheFlush, then
  `while (ioctl(fd, 0xde00)) sched_yield()` to claim a ring slot + submit.
  vpl_dmac's 0xde00 handler claims a free slot (dev[slot+0x308] flag),
  programs/queues via StartHead/StartTail; ISRTail advances the 64-deep ring
  and kicks the next queued descriptor.
- ProcessOneFrame's chunked path (req[0x30]!=0): r9 = ceil(req[0x1c]/0x800)
  chunks; per chunk: src/dst += n*0x800, size = 0x800 (remainder on the last),
  then StartDMAC + **WaitDMAC PER CHUNK**, strictly sequential.
- THE ARITHMETIC HIT: the observed stop 0x30a000 = 1556 * 0x800 EXACTLY. So
  on our runs the card completed ~1556 chunk transfers - 1556 DMAC completion
  IRQs fired fine - and then one chunk's completion never arrived. "The DMAC
  IRQ plumbing is broken" is dead; something stalls MID-STREAM, and the same
  place every run (M25/M30/M33/M35 all end at 0x30a000... as far as we could
  SEE).

...as far as we could see: the stop-dump samples ONE byte per 4 KiB page and
tests non-zero. The fake frame is 0x11 fill + ZERO padding - card-written
zeros are invisible, so the true extent may be far past 0x30a000 and the
apparent determinism an artifact of the metric.

INSTRUMENTATION (driver, this session):
- buf_poison param (default 1): all four stream buffers memset to 0xAA after
  SET_BUF, before SET_AIC/START.
- mz0380-extent/%u kthread (mz0380-dma.c): forward-incremental scan per
  buffer every 20 ms for the first dword still equal to the 0xAAAAAAAA
  poison; logs "extent buf[i]=0x... (+0x...)" on change (20/s cap), final
  extents at stream stop. Gives the true write extent INCLUDING zeros, and
  its progress curve (crawl vs hard stall, exact stop offset, per-buffer).
- stop-dump "sampled pages" is now poison-aware ("touched" = != 0xAA).

Test: mz0380-m36-extent-test.sh (also dumps PCIe MPS/MRRS/AER for the card
and its upstream bridge - a payload/flow-control anomaly is a live suspect
for a deterministic mid-stream outbound stall). Readouts in the script
header: constant growth = crawling (compute B/s); freeze at X = hard stall
at true offset X (compare 0x2F7600 frame, 0x800 grid, run-to-run repeat);
full 4 MiB = card wrote everything and the blocker moves to the SECOND
Start/Wait pair in ProcessOneFrame (metadata DMA) or the loop exits;
buf1-3 nonzero = ring does advance.

RESULT (hardware, 2026-08-07): THE WRITES HAVE A HOLE. One extent line at
t=+0.45 s after START: buf[0]=0x6d384 - then frozen for the remaining 27 s -
while the page sampling still saw touches out to 0x30a000. So the write
pattern is: contiguous data 0..0x6d383, at least one still-poisoned dword AT
0x6d384 (= 233 x 1920-byte lines + 4, mid-chunk: chunk 218 offset 0x384),
then more data beyond, ending around 0x30a000. A chunk was partially
written / a write went missing MID-STREAM - with 0 IOMMU faults, 0 AER
errors (CESta/UESta clean on card AND bridge), MPS 128 matched both ends,
MRRS 512, link 2.5GT/s x1. buf1-3 untouched (pure poison). Everything up to
the first burst (~447 KiB in <450 ms) moved at full speed.

The single-frozen-extent metric cannot see past the first hole, so M37 adds
a full hole map at stream stop (mz0380_extent_hole_map): every data<->poison
transition per buffer (first 8 holes with offset+length, total data bytes,
last-data offset, interior hole count). This distinguishes:
  - ONE small hole            -> a lost write burst (silent PCIe/AXI drop);
  - periodic stride holes     -> the transfer is 2D with dst stride > row,
                                 i.e. STRUCTURAL, and the "hole" is by design
                                 (then the stall story changes completely);
  - many scattered holes      -> systematic write loss along the stream.
Re-run mz0380-m36-extent-test.sh (now prints the hole map).

HOLE MAP RESULT (hardware, 2026-08-07): EXACTLY ONE 4-byte hole, at 0x6d384,
identical offset in both runs. Total data 0x30a5bc bytes, last data at
0x30a5bc, i.e. the stream is CONTIGUOUS 0..0x30a5bb except that single dword,
and stops mid-chunk (0x30a5c0 = 1556*0x800 + 0x5c0). Notable: 0x30a5c0 ==
1920*1107*1.5 exactly. Still 0 faults, 0 AER, buf1-3 pure poison.

A deterministic SINGLE-DWORD hole at the same offset is at least as likely
to be a COLLISION - the frame data itself containing 0xAAAAAAAA at 0x6d384
(the fill is not perfectly uniform; the head already shows a 0x10 byte) - as
a genuinely lost write. New param poison_byte (def 0xaa): re-run once with
poison_byte=0x55; a real un-written hole stays at 0x6d384, a collision moves
or disappears (and then the card's writes are FULLY contiguous and simply
stop at 0x30a5c0, mid-chunk 1556, deterministically).

POISON-BYTE RESULT (hardware, 2026-08-07): COLLISION CONFIRMED - THERE ARE
NO HOLES. With poison 0x55 the 0x6d384 hole vanished and 31 different
"holes" appeared (@0x130cc0 0xc, @0x130d98 0x10, @0x1747f4 0x10, ... - all
clustered 1.2-1.5 MiB into the frame, i.e. mid-picture: frame data that
happens to contain 0x55555555 dwords, likely an OSD/pattern region).
data=0x30a49c vs 0x30a5bc with 0xaa - the difference is exactly the
respective collisions. CONCLUSIONS, now solid:
  1. The card writes ONE FULLY CONTIGUOUS burst 0..0x30a5bc, fast (<450 ms
     from START), deterministically, zero loss, zero faults, zero AER.
  2. 0x30a5c0 = 1557 chunks of 0x800 with a partial last chunk of 0x5c0
     = 1920 x 1107 x 1.5 EXACTLY: the transfer ENDS AT ITS INTENDED SIZE.
     THE TRANSFER COMPLETES. The "parked DMAC / missing completion IRQ"
     theory (M34) is DEAD - the engine finished every chunk including the
     partial last one.
  3. Therefore the park is AFTER the data DMA: between ProcessOneFrame's
     return and the channel_done pwrite - OR there is no park at all and
     the card is writing IDENTICAL frames to buf0 continuously (invisible:
     same bytes, extent frozen after frame 1, tokens silent). M34's reading
     of fake_frame_process supports the latter: the 0x168c8 channel_done
     pwrite sits AFTER the frame LOOP (counter vs limit at [0x16bfc]), not
     inside it - nosg mode may simply never exit the loop and so never
     signal, BY DESIGN of the fake path.

M38 DISCRIMINATOR (mz0380-m38-repoison-test.sh): "echo repoison >
/proc/mz0380-events" re-fills the buffers with poison MID-STREAM (driver
cmd + mz0380_extent_repoison). Three repoisons at t=8/14/20 s:
  - extent regrows after each  -> the card streams continuously; DMA path
    100% healthy end-to-end; blocker = tinyvenc5 frame-loop/completion
    semantics (RE the loop bound [0x16bfc], the alternate exits 0x1686c/
    0x16878 -> 0x170d8 channel_done, and how the REAL encode_handler path
    signals per-frame instead). Repoison cadence also measures fps.
  - nothing regrows            -> exactly one frame ever; the park is real,
    between the last chunk's WaitDMAC return and channel_done.

RESULT (hardware, 2026-08-07): NOTHING REGROWS. One frame at t=+0.45 s, then
three repoisons all stayed pure poison; final extents 0, stop dump 100%
poison (0/1024 pages). EXACTLY ONE FRAME PER STREAMON, then the card never
writes again. Combined with M37 (that one frame is complete, contiguous,
full intended size): the DMA machinery works perfectly ONCE, and whatever
should produce frame 2 never runs.

The frame pacing in fake_frame_process is TIME-based (64-bit clock deltas vs
a period + usleep at 0x16a94-0x16acc, loop-back to 0x161bc) - not gated on
any hardware tick - so a live loop would free-run. Most plausible: the
thread ERRORS OUT/EXITS after frame 1 (TK_MMA_Release + return at
0x16a80-0x16a90), or blocks in one of: the mailbox pread at 0x16cc8 (it
polls the 44-byte command mailbox mid-loop - stale opcode semantics?), the
per-bitstream 4 KiB info-block ProcessOneFrame at 0x16838 (iteration 0
branches to 0x170f0 instead - big frame vs info block), or a flag wait on
[r3+0xe98]/[r3+0xe9c] (writers unknown). Deep RE of the whole loop underway
(subagent); M39 (mz0380-m39-respawn-test.sh) additionally answers whether a
SECOND streamon on the same module load produces its own one frame (thread
dies per spawn - per-frame path fine) or nothing (card-side engine/ring
wedged after the first run).

================================================================================
M43 M42 RAN: NO LOCK. And nothing in that run was actually verified.
    (2026-08-07)
================================================================================

Hardware result of M42: "EDID loaded (256 bytes)" + "HPD asserted" + receiver
brought up, then five detect attempts all -ENOLCK, streamon refused, 0 IRQs.

The important caveat, which invalidates the optimistic reading: **a successful
mailbox return proves nothing about I2C.** The card firmware forces the result
byte to 0x00 on a NAK (M13), so an EDID write that never reached a chip is
indistinguishable at the command layer from one that did. We have no evidence
the EDID is in the EEPROM, that op 0x1f is even implemented in our fw 1.11
ep.ko, or that op 0x1f drives the bus the DDC EEPROM sits on - RE_FINDINGS
M11/M13 already flag that exact bus ambiguity (our yuan_ioctrl RE put 0x1e/
0x1f on BUS1, the Windows-side RE puts them on BUS0).

DIAGNOSTIC ADDED (mz0380_mst3367_diag, /proc/mz0380-hdmi; script
mz0380-m43-sink-diag.sh). Reads every link back instead of trusting writes:
  - GPIO read-back (op 0x14) for hpd/rx_enable/rx_strap/rx_reset;
  - EDID read-back from DDC 0xA0. THIS IS THE SHARP PROBE: bytes 1..6 of a
    valid EDID are 0xff, which the firmware cannot manufacture on failure
    (it forces 0x00). "ff" = the blob is really there; "00 00 .." = the
    writes went nowhere and the op/bus is wrong;
  - MST3367 BANK0 register row (0x55 detect, 0x5f, htotal, vperiod, 0xb0,
    0xb7 HPD, 0xb8, 0x51). An all-zero row means the receiver is not
    answering at all, in which case EDID/HPD are moot and the fault is
    reset/power (pin9).
This splits "sink is fake" from "sink is real but the source is silent" from
"receiver is dead", which the M42 run could not.

RESULT (hardware, 2026-08-07) - two clean facts:
  1. THE RECEIVER IS ALIVE AND OUR I2C WRITES LAND. BANK0 read-back:
     55=03 5f=40 6a=00 6b=00 59=1f 5a=ff b0=20 b7=00 b8=00 51=89.
     b0=0x20, b7=0x00 and 51=0x89 are EXACTLY the values init_regs wrote, read
     back through a separate command - so op 0x1a/0x1b to dev 0x9c works
     end-to-end and the MST3367 is powered, out of reset and configured.
     GPIO read-back: hpd=1, rx_enable=1, rx_strap=1, rx_reset=1 (released).
     0x55=0x03, so (0x55 & 0x3c)==0 -> NO SIGNAL LOCKED. 59/5a = 0x1fff-ish
     is the idle/no-signal counter value.
  2. THE EDID NEVER REACHED ANYTHING: read-back from 0xA0 was
     00 00 00 00 00 00 00 00. Since the firmware forces 0x00 on a NAK and
     bytes 1..6 of any valid EDID are 0xff, this is proof the blob is not
     stored - the M42 "EDID loaded" message was meaningless.

CAUSE FOUND (docs/re-2026-07-05/HD60-PRO-LINUX-DRIVER.md sect 3d item 3 +
windows-findings.md sect 1): **multi-byte/EDID uses the COMBO opcode 0x20
(i2c_combo_cmd_x), not 0x1f.** 0x1f is read_s/write_s, the opcode whose bus
assignment our two RE sources disagree about. The correct frame is
  cmd[4]=dev8  cmd[5]=rw  cmd[6..7]=len  cmd[8..]=payload
i.e. PARAM1 = dev8 | (rw<<8) | (len<<16). Our M42 code also mispacked the
second byte as a block number where the rw flag belongs.

FIX (this session): EDID write switched to op 0x20 with that layout, and the
payload now leads with the EEPROM byte offset (an I2C EEPROM write carries
its target address as the first data byte), so each chunk is a 33-byte
transfer; 5 ms between chunks for the page-write cycle.

ALSO ADDED: an I2C presence scan in /proc/mz0380-hdmi over the addresses the
Windows driver knows (0x9c, 0xa0, 0xa2, 0x88, 0x98, 0x60, 0x90, 0x94). A
non-zero byte proves a device answered. This settles whether anything lives
at the EDID address on this bus at all - if 0xa0 stays silent while 0x9c
answers, the EEPROM is either on the other bus or gated behind the receiver's
DDC, and the EDID would have to go into the MST3367's internal EDID RAM
instead.

SECOND RUN (op 0x20 combo, hardware): EDID read-back STILL all zeros, and the
presence scan (reg 0x00 only) gave:
    9c:00 a0:00 a2:00 88:00 98:54 60:00 90:00 94:00
  - 0x98 ANSWERED with 0x54 - a real device (the firmware cannot fabricate a
    non-zero byte). Per M10's address list 0x98 is one of the MST3367's aux
    banks.
  - 0x9c reading 0x00 is NOT evidence of absence: reg 0x00 on the main bank
    is the bank-select, and bank 0 legitimately reads back 0. The BANK0 row
    already proved 0x9c is alive.
  - 0xa0 silent again => there is no DDC EEPROM at that address on this bus.

WHERE THE EDID REALLY LIVES - two candidates from the RE docs, which disagree
with each other and now have to be settled empirically:
  (a) HD60-PRO-LINUX-DRIVER.md sect 5c step 3: "Driver writes EDID into the
      MST3367" - i.e. internal EDID RAM behind one of the aux slaves.
  (b) same doc sect 5b: the Jump2LDROM/Run APROM/VerifyLDROM strings are a
      "Nuvoton MCU ISP flash dance for the small EDID MCU (the EEPROM the
      HDMI *source* reads)" - i.e. a SEPARATE little MCU holds the EDID, and
      our own earlier RE noted an MCU passthrough slave at 0xaa.
If (b) is right, no amount of writing to the MST3367 will ever make the source
see an EDID, and the blob has to be pushed to that MCU instead.

PROBE WIDENED (this session) to decide it: the scan now samples regs
00/01/02/7f per address (one register is not enough - reg 0 is bank-select on
the main bank) across 0x9c/0xa0/0xa2/0xa4/0xa6/0xa8/0xaa/0x88/0x98/0x60/
0x90/0x94/0x74/0xb0, flagging any address with a non-zero byte as present;
and a second pass dumps the first 8 bytes from every plausible window looking
for the EDID signature 00 ff ff ff ff ff ff 00. Whichever window shows that
signature is where the card's EDID is stored - and therefore where ours must
be written.

WIDENED-SCAN RESULT (hardware): THE BUS HOLDS EXACTLY TWO DEVICES.
    9c: 00 69 d0 00   <- present (the MST3367 main slave)
    98: 54 49 12 0c   <- present
    a0/a2/a4/a6/a8/aa/88/60/90/94/74/b0: all 00 = nobody home
So claim (b) is DEAD: there is no Nuvoton EDID MCU at 0xaa and no EEPROM at
0xa0/0xa2 on this bus. Nothing outside the receiver can hold the EDID, so
claim (a) stands by elimination - the EDID must go into the MST3367 itself.
The signature hunt found no EDID at 0x98's register head either
(54 49 12 16 1c 60 00 00), so it is not a flat EDID window.

NEXT PROBE (built, unrun): the GPL reference states the chip has FOUR banks
(BANK0..BANK3, "256-byte register shadow per bank") - and we have only ever
touched banks 0/1/2. Bank 3 is completely unexplored and is the natural home
for EDID RAM. /proc/mz0380-hdmi now sweeps banks 0..3 on BOTH slaves (0x9c
and 0x98) and dumps the first 16 bytes of each, hunting the signature. If a
bank shows 00 ff ff ff ff ff ff 00, that is the EDID store and the writer
just has to be retargeted at it (bank-select, then sequential registers).

BANK SWEEP RESULT (hardware):
    9c bank0: 00 69 d0 48 00 10 08 20 80 80 80 80 80 40 08
    9c bank1: 01 80 00 00 00 00 00 fd 00 00 00 00 11 00 00 02
    9c bank2: 02 61 f5 00 00 00 00 04 00 08 00 00 00 15 00 00
    9c bank3: 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    98 bank0..3: identical rows (54 49 12 16 1c 60 00 00 00 ff ff ff 00 00 6c 08)
Readings:
  - Banking on 0x9c is REAL and our writes are live: reg 0x00 echoes the
    selected bank on every bank, and bank2's 0x01=0x61 / 0x02=0xf5 are exactly
    what init_regs wrote.
  - 0x98 ignores bank-select entirely (four identical rows), so it is a
    separate device with a flat register map, not a second bank window - and
    it holds no EDID.
  - **BANK3 IS EMPTY**: every byte 0x01..0x0f reads 0 while banks 0-2 are full
    of live config. An all-zero 256-byte shadow is exactly what an EDID RAM
    that has never been loaded looks like - and "the source sees an invalid
    (all-zero) EDID" would explain the no-signal state perfectly.
  - No EDID signature anywhere, which is consistent with that: there is
    nothing to find because nothing was ever written.

================================================================================
M45 THE SOURCE IS REACTING TO US. (2026-08-07)
================================================================================

M44 RESULT (hardware): BANK3 IS NOT WRITABLE - 0/7 pattern bytes held their
value. Bank 3 is an unimplemented shadow, not the EDID store. Lead closed.

THE OBSERVATION THAT REFRAMES EVERYTHING (user report): running the bring-up
script visibly STOPS THE LIVE PREVIEW OF THE CONNECTED CAMERA. That is
exactly what a camera does when it detects an HDMI sink and switches its
output over to HDMI. So:
  - a real source IS attached to the card's input (worth stating plainly -
    every "no signal" result until now was ambiguous about this);
  - the source RESPONDS to what we do, i.e. our HPD toggle is reaching it
    physically. The sink side is not dead.
Which makes the open question sharper: does the source then TRANSMIT, and if
it does, why does the receiver never report lock?

Supporting detail: reg 0x55 reads 0x03 consistently - bits 0 and 1, which are
OUTSIDE the 0x3c lock mask the GPL driver gates on, and which it never uses.
The natural reading is 5V/cable presence and/or clock detect. If those bits
track the cable, the receiver is genuinely wired to the connector.

M45 TOOL (built): "echo watch [secs] > /proc/mz0380-hdmi" samples the whole
detect block (0x55, 0x5f, hperiod, vperiod, htotal) every 250 ms and logs
every change, so a plug/unplug or camera mode-cycle can be correlated with
what the chip sees. Script mz0380-m45-watch-test.sh drives it.
Decision table:
  - 0x55 changes on unplug/replug -> receiver is wired to the connector and
    sees the source; "no-lock + idle counters" then means the source is
    attached but NOT transmitting, i.e. it is refusing for lack of a valid
    EDID -> finding the EDID store remains the blocker.
  - timing counters go non-zero (idle is vper=1fff, htot=0000) -> the
    receiver IS seeing TMDS: the source transmits and the fault is the
    receiver's per-mode config (M10 step 2 values, never recovered), NOT the
    EDID.
  - nothing ever changes -> 0x55's low bits are static and the receiver may
    not be wired to this connector at all; suspect input select (op41 input
    code) or a different physical input.

M45 RESULT (hardware): NOTHING MOVED. One line at t=0
(55=03 no-lock, 5f=40, hper=0000, vper=1fff, htot=0000) and then no change at
all for 40 s across unplug/replug. So 0x55's low bits do NOT track the cable
and the detect block is frozen at its idle values.

CAUSE (user's own observation - "the script does not trigger the hdmi node"):
that watch ran against a receiver whose INPUT HAD NEVER BEEN SELECTED. The
M42 path reached SET_VIC only through streamon; mz0380_mst3367_bringup went
straight to reset/init/EDID/HPD and never told the card which front-end to
route. The Windows order is AUTO.INPUT (input select) -> EDID -> HPD ->
detect, and RE_FINDINGS M13 already named input-select as the prime suspect
for "the receiver domain is gated until Windows does something first". A
receiver that answers I2C perfectly, has a source physically reacting to its
HPD, and yet shows a detect block frozen at idle is exactly what an unrouted
front-end looks like.

FIX (M46, this session): mz0380_mst3367_bringup now sends
SET_VIC(input=HDMI, 1920x1080@60) as its FIRST action, before the receiver
reset. The watch script also forces the bring-up before watching (it was
lazy, running on first detect) and prints the sequence so "input select"
can be confirmed to appear first.

================================================================================
M47 THE EDID IS THE LAST UNVERIFIED LINK - AND op 0x20 IS FIRE-AND-FORGET.
    (2026-08-07)
================================================================================

M46 RESULT (hardware): input select now runs first
("input select: SET_VIC(input=HDMI 1920x1080@60) ret=0"), and the detect block
STILL never moves (55=03, 5f=40, hper=0000, vper=1fff, htot=0000 for 40 s,
unplug/replug included). So input-select was not the gate either.

But the same log finally shows what happens to the EDID:
    "EDID combo write failed at offset 0 (-110)"
-110 is OUR timeout, and every subsequent command (the whole receiver init)
worked - the mailbox was not wedged. That is the START_STREAMING signature
from M22: the card executes the opcode but posts NO completion, so waiting
for one always fails. The earlier op 0x1f attempt returned 0 instantly and
stored nothing, which is what an unimplemented opcode looks like.

So the EDID has never once been delivered, by any route, and that is exactly
the link no read-back can check (nothing answers at the DDC address on this
bus). THE SOURCE IS THE ORACLE: the user reports the camera switches to HDMI
mode when it detects a real sink, i.e. when it can read a valid EDID.

FIX/HARNESS (M47): EDID chunks are now sent fire-and-forget by default
(edid_timeout_ms=0), the opcode is a live module param (edid_opcode, def
0x20), and "echo edid > /proc/mz0380-hdmi" re-pushes the blob and re-pulses
HPD without a reload (a source only re-reads on a hotplug edge).
mz0380-m47-edid-sweep.sh walks 0x20/0x1f/0x1e/0x1b/0x1d and pauses after each
so the camera's screen can be watched.
  - camera flips to HDMI-out on some variant -> that is the EDID opcode.
  - no variant works -> no host opcode in fw 1.11 can store an EDID; next is
    a live Windows I2C trace of UpdateEDID (which also yields the per-mode
    config values from M10 step 2), or checking whether the card's own
    userspace serves the EDID from flash and needs a different trigger.

M47 RESULT (hardware): ALL FIVE OPCODES NEGATIVE. 0x20/0x1f/0x1e/0x1b/0x1d
each pushed the full 256 bytes fire-and-forget with an HPD re-pulse; the
detect block never moved off 55=03 / hper=0000 / vper=1fff / htot=0000 and
the camera never switched to HDMI output. The user also notes the source
reacted only ONCE, on an early run, and never again - which is as consistent
with a camera idle-timeout as with our hotplug edge, so that single reaction
must not be treated as evidence that HPD works.

WHERE THIS LEAVES THE SINK CHAIN - what is proven vs still assumed:
  PROVEN: receiver powered, out of reset, configured, answering I2C with
    read-back of our own values; input select runs first; GPIO pins read back
    at their intended levels; only 0x9c and 0x98 exist on the bus.
  NEVER VERIFIED: (1) that an EDID is stored anywhere, (2) that our HPD write
    physically reaches the connector, (3) the receiver's per-mode config
    values (M10 step 2 - the one part of the Windows bring-up we could not
    recover, ~175 register writes computed at runtime).
Any one of those three can produce exactly what we see.

M48 (built): isolate HPD as a SINGLE variable - "echo hpd [count] [gap_ms] >
/proc/mz0380-hdmi" pulses the hotplug line N times and does nothing else, so
the camera can be watched for a reproducible reaction.
  - reacts every pulse -> we own the hotplug line; the EDID must be served by
    the factory-programmed EDID MCU (not by us), and the remaining fault is
    the receiver config (M10 step 2).
  - never reacts -> HPD is not reaching the connector despite pin1 reading
    back 1; sweep the other GPIO pins one at a time against the camera.
  - reacts once only -> the source latches sink-presence; retry with a longer
    HPD-low period.

HONEST STATUS: three separate unknowns remain and each needs the same thing to
resolve efficiently - a LIVE WINDOWS I2C/mailbox TRACE of the working driver
doing UpdateEDID + receiver config. That single capture would deliver the EDID
mechanism, the HPD sequence and the ~175 per-mode register values at once.
Continuing to guess opcode/bus/bank combinations from static RE has now
produced five consecutive negatives and is the wrong tool for what is left.

================================================================================
M48 RESULT: **HPD WORKS.** THE EDID IS THE ONLY REMAINING FAULT. (2026-08-07)
================================================================================

Five clean HPD pulses, nothing else changed. User observation, and it is
decisive: the camera "reacted to the signal but did not stay in HDMI output
mode" - it responded to the hotplug edge and then fell back to its LCD.

That is the textbook sequence for: source sees HPD assert -> attempts to read
the EDID over DDC -> gets nothing -> concludes there is no valid sink ->
reverts. So:
  - OUR HPD PHYSICALLY REACHES THE CONNECTOR. GPIO pin1 (plus BANK0 0xb7) is
    the real hotplug line and the source honours it. Unknown (2) from M47 is
    CLOSED, and the earlier single reaction was not a coincidence after all -
    it was the same thing, just not repeated because the source only re-probes
    on an edge.
  - The receiver never locking is therefore NOT explained by the per-mode
    config values (unknown 3): the source is not transmitting at all, so
    there is nothing for the receiver to be misconfigured about yet.
  - EVERYTHING now hangs on the EDID: nothing we have done has ever put one
    where the source can read it.
Three unknowns collapsed to one. (Only reacting on some pulses is expected -
a source that has just failed an EDID read may back off before re-probing.)

M49 (built): find the EDID RAM BY WRITABILITY - the last systematic host-side
option before a live trace. Only 0x9c/0x98 exist on the bus, so the EDID must
come out of the receiver's own RAM. A config register has reserved/hardwired
bits and will not return an arbitrary byte; a RAM cell returns exactly what
was written. "echo wscan > /proc/mz0380-hdmi" writes 0x5a then 0xa5 to every
register 0x01-0x80 of every bank, restoring each original immediately, and
prints a per-bank map ('#' = held both values).
  - a long contiguous run of '#' = the EDID window -> retarget the writer.
  - only scattered '#' = ordinary r/w config regs; the EDID RAM is gated
    behind an unknown enable bit -> live Windows trace.
Script: mz0380-m49-wscan.sh. Reg 0x00 is never written (bank select).

M49 FIRST PASS (hardware, regs 0x01-0x80 only):
    bank0: 95 writable, long runs (expected - this is the main config bank)
    bank1: 29 writable, scattered
    bank2: 32 writable, one run at the start then scattered
    bank3: 48 writable, '.' across the whole low half and then a LONG
           CONTIGUOUS RUN that was still going when the scan hit its 0x80
           ceiling
Bank3 is the interesting one: it holds no config at all (every byte reads 0,
M43) yet the top of its register file accepts arbitrary values. That also
explains why the M44 ramtest failed - it probed 0x10..0x17, which is inside
bank3's dead low half.

SCAN WIDENED (M49b): the sweep now covers the FULL file 0x01..0xff and
reports contiguous runs explicitly ("bank3 run 0x60-0xff (160 bytes)") plus
the longest run per bank, flagging anything >= 128 bytes as an EDID-sized
window. If bank3's run continues to 0xff it is a 128-byte block - exactly one
EDID block - and that is where the blob goes.

M49b RESULT (hardware, full 0x01-0xff sweep): NO EDID-SIZED WINDOW EXISTS.
    bank0: 156 writable, longest run 33 bytes @0x6e
           (runs: 0x02-0x13, 0x17-0x24, 0x26-0x31, 0x36-0x3f, 0x41-0x4a,
            0x60-0x67, 0x6e-0x8e, 0x90-0xac, 0xb0-0xb8)
    bank1:  71 writable, longest run 19 bytes @0x81 (0x81-0x93, 0xe0-0xef)
    bank2:  34 writable, longest run  9 bytes @0x01
    bank3:  48 writable, longest run 30 bytes @0x40 (0x40-0x5d, 0x62-0x6b)
The longest run anywhere is 33 bytes - a quarter of one EDID block. The
writable registers are ordinary r/w config scattered through the file, not a
RAM window. bank3's earlier "long run at the ceiling" was the 0x40-0x5d run
ending at the old 0x80 cut-off, not a continuation.

=> THE EDID RAM IS NOT REACHABLE AS A FLAT REGISTER WINDOW ON THIS PATH. It is
either gated behind an enable bit we do not know, addressed through an
indirect port (address register + auto-incrementing data register - which
would appear in this scan as just two writable registers, indistinguishable
from config), or served by hardware we cannot see from the host bus.

================================================================================
STRATEGIC CONCLUSION (2026-08-07): STATIC RE IS EXHAUSTED FOR THE SINK SIDE.
================================================================================

What is now PROVEN on hardware, and should not be re-litigated:
  1. The whole host-side DMA/streaming stack works: addressing, 4GiB-aligned
     IOVA, outbound aperture, per-chunk completion, credits, MSI, event ring.
     One complete 1920x1107x1.5 frame lands within 450 ms of every START,
     contiguous, zero loss, zero IOMMU faults, zero AER errors (M35-M39).
  2. The fake-frame path's stall is card-internal and unreachable from the
     host (M41): a missing DMAC completion IRQ parks tinyvenc5 forever;
     VPL_DMAC_Open clears the busy/head/tail state, which is why every fresh
     spawn yields exactly one more frame.
  3. The MST3367 is powered, out of reset, configured and answering I2C with
     our own written values read back (M43). Only 0x9c and 0x98 exist on the
     bus - no EEPROM, no EDID MCU.
  4. Input select (SET_VIC) runs before receiver bring-up (M46) - not the gate.
  5. **HPD WORKS** (M48): the source reacts to our hotplug edge and then
     reverts, i.e. it sees the sink appear, fails to read an EDID, and gives
     up. This is the single most useful datum of the session.
  6. The EDID has NEVER been delivered by any route: 5 opcodes
     (0x20/0x1f/0x1e/0x1b/0x1d) x fire-and-forget x HPD re-pulse, all
     negative (M47); no EEPROM on the bus (M43); no EDID-sized RAM window in
     any bank (M49b).

The ONE remaining blocker is therefore: HOW DOES THE HOST GET AN EDID INTO
THIS CARD? Everything else is either working or provably not the current
fault.

THE ANSWER REQUIRES A LIVE TRACE, not more static RE. A capture of the
Windows driver performing UpdateEDID would show, in one go: the exact opcode,
the exact frame layout, the target address, and any enable/unlock sequence
that precedes it. The same capture also yields the ~175 per-mode receiver
config writes (M10 step 2) that we will need immediately afterwards. Static
analysis has now produced eight consecutive negatives on this question and has
no remaining unexplored surface: the opcode space is swept, the bus is
enumerated, every bank is dumped and writability-mapped.

HOW TO GET THE TRACE (in preference order):
  a. Boot Windows with the Elgato driver, run its capture app so a source
     locks, and capture the driver's mailbox traffic - a DbgView/WPP capture
     of e60MZ0380.X64.SYS (it logs "[UPDATE.EDID]", "send EDID data",
     "[HOTPLUG %d]"), or an ETW/kernel trace of its BAR0 writes.
  b. Physically probe the card's internal I2C (GPIO12/13) with a logic
     analyser while Windows brings the card up - this reads the wire directly
     and needs no software cooperation.
  c. If neither is possible: accept the sink side as blocked and pivot the
     project to the SDI/other input paths, or to consuming the one raw frame
     the fake path does deliver.

M44 TEST (built, unrun): "echo ramtest > /proc/mz0380-hdmi" writes a walking
pattern (a5 5a 00 ff 12 34 56 78) into BANK3 regs 0x10..0x17, reads it back,
then restores zeros. Reg 0x00 is never touched (bank select on every bank).
  - registers hold the pattern -> BANK3 is writable RAM, i.e. the EDID store:
    retarget the EDID writer at it (bank 3 + sequential register writes,
    skipping/working around reg 0x00) and re-run M42.
  - registers read back 0 -> bank3 is not the EDID store; the RAM is behind an
    indirect address/data port, and the next step is a live Windows I2C trace
    of UpdateEDID (which also yields the per-mode config values, M10 step 2).

================================================================================
M42 DRIVER: SINK-SIDE BRING-UP IMPLEMENTED (EDID + HPD + fps). (2026-08-07)
================================================================================

Acting on M41 (fake path is a host-side dead end), the driver now implements
the real-signal path's missing prerequisites. All compile-verified; hardware
run pending a live HDMI source (mz0380-m42-real-signal-test.sh).

1. EDID LOAD - the gap that made every previous nosg=0 attempt hopeless. An
   HDMI source keeps its transmitter OFF until it can read an EDID from the
   sink, so "no signal" was guaranteed regardless of receiver config. The
   256-byte HD60 Pro blob recovered in M10 (mz0380-edid-hd60pro.txt) is now
   compiled in as mz0380-edid.h (generated with header+checksum validation)
   and written to the DDC EEPROM (I2C 8-bit 0xA0) with the bulk opcode 0x1f,
   8 x 32-byte chunks, PARAM1 = (len<<16)|(block<<8)|dev8, bytes packed LE.
2. HPD - asserted only AFTER the EDID is in place, and deliberately toggled
   low across the load so an already-attached source re-reads it instead of
   keeping a stale/absent copy. Two levers, both driven: board GPIO pin1 and
   the receiver's own enable (BANK0 0xb7, 0x02 = off / 0x00 = on).
3. SET_VIC fps ([5]) was ALWAYS 0 - the card patched it to 60 with its own
   "Error!!! vic_fps cannot be 0" message. Harmless while the fake generator
   paced itself; the real capture path derives its cadence from it. Now sent
   from the detected timing (frame rate, so 1080i60 -> 30, matching the
   card's own convention).
4. streamon re-detects the live timing when stream_nosg=0 and refuses to
   start without lock, so SET_VIC can never arm the encoder for a stale mode.
5. enc_stat handshake (M40) acked at stream start and after every delivered
   frame - required for frame 2 onwards on the real path.

STILL UNKNOWN for the real path: the per-mode MST3367 register VALUES for the
full config sequence (M10 step 2) are not in our dump; hdcapm's init_setup is
what we run, which is enough for lock+detect but may not be enough for clean
BT.1120 output routing into the SSM ring. If the receiver locks but no frames
flow, that (plus vpl_vic config and op 0x50) is the next target - and it also
finally answers whether the DMAC completion IRQ is dead in general or only in
fake_frame_process.

================================================================================
M41 CASE CLOSED (static): THE CARD'S DMAC COMPLETION IRQ NEVER FIRES.
    (2026-08-07. Second subagent + own verification of vpl_dmac state handling)
================================================================================

The submit/wait accounting that M40 left open is now decided, and it is NOT a
ring-slot leak: fake_frame_process issues EXACTLY ONE submit per iteration
(0x17590 and 0x1724c are mutually exclusive variants - 0x175b0 jumps past
0x1723c) and exactly one wait (0x17298 or 0x1839c, keyed on slot[0x53]).
MassMemAccess_StartOneFrame calls StartDMAC once (mma 0x1a9c, every branch
funnels there). Submits == waits, 1:1.

THE MECHANISM (vpl_dmac.ko, verified directly, not just from the report):
  - submit ioctl 0xde00 claims slot dev[0x140] (StartHead 0xb54), sets
    filp[0xc]=n, calls StartTail 0xb5c which kicks the HARDWARE DIRECTLY but
    ONLY IF dev[0x138]==0 (0xb7c/0xb88), setting dev[0x138]=1 at 0xba4.
  - wait ioctl 0xde01 sleeps TASK_INTERRUPTIBLE (prepare_to_wait state=1 at
    0x6b4, schedule at 0x6a0) until dev[0x208 + filp[0xc]*4] == 1, then frees
    the slot (0x6f8) and sets filp[0xc]=-1.
  - ONLY the DMAC hardware ISR sets that done flag (ISR 0x30: done[tail]=1,
    __wake_up 0x44) and ONLY ISRTail (0xe5c) advances the tail (0xe70), starts
    the next queued descriptor, and clears busy dev[0x138]=0 (0xf90).
  - MassMemAccess_WaitDMAC (mma 0x1ee4) retries the wait ioctl FOREVER
    (0x1efc), so even a signal cannot break out.
So: the first transfer is kicked SYNCHRONOUSLY from the submit path. The
preview frame landing therefore proves the DMA ENGINE works and says NOTHING
about the completion interrupt. If that IRQ never arrives: done[n] is never
set (thread sleeps forever at 0x6a0), dev[0x138] stays 1 (every later submit
silently enqueues and is never kicked), tail never advances. Symptom:
EXACTLY ONE DMA, ever, then total silence, with the frame itself complete.
That is precisely M35-M38.

AND IT EXPLAINS M39, which nothing else did: VPL_DMAC_Open (0xd28) clears
busy(0x138), tail(0x13c) and head(0x140), and the driver's Open() re-runs
that full init (plus request_irq/IntrClear/IntrEnable) when
module_refcount()==0 - i.e. once the previous tinyvenc5 has been killed at
streamoff. So each new streamon gets a clean DMAC and produces exactly one
more frame. Frame-per-spawn, forever. Every observation from M24 on now fits
one model with no leftovers.

CONSEQUENCE FOR THE HOST: dev[0x138] and done[n] are written ONLY by the
card's own ISR. No mailbox opcode, BAR write, doorbell, credit re-arm or
buffer-setup change can set them. **From the host side the stream_nosg fake
path is a dead end** - once parked, only a re-spawn recovers it.
(Runner-up hypothesis, if slot[0x53]==0 so the wait comes after the encode:
the thread is instead in vma_h4ee's 0xdb01 wait, schedule() at file 0x660,
TASK_UNINTERRUPTIBLE with no timeout. Ranked second: that encoder is
memory-to-memory with depends= empty, no capture clock needed. Distinguishing
symptom if a card console ever becomes available: thread in D state = encoder;
S state = DMAC wait; R at 100% = the sched_yield submit spin at mma 0x1df0.)

WHY THE IRQ MIGHT BE MISSING is now the only open question, and it is
card-internal: the DMAC's completion interrupt line/mask (fLib_SetIntTrig at
vpl_dmac 0x324, IntrEnable 0xc9c setting bit1 of reg[8]) is set up in Open(),
which our runs do reach. A plausible remaining reading is that the retail
firmware's real path never depends on this wait completing under our exact
conditions, i.e. that the fake path is simply not exercised on shipping units.

WHAT THIS DOES NOT BLOCK: the real-signal path uses the SAME vpl_dmac pair,
but its frame cadence is driven by vpl_vic capture interrupts inside
encode_handler (~0x12b00, TK_MMA calls at 0x151a0/0x14220) rather than by
this software loop. Driving a real HDMI signal in is now both the product
goal and the only way to learn whether the DMAC IRQ is genuinely dead or
whether fake_frame_process alone is parked.

================================================================================
M40 FULL fake_frame_process RE (subagent report, 2026-08-07). Corrected model.
================================================================================

Complete loop decode (report highlights; addresses in tinyvenc5.txt):
- ONE exit only (0x16a48-0x16a90: close channel_done fd, TK_MMA_Release x2,
  pop), guarded solely by this->0x71 which is cleared by EncodingGroup::Stop
  or two setup-failure paths. NO error path exits the loop; every error print
  loops back. The thread cannot "die quietly" mid-loop.
- Iteration order: pacing (time-based) -> OSD/clock render -> SetOptions ->
  [if g_enable_dma] TK_MMA_StartOneFrame(ctxA raw preview Y) @0x17590 +
  optional second StartOneFrame @0x1724c + TK_MMA_WaitOneFrameComplete
  @0x17298 -> per-stream enc_stat poll + TK_H264Enc_ProcessOneFrame ->
  [if g_enable_dma] r4 bitstream-DMA loop (armed: big encoded transfer +
  enc_stat write-back pwrite(1) @0x17eec; idle: 4KiB dummy) -> channel_done
  pwrite EVERY iteration (site A @0x168d8 "re-announce previous" when no
  bitstream armed, site B @0x170e8 when armed) -> unlock, loop.
- THE enc_stat HANDSHAKE (host-facing ABI, new): /sys/vpl_pciep/enc_stat<idx>
  backs onto the SAME host-visible struct as the tokens - BAR0+0x50+idx,
  idx = ch*2+stream. Card preads it each frame: byte 0 -> arm bitstream
  (g.0xe98=1); 1 -> busy, retry x10 then skip; 2 -> skip. After DMAing a
  bitstream the card writes 1 there itself. NOTHING card-side clears it:
  **the HOST must write 0 to BAR0+0x50+idx to ack frame consumption** or
  encoding stops after one frame. (encode_status_store0 in ep.ko is the
  only writer that can return it to 0.) => DRIVER TODO regardless of
  anything else: clear enc_stat byte on every frame-done.
- The mailbox is NOT read by this thread (the 0x16cc8 pread is enc_stat, not
  the command mailbox); stale opcodes are irrelevant to it.
- g_enable_dma (card global 0x7ed88): set to 1 by tinyvenc main's
  START_STREAMING handler; if 0, ALL DMA and BOTH channel_done sites are
  skipped (0x1677c -> 0x168dc).

CROSS-CHECK AGAINST HARDWARE (M35-M39) - the model narrows hard: if the loop
were completing iterations we would see channel_done pwrites (every
iteration, both sites host-visible via BAR0 tokens) and the enc_stat
write-back (BAR0+0x50 byte flip). We see NEITHER, ever, and no idle 4KiB
transfers after repoison. Therefore the thread parks INSIDE iteration 1,
after the raw preview DMA (data landed) and before the r4 loop. Two
candidates remain:
  P1 TK_MMA_WaitOneFrameComplete @0x17298 (preview-DMA completion wait;
     = MassMemAccess_WaitDMAC infinite ioctl retry; kernel frees ring slots
     only in the wait, filp tracks only the LAST submitted slot - a
     2-submit/1-wait imbalance would leak+park deterministically);
  P2 TK_H264Enc_ProcessOneFrame @0x17f08 (hardware H.264 encoder wait; may
     depend on an engine/clock that is dead in nosg mode).
Second subagent dispatched to split P1/P2 (submit/wait balance @0x17580-
0x172a8, encoder blocking semantics via libtk_h264_encoder + vma_h4ee.ko)
and name the host-side unblock if any.

M39 RESULT (hardware, 2026-08-07): FRAME LANDS BOTH ROUNDS. Two streamons on
one module load, extent buf[0]=0x6d384 after each SET_VIC/START (irqs 3/0 ->
7/0, still no video IRQs, 0 faults). The card is NOT wedged: every fresh
tinyvenc5 spawn produces exactly one complete frame and then its loop stops.
The defect is therefore in the thread's own continuation/exit logic after
frame 1 - not in the DMA engine, ring, aperture, host addressing, IRQ
delivery, credits, or any host-visible machinery. All of those are now
proven good end-to-end.
