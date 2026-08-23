# NEXT SESSION START

_Last updated 2026-08-23. Full history in **RE_FINDINGS.md**; the most recent
milestones are **M129 - the card captures real video**, **M140 - the VIC
captures exactly one frame, which explains everything else**, and
**M141-M143 - the Windows ordering is neutral and the host-side sequence is
exhausted**. This file is the handoff only. Everything
below was verified on hardware unless it says otherwise._

**If you read one thing:** the "no signal" story that dominated this project was
wrong (`## STATE after M129`), and **the "VIC captures one frame" model is wrong
too (M147)**. The frame the host receives is raw I420 delivered by the SDK's own
path - `img_handler` -> SSM -> `encode_handler` -> MMA -> `vpl_dmac` - and
reaching that path at all requires `img_handler` to have been called twice. So
**the pipeline completes at least one full pass and then stops.** The host-side
command sequence is exhausted (M141-M146). The live question is
**why `encode_handler` does not complete a second iteration**, and it is static. The card's encoder
has never reported a frame at all, and the one frame that reaches the host does
not come from the SDK's frame path.

---

## HARD CONSTRAINT: do not upload firmware to the card

Use only what is already on the card. Standing instruction, not a preference.

**As of 2026-08-20 the upload path is GONE FROM THE TREE ENTIRELY** - not
neutered, not inert, gone. There is nothing left to accidentally re-enable:

- `mz0380-fw.c` is handshake-only: read the expected version from the
  `MZ0380.FW.TXT` sidecar, `mz0380_card_init()` (CMD_INIT + GET_BOARD_VERSION),
  warn on mismatch, mark READY.
- **The `firmware_upload` module parameter no longer exists.** It survived for a
  while as an inert no-op so old scripts would still insmod; both halves were
  removed together. `modinfo` confirms it is absent.
- **All 32 scripts** that passed `firmware_upload=1` were stripped, plus
  `mz0380-correlation.sh`'s `--firmware-upload` CLI flag and its dead
  `insmod_args+=()` branch.
- **The opcodes are deleted from `mz0380-reg.h`**: `MZ0380_CMD_BEGIN_FW_DL`,
  `MZ0380_CMD_COMMIT_FW`, `MZ0380_CMD_BEGIN_BASE_FW_DL`,
  `MZ0380_CMD_COMMIT_BASE_FW`, `MZ0380_MB_FW_BUFFER` and the prose block
  describing the aperture protocol. A comment marks 0x0b/0x0c/0x0e/0x0f as
  deliberately undefined.
- `struct mz0380_board.firmware_name` / `.firmware_base_name` and both
  `MODULE_FIRMWARE("...HD.HEX")` declarations are gone. The only file the
  driver ever asks the firmware loader for is `mz0380/MZ0380.FW.TXT`, read for
  a version comparison it only ever warns about.
- `MZ0380_FW_STATE_UPLOADING` removed from the state enum.
- **`mz0380-m81-build-cardlog-fw.sh` is DELETED.** It built a patched card
  rootfs to redirect the card's console over PCIe. The previous session's
  attempt at exactly that is what broke the card's userspace.
- **`mz0380-m0m2-test.sh` lost its `m2` stage** - that stage *was* the upload
  attempt. `m0` (read-only observability) and `m4` (bus master + MSI handshake)
  remain.
- `mz0380-bringup.sh`'s presence check now points at the `FW.TXT` sidecar
  instead of the `.HEX` blob.
- `mz0380-re-dump.sh` is untouched and stays: it unpacks the blob **read-only**
  on the host for RE analysis (it is where tinyvenc5 is read from). Reading the
  image is not uploading it.

Keep `/lib/firmware/mz0380/MZ0380.FW.TXT` at `01.11` (matches the card). Never
restore the `01.13` file. The `.HEX` files under `/lib/firmware/mz0380/` are now
unreferenced by the driver; deleting them is optional and needs root - leave
them unless `re-dump.sh` no longer needs the unpack.

Consequence: the VIC hardware status word cannot be read by patching the card.
Plan around that rather than circling back to it.


## Card state

**A soft PC shutdown is enough to reset the card** - M127 ran five streams on a
card that had only been soft powered off, with no wedge. The older "power-cycle
at mains first" advice is over-cautious; do it only if a run shows the wedge.

The card still wedges somewhere in the **8-18 stream** range. M127 ended at
**5 streams**; the 2026-08-21 session spent **12** (M128a-d, M129, M130a,
M132-M135) on top of whatever the current power cycle had already used. None
wedged - but 12 is inside the historical 8-18 wedge range, so the next few runs
are in the risky zone. tinyvenc5 is spawned per stream by `video_capture_mgr`
(`system("./tinyvenc5 -D ...")` plus `killall -9 tinyvenc5`), and
`EncodingGroup::Start`'s failure path calls `exit()` - so "a wall of
`SET_VIC ret=-110`" is consistent with tinyvenc5 finally failing to start and
nothing being left to ACK.

**The wedge has TWO signatures, and the second one was only named on
2026-08-23.** The one this file has always described is a wall of
`SET_VIC ret=-110` mid-run. The other kills the card before anything starts:

    CMD_INIT got no answer (-110), STATUS=00000000 EVENT=00000000
        RESULT=00000000 bar5[dc]=00000000
    card handshake failed (-110) - the mailbox is deaf
    MST3367 bring-up skipped - firmware not ready

PCIe is **fine** in this state - the device enumerates, an IRQ is assigned, and
BAR reads return sane values (`bar5[30]=fc200004`, `bar5[38]=fc20005f`). What is
dead is the card's own mailbox service. Because the handshake never completes,
the driver never brings up the MST3367, so the visible symptom is
**"No coherent HDMI timing was locked during the 45s window"** on every run -
which reads exactly like a dead HDMI source and is not one. The receiver was
never touched.

Two traps this cost a session:

- `mz0380-m55-real-capture.sh` prints `waiting for the card to finish booting
  its own flash image...` and then `using /dev/video0` **whether or not the
  handshake succeeded**. Neither line is evidence the mailbox is alive.
- A run that aborts before stream start leaves `token[0x40]=00000000`,
  `irq_total=0` and **unpoisoned (all-zero) buffers**, because the sentinel seed
  and the poison fill both happen *at* stream start. That is not a wedge
  signature and not a capture - it is an abort. Only compare those fields
  between runs that actually reached `stream start:`.

Confirm it in **2 seconds**, zero spawns, no HDMI source needed - this is the
cheapest test in the tree and it should be the first thing run whenever a
session starts or a run reports no lock:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm; sudo insmod ./mz0380.ko; sudo dmesg | grep -a "CMD_INIT\|handshake" | tail -3; sudo rmmod mz0380
```

- healthy: `CMD_INIT answered on attempt 1 (status=0xdddddddd)`
- wedged: `CMD_INIT got no answer (-110)` + `the mailbox is deaf`

The `modprobe` line is required - without it `insmod` fails with
`Unknown symbol in module`, which is a missing dependency and says nothing about
the card. (`Invalid module format` is different again: that is a kernel/vermagic
mismatch. See the build section.)

The longer I2C probe says the same thing and also shows the bus, if wanted:

```bash
sudo START=0x50 ./mz0380-m83-i2c-devscan.sh 16
```

`periph: card handshake not complete` and `0 of 0 registers read` means the
mailbox is deaf, not that the receiver is missing. Power-cycle; do not chase the
source or the receiver.

**The control capture is the health check:**

```bash
sudo POLLDRAIN=20 ./mz0380-m55-real-capture.sh 1
./mz0380-m127-splash.py /tmp/cap-m55.nv12       # expect: SPLASH
```

`sudo` is required for `dmesg` on this kernel - without it it fails and silently
prints 0.

---

## Building - the driver now targets two kernel series

`make` still does the right thing; read this only when it refuses to.

**Every hardware result in this file was measured on `6.18.42-1-cachyos-lts`,
and that is where runs should stay.** A kernel change is a new variable under
whatever is being measured (method rule 1). 7.x is supported so the tree keeps
building as the distro moves, not so results can be taken there.

| command | what |
|---|---|
| `make` | running kernel, as before |
| `make kernels` | running kernel, which kernels are buildable, which one `make` will use |
| `make KVER=<ver>` | cross-build for another installed kernel |
| `make all-kernels` | build every buildable kernel into `ko/mz0380-<ver>.ko` |
| `make objclean` | top-level object wipe; what `all-kernels` uses between kernels |
| `make distclean` | `clean` plus `ko/` |

**The only 6.x -> 7.x drift was `vb2_ops->wait_prepare`/`wait_finish` and the
`vb2_ops_wait_*` helpers** (`mz0380-video.c`'s `mz0380_qops`). vb2 core took
over the `q->lock` handling and both the ops and the helpers were removed:
present in 6.18, gone in 7.2. The Makefile **greps `videobuf2-v4l2.h`** and
defines `MZ0380_HAVE_VB2_WAIT_OPS`; it does not test `LINUX_VERSION_CODE`,
because the removal release is between 6.18 and 7.2 and a wrong guess yields a
module that builds and then mishandles a blocking DQBUF. Verified both ways:
the 6.18 module still imports both symbols, the 7.2 one imports neither.

### The failure that cost the 2026-08-23 session its first run

A rolling distro can upgrade the kernel package **out from under the running
kernel**, leaving `/lib/modules/$(uname -r)` gone entirely. Nothing
out-of-tree can be built *or loaded* on such a boot, and kbuild says only
`No such file or directory`. `make` now names the state, lists the buildable
kernels, and the hardware scripts fall back to a prebuilt
`ko/mz0380-$(uname -r).ko` (loudly - it may predate your edits) via
`mz0380-build.sh`. If neither exists, **reboot into an installed kernel** -
prefer the LTS.

---

## STATE after M129 - THE CARD CAPTURES REAL VIDEO

**2026-08-21: real 1080p video came out of the card.** A photograph of the scene
in front of the camera, confirmed visually and by measurement. The raw frame is
preserved in the repo as `m129-first-real-frame.raw`
(sha256 `76e7050e...cc8704`).

    sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" ./mz0380-m55-real-capture.sh 1

    captured 3110400 bytes = 1 whole frames + 0 bytes
    VERDICT: NOT SPLASH  (Y 170 distinct values, UV 144)

    row-to-row corr 0.9665 | col-to-col 0.9703 | pixel-shuffled control -0.0008
    22.02% of pixels have |dY/dx| > 8 ; 0 of 1080 rows are video black

Luma is **perfect** - a sharp, artefact-free, correctly-framed 1080p greyscale
image with no tearing or banding. Chroma is wrong (see below).

### Three ingredients, all required

1. **`fake_frame_off=1`** - op `0x31` byte `0x0e`. Stops
   `EncodingGroup::fake_frame_process` ever being created.
2. **`post_mask=0`** - the default `0x1f` enables a mask-gated store that
   truncates the DMA to one 16-byte burst (M128d). With `0x1f` the frame never
   completes, so #1 alone shows nothing.
3. **`post_proc=1`** - the `win_seq=0` baseline never sent `0x31` at all.

M128a had #1 and #3 but not #2. Its "16 bytes of 0x53" was the real frame,
truncated.

### What this overturns - re-read old results with this in mind

**"The VIC reports no signal" was wrong.** The standby thread was painting
NOSG_LOGO_Y over a working capture path, and every previous run measured the
thread, not the capture.

- `fake_frame_process` is not a fallback for a dead input. It runs
  unconditionally beside `encode_handler` because
  `preview_params_settings[ch].byte[0x0a]` is zeroed at startup and nobody ever
  told the card otherwise - **Windows included** (M82's `board_flag_b` = 0).
- The `(Drop this frame) ... bNoSignal` path in `libtkmf_video_source.so.0` that
  M127 named as "the target" is **not** what was happening. It was never
  observed - the card's console is unreachable - and it was wrong.
- M127's corollary now has its explanation: every sweep scored as
  splash/not-splash was reading a thread that ignores every knob those sweeps
  turned. The negatives stand, and they were never about capture.

### Score with `mz0380-m127-splash.py`

Exit 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. **With `fake_frame_off=1` the
oracle inverts in usefulness: 0 / NOT SPLASH is now the PASS.** It also prints
the distinct-value counts, which is the quick real-vs-flat check.

### Chroma - FIXED (M130a)

    corr(A, luma)  -0.9664 -> -0.3194        share of A that is luma  0.93 -> 0.10
    corr(B, luma)  -0.7488 -> +0.3492        share of B that is luma  0.56 -> 0.12

Correct, natural-colour 1080p. The residual is ordinary scene correlation, not a
defect.

**Cause.** HDMI YCbCr 4:4:4 puts Cb on blue, Y on green, Cr on red. hdcapm's CSC
table converts RGB->YCbCr and is written unconditionally - its
`MST3367_HdmiGetPacketColor()` reads the input colour space from BANK2 0x48 bits
6:5, caches it, and never uses it. Fine on its board, whose EDID makes sources
send RGB. We push no EDID, this source sends YUV444, so the matrix computed
`Cb_out = -0.291*Y + ...`: perfect luma, chroma that was mostly inverted luma.

**Fix.** `mst_csc_ctl` defaults to **AUTO** and picks the mode from the detected
colour space - 0x00 (no conversion) for YUV422/YUV444, 0x40 (hdcapm's matrix)
for RGB. It is applied by `mz0380_mst3367_apply_csc_mode()` at **stream start**,
not in `init_regs()`, because init runs before HPD and 0x48 is meaningless
there. An explicit value still forces the byte.

**The frame is planar I420, NOT NV12.** Y, then two 960x540 planes. The
`.nv12` filename is historical and cost two viewing mistakes in one session;
`mz0380-m55-real-capture.sh` now prints the right hint.

```bash
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 m130-colour-correct-frame.raw
```

Before/after pair kept in-tree: `m129-first-real-frame.raw` (broken chroma),
`m130-colour-correct-frame.raw` (correct).

---

## What is CLOSED - do not re-open

| avenue | how |
|---|---|
| **I2C `0x98`** | It is an **IT66121 HDMI transmitter** (vendor `0x4954` "ITE" at regs 0x00/0x01, device `0x612` rev 1 at 0x02/0x03, ignores bank select). The board's output passthrough. **Not on the capture path.** M127c. |
| **I2C `0x90` / `0xa0`** | Not fitted. The bus is exactly two devices: `0x9c` receiver, `0x98` transmitter. |
| **MST3367 `0xb0`** | Fully decoded (M100): bit0 = embedded(1)/external(0) sync, bit2 = 10(1)/8(0) bit. `0x21`,`0x25` (embedded) -> splash. `0x20`,`0x14` (external) -> **nothing renders at all**. Embedded sync is required for VIC init. |
| **MST3367 `0xb1`/`0xb2`/`0xb5`** | These are **CSC-matrix cells**, not an output stage - hdcapm writes a 36-byte CSC table at `0x92..0xb5` that overlaps them, then fixes up `0xb0` last. We write the same table. Sweeping one byte of a matrix was never a hypothesis. M127d. |
| **Receiver init generally** | Ours is a **superset of hdcapm's** - every register it writes, we write, `0xe2` included (we drive auto-position dynamically). M127h. |
| **SET_VIC `in_fmt`** | 3, 6, 7 all splash. 0 is illegal and stops the card rendering (M104). |
| **SET_VIC `vic_in_w`** | 1920 / 2048 / 2200 / **3840** all splash. M76's double-rate width hypothesis is now validly dead - M127g re-measured 3840 at a working `b0` with a working delivery path and a real oracle. |
| **Host wake-ups / kicks** | op `0x2f` x1171 and op `0x06` x1224 (M133): accepted every time, **one frame**, and the receiver **loses lock** under either. Kicks do not gate the cadence. Do not flood these. |
| **Opcode `0x50`** | `SET_OSD` - on-screen-display text. |
| **the Windows ordering (`win_seq`)** | **Neutral, M142.** pre-STOP + 1900 ms settle + SET_BUF-first reproduce the baseline exactly, once op `0x06` is present. Do not spend another spawn on it. |
| **op `0x06`** | **Required, M142.** It arms `vpl_dmac`'s outbound push - the transfer that carries the card's one frame to the host. Without it: 0 frames, 0/1024 pages touched. Not a "kick" (those are the `0x2f`/`0x06` floods of M133) - it is the arming step. |
| **"the VIC sees no signal"** | **WRONG - M129.** The card captures real 1080p video. The standby thread was painting over it. Every splash verdict in this file measured that thread. |
| **frame rejection in `libtkmf_video_source.so.0`** | Not what was happening. M127 named it as the target from static RE alone; it was never observed and it was wrong. M129. **M138 gives the mechanical reason:** `bNoSignal`/`bCCIRErr`/`bFifoFull` are `printf` arguments in the drop banner and are never tested. The only real gate is a width equality test. |
| **the host completion handshake** | **Fully cleared, M138.** `ep.ko` cannot block (no wait/wake/sleep relocation exists in it). The EVENT credit is a real one-shot but `BAR0[0x00]=0x400` re-arms it via card IRQ 42, and M133's 1224 completed commands prove that happens 1000+ times a run. Stop spending spawns here. |
| **`state[0x630]` / SET_VIC byte 34** | M136 read it as gating EVENT-vs-accumulate. **Wrong, M138:** both branches are credit-gated identically and byte 34 only shifts the *audio* bit. M137's null result was expected. |
| **`skip` / `fps` / `avg` as preview pacers** | **Dead, M138.** `m_fps` and `m_avg` are stored by `TKMF_VideoSrc_Setup` and never read. `m_skip` drops one frame in `(skip+1)` - so `skip=0` is the *no-drop* setting, the opposite of M134's theory. |
| **EDID / HDCP** | Dead ends. Windows captures with no EDID pushed. |
| **op `0x2f`** | `SET_ENC_PARAMS_POST` - the SAME handler as `0x2d` (0xebdc), differing only in which banner it prints. H.264 knobs; cannot touch capture. M128. |
| **op `0x51`** | `SET_BAR` - a colour-bar overlay rect per (ch, line). Overlay only. M128. |
| **op `0x62`** | `SET_LOGO` - logo overlay, `/tmp/PIC_LOGO_%d`, max 320x240. Overlay only. M128. |
| **op `0x29` after start** | Ignored. The in-loop dispatch maps `0x29` to the default; tinyvenc5 reads SET_VIC exactly once, before the loop. M128. |
| **"an extra command before START truncates"** | No. The `0x09` control delivers a full frame. Nor is it cadence: a 200 ms gap changed nothing. M128c/d. |
| **M91's "something else in `win_seq`"** | It is `0x31` carrying `post_mask=0x1f`. With mask 0 the same command delivers a whole frame. M128d. |

The receiver holds a clean `R55=0x7f` 1080p60 HDMI lock in **every one** of
these. The VIC reports no signal in **every one** of these.

---

## Verified ABI (M127) - trust this over older field maps

`ep.ko`'s `epint_show` memcpy's `rodata[0xa0 + cmd]` bytes of the mailbox to the
card's userspace. Per-opcode payload length:

| cmd | 0x06 | 0x07 | 0x09 | 0x29 | 0x2a | 0x2d | 0x2f | 0x31 | 0x50 | 0x51 | 0x52 | 0x62 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| len | 8 | 8 | 8 | **40** | 20 | 44 | 44 | 20 | 44 | 20 | 7 | 12 |

SET_VIC (0x29), 40 bytes, verified from both `ep.ko` and `video_capture_mgr`:

```
 0..3  cmd = 0x29        20..21 x_start            32     vanc_lines
 4     ch                22..23 y_start            33     fast_kill
 5     fps               24..25 input_frame_width   35     is_slave
 6     fw                26..27 input_frame_height  36..39 nosg back/y/u/v
 7     input format      28     bitstream_num
 8..9  width             29     osd_enabled
10..11 height            30     osd_size
12     m                 31     is_nosg
13     flip  14 mirror   16..19 color_info[4]
```

- **Byte 7 is the cfg `input format` enum, not an interlace bool.**
  `video_capture_mgr` *labels* it `interlace(%d)` in its printf, but it is
  `sprintf`'d **raw** into the cfg line matching `"input format"`. `6 = BT1120p`,
  `7 = BT1120i`. The code is right. (`mz0380-dma.c`'s M71 comment block said
  otherwise; **fixed in M128**.)
- Bytes 40..41 exist in video_capture_mgr's per-channel struct but are past the
  40-byte payload, so they are not host-settable through SET_VIC.

**The cfg is patched by value as well as by comment.** M126's "tinyvenc5 patches
exactly nine keys" is wrong; the patcher is in `video_capture_mgr` (0xa290) and
it `atoi()`s every line, rewriting **any line valued 1080** with SET_VIC
`height` (bytes 10..11) and any line valued 1920 with SET_VIC `width`
(bytes 8..9). So captured geometry IS host-driven - through bytes 8..11, not
through `input frame width/height` (bytes 24..27, the `vic_in_w`/`vic_in_h`
knobs).

**ep.ko has a sticky `no_signal` latch.** SET_VIC with `width == 0 || height == 0`
sets it, does **not** `sysfs_notify`, and then ops
`0x06 0x2a 0x2f 0x31 0x50 0x51 0x52 0x60 0x61 0x62` all early-out until a good
SET_VIC clears it.

**tinyvenc5's complete command set:** `0x06 0x09 0x2a 0x2d 0x2f 0x31 0x50 0x51
0x52 0x62`, plus `0x29` handled before the loop. Everything else is ignored.

**M128 named every one of them** - full table in `mz0380-reg.h` next to
`MZ0380_CMD_SET_AIC_PARAMS`, derivation in RE_FINDINGS M128. The one that
matters:

`0x31` is **SET_PREVIEW_PARAMS**, not "POST_PROC", 20 bytes:

```
[4..7] mask (u32)  [8] ch   [9] fps   [0x0a] skip   [0x0b] avg
[0x0c] die_en      [0x0d] preview_off [0x0e] fake_frame_off
[0x0f] preview_no_osd  [0x10] mirror  [0x11] flip   [0x12] hw_d
```

The mask gates **only** byte `0x0c`; `0x0d`..`0x12` are stored unconditionally.
Byte `0x0e` lands in `preview_params_settings[ch].byte[0x0a]`, which is the
`pthread_create` guard on `EncodingGroup::fake_frame_process` in
`init_func` - i.e. the splash. It is read **once**, inside the `0x06` handler,
so `0x31` only bites if it arrives **before** START_STREAMING. Windows sends 0
there (M82 called that byte `board_flag_b`), so retail leaves the splash armed
too. M82 also had mirror/flip swapped; the card's own printf wins.

---

## Where to go next

Capture works. The remaining defect is **one frame per stream**, and M138
relocated it decisively: the stall is **on the card, upstream of tinyvenc5**.

### What M138 settled (static, zero spawns) - do not redo any of it

`EncodingGroup::encode_handler` is read. It is at **0x12b00** (0x12c60 is the
`open()` of `channel_done` inside it). Its loop head is **0x12f04** and the
first call of every iteration is `SSM_ReleaseAndReceive` (0x12f24).
`libsyncsharedmemory.so.0` has **no timed condvar wait**: when reader index ==
writer index the thread parks in `pthread_cond_wait` forever. So the encoder is
**starved, not stuck** - and none of M137's five PLT candidates is reached,
because all of them sit downstream of that first call.

The whole producer chain is now mapped:

    vpl_vic.ko ISR -> chan->frame_ready = 1 + __wake_up
      -> ioctl(/dev/vpl_vic, 0xe301) = wait_event_interruptible_timeout(...)
      -> libvideocap VideoCap_WaitVIC / VideoCap_Sleep
      -> libtk_video_capture process() loop
      -> libtkmf_video_source img_handler  (the callback)
      -> SSM_DeliverAndAllocate -> wr_idx++ -> broadcast
      -> tinyvenc5 encode_handler -> pwrite channel_done -> ep.ko -> host

`process()` **never exits on a stalled VIC** - it retries `VideoCap_Sleep` every
1 ms forever. A dead VIC therefore looks exactly like what we have: one frame,
no error, no log, no host-visible symptom.

Also closed by M138:

| claim | status |
|---|---|
| `store_channel_done` blocks waiting for a host ack | **dead.** `ep.ko` has no wait/wake/sleep relocation at all; the handler runs with IRQs masked and is straight-line register writes. |
| the EVENT credit is the gate | **dead, and the driver is correct.** `.data[0]` starts at 1, is consumed by every event and re-armed only by card **IRQ 42** = `pciep_isr_clrint`, reached by `BAR0[0x00] = 0x400` (bit 10; `0x800`/bit 11 -> IRQ 43 = command dispatcher). M133's 1224 successful commands prove it is re-armed 1000+ times per run. |
| `state[0x630]` / SET_VIC byte 34 gates EVENT vs accumulate | **wrong (M136 corrected).** Both branches are credit-gated identically; byte 34 only shifts the **audio** bit by 15 vs 16. M137's null result was the expected one. |
| `bNoSignal` / `bCCIRErr` / `bFifoFull` gate frame delivery | **dead.** They are `printf` arguments in `img_handler`'s drop banner and are never tested anywhere in the library. |
| `skip` paces the preview (M134's theory) | **backwards.** `img_handler` drops when `Count % (m_skip+1) == 1`, i.e. it drops **one in (skip+1)**. `m_skip = 0` is the *no-drop* setting. |
| `m_fps` / `m_avg` reach the video source | **dead.** `TKMF_VideoSrc_Setup` stores five halfwords; `img_handler` reads only width (offset 0) and skip (offset 6). fps and avg are written and never read. |

### The one remaining host-side gate in the source layer

`img_handler`'s only real early-out is a **geometry equality test**:

    VIC_Get.width (capture descriptor +0x1c)  ==  Tiny_Set.width (.bss 0xaa54)

`Tiny_Set` comes from a **single** `TKMF_VideoSrc_Setup` call in
`EncodingGroup::Start` (tinyvenc5 **0x11014**), filled from the per-channel
config record: width `cfg[ch]+0x0c`, height `+0x0e`, fps `+0x09`, skip `+0x38`.
One call, before the loop - so the gate cannot change mid-stream, and it cannot
be what kills frame 2 while letting frame 1 through. But `cfg[ch]+0x0c/0x0e` are
now known to be the **only** host-settable values the source layer reads at all,
which is what makes SET_VIC geometry worth a look.

### M139/M140 settled it further - read this before planning anything

**`store_channel_done()` has never run.** A sentinel seeded into BAR0
0x40/0x44/0x48/0x4c survived 56 s of streaming untouched, with the readback line
proving the registers are host-writable. No video channel and no audio channel
has ever reported a frame. `enc[0x50]=0` says no bitstream was ever DMA'd
either. M138's "writes once then blocks" is **wrong**: `encode_handler` never
writes at all.

`encode_handler` is nonetheless alive - `EncodingGroup::init_func` creates it
unconditionally at **0x10ab0**, with its run flag set one instruction earlier at
0x10a9c. (The gated `pthread_create` at 0x10c08 is `fake_frame_process`, the
splash.) So it is parked on its first `SSM_ReleaseAndReceive`, having received
zero frames.

**The single frame that reaches the host is not from the SDK's frame path at
all.** It arrives with no `channel_done`, no `enc_stat` and no EVENT, over
`vpl_dmac`'s PCIe outbound transfer (`ep.ko` exports `pcie_set_outbound`;
`vpl_dmac.ko` is its only importer, `VPL_DMAC_StartTail` 0xc94 /
`VPL_DMAC_ISRTail` 0xf70).

**One model explains every observation, and nothing else needs to be true: the
card's VIC captures a single frame and stops.** `img_handler`'s *first* call is
the init path - `SSM_Writer`, a prime `SSM_DeliverAndAllocate` with a NULL
descriptor that does not advance `wr_idx`, `TK_ImgProc_Init`, return - so it
publishes nothing. With no second call the ring stays empty forever, the encoder
parks, `channel_done` never fires, and the one captured frame is pushed to the
host by the DMAC. Buffers 1-3 are never touched because there was never a second
capture.

### HARD METHOD RULE, new and expensive: SET_VIC geometry is never one variable

`re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg` - the card's own capture
config, in the tree - has **twelve lines valued 1920 and twelve valued 1080**:
maximum frame w/h (the ISP allocation), captured frame w/h, input frame w/h, and
nine AE HardwareCtl window w/h pairs. `video_capture_mgr`'s patcher rewrites
every one of them blindly.

So `vic_out_w=3840 vic_out_h=540` (M140) did not move `img_handler`'s gate
input; it moved the ISP allocation and all nine auto-exposure windows too, and
the capture path did not come up at all - **zero** frames, one worse than
baseline. That result says nothing about the gate, and the same objection
applies retroactively to M76 and every other width sweep in `RE_FINDINGS.md`.

The cfg also documents the enums inline: output format `1:YUV420 2:YUV422`,
input format `6:BT1120p 7:BT1120i`. `in_fmt=6` is confirmed right.

**M143 decoded the patcher exactly** (vcm 0xa290; loop counter `#163` = the
cfg's 163 lines, so the on-card template is the one in the tree). Two
mechanisms, in this order per line:

| test | rewritten with |
|---|---|
| `strstr "input format"` / `"output format"` | args 4 / 5 |
| `strstr "start x position"` / `"start y position"` | r9 / r10 |
| `strstr "input frame width"` / `"input frame height"` | r11 / struct halfword |
| `atoi(line) == 1920` **and** `strstr "maximum frame width"` | struct halfword (ISP allocation) |
| `atoi(line) == 1920` otherwise | SET_VIC width |
| `atoi(line) == 1080` | SET_VIC height |
| `strstr "flip video"` / `"mirror video"` | struct halfwords |
| anything else | copied through unchanged |

So `maximum frame **width**` is special-cased and does *not* take the swept
width; `maximum frame **height**` has no such case and does move with height.
The nine AE window pairs still move with both, so the method rule stands.

**Line 4 is `60 // captured frame count`, and nothing can reach it.** Not a
patcher key, and 60 is neither 1920 nor 1080, so it passes through untouched.
No binary in the image contains that string at all - the readers parse the cfg
positionally - so every stream this card runs is configured for a **60-frame**
capture. Not the one-frame cause (60 is not 1), but a ceiling that will bite the
moment the cadence is fixed. Do not read "60 frames then stops" as a new defect.

### M141-M142: the Windows ordering was run. It is NEUTRAL. Do not run it again.

**Run on 2026-08-23, and the kernel was cleared as a variable** - both runs on
`7.2.0-1-cachyos`, same power cycle:

| run | sequence | frames | token[0x40] |
|---|---|---|---|
| M141c | baseline | 1, real video, NOT SPLASH, CHROMA OK | `a5a5a5a5` |
| M141 | `win_seq=1` | **0**, 0/1024 pages touched | `a5a5a5a5` |
| M142 | `win_seq=1 win_start_op6=1` | **1**, real video | `a5a5a5a5` |

All three on `7.2.0-1-cachyos`, one power cycle, `R55=0x7f` locked throughout,
every command `ret=0`.

**Op `0x06` arms the `vpl_dmac` outbound push.** That is the whole of M141's
zero: `0x06` is the only difference between M141 and M142, and adding it back
restores the frame exactly. M140's reading - that `0x2d`/`0x31` do the same bare
`sysfs_notify("epint")` - is true of the *card's* dispatch and says nothing
about what arms the DMAC on the way out.

**With `0x06` present the Windows ordering reproduces the baseline precisely.**
pre-STOP, 1900 ms settle, SET_BUF-first: all neutral. Sentinel intact,
`enc[0x50]=0`, `EVENT=0`, 0 producer-watch changes.

So **the host-side sequence is exhausted.** Ordering, opcode set, kicks, credit,
completion handshake, geometry, preview params, CSC - all measured, all neutral
or worse. Nothing the host sends changes how many times the VIC captures.

**The frame count is not a VIC-capture counter.** The one host-visible frame
comes from the DMAC one-shot, not the SDK path, which has never run (M139). No
host counter can tell one VIC capture from several that were never published.

<details>
<summary>Original M140 write-up of why this run was worth making (kept for the field derivations)</summary>

### The Windows ordering - one spawn, three distinguishable answers.

```bash
sudo POLLDRAIN=20 EXTRA="win_seq=1" ./mz0380-m55-real-capture.sh 5
```

**Why this and not more RE.** Windows streams continuously off this exact card
with the exact same on-card firmware - the card boots its own flash, so nothing
about it differs between hosts. Whatever makes the VIC capture more than once is
therefore in the host->card sequence, and that sequence is already decoded.
`win_seq=1` assembles it, and every field inside it is byte-matched from
M82/M127/M128:

1. pre-**STOP** (op 0x07, all channels). Windows precedes *every*
   reconfiguration with this; we have only ever sent STOP on the unwind path.
2. **1900 ms settle** (`stop_settle_ms`, default 1900 - Windows measures
   1.84-1.91 s between that stop and the SET_VIC that follows).
3. **SET_BUF first** (`win_bufs_first`, default true). Windows registers its
   capture buffers when the pin opens, i.e. before the reconfiguration.
4. SET_VIC -> SET_AIC -> 0x2d -> 0x31.
5. **No op 0x06.** Windows never sends START_STREAMING on the capture path;
   ep.ko routes 0x2d and 0x31 to the same bare `sysfs_notify("epint")` that
   0x06 performs, so the tail *is* the kick. (`win_start_op6=1` adds it back.)

**It has never been run.** Until M135a the `0x31` in this path carried
`post_mask=0x1f`, which truncates the DMA to 16 bytes and made the whole
ordering unscoreable - that is the entire content of M90/M91's "win_seq renders
nothing". Mask is 0 by default since M131.

**SUPERSEDED BY M150/M151 - the sentinel is blind. Read the FRAME COUNT.**
The registers below are written by `ep.ko`'s `store_channel_done`, which runs
only when the card's userspace `pwrite`s `channel_done` - and M150 proved that
`pwrite` unreachable (guarded by `mma_already_start`, nine reads and zero
writes in the whole binary). **`token[0x40]` cannot move whatever the card
does**, so every "moves off `a5a5a5a5`" row here is unsatisfiable and the runs
that used it were reading a constant. "producer watch saw 0 change(s)" means
"the dead path is still dead", nothing more.

The frame counts in those runs are unaffected - the poll-drain measures them
against the poison boundary, independently. **Frames delivered is the only
working oracle this project has for card-side progress.** Design future tests
against it.

Original text, kept because the run history refers to it:

**Read the result off `token[0x40]`, not off the frame count.** The M139
sentinel turned the card's own encoder into a host-visible oracle that is
independent of the DMA, the notification path and the poll-drain:

| `token[0x40]` at stop | meaning |
|---|---|
| moves off `a5a5a5a5` | **The encoder ran.** The Windows ordering is the fix. Chase cadence from there with a live pipeline instead of a dead one. |
| `a5a5a5a5`, 0 frames | Ordering is not it, and the no-0x06 variant is eliminated with it. Retry once with `win_seq=1 win_start_op6=1` before abandoning. |
| `a5a5a5a5`, 1 frame | Same one-shot as baseline; ordering is neutral. Host-side is then genuinely exhausted - go to step 2. |

</details>

### START HERE: why does `encode_handler` not iterate twice? (static, zero spawns)

M147 relocated the defect. The chain of inference, each link checkable:

1. The delivered frame is **raw I420**, not H.264 (M146) - `enc[0x50]=0` and the
   sentinel say no bitstream was ever produced, yet the image is valid.
2. Only `vpl_dmac` can write host memory (`ep.ko` exports `pcie_set_outbound`,
   `vpl_dmac` is its only importer). It is reached only via `/dev/vpl_dmac` ->
   `libmassmemaccess.so.9` -> `libtk_mass_mem_access.so.0`, and of the video
   binaries only **tinyvenc5** links those.
3. Every `TK_MMA_*` call in tinyvenc5 is inside **`EncodingGroup::encode_handler`**,
   with the push sites (0x1349c, 0x13e64, 0x142ac, 0x1430c) in the loop body,
   after the first `SSM_ReleaseAndReceive` (0x12f24).
4. `img_handler` publishes at **0x1290**, reachable only when `[r4+0xc] != 0`;
   the first call goes to 0x129c instead - the init path with `SSM_Writer`
   (0x14f4) and the priming deliver (0x1544).

So: a frame arrived -> the ring was non-empty -> `img_handler` was called **at
least twice** -> **the VIC captured at least two frames**, and `encode_handler`
ran at least one complete iteration.

M150's warm-up counter pushes that further: if the first two receives are
discarded, the delivered frame is the **third**, so `img_handler` published
three times and was called at least four. That is stacked inference and flagged
as such - but every closer look has moved the captured-frame count **up**, never
down. The card's capture is not the broken part.

**Read the loop body, 0x12f24 to the branch back to 0x12f04**, in
`re-dump/tinyvenc5.txt`. The loop has four back-edges (0x12fa8, 0x13734,
0x13d28, 0x13dd0) and one **exit** on an SSM error: a return of -1 or -2 leaves
via 0x1512c, which sets a flag and jumps to the cleanup at 0x12fc0. So "parked
forever in `pthread_cond_wait`" (M138) is not the only way this thread stops -
it can also leave the loop entirely.

**Already read, do not redo (M150):**

- The first post-receive test at 0x12f4c (`[chan+0x34] <= 1 -> 0x13d04`) is a
  **self-incrementing warm-up counter**: the branch it takes increments that
  same address (0x13d20) and loops. The first two receives are discarded; the
  main body runs from the third. It is not `bitstream_num`, which is why M148's
  hardware sweep was flat.
- **`channel_done` is dead code.** The 24-byte `pwrite` at 0x13538 (fd from the
  `open()` at 0x12c64) sits in a block jumped over by
  `mma_already_start == 0` at 0x133f0 - and that flag has nine reads and zero
  writes in the whole binary. So the completion path the driver waits on
  **cannot fire for anybody, Windows included**, and the poll-drain is not a
  workaround but the only mechanism this image offers. A second 24-byte
  `pwrite` at 0x13dfc is on a path that is *not* behind the dead flag; it did
  not fire either, and it is where any surviving report would come from.
- The `TK_MMA_ProcessOneFrame` at 0x1349c is inside the same dead block, so the
  delivered frame comes from one of the other push sites (0x13e64, 0x142ac,
  0x1430c). Known already: `EncodingGroup::mma_already_start`
(.bss 0x7eda0, `[base-0xfa8]`) has **nine reads and zero writes** - it is
permanently 0, so every branch requiring it is dead, including the synchronous
`TK_MMA_WaitOneFrameComplete` at 0x14358. Map those dead branches before
trusting any control flow in this function.

**Corrections this forces** - do not reason from the superseded versions:

| claim | status |
|---|---|
| `store_channel_done` has never run (M139) | **stands** - measured |
| `encode_handler` received zero frames (M139) | **too strong** - it ran a full iteration |
| the VIC captures exactly one frame (M140/M144) | **at least two** - never measured; the host counts DMA pushes, not captures |
| the one frame is not from the SDK path (M139) | **wrong** - it is, just not the H.264 bitstream path |

<details>
<summary>Superseded: the VIC re-arm investigation (M144/M145)</summary>

### who re-arms the VIC? (static, zero spawns)

M144 disassembled `vpl_vic.ko` (kept as `re-dump/vpl_vic.txt` +
`re-dump/vpl_vic.sym`) and found the mechanism, though not yet its cause.

`0x60` - bits 5:6 of the register-block words at offsets **8** and **0xc** -
appears exactly four times in the module and nowhere else:

| site | bits SET | bits CLEAR |
|---|---|---|
| `Ioctl` 0x3a3c/0x3a48 | skip - do not disturb an in-flight capture | program the next capture target (`[dev+0x1c]`, `[dev+0x20]` from `[chan+0x40]`/`[chan+0x44]`) |
| `ISR` 0x17d0/0x17dc | keep bit 10 - capture stays enabled | **0x1928: clear bit 0 of `[block+4]` - HALT** |

A one-shot machine: the VIC runs while something keeps arming it, and the ISR
shuts it down the instant it finds nothing armed. The ISR makes exactly **one**
`__wake_up` (0x17a8), so `frame_ready` has a single source and the cadence
question is entirely "how often does the ISR reach it".

**M145 answered that question: nobody re-arms per frame.** The steady-state
loop is `VideoCap_GetBuf` -> consumer -> `VideoCap_ReleaseBuf`, and `GetBuf`
calls **only** `VideoCap_GetBufVIC` (`0x8078e303`, a 120-byte read - exactly the
per-buffer struct size). The `VideoCap_StartVIC` call sits past `GetBuf`'s
literal pool, in a helper `VideoCap_Start` uses. The target-programming block at
`vpl_vic` 0x3a50 belongs to the **setup** ioctl `0x4028e302` (0x37c0..0x3a8c)
and runs once.

So: setup programs the target, `StartVIC` (`0xe313`) sets bit 10 plus `0xe8` in
the channel word at `[regs + chan*4 + 0x10]`, and the hardware **free-runs**.
The ISR only keeps it enabled - or halts it. **The card halting after one frame
is a hardware-state condition, not a missing call**, and
`VideoCap_ReleaseBufVIC` writing no VIC register is consistent rather than
suspicious.

The VIC ioctl map, for reference:

| wrapper | ioctl | vpl_vic dispatch |
|---|---|---|
| `VideoCap_WaitVIC` (and `VideoCap_Sleep`, a tail-jump to it) | `0xe301` | - |
| setup | `0x4028e302` | 0x37c0 |
| `VideoCap_GetBufVIC` | `0x8078e303` | 0x2b00 |
| `VideoCap_ReleaseBufVIC` | `0x4004e304` | 0x29d4 (built inline from the pooled `0x4020e305`) |
| `VideoCap_StartVIC` | `0xe313` | 0x3c54 |

### The one lead left in the ISR

At 0x16xx-0x1728 the ISR copies `[chan+0x200..0x20c]` into
`[block+0x278..0x284]`, then two extras gated on **software flags**:

    1728  [chan+0x210] == 1 -> [chan+0x214] into [block+0x2c8]
    173c  [chan+0x218] == 1 -> [block+0x204] bits 1:0 from [chan+0x21c]

Those flags are filled by the setup ioctl from the SDK's options - ultimately
from the cfg. It is the only remaining place where something host-influenced
changes what the ISR does per frame. Read that next; it is static and costs zero
spawns.

Still true and still worth knowing, closed by M144:

- **the release ioctl is not the re-arm.** `VideoCap_ReleaseBufVIC`
  (`0x4004e304`, dispatch 0x29d4) only compacts the file's queued list
  (`[file+0x10]`, count `[file+0x18]`), decrements `[buf+0x64]`, and branches to
  the force-release path at 0x4754 when `[file+0x1c]==1`. Per-buffer struct is
  **120 bytes**, array at `[dev+0x90]`. It writes **no** VIC register.
- **`capture_app_infinite` is an AUDIO app** - `libasound`, `TK_MMA_*`,
  `-R 48000 -F 256 -B 4`. There is no continuous-video reference app in the
  image.

</details>

<details>
<summary>M140's original framing of this step (superseded by M144)</summary>

1. **`vpl_vic.ko`'s buffer lifecycle.**
   The release path is `VideoCap_ReleaseBufVIC` = `ioctl(fd, 0x4004e304, idx)`,
   dispatched at **vpl_vic 0x29d4** (the tree builds that constant inline from
   the pooled `0x4020e305`, which is why grepping for `4004e304` finds nothing).
   It dequeues from the file's list and decrements a per-buffer refcount at
   `[buf+0x64]`; the `[file+0x1c]==1` branch at 0x4754 is the *force-release*
   path (it matches libvideocap's "[yuan][Sleep] Force Release Done"), not the
   normal one. The ISR's ready gate is `[chan+0x1c]`, set to **0xffff** by the
   `0x4028e302` setup ioctl at 0x3394.
   **Warning from experience:** `+0x1c` is a generic offset reused across a
   dozen structs in this module and there is no VIC register datasheet in the
   tree. This search did not converge in one sitting. Time-box it.

</details>

3. **`out_fmt` / `fw`.** The card's own cfg documents the enum inline -
   `output format (1:YUV420, 2:YUV422)` - but M79 found SET_VIC byte 12 never
   reaches the cfg (vcm passes it on tinyvenc's argv) and M82 settled it as a
   fractional-rate flag Windows sends 0 for. The cfg's output format comes from
   byte 6 (`fw`), where M88 established 5 is the only value that works. Both
   already worked; listed so nobody re-derives them.

4. **Possible mirror.** The "Boss" logo at top-left of
   `m130-colour-correct-frame.raw` looks mirrored. Check against the physical
   scene before touching `mirror`/`flip` - the camera may simply be pointed
   that way.

**Do NOT** sweep SET_VIC geometry again without first solving the ISP-allocation
problem (see the method rule above), and do NOT spend another spawn on any
completion or reporting handshake - credit, `enc_stat`, kicks, `vic_int_mode`,
EVENT. All of them are downstream of a capture that only ever happens once, and
M138/M139 proved the reporting path correct end to end and never reached.

## Tools

| thing | what it does |
|---|---|
| `mz0380-m55-real-capture.sh` | the real-path run. **The control capture is also the health check.** Always unloads on exit. |
| `mz0380-m130-chroma.py` | **the chroma oracle (M130).** `corr(chroma, luma)` per plane; 0 = CHROMA OK, 1 = CONTAMINATED/PARTIAL, 2 = no frame. Validated against `m129-first-real-frame.raw`. |
| `m129-first-real-frame.raw` | the first real captured frame, kept in-tree. planar 4:2:0, sha256 `76e7050e...cc8704`. |
| `mz0380-m127-splash.py` | **the oracle.** SHA-256s the 320x240 crop at (800,420) against `NOSG_LOGO_Y`. 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. |
| `mz0380-live.sh` | `load` / `status` / `watch` / `unload`. Leaves the module loaded so OBS can open the node. Defaults `POLLDRAIN=20`. |
| `mz0380-m85-unload-smoke.sh` | run after every build. Load/unload, ~5 s, zero spawns. |
| `mz0380-build.sh` | sourced by the three loading scripts: build for the running kernel, else fall back to a prebuilt `ko/mz0380-$(uname -r).ko`, else show the real build error. |
| `mz0380-m83-i2c-devscan.sh` | zero-spawn I2C probe. `START=<reg>` picks the window (default `0x00`), `$1` the count. |
| `/proc/mz0380-periph-scan` | arbitrary I2C register dump; `periph_chip` takes any 8-bit address |
| `/proc/mz0380-buf0` | raw stream buffer 0 |
| `mz0380-m126-score.py` | **superseded** by m127-splash.py. |

New knob (M151): `enc_stat_ack` - clear enc_stat after each delivered frame,
default 1 (the M40 behaviour every prior result used). Setting 0 removes the
**last** write the driver makes to the card after START; measured, and the
cadence is unchanged, so no host-side write after START causes the stall.

New knob (M148): `bitstream_num` - SET_VIC byte 28, default 1. Hardcoded since
M22, swept once and negative; kept because the field is now settable for free.

m55/live env knobs: `VICFW VICM VICB0 VICINW VICINH VICINFMT WINSEQ INTX ENCSUB
OP6 WINBUFS OP8 PROBEWIN MSTOUT MSTAD SETBUF RXSTRAP POLLDRAIN POLLCREDIT
OP6KICK NOSRC MSTB1 MSTB2 MSTB5 B0LATE KICKOP KICKREP`, plus
`EXTRA="param=value ..."` and `SKIPSMOKE=1`.

### Re-unpacking the blob for static RE (read-only, NOT an upload)

```bash
tar xzf /usr/lib/firmware/mz0380/MZ0380.HD.HEX.stock -C <scratch>
llvm-objdump -d --triple=armv5te-linux-gnueabi <scratch>/yuan_demo_sdi/drivers/vpl_vic.ko
```

Everything is unstripped: `ep.ko`, `vpl_vic.ko`, `tinyvenc5`,
`video_capture_mgr`, `libvideocap.so.13`, `libtkmf_video_source.so.0`.
`libvideocap` is PIC and addresses strings as `GOT_base(0x18e4c) + literal`;
tinyvenc5 and video_capture_mgr are EXEC with plain absolute literals.

## Watching in OBS

```bash
sudo ./mz0380-live.sh load     # prints the node, leaves the module loaded
sudo ./mz0380-live.sh status   # signal, spawn count, last log lines
sudo ./mz0380-live.sh unload
```

Expect **one frame and then a frozen preview** - still true, but since M129 that
one frame can be real video rather than the splash. Load with
`fake_frame_off=1 post_mask=0 post_proc=1` to get it. Continuous streaming is
item 3 in "Where to go next".
Every OBS start/stop is one stream out of ~8-18 - turn off "Deactivate when not
showing" and do not leave it looping.

---

## Method rules - these keep being paid for

1. **A bisect step is only informative if every OTHER variable sits at a value
   already known to permit the outcome you are measuring.** Casualties so far:
   M76's "width hypothesis DEAD" (scored `captured 0 bytes`, structurally zero
   for *every* setting before M112 - re-measured and confirmed dead in M127g),
   the `win_seq` results, and the `fw=6` run.
2. **One structural change at a time.**
3. **A caveat in a source comment describes the state when it was written - it
   is not a statement about the current result set.** M127 burned two spawns
   re-running M96 because core.c:1008 says "that is exactly how the
   `vic_b0=0x20` test was wasted" (written about the *M73-era* attempt) and it
   was read as "`0x20` has never been validly measured". **Grep RE_FINDINGS for
   the actual result table before calling anything untested.**
4. **Byte-for-byte parity with the retail driver is a hypothesis generator, not
   a rule.** Same firmware, same opcode, same struct - and the value Windows
   sends breaks us (`fw=7`).
5. **Never let a test harness carry its own copy of a driver default.** Use
   `add_opt`, which passes a knob only when the caller sets it.
6. **grep can lie on these files.** The Windows I2C summary is extended-ASCII;
   plain `grep` prints nothing. Use `grep -a`.
7. **`sudo` resets the environment.** `START=0x90 sudo ./script` silently runs
   with the default; the variable must go AFTER `sudo`. Read the header line
   that echoes the setting actually used.
8. **`dmesg -C` runs at the top of m55 and m83.** A `dmesg | grep` issued after
   a later run has already lost the earlier one's evidence. Grep in the same
   command as the run, or not at all.
9. **Confirm the knob landed before reading the result.** Every m55 run echoes
   `b0=..` in three `output stage` lines and `vic_in=..x..`/`in_fmt=..` in the
   SET_VIC line. If they do not show what you set, the run measured nothing.

## Gotchas that still cost time

- This shell is **fish**, not bash. Put loops and parameter expansion in a
  `bash -c` or a script.
- System `objdump` has **no ARM support**. Use `llvm-objdump` or Ghidra headless
  (`-processor ARM:LE:32:v5`).
- `m55` runs `make` as root, leaving root-owned `*.o` that break the next
  non-root build. `mz0380-live.sh unload` cleans them up; otherwise `rm -f *.o`.
- Always reload the module - a stale one silently lacks new params.
- The sweep scripts grep-filter m55's output; read a full unfiltered run before
  concluding anything.
- The card console route is **dead**: redirecting it needs
  `yuan_start_process.sh` modified, i.e. an upload. See the hard constraint.
- The steady-state card boot path is `etc/rc.local` (which starts
  `video_capture_mgr -D -P 5`), **not** `yuan_start_process.sh` (the first-boot
  / restore path, which is the one that starts tinyvenc5 directly).
