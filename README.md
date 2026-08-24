# mz0380 — Linux driver for Elgato Game Capture HD60 Pro

PCIe capture card driver for the Elgato Game Capture **HD60 Pro** family
(YUAN MZ0380 chipset, PCI `12ab:0380` / `12ab:0381`). The card has an
onboard ARM SoC that runs its own embedded Linux and performs HDMI
receive, signal detect (MST3367), and H.264 encoding (tinyvenc). The host
driver uploads firmware, configures the receiver and
encoder, and exposes the encoded result as a V4L2 H.264 capture device.
An ALSA scaffold exists, but PCM DMA is not implemented and must remain
disabled for normal use.

> Legacy note: this repo started as a reverse-engineering project for
> the Elgato 4K60 Pro Mk.2 (`sc0710-*.c`). That code is preserved but
> no longer builds on modern kernels and is excluded from the default
> target. Pass `MZ0380_LEGACY_SC0710=1` to `make` to attempt it.

## Supported cards

| Subsys           | Variant                       | Firmware blob       |
|------------------|-------------------------------|---------------------|
| `1cfa:0003`      | HD60 Pro Rev. 1               | `MZ0380.HD.HEX`     |
| `1cfa:0005`      | HD60 Pro Rev. 2 (unreleased)  | `MZ0380.HD.HEX`     |
| `1cfa:0006`      | HD60 Pro Rev. 1 + Ryzen fix   | `MZ0380.HD.HEX`     |
| `1cfa:0010`      | HD60 Pro Rev. 3 (`DEV_0381`)  | `MZ0381.HD.HEX`     |
| `12ab:05cf`      | HD60 Pro Prototype            | `MZ0380.HD.HEX`     |

## Firmware

The card needs its onboard ARM firmware blob uploaded at probe.

The blob is **not redistributable** in this repo. Extract it from the
Windows installer:

```bash
7z x Game_Capture_HD60_Pro_*.exe -o/tmp/hd60pro_extract/
sudo install -d /lib/firmware/mz0380
sudo cp /tmp/hd60pro_extract/MZ0380.HD.HEX /lib/firmware/mz0380/
# Rev.3 owners also:
sudo cp /tmp/hd60pro_extract/MZ0381.HD.HEX /lib/firmware/mz0380/
```

The blob is a gzipped tar with a full mini-Linux for the card; the
host driver pushes it byte-for-byte through the BAR0 firmware aperture
and the onboard bootloader untars and boots.

## Build

```bash
make
```

Build needs `linux-headers-$(uname -r)`. The default build emits only
`mz0380.ko`; the legacy `sc0710.ko` (4K60 Pro Mk.2) is opt-in.

## Load

Three escalating modes during bring-up. Each requires the previous to
work first.

```bash
# 1. probe-safe: V4L2 node only, no firmware upload, no DMA
sudo insmod ./mz0380.ko procfs_verbosity=2 enable_video=1

# 2. firmware: upload the blob to the card, leave DMA off
sudo insmod ./mz0380.ko procfs_verbosity=2 enable_video=1 firmware_upload=1

# 3. full video streaming: firmware + MSI + 4GiB-aligned IOVA buffers
sudo insmod ./mz0380.ko procfs_verbosity=2 \
    enable_video=1 firmware_upload=1 enable_dma=1 dma_iova_remap=1
# or:
make load-streaming
```

## Module parameters

| param                      | default | meaning                                   |
|---                         |---      |---                                        |
| `enable_video`             | 0       | register an `mz0380 H.264` `/dev/video*` node |
| `firmware_upload`          | 0       | upload `MZ0380.HD.HEX` to the card        |
| `enable_dma`               | 0       | allocate stream buffers, request MSI, set bus master |
| `dma_iova_remap`           | 1       | map four stream buffers at required 4GiB IOVAs |
| `stream_nosg`              | 0       | diagnostic card-generated NV12; not HDMI capture |
| `enable_audio`             | 0       | experimental inert ALSA scaffold; leave disabled |
| `procfs_verbosity`         | 1       | debug-richness of `/proc/mz0380-*`        |
| `debug`                    | 0       | dprintk gate                              |

## Capture

Use the hardware-validation harness. It resolves the actual `/dev/video*`
node, reloads the module, verifies a coherent HDMI timing, and captures an
H.264 elementary stream:

```bash
sudo ./mz0380-m55-real-capture.sh 6 45
```

Power-cycle or reconnect the source when prompted. The machine needs a
translating IOMMU domain; the card cannot address ordinary, unaligned DMA
buffers. Output is written to `/tmp/cap-m55.h264`.

## Diagnostics

| Path                            | What                                     |
|---                              |---                                       |
| `/proc/mz0380`                  | device list + BAR map                    |
| `/proc/mz0380-state`            | live state (firmware/IRQ counts/format)  |
| `/proc/mz0380-snapshot`         | targeted register snapshot (5 profiles)  |
| `/proc/mz0380-control`          | SDK-grouped control surface              |
| `/proc/mz0380-experiment`       | bounded register probe + property write  |

## Status

This is a bring-up driver. Phases:

- [x] PCI probe, BAR map, V4L2 node registered (probe-safe)
- [x] 7 H.264 encoder controls correlated to BAR5 mailbox slots
- [x] Firmware loader + upload state machine (offsets `CHECKME`)
- [x] Four-buffer SET_BUF path + pre-ACK MSI event FIFO and vb2 delivery
- [x] vb2 streaming: REQBUFS/QBUF/DQBUF/STREAMON/STREAMOFF
- [x] coherent MST3367 HDMI timing detect + V4L2 DV_TIMINGS
- [x] exact EDID, SET_VIC, encoder-parameter and start/stop mailbox framing
- [ ] hardware validation of the real BT.1120 -> H.264 completion path
- [ ] ALSA PCM DMA/ownership protocol
- [ ] Mainline `linux-media` submission

Register offsets marked `CHECKME` in `mz0380-reg.h` are working
hypotheses derived from device-side `ep.ko` strings. Refine via
`/proc/mz0380-experiment` correlation runs before flipping
`firmware_upload=1` / `enable_dma=1` on production hardware.

sudo chown user:user *.o *.ko *.mod *.mod.c Module.symvers modules.order

## License

GPLv2 or later.
