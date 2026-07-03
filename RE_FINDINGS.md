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
