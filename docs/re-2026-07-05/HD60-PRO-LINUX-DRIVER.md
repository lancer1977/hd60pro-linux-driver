# HD60 Pro — Linux driver working document

**Target:** clean-room V4L2 driver for Elgato Game Capture HD60 Pro (PCIe, `12ab:0380`).
**Source of this doc:** static reverse-engineering of the retail Windows driver **and the
on-card firmware**, done on the Windows side of the dual-boot machine. Interop RE on the
user's own hardware.
**Where to continue:** cashyos (Linux) side. Everything below is what the Windows install
told us; artifacts are in `artifacts/` next to this file.

> **STATUS — read first.** The *live* DebugView trace (the `capture-hd60-trace.bat` flow:
> plug 1080p60, start preview, unplug/replug) was **NOT captured in this session** — it
> needs a human at the machine to drive the HDMI edge and the Elgato app. That capture is
> still worth doing (see §8) and would confirm the runtime ordering. **But it turned out to
> be less important than expected:** the firmware `.HEX` files on disk are gzip'd tarballs
> of the *entire on-card embedded-Linux root*, including the I2C kernel modules, the
> `yuan_ioctrl` daemon, and the boot scripts. That is the ground truth the Linux side was
> missing, and it directly explains the "NAK on every address" problem. Read §3 and §5.

---

## 1. Hardware / driver identity (confirmed on this machine)

| Item | Value |
|---|---|
| PCI device present | `PCI\VEN_12AB&DEV_0380&SUBSYS_00061CFA&REV_00`, status OK |
| Device name | Game Capture HD60 Pro |
| Windows driver file | `C:\Program Files\Elgato\Game Capture HD60 Pro\e60MZ0380.X64.SYS` (installer copy) |
| Driver version | **disassembled = 1.1.0.194** (Program Files); **loaded = 1.1.0.195** (`C:\Windows\System32\drivers\…`). EDID byte-identical across both; protocol/registers unchanged, only code offsets differ (195 is ~30 KB larger). See Appendix H. |
| Firmware (running) | **1.11.1.11, 2021.09.28** (live readout; the `MZ0380.FW.TXT` `01.11` is just the major.minor) |
| Driver framework | KMDF + AVStream/BDA minidriver ("Custom Broadcast Driver Architecture") |
| PDB path in binary | `C:\WDK\AMEBDAD.DRIVERS\x64\MZ0380\AMEBDAD.DRIVERS.pdb` |
| Signed by | Corsair Memory, Inc. (Elgato's parent) |
| Board string in FW | `ELGATO BOARD SC5C0N1 1080P`, hostname `SC5C0`, EEPROM name `SC530-N1` |
| SoC / encoder | **Vatics Mozart 395s** hardware H.264 encoder SoC (ARMv5, kernel `2.6.28.9-Mozart-8G`), Yuan MZ0380 module. (Briefing's "MStar SL6010" was wrong — confirmed Vatics Mozart via firmware + teardowns.) |
| HDMI receiver | **MStar MST3367CMK-LF-170** — driven by host over I2C at dev `0x9C` (§3) |
| HDMI front-end | **ITE IT6621FN** (HDMI input interface ahead of the MST3367) |
| RAM | 2× Samsung K4B2G1646Q DDR3 |

> **MST3367 identification — CONFIRMED.** Originally inferred from the driver (functions named
> `MST3367_HDMI_MODE_DETECT`, only HDMI RX among ~12 supported input chips, EDID mfr `MST`
> product `0x3367`, BT.1120p input, dev-0x9C paged register model). Now corroborated by public
> teardowns/reviews of the HD60 Pro, which list the exact parts: **MST3367CMK-LF-170** (HDMI
> receiver), **Vatics Mozart 395s** (encoder SoC), **ITE IT6621FN** (HDMI interface), Samsung
> DDR3. So "the HDMI receiver at I2C 0x9C" = MStar MST3367CMK. (Non-invasive ways to re-verify
> on your own unit: teardown/FCC internal photos; a DebugView trace showing `MST3367_…` lines
> during live bring-up; or an I2C ID readback at 0x9C.)

**INF supports these board revisions** (`GameCaptureHD60Pro.inf`):
- `DEV_0380 SUBSYS_00031CFA` — HD60 Pro Rev.1
- `DEV_0380 SUBSYS_00051CFA` — Rev.2 (never released)
- `DEV_0380 SUBSYS_00061CFA` — Rev.1 + Ryzen fix  ← **this machine**
- `DEV_0381 SUBSYS_00101CFA` — Rev.3 (different firmware: `MZ0381.*`)
- `DEV_0380 SUBSYS_12AB05CF` — prototype

The card is a PCIe **endpoint** (`ep.ko` on the card side). The host driver DMA-maps card
memory (`MmMapIoSpace`, `MmAllocateContiguousMemorySpecifyCache`, `IoConnectInterrupt`)
and talks to the on-card app through a **firmware mailbox** (`MZ0380_SEND_COMMAND`).

---

## 2. Firmware layout (this is the big find)

The three `.HEX` files shipped in the driver package are **gzip-compressed tar archives** of
the on-card Linux root filesystem, not Intel-HEX:

| File | Content | Notes |
|---|---|---|
| `MZ0380.HD.HEX` | `yuan_demo_sdi/` tree | HD (1080) firmware for Rev.1/2, FW ver `01.11` |
| `MZ0380.SD.HEX` | `yuan_sd/` tree | SD firmware, has extra `debug_i2c` tool |
| `MZ0381.HD.HEX` | `yuan_demo_sdi/` tree | Rev.3 firmware, FW ver `11.11` |

Decompressed copies are in `artifacts/MZ0380.HD.firmware.tar`, `...SD...`, `MZ0381...`.
Extract with plain `tar xf`. Key files inside `yuan_demo_sdi/`:

```
etc/rc.local              # boot: loads drivers, starts apps  (see §5)
drivers/drivers.sh        # insmod order for all card kernel modules
drivers/gpio_i2c.sh       # ← GPIO-bitbang I2C bus setup (the critical file)
drivers/gpio_i2c.ko       # bit-bang I2C char driver ($Version 3.1.0.8)
drivers/i2c-gpio.ko       # standard Linux i2c-gpio, second instance
drivers/IICCtrl.ko        # I2C control wrapper (GPIOI2C_get_fops)
drivers/ep.ko             # PCIe endpoint driver (host<->card mailbox)
drivers/Godshand.ko/.bin  # register poke tool (BAR readiness check)
yuan_ioctrl               # ← the daemon that services host I2C-proxy opcodes
i2cget / i2cset / i2cdump / i2cdetect   # busybox-style I2C userspace tools
```

The card is an ARM SoC (`/lib/ld-uClibc.so.0`, `GCC (Buildroot 2013.08.1) 4.8.2`,
`.ARM.attributes`). "Godshand" = MStar's register-access helper.

---

## 3. THE I2C PATH — how the host reaches the MST3367

This is the question the Linux side has been stuck on ("direct reads NAK at every address on
both buses"). The firmware answers it.

### 3a. What the on-card I2C buses actually are

`drivers/gpio_i2c.sh` (identical in HD and SD firmware) loads the bit-bang driver with:

```sh
insmod gpio_i2c.ko iSCL_bus0=0x00001000 iSDA_bus=0x00002000
# commented-out alternative:
#insmod gpio_i2c.ko iSCL_bus0=0x00000040 iSDA_bus=0x00000080 iSCL_bus1=0x00001000 iSDA_bus1=0x00002000
```

The params are **GPIO bit-masks**, so the active config is:
- **bus0 SCL = GPIO bit 12 (0x1000), bus0 SDA = GPIO bit 13 (0x2000).**
- bus1 is **not configured** in the active line (only bus0 exists on this board).
- The alt/commented config would put bus0 on GPIO6/7 and bus1 on GPIO12/13.

`gpio_i2c.ko` then creates char devices `/dev/gpio_i2c0..3`.

Separately, `drivers/drivers.sh` also loads the *stock* Linux `i2c-gpio.ko`:

```sh
insmod i2c-gpio.ko bus_num=2 scl0=12 sda0=13 scl1=4 sda1=5
```

→ giving Linux i2c adapters on **GPIO12/13 (bus "2"/0)** and **GPIO4/5 (bus 1)**. Note
GPIO12/13 is the *same pin pair* the bit-bang `gpio_i2c.ko` uses for bus0. So on this board
**the HDMI-side I2C physically lives on GPIO12 (SCL) / GPIO13 (SDA).**

### 3b. What `yuan_ioctrl` (the mailbox servicer) actually does

`yuan_ioctrl` is the on-card daemon that services the host's I2C-proxy opcodes. Its strings
show it opens **`/dev/i2c-0` and `/dev/i2c-1`** (the *kernel* i2c-gpio adapters, not the raw
`gpio_i2c` char dev) plus `/dev/gv7601` (SDI chip) and `/sys/vpl_pciep/command` (the mailbox
doorbell). Relevant symbols/strings:

```
i2c_write / i2c_read / i2c_write_s / i2c_read_s
i2c_combo_cmd_x            ← host opcode 0x20 combo route
i2c_encrypt_read / i2c_encrypt_write / Encrypt_Check   ← HDCP/encrypted path
MZ0380_SetSpiRegister / MZ0380_GetSpiRegister / spi_combo_cmd
[APP_YAUNIO] write I2C addr-%d, reg-%d, val-0x%x
[APP_YAUNIO] read  I2C addr-%d, reg-%d, val-0x%x
/dev/i2c-0 , /dev/i2c-1
/sys/vpl_pciep/command
```

So: host opcode → mailbox → `yuan_ioctrl` → **`ioctl` on `/dev/i2c-0` (kernel i2c-gpio on
GPIO12/13)** → bit-banged to the receiver. The host **never** bit-bangs the receiver's I2C
directly; the card's ARM does, and the host only sends high-level opcodes.

### 3c. What the Windows driver does (register-helper signature)

The Windows briefing already reverse-engineered the write helper at `e60MZ0380+0x28658c`:

```
args: r8b = dev (0x9C), r9b = page, [rsp+0x20] = reg, [rsp+0x28] = value
read helper: +0x277884 ;  mailbox send_command: +0x285074
```

**`dev = 0x9C` is the MST3367's I2C slave address** (0x9C = 7-bit 0x4E, write). The Windows
driver builds a `{dev=0x9C, page, reg, value}` tuple and hands it to the mailbox — it does
**not** choose a GPIO bus. Bus selection happens on the card, inside `yuan_ioctrl`, against
`/dev/i2c-0`.

### 3d. → Why Linux was NAKing (root cause, disassembly-verified)

> **This supersedes an earlier hypothesis in this doc.** After disassembling both the on-card
> `yuan_ioctrl` (ARM) and the three Windows mailbox helpers (x86-64), the picture is exact —
> see **Appendix A** for the full protocol on both sides. Summary of what changed: the opcode
> was **never** the problem. Windows reaches the MST3367 with the *same* opcodes the briefing
> said Linux was already trying (`0x1a` read / `0x1b` write). The real fault is almost
> certainly the **I2C address encoding** and/or the missing **page-select**, below.

**Verified facts (both sides agree):**
- Windows READ = mailbox opcode **`0x1a`**, WRITE = **`0x1b`**, both to device **`0x9C`**.
- The card firmware does **`addr = cmd[+4] >> 1`** before the Linux `i2c-dev` call. So the host
  must place the **8-bit** address `0x9C` in the command; the card right-shifts it to the 7-bit
  `0x4E`. (Windows passes `r8b = 0x9C`; `0x9C >> 1 = 0x4E` = the MST3367.)
- Register access is **paged**: before a reg R/W, the Windows driver writes **reg `0x00` = page**
  (a separate `0x1b` write) whenever the page changes, and caches the current page. Reg `0x55`
  and friends are only valid on the correct page.
- Opcodes `0x1a/0x1b/0x1e/0x1f/0x20` **all** target bus0 (`/dev/i2c-0`, GPIO12/13). Only the
  HDCP opcodes `0x1c/0x1d` use bus1 (`/dev/i2c-1`, GPIO4/5). The briefing's "0x1e/0x1f = bus1"
  was wrong.

**→ Most likely root cause of "NAK at every address on both buses":**
1. **8-bit vs 7-bit address.** If the Linux driver puts the **7-bit** `0x4E` (or `0x9C` already
   shifted) into the command word, the card does `>> 1` and ends up driving `0x27` (or `0x4E`
   halved) → every device NAKs, *including the EDID EEPROM* (its 8-bit `0xA0` must be sent as
   `0xA0`, not `0x50`). **Send 8-bit addresses in the mailbox command.** This single bug
   explains "nothing answers on either bus."
2. **No page-select.** Reading `0x55` without first writing `reg0=page` reads garbage/NAK.
3. **Receiver held in reset.** Until the bring-up runs `VSTATE_SWReset` / `SA7160_HARDWARE_I2C_RESET`
   and asserts HPD, the MST3367 can NAK. Do reset + EDID + HPD first (§5c).

**Action items for the Linux driver:**
1. Use opcode **`0x1b`** (write) / **`0x1a`** (read), device **`0x9C`** as an **8-bit** value in
   the command — let the card shift it. Verify by reading the EDID EEPROM at 8-bit `0xA0` first;
   if that answers, the address encoding is correct.
2. Implement **page-select**: before touching a paged register, write `dev=0x9C, reg=0x00,
   val=<page>` (opcode 0x1b). Cache the current page like Windows does.
3. For multi-byte / bulk (EDID blocks), use the **combo opcode `0x20`** (`i2c_combo_cmd_x`):
   `cmd[+4]=addr(8-bit)`, `cmd[+5]=rw`, `cmd[+6..7]=len`, `cmd[+8..]=payload`.
4. HDCP key traffic uses the separate encrypted opcodes `0x1c/0x1d` on bus1 — ignore for plain
   config/EDID/mode-detect.
5. Full command-struct byte layout and the send/doorbell mechanism: **Appendix A**.

---

## 4. The five briefing questions — answered from static RE

Live-trace confirmation still recommended (§8), but here is what the binaries already prove.

**Q1 — Which I2C path to the MST3367?** *(disassembly-verified, both sides — Appendix A)*
Mailbox opcode **`0x1b` (write) / `0x1a` (read)** to device **`0x9C`** (8-bit; card shifts to
7-bit `0x4E`), serviced by `yuan_ioctrl` on the card's `/dev/i2c-0` (GPIO12/13). Multi-byte
uses the **combo opcode `0x20`**. There is **no separate MCU** — `"mcu version = 0"` = the SoC
itself is the I2C master (`NO MCU ....` / `MCU CHECK FAIL !!!`). So "MCU-passthrough" and
"direct" collapse to the same thing here: every plain opcode is the SoC bit-banging bus0. The
Linux NAK was an **address-encoding / page-select** issue (§3d), not a wrong opcode.

**Q2 — Hardware reset / GPIO / power before I2C?**
Yes. The Windows driver has `SA7160_HARDWARE_I2C_RESET( %d I2C:%08X:%08X:%08X )`. On the card
side, `ep.ko` exposes GPIO direction/set/get (`gpio_direction_output`, `gpio_set_value`,
`$$$ gpio_direction_input fail`), and the boot sequence (§5) brings up GPIO/I2C **before** any
receiver access. The VState machine also has an explicit `VSTATE_SWReset` and
`VSTATE_HDCP_Reset` before `VSTATE_ModeDetecting`. So the order is reset → config → detect.

**Q3 — Bring-up order?**
From the firmware boot script + the driver's VState enum (§5, §6). Rough timeline:
`ep.ko/PCIe up → drivers loaded → yuan_ioctrl starts → [FIRMWARE RESET] → SW reset →
EDID load ([UPDATE.EDID] ELGATO BOARD SC5C0N1 1080P) → HPD/[HOTPLUG] assert → HDCP set →
MST3367 mode detect (reg 0x55) → VideoOn`.

**Q4 — reg-0x55 mode-detect format strings** (exact, from the binary):
```
MST3367_HDMI_MODE_DETECT( 0x55 = 0x%x )
MST3367_HDMI_MODE_DETECT( %d ? %d ? %d ? %d ? %d ? R0055 = %02X %c )
MST3367_HDMI_MODE_DETECT, unstable signal %d, %d, fv %d, calc_vfreq %d, R0055 = %02X %c, wx(%d)
```
The *values* need the live trace (or the KDNET breakpoint in §8) — static RE gives the format,
not the runtime hex.

**Q5 — Does the MST3367 respond?**
On Windows: yes (retail driver works, produces picture). The driver has a full
`VSTATE_ModeDetecting → VSTATE_VideoOn` path and logs non-zero `R0055`. Proof for the Linux
side is precisely that the combo/0x9C path works where the bit-bang path NAKs.

---

## 5. On-card boot / bring-up sequence (`etc/rc.local`, verbatim summary)

Order the card executes on power-up (host must not fight this; it only sends opcodes after):

1. `hostname SC5C0`; set `LD_LIBRARY_PATH`.
2. `yuan_fw_check` — validate firmware; auto-create `_bak` and `update/` dirs.
3. `cd drivers; sh drivers.sh` — **module load order** (see §5a).
4. `mknod /dev/gv7601 c 103 0` — SDI receiver node.
5. `yuan_update_mgr -D` — base-firmware update manager.
6. `./yuan_ioctrl -D -T 200` — **start the I2C-proxy / mailbox servicer** (HD firmware:
   `-T 200`; SD: `-T 110`; restart path: `-T 500`). `-T` = I2C delay in µs
   (`[APP_YAUNIO] i2c delay %d`).
7. `./audio_capture_mgr -D -P 5 -S 0 -F 256 -B 4`
8. `./video_capture_mgr -D -P 5`   (restart path also runs `tinyvenc5/7 -c nullsensor_1920x1080.cfg`)

### 5a. `drivers.sh` insmod order (relevant to I2C/PCIe)
```
Godshand.ko                       # register poke helper
i2c-algo-bit.ko
i2c-gpio.ko bus_num=2 scl0=12 sda0=13 scl1=4 sda1=5   # kernel i2c-gpio adapters
gpio_i2c.sh  → gpio_i2c.ko iSCL_bus0=0x1000 iSDA_bus=0x2000   # bit-bang char dev
IICCtrl.ko                        # I2C control wrapper
NULLSensor.ko , vpl_vic.sh , vpl_edmc.ko , vma_*.sh (image pipeline)
spi_gv7601.ko , gv7601_audio.ko   # SDI path
wdt.ko                            # watchdog
# --- yuan_pcie_initial: poll BAR0 @ 0x82000010 until non-zero, then: ---
ep.ko                             # PCIe endpoint (host mailbox) comes up LAST
vpl_dmac.sh
```
Key ordering fact: **all I2C + the receiver path is up before `ep.ko`**, i.e. before the host
can even talk to the card. So from the host's view the I2C subsystem is always ready; the
host just needs the right opcode.

---

## 5b. How the card actually boots (IMPORTANT for the Linux side)

The card does **not** boot when the host driver loads. Two independent stages:

**Stage 1 — self-boot off onboard flash, at PCIe slot power (PC power-on).**
The SoC has persistent flash: `yuan_update_mgr` programs `/dev/mtd%d`, mounted at `/mnt/flash`.
The entire `yuan_demo_sdi/` root — `yuan_ioctrl`, the i2c `.ko`s, `ep.ko` — lives there, with a
self-healing backup (`yuan_demo_sdi_bak`) and `yuan_fw_check` that restores it. So on power-up
the ARM SoC runs bootloader → BASE firmware → Linux → `rc.local` (§5) → starts `yuan_ioctrl`,
**before and independent of the host OS.** This is identical under Windows and under Linux.
The card signals readiness via BAR0 reg `0x82000010` going non-zero (polled in `drivers.sh`).

**Stage 2 — host driver does version-check + conditional reflash + runtime init.**
At device-start (PnP D0), the Windows driver:
- `MZ0380_HwInitialize` reads the card's live `FIRMWARE VERSION` / `BASE VERSION` over the mailbox;
- compares to files in `C:\WINDOWS\MZ0380\` (`MZ0380.FW.TXT`=`01.11`, `MZ0380.HD.HEX`, …; placed
  there by the INF `CopyFirmwares` → confirmed present);
- **only if the card is older** pushes an image (`MZ0380_DownloadFirmware` → `yuan_update_mgr`
  "version too old"/"new version" → MTD erase → reflash). Matched card = no-op.
- then runs runtime bring-up (reset → EDID → HPD → detect).

The `Jump2LDROM` / `Run APROM` / `CheckLD_ROM_Idle` / `VerifyLDROM` / `EnableSecurity` strings
are a **Nuvoton MCU ISP** flash dance for the small EDID MCU (the EEPROM the HDMI *source* reads),
**not** the main SoC boot.

**Consequences for the cashyos driver:**
1. The card is almost certainly **already booted and running `yuan_ioctrl` from flash** by the
   time Linux enumerates PCIe. The Linux driver normally needs **no firmware download** — the
   flash image persists across a Windows→Linux reboot. Just do version-check + init handshake +
   mailbox opcodes.
2. **Still implement the download/reflash fallback:** if the card was never provisioned or flash
   is wiped, the mailbox won't answer until a host image is pushed.
3. **Version sentinel:** if the mailbox reports firmware `00.00` (base) or `00.10`/`00.11`
   (the `MZ0380.BASE.TXT` = `00.00` / "wrong FW" values in `rc.local`), the card is sitting in
   **base/recovery firmware** and needs a host download — it is NOT a normal boot.
4. Firmware source files the host uses at runtime live in `C:\WINDOWS\MZ0380\`; the Linux
   equivalents are the decompressed tars in `artifacts/` (push the tar payload, not the `.HEX`).

## 5c. HDMI detection — ordering and the second "NAK-like" trap

Detection happens **after** both the card self-boots and the host driver initialises, and it is
a **continuous, interrupt-driven** state machine (`Interrupt_Handler() VState(...)`,
`[KSEVENT_DEVICE_LOST]` on loss) — not a one-shot at boot. Prerequisite chain:

1. Card self-boots off flash (§5b) → `yuan_ioctrl` + mailbox alive.
2. Host driver reaches D0 → `MZ0380_HwInitialize`, version check, `windows_select_fw`
   (host picks the card's firmware profile), `AUTO.INPUT` selects the input.
3. **Driver writes EDID into the MST3367 and asserts HPD** (`[UPDATE.EDID]`, `[HOTPLUG %d]`,
   `SA7160_HARDWARE_I2C_RESET`).
4. **Only now does the HDMI source transmit** — a source will not output TMDS until it sees a
   sink presenting valid EDID with hotplug asserted.
5. MST3367 locks; the ISR walks `SyncWait → SyncChecking → HDCPSet → ModeDetecting → VideoOn`,
   reading **reg 0x55** each step (`is_nosg`/`nosg` while unlocked).
6. Opening a capture/preview pin starts the video pipe (`SET_VIC`, `[PREVIEW PIN]`,
   `[CAPTURE PIN]`, `STOP_STREAMING`). Detection itself is independent of an app streaming.

**THE TRAP (Linux):** EDID and HPD are **host-controlled**. If the driver reads reg 0x55
*before* it has pushed EDID + asserted HPD, it reads **no-signal / unstable even with the I2C
combo path working perfectly** — because the source isn't transmitting. This looks like the
original "reads zero" bug but is a different cause. Distinguish three states:

| Symptom | Cause | Fix |
|---|---|---|
| I2C **NAK** at dev 0x9C | wrong path (direct bit-bang) | use combo opcode 0x20 (§3d) |
| I2C **works**, reg 0x55 = no-signal | EDID/HPD not set, or source unplugged | write EDID + assert HPD first, then re-read |
| reg 0x55 = stable non-zero | locked | walk to VideoOn, open pin |

Order on Linux: **combo path up (0x9C) → write EDID into receiver → assert HPD → poll/IRQ reg
0x55 → mode → stream.** Do not read 0x55 for "is there signal?" until after EDID+HPD.

## 6. Driver state machines & mailbox (from `e60MZ0380.X64.SYS` strings)

**Video state machine** (`VSTATE_*`), in order:
`PwrOff → SyncWait → SWReset → SyncChecking → HDCPSet → HDCP_Reset → ModeDetecting →
VideoOn → Reserved`.

**Audio state machine** (`ASTATE_*`):
`AudioOff → RequestAudio → ResetAudio → WaitForReady → AudioOn → Reserved`.

**Mailbox command interface:**
```
MZ0380_SEND_COMMAND( %08X, %d )        # send opcode
[%02X] MZ0380_SEND_COMMAND(...) TIMEOUT!! / ERROR!!
MZ0380 COMMAND TIMEOUT( %d %08X )
MZ0380_GetFirmwareVersion( "%ws" )
MZ0380_DownloadBaseFirmware / DownloadFirmware / DownloadOsdPicture / DownloadLogoPictures1..3
MZ0380_HwInitialize( %d, %04X%02X%02X )
FIRMWARE VERSION: %d.%d   BOARD VERSION: %d.%d / %d.%d
[%02X] [FPGA] [VERSION: %d]  [MCU] [VERSION: %d]  [CPLD] [VERSION: %d]
```
The host **downloads the firmware tarball to the card at init** (`MZ0380_DownloadFirmware`) —
this is where the `.HEX` tar goes. The Linux driver must implement the same firmware-push
handshake (or rely on the card's persisted `/mnt/flash` copy if already provisioned).

**EDID handling:**
```
[UPDATE.EDID] ELGATO BOARD SC5C0N1 1080P
CDevice::Enter UpdateEDID / send EDID data / update EDID checksum / Exit UpdateEDID wait
CDevice::Enter ReadEDID / Read EDID 0x%x, (16 bytes...) / read EDID data checksum
[HOTPLUG %d]                        # HPD toggle
[READ.PASS.MONITOR.EDID]
```
EDID is written into the MST3367 (so downstream HDMI source sees the card's EDID), then HPD is
asserted via `[HOTPLUG]`. Do EDID **before** HPD.

**Crossbar / input select** (from INF `AddReg`):
```
AnalogCrossbarVideoInputProperty: 0=HDMI 1=DVI-D 2=COMPONENT 3=DVI-A 4=SDI  (default 0=HDMI)
AnalogCrossbarAudioInputProperty: 0=EMBEDDED AUDIO 1=LINE IN
```

---

## 7. Concrete constants for the Linux driver

| Thing | Value |
|---|---|
| PCI ID | `12ab:0380` (Rev.1/2), `12ab:0381` (Rev.3) |
| Card SoC | MStar **Mozart**, ARMv5, Linux **2.6.28.9-Mozart-8G** (uClibc/Buildroot) |
| MST3367 I2C slave addr | send **8-bit `0x9C`** in the command; card does `>>1` → 7-bit `0x4E` |
| Receiver reg model | **paged** — write `reg 0x00 = page` (opcode 0x1b) before reg R/W |
| Mode-detect register | **0x55** (`R0055`) |
| Card I2C (SCL/SDA) | bus0 = `/dev/i2c-0` = GPIO12/13 (receiver); bus1 = `/dev/i2c-1` = GPIO4/5 (HDCP) |
| I2C READ opcode | **`0x1a`** → `i2c_read` on bus0 |
| I2C WRITE opcode | **`0x1b`** → `i2c_write` on bus0 |
| I2C bulk/combo opcode | **`0x20`** → `i2c_combo_cmd_x` on bus0 (arbitrary length) |
| I2C `_s` variants | `0x1e` read_s / `0x1f` write_s (bus0, multi-byte) |
| HDCP encrypted I2C | `0x1c` read / `0x1d` write on **bus1** |
| GV7601 (SDI) SPI | `0x18` read / `0x19` write / `0x21` combo-rd / `0x22` combo-wr via `/dev/gv7601` |
| Mailbox ioctl (card) | Linux `I2C_RDWR` = `0x0707` on `/dev/i2c-N` |
| Mailbox doorbell marker | first command word = **`0x800`** (written last to trigger) |
| I2C bit delay | `yuan_ioctrl -T` µs (200 HD / 110 SD / 500 restart) |
| Firmware version | HD `01.11`, Rev.3 `11.11`, base `00.00` |
| BAR0 readiness poll | card reg `0x82000010` must be non-zero before `ep.ko` |
| Windows write helper | `e60MZ0380+0x28658c` (r8b=dev 0x9C, r9b=page, [rsp+0x20]=reg, [rsp+0x28]=val) |
| Windows read helper | `e60MZ0380+0x277884` (opcode 0x1a) |
| Windows mailbox send | `e60MZ0380+0x285074` (writes cmd words to BAR mailbox @ ctx+0x108, then 0x800) |

Full opcode table and command-struct layout: **Appendix A**.

---

## 8. If you still want the live trace (optional, confirms runtime values)

Static RE gives the *path, order, addresses, opcodes*. It does **not** give the runtime
`R0055` hex or the exact per-register MST3367 config dump. To get those on Windows:

**Easy (flow only):** run `capture-hd60-trace.bat` as admin, then in the Elgato app: 1080p60
into HDMI IN → start preview → unplug/replug HDMI once → close app. Produces
`hd60_bringup_trace.log`. Filter:
```
findstr /I "MST3367 i2c mcu_i2c HOTPLUG UPDATE.EDID SA7160 RESET R0055 DELAY" hd60_bringup_trace.log
```

**Exact register values (needs 2nd PC, WinDbg/KDNET):** breakpoint the write helper —
```
bp e60MZ0380+0x28658c ".printf \"WR dev=%02x pg=%x reg=%02x val=%02x\\n\", @r8b, @r9b, poi(@rsp+0x20), poi(@rsp+0x28); gc"
```
That dumps every MST3367 register write (dev/page/reg/value) during bring-up — the full init
sequence the Linux driver should replay.

**Fastest offline alternative:** disassemble the on-card `yuan_ioctrl` (ARM ELF in
`artifacts/MZ0380.HD.firmware.tar`) to see exactly how opcode 0x20 maps to the `/dev/i2c-0`
ioctl and how `page`/`reg` are packed — this is the actual code that talks to the receiver,
and it's not obfuscated.

---

## 9. Artifacts saved (in `artifacts/`, readable from Linux)

| File | What |
|---|---|
| `e60MZ0380.X64.SYS` | retail Windows driver v1.1.0.194 (for further disasm) |
| `e60MZ0380-strings.txt` | all printable strings extracted from the driver |
| `GameCaptureHD60Pro.inf` | INF: PCI IDs, crossbar defaults, reg config |
| `elgato-hd60pro-EDID.bin` | **256-byte EDID** the card presents (Appendix C) — push verbatim |
| `MZ0380.HD.firmware.tar` | decompressed on-card FS (Rev.1/2 HD) — has `yuan_ioctrl`, i2c .ko's, boot scripts |
| `MZ0380.SD.firmware.tar` | on-card FS (SD) — also has `debug_i2c` register tool |
| `MZ0381.HD.firmware.tar` | on-card FS (Rev.3) |
| `oncard-binaries/` | extracted `yuan_ioctrl`, `ep.ko`, i2c `.ko`s, `video_capture_mgr`, boot scripts |
| `oncard-config/` | `videocap.ini`, `ibpe.ini`, `nullsensor_1920x1080.cfg`, start scripts |
| `driver-analysis/` | `mst3367_clean.txt` / `_regs_full.txt` — raw register-write dumps (Appendix D) |
| `analysis-scripts/` | capstone/pyelftools scripts (ELF + PE disasm, EDID, xref, regtab) to reproduce/extend |

**Next step on the Linux side:** fix the I2C **address encoding** (send 8-bit `0x9C`, let the
card `>>1`) and add **page-select** (write `reg0=page`) — see §3d and Appendix A — then replay
the reset→EDID→HPD→detect order (§5/§6) and verify against `R0055`. Validate the address fix by
first reading the EDID EEPROM at 8-bit `0xA0`.

---

## Appendix A — Verified mailbox I2C protocol (disassembled, both sides)

Recovered by disassembling the on-card `yuan_ioctrl` (ARM ELF, fully symbolised) and the three
Windows mailbox helpers (x86-64 in `e60MZ0380.X64.SYS`). The two sides agree, so this is the
authoritative reference for the Linux driver's mailbox I2C.

### A.1 Opcode dispatch table (from `yuan_ioctrl` `main`, jump table @ `0x97d8`)

The daemon reads a **0x2c-byte (44) command struct** from `/sys/vpl_pciep/command`, then
switches on the opcode (struct offset 0). Opcodes 0x18–0x22:

| Opcode | Handler | On-card fn | Bus / dev | Purpose |
|---|---|---|---|---|
| `0x18` | 0x9804 | `spi_combo_cmd(rd)` | `/dev/gv7601` SPI | GV7601 (SDI) read |
| `0x19` | 0x9888 | `spi_combo_cmd(wr)` | SPI | GV7601 write |
| `0x1a` | 0xa2d0 | `i2c_read` | **bus0** `/dev/i2c-0` | **single-reg read** (MST3367) |
| `0x1b` | 0xa1f0 | `i2c_write` | **bus0** | **single-reg write** (MST3367, page-select) |
| `0x1c` | 0xa490 | `i2c_encrypt_read` | bus1 `/dev/i2c-1` | HDCP read (4 bytes) |
| `0x1d` | 0xa3b8 | `i2c_encrypt_write` | bus1 | HDCP write (5 bytes) |
| `0x1e` | 0xa084 | `i2c_read_s` | bus0 | multi-byte read |
| `0x1f` | 0x9f18 | `i2c_write_s` | bus0 | multi-byte write |
| `0x20` | 0x9d98 | `i2c_combo_cmd_x` | **bus0** | **arbitrary-length txn** (paged/EDID bulk) |
| `0x21` | 0x990c | `spi_combo_cmd` | SPI | GV7601 combo read (8 byte) |
| `0x22` | 0x9a94 | `spi_combo_cmd` | SPI | GV7601 combo write |

### A.2 Command struct (card-side, what `yuan_ioctrl` reads — 44 bytes)

```
offset  size  field                     notes
0x00    u32   opcode                    0x1a/0x1b/0x20/...
0x04    u8    i2c_addr (8-bit)          card computes addr7 = addr8 >> 1  (0x9C -> 0x4E)
0x05    u8    rw_flag                   combo (0x20): 0=write, 1=read
0x06    u16   length                    combo: payload byte count
0x08    ...   reg / payload             simple: reg @ +0x08, value @ +0x0c
                                        combo:  payload bytes start @ +0x08
```
- **Simple write (0x1b):** `addr=cmd[4]>>1`, `reg=cmd[8]`, `val=cmd[0xc]` → 2-byte I2C write
  `[reg,val]` to `addr` via `ioctl(I2C_RDWR)`.
- **Simple read (0x1a):** write `[reg]` then repeated-start read 1 byte; result returned in the
  struct, `pwrite` back to the mailbox.
- **Combo (0x20):** one I2C message of `length` bytes (dir = `rw_flag`) to `addr`; payload from
  `cmd[8..]`; chunked internally in 16-byte blocks, then `usleep(100)`.

### A.3 Host-side send (from Windows `SEND_COMMAND` @ `+0x285074`)

Command is a **word array**; `SEND_COMMAND(ctx, cmdbuf, count, flag)`:
```
word[0] = 0x800          ; header/doorbell marker
word[1] = opcode         ; 0x1a / 0x1b / 0x20 ...
word[2] = dev addr (8-bit, e.g. 0x9C)
word[3] = reg            ; (or page for a page-select write)
word[4] = value
```
Mechanism: BAR mailbox base is at driver `ctx+0x108`; it copies `word[1..count-1]` to mailbox
`+4,+8,+0xc,…`, then writes **`0x800` to mailbox `+0`** to trigger. If `flag==0` it polls a
completion counter (`ctx+0x207c`) up to 100× (source of `MZ0380 COMMAND TIMEOUT`). The card's
`ep.ko` surfaces the words at `/sys/vpl_pciep/command` (opcode at struct offset 0). Note the
`0x800` marker also appears card-side (`ep.ko` `command` handler) — it is the doorbell value.

### A.4 Paging (from Windows `WRITE_helper` @ `+0x28658c`)

Signature `WRITE(ctx, r8b=dev 0x9C, r9b=page, [rsp+0x20]=reg, [rsp+0x28]=val)`:
1. Compares requested `page` (r9b) to a cached page byte at `ctx+…+0x2090`.
2. If different, first sends **opcode 0x1b, dev 0x9C, reg `0x00`, val=`page`** (the page-select
   write) and updates the cache.
3. Then sends **opcode 0x1b, dev 0x9C, reg, val** for the actual register.

`READ_helper` (`+0x277884`) is identical but issues **opcode 0x1a** for the data phase (page
select still uses 0x1b). So: **register bank/page is selected by writing register `0x00`.**

### A.5 Minimal Linux sequence to read MST3367 reg 0x55

```
# addresses are 8-bit as placed in the command; card does >>1
mbox(op=0x1b, dev=0x9C, reg=0x00, val=<page_of_0x55>)   # select page
mbox(op=0x1a, dev=0x9C, reg=0x55) -> R0055               # read mode-detect
```
Prerequisites first: card booted (mailbox answers version), receiver reset (`SWReset` /
`SA7160_HARDWARE_I2C_RESET`), EDID written, HPD asserted (§5c) — else `R0055` reads no-signal.

### A.6 Tooling note
Disassembly done with Python `capstone` 5.0.7 + `pyelftools` (installed via pip this session);
scripts saved in the session scratchpad (`elfinfo.py`, `disasm*.py`, `handlers.py`, `pe.py`).
`yuan_ioctrl` and `ep.ko` (ARM) live in `artifacts/MZ0380.HD.firmware.tar`; re-disassemble the
combo/encrypt handlers there if you need byte-exact message framing beyond the above.

---

## Appendix B — Card control surface (how you drive the card)

Four layers of control, from highest (app-facing) to lowest (registers). The Linux driver
re-implements the ones Windows does over the mailbox; the rest are on-card and already handled
by the firmware apps.

### B.1 Windows KS / DirectShow property API (the documented control set)

The retail driver exposes these as KS custom properties (interface CLSID
`{D1E5209F-68FD-4529-BEE0-5E7A1F479222}`). Each has a Get and/or Set handler
(`On[GS]etCustom…Property`). Defaults live in the INF `[MZ0380.AddReg]` (see §6). Grouped:

**Encoder (H.264) — per stream A–H:**
`EncoderVideoFormat`, `EncoderVideoResolution`, `EncoderVideoBitRate`,
`EncoderVideoBitRateMode` (0=CBR/1=VBR/2=CQ), `EncoderVideoQuality`, `EncoderVideoGop`,
`EncoderVideoProfile`, `EncoderVideoLevel`, `EncoderVideoEntropy` (CAVLC/CABAC),
`EncoderVideoMinQP`, `EncoderVideoMaxQP`, `EncoderVideoKeyFrame` (force keyframe),
`EncoderVideoSkipFrameRate`, `EncoderVideoAvgFrameRate`, `EncoderVideoAspectRatio`,
`EncoderVideoCrop`, plus `AnalogVideoCompressionKeyframeRate/Quality` A–H and
`EncoderAudioFormat/BitRate/Frequency`.

**Video processing / format:**
`AnalogVideoNativeColorSpace` (0=4:2:2/1=4:4:4/2=auto), `AnalogVideoNativeColorDeep`
(0=8b/1=10b/2=auto), `AnalogVideoColorRange` / `RGBColorConvertColorRange`,
`AnalogVideoHVSyncDelay`, `AnalogVideoScaleOutput`, `AnalogVideoMaxChannelSize`,
`AnalogVideoMultiChannelsSupport`, `AnalogVideo720Output`, `CustomAnalogVideoDeinterlaceType`
(0=BOB/1=WEAVE/2=HYBRID/3=BLEND), `ScaleType` (0=STRETCH/1=FIT), plus ProcAmp
(brightness/contrast/hue/saturation/sharpness/gamma/white-balance/gain/backlight).

**Preview:** `PreviewVideoResolution`, `PreviewAudioSampleFrequency`.

**Audio / crossbar (input select):** `AnalogAudioVolume` / `VolumeExtra` /
`FrameOutputSize`, `AnalogCrossbarVideoInputProperty` (0=HDMI/1=DVI-D/2=COMPONENT/3=DVI-A/4=SDI),
`AnalogCrossbarAudioInputProperty` (0=embedded/1=line-in), `AnalogVideoDecoderStandard`
(1=NTSC/16=PAL).

**Debug / raw access (useful for RE):** `CustomCommandProperty` (send a raw mailbox command),
`DebugEepromProperty` / `DebugEepromPidVid` / `DebugEepromProductName` (board EEPROM R/W),
`DebugBoardMemroyProperty` (read card memory windows 0–4), `DirectMemoryModeProperty`,
`DebugEdidAccessEnableProperty` (enable EDID R/W), `DebugMcuAccessEnableProperty` (enable MCU
access), `PrintDebugInfoProperty`, `HardwareVersionProperty`, `DeviceTopologyProperty`.
→ `CustomCommandProperty` + `DebugEdidAccessEnable` + `DebugMcuAccessEnable` are the hooks the
Windows app itself uses to poke I2C/EDID — the Linux equivalent is sending mailbox opcodes
directly (Appendix A).

### B.2 On-card mailbox commands (high-level, beyond the I2C proxy)

Same mailbox as Appendix A (`/sys/vpl_pciep/command`, word[1]=opcode), but these opcodes are
serviced by the video/encoder apps, not `yuan_ioctrl`. Named commands seen in `ep.ko` /
`video_capture_mgr`:

| Command | Purpose |
|---|---|
| `GET_FIRMWARE_VERSION` | read FW/BASE version (the handshake Linux already does) |
| `BEGIN/END_FIRMWARE_DOWNLOAD`, `BEGIN/END_BASE_FIRMWARE_DOWNLOAD` | push firmware image to flash |
| `SET_VIC` / `SET_VIC_PARAMS` | **configure a capture channel** (see param list below) |
| `STOP_STREAMING(fw)` | stop a channel |
| `SET_AIC` (`SET_AIC INT MODE`) | configure audio input capture |
| `LINUX_PREVIEW_UV`, `PREVIEW_BUF_EX` | preview buffer / chroma layout control |

**`SET_VIC` parameter block** (the core streaming-config command; fields from `video_capture_mgr`):
`fw, fps, resolution(w×h), interlace, m, color_info[4], x_start, y_start,
input_frame_width, input_frame_height, bitstream_num, osd_enabled, osd_size, is_nosg,
vanc_lines, fast_kill, is_slave, nosg_back_color, nosg_y/u/v, flip, mirror`.
(`nosg` = "no signal" logo handling; `is_slave` = pass-through/clone; `bitstream_num` selects
which encoder stream A–H.)

### B.3 `ep.ko` sysfs interface — `/sys/vpl_pciep/*`

Attributes the card exposes (host writes/reads these via PCIe BAR; several are driven by
`yuan_ioctrl`'s `echo N > …` at startup):

| sysfs attr | role |
|---|---|
| `command` | the mailbox command channel (Appendix A) |
| `dency`, `qency`, `aency`, `hency`, `wency` | stream enable/notify queues (decode/queue/audio/host/write) |
| `hready` / host-ready | host-ready handshake |
| `status` | device status |
| `epint`, `epint_1080p` | endpoint interrupt config |
| `pre_uv` | preview UV/chroma mode |
| `audio_ctrl` | audio control |
| `osd_timer0/1` | OSD timers |
| `logo` | upload no-signal logo bitmap |
| `fw` | firmware image upload |
| `channel_done` (class attr) | per-channel completion |
| `encode_status0 … encode_status15` | 16 encoder-channel status registers |

There is also a **`livectrl` char device** (class `vpl_pciep`) with ioctls
`LIVECTRL_IOCTL_GET_INFO` / `LIVECTRL_IOCTL_SET_INFO` — an alternate control path used by the
host userspace/app.

### B.4 On-card app CLI flags (how the firmware starts each subsystem)

From `rc.local` / `yuan_start_process.sh` and each binary's getopt:

| App | Flags | Meaning |
|---|---|---|
| `yuan_ioctrl` | `-D -T <us> -P <us> -L -E -h` | `-D` daemon, `-T` I2C bit delay µs (200 HD/110 SD/500 restart), `-P` extra delay, `-E` disable encrypt/ENCY, `-L` … |
| `video_capture_mgr` | `-D -S <0-3> -N -F <n> -P <n> -h` | `-S` I2S number, `-N` audio disable, `-F` frames/period, `-P` periods/buffer |
| `audio_capture_mgr` | `-D -S <0-3> -F -P -B` | audio I2S/period/buffer config |
| `tinyvenc5/7/8` | `-D -c <cfg>` | H.264 encoder daemon, config = `nullsensor_1920x1080.cfg` |
| `capture_audio_8ch` | `-D -d <dev> -R <rate> -F -B` | 8-ch audio capture (48000 Hz, 256 frames, 4 buffers) |

### B.5 On-card config files (image pipeline & encoder tuning)

These live in the firmware tar and tune the ISP/encoder — the Linux driver doesn't need them
to *talk* to the card, but they document what's adjustable:
- **`videocap.ini`** — ISP: frequency (50/60 Hz), exposure (auto/backlight/manual, shutter/gain
  bounds, target luminance), white balance (auto/simple/manual, R/B gain), 3×3 color-correction
  matrix, gamma table path, brightness/contrast/saturation, tone-mapping, contrast enhancement,
  black clamp, de-impulse, photometric LDC.
- **`ibpe.ini`** — DeNoise (2D/3D modes, strength), EdgeEnhancement, DeInterlace
  (WEAVE/BLEND/adaptive), LDC (lens-distortion coefficients).
- **`nullsensor_1920x1080.cfg`** — VIC capture format: **input format 6 = BT1120p**, 1920×1080,
  output YUV420, field mode, flip/mirror, plus a large AE/AWB/AF/CFA block (mostly inert for a
  video-in card vs a camera sensor).
- **H.264 encoder** (via `tinyvenc`, matching the KS Encoder props): profile, GOP, bitrate,
  fixed/min/init/max QP, skip-FPS / avg-FPS / default-FPS rate control, VANC lines.

**Bottom line for Linux control:** to *capture*, the driver needs (1) the mailbox up, (2) I2C
per Appendix A to bring up the MST3367 + EDID/HPD, then (3) a `SET_VIC` command (B.2) with the
detected resolution/fps to start a channel, and optionally the encoder params if you want the
on-card H.264 stream rather than raw frames. Everything in B.1 is a *convenience* layer over
those same mailbox commands.

---

## Appendix C — The EDID the card presents (extracted, exact)

Recovered from the Windows driver `.data` at VA `0x350a20` (file `0x34f620`): a **valid
256-byte EDID** (base + 1 CEA extension, both checksums = 0). This is what `UpdateEDID` writes
into the MST3367 so the HDMI source will output. Saved as
**`artifacts/elgato-hd60pro-EDID.bin`** — the Linux driver can push these bytes verbatim.

Decoded highlights:
- Mfr **`MST`**, product **`0x3367`**, EDID 1.3, **digital/HDMI**, 60×34 cm, monitor name
  **`SC530-N1`**.
- Preferred DTD **1920×1080 @ 148.50 MHz** (1080p60); second DTD 1360×768 @ 85.50 MHz.
- CEA-861 extension (`02 03 …`) advertises the supported VICs / audio (SCDB), enabling 1080p60,
  1080i, 720p, etc. so sources negotiate those modes.

```
Base block (128 B):
00 ff ff ff ff ff ff 00 36 74 67 33 01 00 00 00
06 16 01 03 80 3c 22 78 0a 8e 05 ad 4f 33 b0 26
0d 50 54 2f ce 80 81 00 81 80 81 40 81 c0 95 00
31 59 45 59 61 59 02 3a 80 18 71 38 2d 40 58 2c
45 00 96 e4 10 00 00 1e 66 21 50 b0 51 00 1b 30
40 70 36 00 10 09 00 00 00 1e 00 00 00 fc 00 53
43 35 33 30 2d 4e 31 0a 20 20 20 20 00 00 00 fd
00 31 56 1e 51 08 00 0a 20 20 20 20 20 20 01 bf
CEA extension (128 B):
02 03 27 f4 52 85 04 03 02 07 16 01 06 11 12 15
13 94 20 21 22 90 9f 23 09 07 01 67 03 0c 00 10
00 38 2d 83 01 00 00 8c 0a a0 14 51 f0 16 00 26
7c 43 00 c4 8e 21 00 00 98 8c 0a d0 8a 20 e0 2d
10 10 3e 96 00 c4 8e 21 00 00 19 01 1d 00 72 51
d0 1e 20 6e 28 55 00 c4 8e 21 00 00 1f 01 1d 80
18 71 1c 16 20 58 2c 25 00 c4 8e 21 00 00 9e 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 fb
```

---

## Appendix D — MST3367 register map & bring-up (from driver disassembly)

Recovered by finding all 283 callers of the WRITE helper and 179 of the READ helper in
`e60MZ0380.X64.SYS` and tracking the `dev/page/reg/val` set up before each. Full raw dumps:
`artifacts/driver-analysis/mst3367_clean.txt` (state-reset, reliable) and `…_regs_full.txt`.

> **Important — the bring-up is algorithmic, not a flat table.** The driver does
> read-modify-write and **mode-adaptive** configuration: it reads the mode-detect status block,
> then programs timing based on the detected mode. So you cannot just blast a fixed register
> list; you must implement the read→decide→write logic. Many `val`s are computed at runtime and
> therefore not present in a static dump (shown as `?`). The **static** writes below are the
> ones with immediate values — safe to treat as fixed init; the rest are conditional.

**Device / paging:** all access is dev **`0x9C`** (8-bit), register bank selected by writing
**reg `0x00` = page** (Appendix A.4). Pages actually used: **`0x00` (default), `0x01`, `0x02`,
`0x05`, `0x80`, `0xc6`** (the larger "pages" in the raw dump are tracker false-positives).

**Register roles identified (page 0 / default unless noted):**
| reg(s) | role (inferred from sequence + strings) |
|---|---|
| `0x1e`, `0x1f` | soft-reset / clock pair — toggled at start of bring-up |
| `0xe0` | block enable/reset (writes `0x80` then `0x40` then `0x00`) |
| `0x04` | control / input select |
| `0x10`, `0x11` | timing/PLL (`0x11`=`0x2d`, later `0x20`) |
| `0x20`, `0x21`, `0x24` | HDMI control (`0x24`=`0xc0`, `0x20`=`0xc0`) |
| `0x0e` | status/control, repeatedly read-modify-written |
| `0x14`, `0x15` | H/V measurement counters (read) |
| **`0x54`–`0x5f`** | **mode-detect status block; `0x55` = `R0055` mode-detect** (all read) |
| `0x57`–`0x5f` | sync/geometry measurement registers (read during detect) |

**Static (fixed) writes recovered — replayable init:**
```
page 0x02: reg 0x02=0xf5  reg 0x06=0x08  reg 0x07=0xf4 then 0x04  reg 0x13=0x08
           reg 0x17=0xc0  reg 0x19=0xff  reg 0x1a=0xff  reg 0x1b=0xfc
           reg 0x1c=0x1a  reg 0x22=0x26  reg 0x41=0x6f
page 0x01: reg 0xb8=0x10 then 0x00
page 0x05: reg 0xe0=0x40
```
(Interpretation: page 2 is the HDMI receiver's main config bank — sync/AVI/packet handling;
page 1 and page 5 are secondary. These are the values Windows programs before/around detect.)

**Mode-detect read sequence (the `R0055` logic):** during `VSTATE_ModeDetecting` the driver
reads `0x54,0x55(R0055),0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f`, then compares against the
`SearchDisplayModeTable` / `GetTotalDisplayModeIndex` timing table (VTotal/VFreq/polarity) to
pick the resolution. So `R0055` alone doesn't give the mode — it's a lock/status byte; the H/V
totals from `0x14/0x15/0x57…0x5f` feed the mode table.

**What's still runtime-only from *this* extraction:** the exact conditional `val`s. **But you
no longer need to recover them from Windows** — a complete GPL MST3367 driver already has them.
See **Appendix G**. (Legacy options, no longer necessary: a KDNET breakpoint on
`e60MZ0380+0x28658c`, or a Ghidra decompile around `0x14024bf00–0x14024e900`.)

> **Note:** my page-tracking mislabeled some banks (e.g. `0x41=0x6f`, `0x02=0xf5`, the `0x07`
> and `0xb8` reset pairs). Appendix G has the correct banks from the GPL driver — trust those.

---

## Appendix E — PCIe endpoint / interrupt / DMA architecture (`ep.ko`)

The card side of the PCIe link (host↔card transport the mailbox rides on). SoC = MStar Mozart,
kernel `2.6.28.9-Mozart-8G`, ARMv5.

- **`mozart_module_init`** maps the BARs and sets up the link; logs
  `[init]bar 0: … Inbound mem0 start 0x%08x, Limit 0x%08x` and `bar 1: …`. **BAR0** = the
  inbound memory window (host writes land here — this is where the mailbox `command` region and
  the DMA frame buffers live); **BAR1** = secondary/register window.
- **`pcie_set_outbound`** (exported symbol) programs the card→host outbound window (how the card
  DMAs captured frames back into host memory).
- **Interrupts:** `request_irq` + **MSI** (`msi_enable`, `msi.constprop`, `pending_irqs`,
  `pirq_base`); the ISR `pciep_isr` / `pciep_isr_clrint` services them. `store_channel_done` and
  the `encode_status0..15` sysfs attrs report per-channel frame/encode completion — this is how
  the host learns a frame is ready.
- **DMA:** `dma_alloc_coherent` / `dma_free_coherent` for the frame/bitstream buffers.
- **Host-side counterpart (Windows):** driver ctx holds the BAR mailbox pointer at `+0x108`
  (Appendix A.3), the "current MST3367 page" cache at `+0x2090`, and a completion counter at
  `+0x207c`. The Linux driver already has its own BAR mapping working (version reads succeed),
  so this appendix is mainly to confirm: **frames arrive by card→host DMA into a BAR0/outbound
  window, signalled by MSI; poll `encode_status`/`channel_done` semantics for completion.**

---

## Appendix F — Completeness statement (what is and isn't in these docs)

**Fully recovered, exact, no Windows needed:**
- Driver/firmware identity, PCI IDs, board revs, SoC (§1, §7).
- Card boot model + on-card boot scripts (§5, §5b, artifacts).
- **Mailbox I2C protocol, both sides, byte-exact** (Appendix A).
- **The EDID blob** (Appendix C, `elgato-hd60pro-EDID.bin`).
- Full control surface: KS property list, mailbox command names + `SET_VIC` fields, `ep.ko`
  sysfs, app CLI flags, config files (Appendix B).
- MST3367 register **map**, pages, roles, and the **static init writes** (Appendix D).
- PCIe/IRQ/DMA architecture (Appendix E).

- **MST3367 exact register values, init, mode-detect, timing table** — now available from a
  **GPL driver** RE'd from the same Windows driver (Appendix G). No Windows needed after all.

**Still only structure/names, not byte-exact (minor, doesn't block bring-up):**
- The exact `SET_VIC`/`SET_AIC` command **word offsets** (names + fields are in Appendix B;
  byte layout needs disassembly of `video_capture_mgr`/`tinyvenc`, both in `artifacts/`).

**Nothing left requires returning to Windows.** Path: push the EDID (Appendix C), drive I2C via
opcode 0x1a/0x1b/0x20 (8-bit addr, bank-select reg 0x00), run the MST3367 init + mode-detect
from Appendix G, then `SET_VIC` (Appendix B).

---

## Appendix G — Complete MST3367 register logic (from a GPL driver, RE'd from the same Windows driver)

**This eliminates the last Windows dependency.** An existing GPL-v2 driver reverse-engineered
the MST3367 from the *same* Elgato/Yuan MZ0380 Windows driver, for the Startech **USB2HDCAPM**
(also Vatics Mozart 395s + MST3367). It cross-validates our disassembly exactly and supplies the
conditional register values we couldn't get statically. **Reuse it.**

- Repo: **https://github.com/stoth68000/hdcapm** — files `mst3367-drv.c`, `mst3367-common.h`,
  `mst3367-drv.h` (also a linux-media patch by Michael Grzeschik, 2018). GPL-v2.
- Local copy of the extracted register logic: **`artifacts/mst3367-reference-from-gpl-driver.md`**.

**Cross-validation (their RE vs our disassembly — independent, agree):**
| Fact | GPL driver | Our disassembly |
|---|---|---|
| I2C addr | `0x9c` → `>>1` → `0x4e` | `0x9c`, card does `>>1` ✓ |
| Bank select | write reg `0x00` = bank | write reg `0x00` = page ✓ |
| Banks | `BANK0..3` (0..3) | pages 0,1,2 seen ✓ |
| HDCP reset | `0xb8`=`0x10` then `0x00` | `0xb8`=`0x10`/`0x00` ✓ |
| HDMI reset | BANK2 `0x07`=`0xf4`/`0x04` | `0x07`=`0xf4`/`0x04` ✓ |
| init write | BANK2 `0x02`=`0xf5`, BANK0 `0x41`=`0x6f` | `0x02`=`0xf5`, `0x41`=`0x6f` ✓ |
| detect reg | BANK0 `0x55 & 0x3c` = signal | `0x55` = `R0055` ✓ |

**Mode detect (BANK0 unless noted):** signal present when `rd(0x55) & 0x3c`. Then:
`htotal = 0x6a<<8|0x6b`, `vtotal = 0x5b<<8|0x5c`, `hperiod = 1600000/(0x57<<8|0x58)`,
`vperiod = 1250000/(0x59<<8|0x5a)`, `interlaced = 0x5f & 0x02`, hactive = BANK2 `0x29<<8|0x28`.
Match `htotal/vtotal/hperiod/vperiod` to the timing table (below) with tolerance windows.

**Init sequence** (`mst3367_init_setup`): HPD off → `BANK0 0x41=0x6f, 0xb8=0x00` →
`BANK1 0x0f=0x02, 0x16=0x30, 0x24=0x40(HDCP)` → `BANK0 0xb0=0x14, 0xb1=0xe0` →
`BANK2 0x01=0x61, 0x02=0xf5` → `BANK0 0x51=0x89` → HPD/link on → `BANK0 0xB0=0x20 (YUV422 8-bit)`
→ `MST3367_HDMI_INIT()`.

**HPD:** BANK0 reg `0xB7` bit1 (set bit1 = HPD off; clear = link on), then `msleep(20)`.
`enum hpt_e { RX_TMDS_HPD_OFF=0, A_HPD_ON=0x01, A_LINK_ON=0x02, B_HPD_ON=0x10, B_LINK_ON=0x20 }`.

**Timing table** (`mst3367_video_standards[]`; cols: std, htot min/max, vtot min/max, hper
min/max, vper min/max, interlaced, fps, fpsx100):
```
720x480p59.94   845  865   520  525  310 320  595 605  0 60 5994
1280x720p30    2300 2500   745  755  215 235  290 310  0 30 3000
1280x720p50    2965 2985   745  755  360 380  480 520  0 50 5000
1280x720p60    2470 2480   745  755  445 455  595 605  0 60 6000
1280x720p60    1645 1655   745  755  445 455  595 605  0 60 6000
1920x1080p24   4080 4105  1120 1130  260 280  230 250  0 24 2400
1920x1080p25   3950 3970  1120 1130  270 290  240 254  0 25 2500
1920x1080p30   2295 3305  1120 1130  330 345  290 310  0 30 3000
1920x1080p50   3950 3970  1120 1130  550 570  480 520  0 25 5000
1920x1080p60   3290 3310  1120 1130  665 685  595 605  0 30 6000
```
(A few fields look like upstream typos — 1080p30 htot_max `3305`→prob `3295`; 1080p50 fps
`25`→`50`; 1080p60 fps `30`→`60`. Also no 1080i/576i rows — extend from your own detect reads.)

**Architecture:** `mst3367-drv.c` is a standard V4L2 **i2c sub-device** using `i2c_transfer`.
To reuse it on the HD60 Pro (PCIe), register an `i2c_adapter` whose `master_xfer` tunnels each
transaction through the card mailbox (opcode 0x1b/0x1a/0x20, dev 0x9c 8-bit, Appendix A). Then
the entire chip driver above runs unmodified — you only write the PCIe/mailbox transport glue.

---

## Appendix H — Observed runtime status (live, this machine)

Captured from the Elgato app's hardware/debug panel on this PC (no HDMI source connected at the
time). This is live device state — useful as a **validation reference** for the Linux driver.

```
DRIVER VERSION   : 1.1.0.195.0, FIRMWARE : 1.11.1.11, 2021.09.28
CARD INFO        : SN00061CFA, BN00000004, MN12AB0380
STREAM INFO      : PV00054063, PA00513457, EV00000000
SIGNAL STATUS    : NO SIGNAL
SIGNAL COLORIMETRY : UNKNOWN
SIGNAL COLORRANGE  : UNKNOWN
SIGNAL DEBUG INFO  : 0x00000000
```

**Decode:**
- **DRIVER 1.1.0.195.0** — the *loaded* driver (`System32\drivers`), newer than the `1.1.0.194`
  installer copy I disassembled. Byte offsets in Appendix A/D (e.g. `+0x28658c`) are 194-specific;
  the 195 image is in `artifacts/e60MZ0380.X64.v195.SYS` if you need its offsets. Protocol,
  register logic, and the EDID are identical between the two — verified.
- **FIRMWARE 1.11.1.11 (2021.09.28)** — the card's running firmware (matches `MZ0380.FW.TXT`
  major.minor `01.11`; the `.1.11` is base/app sub-versions). This is what the mailbox
  `GET_FIRMWARE_VERSION` returns — your Linux version-check should expect `1.11.x.x`.
- **CARD INFO** — `SN00061CFA` = PCI subsystem `0x00061CFA` (Rev.1+Ryzen board); `BN00000004`
  = board number/rev 4; `MN12AB0380` = model = PCI `VEN_12AB DEV_0380`.
- **STREAM INFO** — running frame/sample counters from the KS props (§B.1):
  `PV` = Preview Video frames (`CustomPreviewVideoFrameCountsProperty`),
  `PA` = Preview Audio frames, `EV` = Encoder Video frames (`0` = not encoding here).
- **SIGNAL STATUS / COLORIMETRY / COLORRANGE** — decoded from the MST3367 detect + the source's
  AVI infoframe (colorimetry = BT.601/709/2020; colorrange = limited/full). `UNKNOWN` because no
  source. Map to the driver's `NativeColorSpace` / `ColorRange` properties.
- **SIGNAL DEBUG INFO `0x00000000`** — the **live detect-status readout**; `0` = no signal.
  This is effectively the MST3367 detect state (the `0x55 & 0x3c` / timing block from Appendix G)
  surfaced as a 32-bit word. **With a source connected it goes non-zero.**

**→ Use this panel as your ground-truth validation (replaces the DebugView trace need):**
plug in each source you care about (1080p60, 1080p50, 720p60, …) and record `SIGNAL STATUS`,
`SIGNAL COLORIMETRY`, `SIGNAL COLORRANGE`, and `SIGNAL DEBUG INFO`. Those are exactly what your
Linux MST3367 mode-detect (Appendix G: reg `0x55` + `htotal/vtotal/hperiod/vperiod`) must
reproduce. If your Linux `0x55 & 0x3c` and computed timings match a row in the timing table for
the same source the panel identifies, detection is correct. `SIGNAL DEBUG INFO` non-zero is the
quickest "the receiver sees the signal" check.
