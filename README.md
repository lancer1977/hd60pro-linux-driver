# mz0380 — Linux driver for Elgato Game Capture HD60 Pro

PCIe capture driver for the Elgato Game Capture **HD60 Pro** family
(YUAN MZ0380, PCI `12ab:0380` / `12ab:0381`). The card carries an ARM SoC
running its own embedded Linux, which does HDMI receive, signal detect
(MST3367) and encoding. This driver configures that SoC over a BAR0 mailbox
and delivers the captured frame to userspace through V4L2.

## Read this first: what the device actually does

**With the firmware it boots, this card is a single-shot 1080p frame grabber.**
One correct frame per stream, then the stream ends. That is a proven bound with
a fully traced cause, not a missing configuration step — see `RE_FINDINGS.md`
M153/M158/M166 for the derivation, and `NEXT_SESSION_START.md` for the short
version.

Concretely, from a plain `insmod`:

- one 1920x1080 **planar I420** frame, 3110400 bytes, correct colour;
- then `DQBUF` returns `-EIO` and `poll()` reports `EPOLLERR`, so applications
  finish and exit rather than blocking forever;
- `STREAMOFF` + `STREAMON` gets you the next frame.

Do **not** work around this by restarting the stream per frame to fake video.
Each restart spawns a fresh encoder on the card, and the card wedges for good
somewhere in the 8–18 spawn range — recovery is a mains-off cold boot. See
[Spawn budget](#spawn-budget).

Audio is not implemented. `enable_audio` registers an inert ALSA scaffold with
no PCM DMA behind it; leave it off.

> Legacy note: this repo began as a reverse-engineering project for the Elgato
> 4K60 Pro Mk.2 (`sc0710-*.c`). That code is preserved but no longer builds on
> modern kernels and is excluded from the default target. Pass
> `MZ0380_LEGACY_SC0710=1` to `make` to attempt it.

## Supported cards

| Subsys      | Variant                      |
|-------------|------------------------------|
| `1cfa:0003` | HD60 Pro Rev. 1              |
| `1cfa:0005` | HD60 Pro Rev. 2 (unreleased) |
| `1cfa:0006` | HD60 Pro Rev. 1 + Ryzen fix  |
| `1cfa:0010` | HD60 Pro Rev. 3 (`DEV_0381`) |
| `12ab:05cf` | HD60 Pro Prototype           |

## Firmware: nothing is uploaded

**This driver does not upload firmware, and the code to do so has been removed
from the tree.** The card boots its own onboard flash image; a host-side upload
is unnecessary, and an attempt at one previously broke the card's userspace.
There is no `firmware_upload` parameter — earlier revisions of this README told
you to pass one, and `insmod` will reject it.

The only file the driver ever requests is an optional ASCII sidecar,
`/lib/firmware/mz0380/MZ0380.FW.TXT`, holding the expected version as `MM.mm`.
It is compared against what the card reports and a mismatch produces a warning,
nothing more. On this hardware the card reports `01.11`:

```bash
echo 01.11 | sudo tee /lib/firmware/mz0380/MZ0380.FW.TXT
```

Without it the check is skipped and the driver runs normally.

## Build

```bash
make
```

Needs `linux-headers-$(uname -r)`. `make kernels` lists which installed kernels
are buildable and which one `make` will use; `make KVER=<ver>` cross-builds for
another; `make all-kernels` builds every buildable kernel into `ko/`.

The tree supports both 6.x and 7.x. The only source difference is vb2's
`wait_prepare`/`wait_finish` ops, detected by grepping `videobuf2-v4l2.h`
rather than by version number.

## Load and capture

The shipping defaults are the known-good capture configuration — DMA, the V4L2
node, the firmware handshake and the 20 ms poll-drain are all on. A bare
`insmod` captures:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 --stream-to=/tmp/frame.i420
```

The frame is planar I420 at 1920x1080:

```bash
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 /tmp/frame.i420
```

Decoding it as NV12 gives magenta/green interleave banding — the byte count is
identical and only the chroma layout differs, so nothing errors, it just looks
wrong.

`make load-streaming` does the load; `make capture` runs the full validation
harness, which reloads the module, waits for an HDMI lock, captures and scores
the result:

```bash
sudo ./mz0380-m55-real-capture.sh 1 45
```

The host needs a translating IOMMU domain: the card's outbound window is
high-32-bits-only, so each stream buffer is mapped at its own 4 GiB-aligned
IOVA.

## Is the card healthy?

Two seconds, no HDMI source needed, no encoder spawned. Run it first whenever
anything looks wrong:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm; sudo insmod ./mz0380.ko; sudo dmesg | grep -a "CMD_INIT\|handshake" | tail -3; sudo rmmod mz0380
```

- healthy: `CMD_INIT answered on attempt 1 (status=0xdddddddd)`
- wedged: `CMD_INIT got no answer (-110)` and `the mailbox is deaf`

A wedged card still enumerates on PCIe with sane BAR reads — what is dead is the
card's own mailbox service. Because the handshake never completes the receiver
is never brought up, so the visible symptom is "no HDMI timing locked", which
reads exactly like a dead source and is not one. Power-cycle; don't chase the
source.

## Spawn budget

Every stream start forks a fresh encoder on the card, and the card wedges
somewhere in the 8–18 spawn range per power cycle. The budget is therefore the
number that decides whether the next run is safe:

```bash
./mz0380-spawns.sh
```

It accumulates across module reloads in `/run`, which a tmpfs clears at boot —
the same event that resets the card. `mz0380-m55-real-capture.sh` banks each
run's spawns automatically.

## Diagnostics

| Path                      | What                                    |
|---------------------------|-----------------------------------------|
| `/proc/mz0380`            | device list + BAR map                   |
| `/proc/mz0380-state`      | live state, format, IRQ and spawn counts |
| `/proc/mz0380-snapshot`   | targeted register snapshot (5 profiles) |
| `/proc/mz0380-control`    | SDK-grouped control surface             |
| `/proc/mz0380-experiment` | bounded register probe + property write |

`modinfo ./mz0380.ko` documents all module parameters; most are
reverse-engineering levers with a milestone reference in their help text, and
the defaults are the configuration every successful capture used.

## Status

- [x] PCI probe, BAR map, mailbox command path, firmware handshake
- [x] MST3367 receiver bring-up, HDMI detect, DV timings, CSC
- [x] Four-buffer SET_BUF path, MSI event FIFO, vb2 delivery
- [x] Real 1080p capture, correct colour, from a plain `insmod`
- [x] V4L2 node describes itself correctly (format, frame sizes, input status)
- [x] The one-frame bound is reported as end-of-stream rather than a hang
- [x] `v4l2-compliance` clean (148/148; 5 warnings are `DV_RX_POWER_PRESENT`,
      which this card cannot answer - M173)
- [ ] Capture verified at resolutions other than 1080p
- [ ] ALSA PCM DMA
- [ ] Mainline `linux-media` submission

Continuous video is **not** on this list. It needs a different `ep.ko` or a
different `tinyvenc5` on the card, and putting either there is out of scope by
standing rule — `NEXT_SESSION_START.md` records both routes and the specific
blocker for each.

## Documentation

| File                     | What                                              |
|--------------------------|---------------------------------------------------|
| `NEXT_SESSION_START.md`  | current state, what is closed, what to do next    |
| `RE_FINDINGS.md`         | full reverse-engineering history, milestone by milestone |
| `MZ0380_SDK_CONTROL_PATH.md` | the card-side SDK control path                |

## License

GPLv2 or later.
