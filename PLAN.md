# MZ0380 Bring-Up Plan

## Current Status
- A new probe-safe `mz0380` module exists alongside `sc0710` in [Makefile](/home/wolffyx/Projects/sc0710/Makefile).
- The new source set is:
  - [mz0380.h](/home/wolffyx/Projects/sc0710/mz0380.h)
  - [mz0380-reg.h](/home/wolffyx/Projects/sc0710/mz0380-reg.h)
  - [mz0380-cards.c](/home/wolffyx/Projects/sc0710/mz0380-cards.c)
  - [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c)
- The module binds only PCI ID `12ab:0380` with subsystem `1cfa:0006`.
- The detected board is reported as `Elgato Game Capture HD60 Pro`.
- The current safe baseline is:
  - PCI enable succeeds.
  - BAR0 and BAR5 are requested, validated, and mapped.
  - `/proc/mz0380` and `/proc/mz0380-state` expose metadata plus carefully limited diagnostics.
  - PCI bus mastering is disabled by default in probe-safe mode.
  - No DMA, IRQ handling, firmware upload, I2C transactions, or V4L2 capture is started yet.

## Confirmed Hardware Facts
- Local card: `12ab:0380`
- Local subsystem: `1cfa:0006`
- Expected BAR layout on this host:
  - BAR0: `0x100000`
  - BAR5: `0x1000`
- Safe PCI findings:
  - PCI command register is `0x0002` in probe-safe mode, so memory decode is enabled and bus mastering is disabled.
  - PCI status is `0x0010`, matching a normal capability list.
  - PCI capabilities:
    - PM at `0x40`
    - MSI at `0x50`
    - PCIe at `0x70`
  - MSI is 64-bit capable and currently disabled.
  - The endpoint is PCIe capability version `2`, type `0` (endpoint).
  - Link capability and negotiated status both indicate `Gen1 x1`.
  - Linux routes the device to IRQ `40`; `irq line = ff` is normal for PCIe.
- MMIO facts observed repeatedly:
  - `mmio[0x0000] = 0xffffffff`
  - `mmio[0x0004] = 0x0000000a`
  - `cfg[0x0000] = 0x11000001`
  - `cfg[0x0004] = 0x00010001`
- Safety finding:
  - Enabling PCI bus mastering too early triggered an `AMD-Vi IO_PAGE_FAULT` at `0x90000000`.
  - Keeping bus mastering disabled by default removes that fault.

## SDK Findings
- Real SDK path used for reference: `/home/wolffyx/Downloads/test/SDK 1.1.0.202.0`
- The schedule-recording sample explicitly recognizes `"MZ0380 PCI"`:
  - [SetupDialog.cpp](/home/wolffyx/Downloads/test/SDK%201.1.0.202.0/RELEASES%201.1.0.202.0/AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/SetupDialog.cpp): line containing `if ( strDeviceName == "MZ0380 PCI" )`
- The same sample routes MZ0380 through the SC540 application flow:
  - [ScheduleRecordingDlg.cpp](/home/wolffyx/Downloads/test/SDK%201.1.0.202.0/RELEASES%201.1.0.202.0/AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/ScheduleRecordingDlg.cpp)
  - [FileRenderer.cpp](/home/wolffyx/Downloads/test/SDK%201.1.0.202.0/RELEASES%201.1.0.202.0/AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/FileRenderer.cpp)
- The MZ0380 resource profile is explicitly SC540-shaped:
  - [MZ0380.txt](/home/wolffyx/Downloads/test/SDK%201.1.0.202.0/RELEASES%201.1.0.202.0/AMESDK/SAMPLES/OTHERS/SCHEDULE.RECORDING/ScheduleRecording/res/MZ0380.txt)
  - It defines:
    - `SC540.NTSC/PAL.RESOLUTION`
    - `SC540.NTSC/PAL.FRAMERATE`
    - `SC540.NTSC/PAL.RECORDMODE`
    - `SC540.NTSC/PAL.BITRATE`
    - `SC540.VIDEO.BFRAME`
    - `SC540.VIDEO.INPUT`
- The available input list in that profile is:
  - `HDMI`
  - `DVI-D`
  - `COMPONENTS ( YCBCR )`
  - `DVI-A ( RGB )`
  - `SDI`
- The SDK also contains dedicated SC540 product samples:
  - [SC540.PRODUCTS](/home/wolffyx/Downloads/test/SDK%201.1.0.202.0/RELEASES%201.1.0.202.0/AMESDK/SAMPLES/PRODUCTS/SC540.PRODUCTS)

## Important Inference
- The combined evidence now points away from a naive raw-frame capture model:
  - `MZ0380 PCI` is treated by the SDK as an SC540-style device.
  - SC540 resources are built around record modes, bitrates, B-frames, and input selection.
  - The physical PCIe link is only `Gen1 x1`.
- Because `Gen1 x1` offers limited usable payload bandwidth, a raw `1920x1080@60` packed `YUYV/UYVY 4:2:2` DMA path is unlikely to fit with real PCIe overhead.
- Treat this as an inference from the PCIe and SDK evidence, not final proof:
  - The first usable transport may be compressed, semi-planar, tiled, or otherwise vendor-specific.
  - Do not assume the eventual stream exposed by the hardware is raw packed 4:2:2 video.

## Implemented Code Landmarks
- PCI/BAR setup: [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c#L48)
- Board autodetect and BAR validation: [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c#L94)
- PCI config and capability dumps: [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c#L172)
- Probe-safe bus-master gating: [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c#L330)
- PCI table: [mz0380-core.c](/home/wolffyx/Projects/sc0710/mz0380-core.c#L398)
- HD60 Pro board entry: [mz0380-cards.c](/home/wolffyx/Projects/sc0710/mz0380-cards.c#L7)

## Build And Manual Validation
- Targeted module build:
```bash
make -C /lib/modules/$(uname -r)/build M="$PWD" CC=clang LD=ld.lld mz0380.ko
```
- Expected alias check:
```bash
modinfo ./mz0380.ko
```
- Safe load/unload sequence:
```bash
sudo insmod ./mz0380.ko procfs_verbosity=2
cat /proc/mz0380
cat /proc/mz0380-state
sudo dmesg | grep -E 'mz0380|AMD-Vi|IOMMU'
sudo rmmod mz0380
```

## Current Blockers
- Full repo build is still blocked by a pre-existing `sc0710` kernel-compat issue on kernel `6.19.10-1-cachyos`.
- The current failure is missing header `media/videobuf-vmalloc.h`.
- `mz0380.ko` remains the isolated working path.
- A second blocker is semantic, not mechanical:
  - the driver does not yet know the control protocol,
  - the DMA/IOMMU setup,
  - or the actual transport format.

## Immediate Next Step
- Keep the current probe-safe baseline stable.
- Do not re-enable bus mastering until:
  - a DMA buffer strategy exists,
  - IOMMU-visible addresses are programmed intentionally,
  - and the device-side DMA enable path is understood.
- Continue gathering information from:
  - PCI config space
  - the SC540/MZ0380 SDK path
  - only individually vetted MMIO reads

## Next Reverse-Engineering Goals
1. Identify whether BAR5 is the primary MCU/control/mailbox window.
2. Determine whether any additional MMIO reads can be made safely without hangs.
3. Locate likely firmware command, reset, or handshake registers.
4. Map the SC540-style control surface from the SDK to probable hardware behavior.
5. Determine whether the eventual data path is compressed, semi-planar, or otherwise non-raw.
6. Delay interrupt and DMA work until a control path exists and bus mastering can be re-enabled intentionally.

## Planned Bring-Up Stages
### Stage 1: Probe-Safe Baseline
- Keep existing behavior: bind, map, inspect, unload cleanly.
- Add only read-only diagnostics that do not trigger MMIO instability or IOMMU faults.
- Exit criteria:
  - Repeated load/unload is stable.
  - BAR mapping and proc dumps are consistent.
  - Bus mastering stays disabled by default.
  - No `AMD-Vi` or IOMMU faults are observed during probe.
  - No kernel warnings, resource leaks, or hangs are observed.

### Stage 2: Read-Only Hardware Identification
- Expand only carefully chosen diagnostics.
- Prefer PCI config space and individually vetted MMIO offsets over wide BAR walks.
- Correlate safe reads with HDMI connected vs disconnected states if possible.
- Exit criteria:
  - At least one reliable signal-presence or mode-related register set is identified.
  - Candidate firmware and command registers are narrowed down enough to avoid blind writes.

### Stage 3: Safe Control-Path Discovery
- Investigate command, mailbox, reset, and I2C/EDID paths with minimal, controlled writes only after read-only mapping is understood.
- Use the SDK’s SC540/MZ0380 behavior as a hint for expected control features.
- Exit criteria:
  - A defensible command/control sequence exists for basic hardware initialization.
  - Any required firmware handshake is identified or ruled out.

### Stage 4: Data-Path Identification
- Add the minimum internal structures needed to reason about the streaming path.
- Do not expose a `/dev/video*` node until transport assumptions are understood.
- Identify:
  - capture engine control registers
  - DMA ring or descriptor layout
  - frame status/interrupt sources
  - actual transport format expectations
- Exit criteria:
  - Hardware can be initialized without destabilizing the system.
  - A plausible data path is mapped far enough to begin controlled streaming work.

### Stage 5: First Capture Milestone
- Register one Linux-facing capture path for HD60 Pro.
- Treat `1280x720@60` and `1920x1080@60` as mode goals, not proof of raw transport.
- Audio remains out of scope.
- Exit criteria:
  - Frames or stream units can be captured repeatedly without kernel faults.
  - Start/stop streaming is reliable.

## Guardrails
- Keep `mz0380` isolated from `sc0710` register, DMA, and board assumptions unless equivalence is proven.
- Preserve probe-safe behavior as the default until control semantics are understood.
- Keep PCI bus mastering disabled by default until DMA setup is explicit and verified.
- Treat Windows driver strings and SDK samples as family hints and behavioral references, not register truth.
- Avoid broad PCI ID matching until more subsystem variants are validated.
- Prefer explicit failure over partially initialized capture paths.

## Data Needed For Future Sessions
- `/proc/mz0380-state`
- `sudo dmesg | grep -E 'mz0380|AMD-Vi|IOMMU'`
- Targeted SDK snippets for MZ0380/SC540 behavior
- Any additional safe MMIO observations from carefully chosen offsets

## Out Of Scope For Now
- ALSA or HDMI audio capture
- Broad MZ0380-family PCI ID support
- HDR, 4K, multi-input, or multi-board support
- Refactoring `sc0710` for kernel `6.19` compatibility as part of the immediate `mz0380` task
