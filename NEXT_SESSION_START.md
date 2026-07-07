# NEXT SESSION START

## Where we are (2026-07-08, paused)

Full frame path RE'd end-to-end against the card's OWN firmware
(`MZ0380.HD.HEX` = gzip+tar → `yuan_demo_sdi/`: ep.ko + video_capture_mgr +
tinyvenc5, disasm via `llvm-objdump --triple=armv7-linux-gnueabi`). Extracted
tree + cached disasm in the scratchpad. See **RE_FINDINGS.md M23 + M24** for the
full proof.

### FIXED and hardware-verified this session
- **op6 START is fire-and-forget** — send with `timeout_ms=0`. Waiting for a
  cmd-done that op6 never posts was the bogus `-110`. (mz0380-dma.c)
- **SET_VIC 44-byte packing was +2 bytes off** (M20/M22 bug) → firmware read
  `bitstream_num=0` → encoder spawned but silent. Corrected: `bitstream_num@28`,
  `input_w@24`, `input_h@26`. This (not timing, not no_signal) was the silence.
- **`no_signal` is pure `(W==0||H==0)` from op41** — no hw signal detect; gate was
  always open. `start_delay_ms` timing was a red herring (2000/4000ms = flat 3 IRQ).
- **`stream_nosg=1` module param** → forces the card's `fake_frame_process`
  (black-frame generator, no capture dep). PROVED encode→channel_done→MSI→
  outbound-ATU→DMA is ALIVE: the card actively DMAs frames.

### THE REMAINING BLOCKER (M24) — DMA destination low32
The card DMAs frames but to the wrong host address:
`host_target = (cmd[0xc] << 32) + aperture_offset`, and **LOW32 is stuck at 0**.
Proven: `cmd[0xc]=0` → IOMMU faults at `0x0,0x80,0x100..`; `cmd[0xc]=0xfff80000`
→ `0xfff80000<<32`. Card-side `pcie_set_outbound` (ep.ko, sole ELBI writer) writes
the low target to ELBI `0x54`, which the HW ignores (Vatics-proprietary block,
init zeroes `0x54..0xCC`; effective low-target likely `0x84`).

**Caveat: the HD60 Pro works normally with Elgato's driver on <4GB PCs**, so low32
IS reachable — we're likely missing the correct buffer-setup path, not hitting a
hw dead-end.

### Two ways forward (pick next session)
1. **RIGHT WAY** — RE how the *working* path gets the host address into the DMA
   descriptor: trace `vpl_dmac.ko` `profile[dst]` source (`VPL_DMAC_StartTail/
   ISRTail` copy `profile[0x8..0x34]`→DMA MMR) + the Windows driver's frame-buffer
   setup. Our op2 `SET_BUF` came from M17 and may be the wrong command.
2. **WORKAROUND** (guaranteed by the confirmed model) — put the DMA buffer at a
   4GB-aligned IOVA (`iommu_map` at `0x1_0000_0000`), set `cmd[0xc]=1`,`cmd[0x10]=0`
   → frames land in-buffer. Needs IOMMU remap or a reserved-mem boot param.

Also still open after that: **upstream real BT1120→SSM capture** (nosg=0 stayed
flat at 3 IRQ — encoder idle on an empty SSM ring; needs VIC/MST3367 BT1120
output routing).

## Load + test (all gates)
```
cd /home/wolffyx/Projects/sc0710
sudo fuser -k /dev/video0 2>/dev/null; sleep 1     # prior streamon can wedge rmmod
sudo rmmod mz0380
lsmod | grep -q mz0380 && echo "STILL LOADED" || echo "unloaded OK"
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko firmware_upload=1 dma_handshake=1 enable_dma=1 enable_video=1 procfs_verbosity=2
sudo dmesg | grep 'driver version' | tail -1      # confirm FRESH load (uptime advances)

echo 1 | sudo tee /sys/module/mz0380/parameters/stream_nosg   # fake-frame path (DMA test)
sudo timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 --stream-to=/tmp/cap.h264
grep '164:' /proc/interrupts
sudo dmesg | grep -E 'mz0380|frame token|AMD-Vi' | tail -25
ls -l /tmp/cap.h264
```
Gotchas: `modprobe -a` (not bare list); needs videobuf2-vmalloc; `enable_dma=1`
arms DMA (else streamon bails); `enable_video=1` creates /dev/video0. rmmod fails
if a prior hung streamon holds the device — `fuser -k` first and VERIFY unload.

## Key artifacts
- Firmware tree + disasm: `scratchpad/fw/yuan_demo_sdi/`, `scratchpad/disasm.txt`
  (ep.ko), `scratchpad/dmac.txt` (vpl_dmac.ko).
- Full proof chain: `RE_FINDINGS.md` M22 (op6) → M23 (offsets/is_nosg) → M24 (DMA
  target).
