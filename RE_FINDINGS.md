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
