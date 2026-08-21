# NEXT SESSION START

_Last updated 2026-08-21. Full history in **RE_FINDINGS.md**; the most recent
milestones are **M129 - the card captures real video** and **M138 - the
producer chain read end to end**. This file is the handoff only. Everything
below was verified on hardware unless it says otherwise._

**If you read one thing:** the "no signal" story that dominated this project was
wrong (`## STATE after M129`), and the surviving one-frame defect is **on the
card, upstream of tinyvenc5** (`## Where to go next`). No host-side handshake
remains untried or unexplained.

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

**The control capture is the health check:**

```bash
sudo POLLDRAIN=20 ./mz0380-m55-real-capture.sh 1
./mz0380-m127-splash.py /tmp/cap-m55.nv12       # expect: SPLASH
```

`sudo` is required for `dmesg` on this kernel - without it it fails and silently
prints 0.

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

### Ranked next steps

1. **Distinguish parked from dead, one spawn.** `encode_handler`'s two exits
   differ observably: on a clean exit (run flag cleared, reader NULL, or
   `SSM_ReleaseAndReceive < 0`) it **`close()`s the `channel_done` fd** at
   0x12fc0 and releases both `TK_MMA` handles; while merely starved it holds
   them open. Anything the host can read that reflects the card's fd/handle
   state separates "the encoder died" from "the encoder is waiting", and only
   the second sends us to the VIC. Check what `epint_show` / the `/sys` mirrors
   expose before spending the spawn.

2. **SET_VIC geometry as the source-layer lever.** Bytes 8..11 are already known
   to drive captured geometry (M127's cfg-patcher finding). They are now also
   known to be the values `img_handler` compares against. A run where
   `Tiny_Set != VIC_Get` should deliver **zero** frames, not one - so a
   deliberate mismatch is a cheap positive control that proves the source layer
   is alive and being called repeatedly. If a mismatched run still delivers
   exactly one frame, `img_handler` is not being called at all and the VIC is
   confirmed dead after frame 1.

3. **`win_seq=1 post_mask=0`, one spawn.** Unchanged from M137's list and still
   untested. M135a completed the mask bisect: bit 0 (skip) truncates because it
   makes tinyvenc5 skip `tiny_calculate_skip_fps()`; **die_en (bit 4) is safe**
   (`post_mask=0x10` gives a full frame). So `win_seq=1` "renders nothing"
   (M90/M91) only because it sends `0x31` with mask 0x1f. The Windows ordering
   is now testable.

4. **`out_fmt`.** SET_VIC byte 12 is the output format and we still send **0**,
   which M72 flagged as not a legal value - the card falls back to the cfg. Now
   that the picture is right, setting it deliberately is worth one spawn.
   **Warning:** `fw=6`/YUY2 makes frames 4:2:2 = 4147200 bytes and
   `mz0380_infer_frame_length`'s `want` is hardcoded `w*h*3/2`; fix that first
   or poll-drain will never see a complete frame.

5. **Possible mirror.** The "Boss" logo at top-left of the captured frame looks
   mirrored. Check against the physical scene before touching `mirror`/`flip`
   (SET_VIC bytes 13/14, SET_PREVIEW_PARAMS bytes 0x10/0x11) - the camera may
   simply be pointed that way.

Dropped from this list: **`fw = 6`** as a capture fix, **GPIO pins beyond
1/3/8/9**, and **every remaining host-side completion handshake**. The reporting
path is proven correct end to end; the producer is what is stalled.

## Tools

| thing | what it does |
|---|---|
| `mz0380-m55-real-capture.sh` | the real-path run. **The control capture is also the health check.** Always unloads on exit. |
| `mz0380-m130-chroma.py` | **the chroma oracle (M130).** `corr(chroma, luma)` per plane; 0 = CHROMA OK, 1 = CONTAMINATED/PARTIAL, 2 = no frame. Validated against `m129-first-real-frame.raw`. |
| `m129-first-real-frame.raw` | the first real captured frame, kept in-tree. planar 4:2:0, sha256 `76e7050e...cc8704`. |
| `mz0380-m127-splash.py` | **the oracle.** SHA-256s the 320x240 crop at (800,420) against `NOSG_LOGO_Y`. 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. |
| `mz0380-live.sh` | `load` / `status` / `watch` / `unload`. Leaves the module loaded so OBS can open the node. Defaults `POLLDRAIN=20`. |
| `mz0380-m85-unload-smoke.sh` | run after every build. Load/unload, ~5 s, zero spawns. |
| `mz0380-m83-i2c-devscan.sh` | zero-spawn I2C probe. `START=<reg>` picks the window (default `0x00`), `$1` the count. |
| `/proc/mz0380-periph-scan` | arbitrary I2C register dump; `periph_chip` takes any 8-bit address |
| `/proc/mz0380-buf0` | raw stream buffer 0 |
| `mz0380-m126-score.py` | **superseded** by m127-splash.py. |

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
