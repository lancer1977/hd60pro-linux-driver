# mz0380 — Linux driver for Elgato Game Capture HD60 Pro

PCIe capture card driver for the Elgato Game Capture **HD60 Pro** family
(YUAN MZ0380 chipset, PCI `12ab:0380` / `12ab:0381`). The card has an
onboard ARM SoC that runs its own embedded Linux and performs HDMI
receive, signal detect (MST3367), and H.264 encoding (TP2834 +
tinyvenc). The host driver uploads firmware, manages two DMA rings,
and exposes the result as a V4L2 H.264 capture device plus an ALSA
PCM audio device.

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
host driver pushes it byte-for-byte through a BAR5 scratch window
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

# 3. full streaming: firmware + DMA rings + MSI + ALSA
sudo insmod ./mz0380.ko procfs_verbosity=2 \
    enable_video=1 firmware_upload=1 enable_dma=1 enable_audio=1
# or:
make load-streaming
```

## Module parameters

| param                      | default | meaning                                   |
|---                         |---      |---                                        |
| `enable_video`             | 0       | register `/dev/video0`                    |
| `firmware_upload`          | 0       | upload `MZ0380.HD.HEX` to the card        |
| `enable_dma`               | 0       | alloc rings, request MSI, set bus master  |
| `enable_audio`             | 0       | register an ALSA snd_card                 |
| `video_ring_entries`       | 16      | video DMA ring slot count                 |
| `video_ring_entry_size`    | 524288  | bytes per video ring slot                 |
| `audio_ring_entries`       | 8       | audio DMA ring slot count                 |
| `audio_ring_entry_size`    | 32768   | bytes per audio ring slot                 |
| `procfs_verbosity`         | 1       | debug-richness of `/proc/mz0380-*`        |
| `debug`                    | 0       | dprintk gate                              |

## Capture

```bash
# H.264 elementary stream
ffmpeg -f v4l2 -pixel_format h264 -i /dev/video0 -t 10 -c copy out.h264

# PCM audio
arecord -D hw:CARD=mz0380,DEV=0 -f S16_LE -r 48000 -c 2 -d 10 out.wav
```

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
- [x] DMA ring alloc + MSI IRQ handler + ring drain (offsets `CHECKME`)
- [x] vb2 streaming: REQBUFS/QBUF/DQBUF/STREAMON/STREAMOFF
- [x] HDMI signal detect via mailbox + V4L2 DV_TIMINGS
- [x] ALSA HDMI audio capture (PCM S16_LE)
- [ ] Mainline `linux-media` submission

Register offsets marked `CHECKME` in `mz0380-reg.h` are working
hypotheses derived from device-side `ep.ko` strings. Refine via
`/proc/mz0380-experiment` correlation runs before flipping
`firmware_upload=1` / `enable_dma=1` on production hardware.

## License

GPLv2 or later.
