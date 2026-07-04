# MZ0380 driver — session handoff

## Project
Open-source Linux V4L2 driver for the Elgato HD60 Pro (YUAN MZ0380,
PCI 12ab:0380, subsys 1cfa:0006). Repo: /home/wolffyx/Projects/sc0710.
Clean-room RE of the Windows driver + firmware; own hardware. Not security work.

## State: CONTROL PATH WORKS (verified on hardware)
Firmware upload+boot AND the post-boot command mailbox are fully working and
deterministic. Card runs firmware 1.11, answers commands, reports version, and
peripheral register read/write over the mailbox works.

Load it:
    sudo ./mz0380-m0m2-test.sh m2      # upload/handshake (no bus master needed)
    sudo ./mz0380-m0m2-test.sh m4      # same + bus master + mailbox scan diag
m2 alone reaches: "CMD_INIT answered" -> "board reports running firmware 1.11"
-> "card already runs firmware 1.11, skipping upload" (fast path, no 21s boot).

## The protocol (RE-confirmed, on BAR0 — see RE_FINDINGS.md + memory)
- Mailbox in BAR0: opcode @0x04, params @0x08.., doorbell @0x00 (=0x800),
  STATUS @0x2c, fw buffer @0x60, event word @0x30.
- Completion: STATUS reads 0xaaaaaaaa success stamp (NOT bit0). 0xdddddddd is a
  boot stamp, NOT completion. Long ops (fw dl) signal via EVENT(0x30) bit11.
- Event ack (ONLY when EVENT!=0): BAR5[0xdc]=2; BAR0[0x30]=0; doorbell 0x400.
- Opcodes: 0x01 INIT, 0x0a GET_BOARD_VERSION, 0x0b/0x0c fw dl begin/commit
  (commit = fire-and-forget, ~21s boot), 0x1a/0x1b peripheral reg read/write
  (chip 0x90 bridge, 0xb8 TVP5160 analog).
- Firmware blob = gzip+tar = full ARM Linux SDK; card serves the mailbox from
  drivers/ep.ko. ep.ko IS the command spec oracle (decompiled).

## THE key fix (why it was deaf before)
The poll loop fired the 0x400 ack doorbell every 1ms unconditionally; that
ABORTS a STATUS-completing command before the card finishes. Fix: poll
silently, ack ONLY when EVENT(0x30)!=0. Bus mastering is NOT required.

## Commits this session (on main, not pushed)
550fab0 honest bridge probe (stop asserting bogus signal)
71c2862 docs: M4 control-path success
2ab7c36 fix: quiet mailbox poll  <-- the breakthrough
4c267c0 M4 mailbox layout scan diagnostic
43ad905 fix: bus master ordering
728c01e M4 dma_handshake diagnostic
3e4440a post-boot handshake + ep.ko command spec
1b05934 periph reg access, event drain, DMA RE notes
6b1d434 firmware upload+boot via BAR0 mailbox

## OPEN / next milestone: frame capture (M4 second half)
1. HDMI signal register UNKNOWN. bridge[0x12] was a wrong guess. Windows
   streaming thread FUN_1402829a0 is the ANALOG (TVP5160/chip 0xb8) path — the
   HD60 Pro's HDMI receiver is a different chip, not yet located.
   CHEAP: cat /proc/mz0380-state | grep 'bridge probe' with source connected vs
   not; see which of 0x11/0x12/0x16/0x17/0x8b flips.
2. Nothing yet COMMANDS the card to select HDMI input / start the video front
   end — we only cache the input value. Need input-select + SET_VIC +
   START_STREAMING opcodes (RE ep.ko pciep_isr cmd 2/3/4 SET banks + Windows
   SET_VIC_PARAMS / AUTO.INPUT path).
3. Frame DMA ring: mz0380-dma.c ring-register offsets are GUESSES. Need real
   offsets before enabling frame DMA (RE DMA alloc FUN_14028d254 consumers +
   XDMA channel regs). enable_dma=0 still the guardrail.

## RE tooling (reusable)
Ghidra project cached: scratchpad/re/proj/mz0380 (Windows .sys AND card ep.ko
both imported). Reuse scratchpad/re/DecompMZ0380.java (edit addr list),
-process <name> -noanalysis -postScript. Firmware extracted:
scratchpad/fw/yuan_demo_sdi/. Key funcs: Windows init FUN_140278bb0, streaming
FUN_1402829a0, DMA alloc FUN_14028d254; card ep.ko pciep_isr @0x11234.

## Guardrails
No manual bus-mastering needed for control (confirmed). enable_dma=0 until ring
offsets verified. Blob + card present; firmware persists across reloads.
