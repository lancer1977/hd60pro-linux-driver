# windows-findings.md — HD60 Pro bring-up (answers to the 5 questions)

> Full detail + firmware/driver artifacts: **`HD60-PRO-LINUX-DRIVER.md`** (same folder).
> Live DebugView trace was NOT run this session (needs a human to drive HDMI replug + app).
> These answers come from static RE of `e60MZ0380.X64.SYS` **and the on-card firmware**
> (the `.HEX` files are gzip'd tars of the card's embedded-Linux root — the real ground truth).

**Driver:** `e60MZ0380.X64.SYS` v**1.1.0.194** (07/13/2021, built 2021.06.29), signed Corsair.
**App/board:** Elgato Game Capture HD60 Pro, board `ELGATO BOARD SC5C0N1`, PCI `12ab:0380`
SUBSYS `00061CFA` (Rev.1 + Ryzen fix). SoC firmware ver `01.11`.

### 1. Which I2C path to the MST3367? *(disassembly-verified, both driver + firmware)*
Mailbox opcode **`0x1a` (read) / `0x1b` (write)** to device **`0x9C`**, serviced by
`yuan_ioctrl` on the card's `/dev/i2c-0` (kernel i2c-gpio, GPIO12=SCL/GPIO13=SDA). Multi-byte
(EDID) uses **combo opcode `0x20`**. `"mcu version = 0"` = **no separate MCU** (the SoC is the
I2C master; `NO MCU ....`), so "MCU-passthrough" and "direct" are the same path here.
**Correction to the briefing:** these are the *same* opcodes Linux was already trying — the
opcode was never the problem (see the fix box below). Full protocol in
`HD60-PRO-LINUX-DRIVER.md` Appendix A.

### 2. Hardware reset / GPIO / power before I2C?
**Yes.** Driver has `SA7160_HARDWARE_I2C_RESET( %d I2C:%08X:%08X:%08X )`. Card `ep.ko` drives
GPIO (`gpio_direction_output`, `gpio_set_value`). VState machine runs `SWReset` and
`HDCP_Reset` **before** `ModeDetecting`. On-card boot brings up all GPIO+I2C before the PCIe
mailbox (`ep.ko`) even comes online, so I2C is always ready from the host's side.

### 3. Bring-up order
`PCIe/ep up → yuan_ioctrl starts → [FIRMWARE RESET] → VSTATE_SWReset → UpdateEDID
([UPDATE.EDID] ELGATO BOARD SC5C0N1 1080P) → [HOTPLUG] HPD assert → VSTATE_HDCPSet →
VSTATE_ModeDetecting (MST3367 reg 0x55) → VSTATE_VideoOn`. EDID is written **before** HPD.
Full VState chain: `PwrOff→SyncWait→SWReset→SyncChecking→HDCPSet→HDCP_Reset→ModeDetecting→VideoOn`.

### 4. reg-0x55 mode-detect format strings (values need live trace / KDNET §8)
```
MST3367_HDMI_MODE_DETECT( 0x55 = 0x%x )
MST3367_HDMI_MODE_DETECT( %d ? %d ? %d ? %d ? %d ? R0055 = %02X %c )
MST3367_HDMI_MODE_DETECT, unstable signal %d, %d, fv %d, calc_vfreq %d, R0055 = %02X %c, wx(%d)
```

### 5. Does the MST3367 respond?
**Yes on Windows** (retail driver produces picture; full `ModeDetecting→VideoOn` path with
non-zero `R0055`). The proof for Linux: the **combo/0x9C path works where the direct bit-bang
NAKs** — that's the fix, not a hardware fault.

---
### Fix for the Linux "NAK on every address" problem  *(root cause found via disassembly)*
The opcode was never wrong. The card firmware does **`addr7 = addr8 >> 1`**, so the host must
place the **8-bit** address (`0x9C`, EDID EEPROM `0xA0`) in the mailbox command — the card
shifts it to 7-bit. If Linux sends 7-bit `0x4E`/`0x50`, every device NAKs (address halved).
Two things to fix:
1. **Send 8-bit I2C addresses** in the command word. Validate by reading EDID EEPROM at 8-bit
   `0xA0` first.
2. **Page-select before paged regs:** write `dev=0x9C, reg=0x00, val=<page>` (opcode 0x1b),
   caching current page like Windows does. Reg 0x55 is only valid on its page.

Command word array the Windows driver sends: `[0]=0x800 (doorbell)`, `[1]=opcode`, `[2]=dev8`,
`[3]=reg`, `[4]=val`. READ=0x1a, WRITE=0x1b, bulk=0x20. Then run reset→EDID→HPD→detect.
Byte-exact struct + both-sides disassembly: `HD60-PRO-LINUX-DRIVER.md` Appendix A.
