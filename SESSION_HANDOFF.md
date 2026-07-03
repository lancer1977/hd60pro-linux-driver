# Session Handoff — MZ0380 / HD60 Pro driver

Plan file: `/home/wolffyx/.claude/plans/i-want-to-create-happy-prism.md`
RE details: `RE_FINDINGS.md`  |  Test script: `mz0380-m0m2-test.sh`

## Project
Open-source Linux PCIe driver for Elgato HD60 Pro (YUAN MZ0380, PCI `12ab:0380`,
subsys `1cfa:0006`). Repo `/home/wolffyx/Projects/sc0710` had full scaffolding
(`mz0380-*.c/h`) + 7 correlation-confirmed BAR5 encoder-property offsets.

## Decisions locked (with user)
1. Format path: **hybrid** — raw YV12 for validation, keep H.264 encoder controls, expose both.
2. Register oracle: **Ghidra RE** of Windows driver `e60MZ0380.X64.SYS` (on disk, Ghidra installed).
3. Live risk: proceed **up to firmware upload** (M0–M2). DMA/bus-master (M3+) needs separate go-ahead.

## THE key finding (RE-confirmed, corrects the repo)
Command mailbox + firmware buffer are on **BAR0**, NOT BAR5 (repo's model was inverted).
- `ctx+0x108` = BAR0 (mailbox+fw), `ctx+0x110` = BAR5 (DMA ptrs + property cache).
- Proof: BAR5 0x30/0x38 hold BAR0 phys ptrs `fc200004`/`fc20005f` = lspci BAR0 `fc200000`.
- Mailbox: doorbell@0x00 (fire=0x800), opcode@0x04, result@0x08 (0=ok), status@0x2c (bit0=done), fw buf@0x60.
- Firmware = whole blob to BAR0+0x60 between BEGIN(0x0b)/COMMIT(0x0c); base fw 0x0e/0x0f. No chunk protocol.

## Done this session (builds clean)
- M0 observability: snapshot profile 6, CFG trace → 0x00e4, BAR0 mailbox dump in `/proc/mz0380-state`.
- M1 RE: `RE_FINDINGS.md` (decompiled `e60MZ0380.X64.SYS`).
- M2 code: rewrote `mz0380-reg.h` mailbox/fw defs, `mz0380_send_command` (core.c),
  `mz0380-fw.c` upload, fixed ISR (dma.c) + signal (video.c) to BAR0.

## NEXT — start here
1. **User runs live test** (needs sudo/TTY — agent has none):
   - `sudo ./mz0380-m0m2-test.sh m0` → gate: do `bar0[...]` lines read structured
     values (mailbox live) or `0xffffffff` (asleep, need wake sequence)?
   - if live: `sudo ./mz0380-m0m2-test.sh m2` → pass = boot success + BAR0 ≠ 0xffffffff.
2. **M3 RE pass (offline, safe, do anytime):** decompile IRQ + ring offsets —
   `Interrupt_Handler` = FUN_14024ba60, streaming setup = FUN_1402829a0.
   Ghidra project cached at `scratchpad/re/proj/mz0380`; reuse `DecompMZ0380.java`
   (edit the addr list). Logs: `scratchpad/re/{ghidra,decomp}.log`.
3. **M4:** add `V4L2_PIX_FMT_YV12` to `mz0380-video.c` enum/try/s/g_fmt (lines ~311–380),
   size `w*h*3/2`, colorspace SMPTE170M, keep H.264 as 2nd format.

## Watch out
- Never enable bus mastering by hand; only `mz0380_dma_setup()` may, after ring base
  programmed (IOMMU-fault guard, group 24). Keep `enable_dma=0` until M3 authorized.
- Firmware blob at `/lib/firmware/mz0380/MZ0380.HD.HEX` (present). Card at `04:00.0`.
- Unresolved/CHECKME: GET_FW_VERSION opcode, QUERY_SIGNAL opcode, IRQ regs, ring regs.
