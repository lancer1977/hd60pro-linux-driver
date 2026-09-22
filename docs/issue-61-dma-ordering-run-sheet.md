# Issue #61: DMA ordering diagnostic run sheet

This is an operator-run hardware measurement, not a delivery change. The
diagnostic is disabled by default (`raw_ladder_diag=0`) and the normal delivery
and completion gates remain unchanged when it is disabled.

## Before running

1. Build the exact checked-out commit with `make test-ladder` and the normal
   hardware-free build/check targets. Record the module commit SHA.
2. Confirm the card, source, capture format, and expected 1920x1080 mode. Do
   not infer hardware identity from a successful module load.
3. Capture baseline `/proc/mz0380-state` and kernel logs before enabling the
   diagnostic. Do not run this during an unrelated capture or production use.

## Bounded measurement

From the reviewed build directory, run one diagnostic load and a 60-second raw
capture (replace `/dev/video0` only if the recorded preflight identifies a
different node):

```bash
sudo insmod ./mz0380.ko raw_deliver=1 raw_ladder_diag=1
timeout 60s ffmpeg -hide_banner -loglevel warning \
  -f v4l2 -input_format yuv420p -video_size 1920x1080 -framerate 60 \
  -i /dev/video0 -f null -
cat /proc/mz0380-state
sudo dmesg | grep -E 'mz0380|DMA|IOMMU|AER|WARNING|BUG|Oops'
```

Save the complete command output, `/proc/mz0380-state`, and bounded kernel log
in the session evidence directory. The parameter causes full raw-slot
re-poisoning after delivery, so frame rate and host load may change; that is
expected diagnostic behavior.

Repeat the same bounded run with a different nonzero `raw_probe_poison` byte,
for example `raw_probe_poison=0x55`, after a clean unload/reload. Do not compare
runs with different source content, mode, or timing.

## Interpretation and stop conditions

- Empty, prefix, and full masks are not evidence of ordering by themselves.
- A non-prefix `touched` mask is a hardware lead: it is a high rung differing
  from poison while a lower rung still matches poison.
- A non-prefix `filled` mask is weaker because neutral chroma or picture data
  can equal the clear value. Treat it as a lead only.
- Stop and preserve both masks if a non-prefix repeats at the same rung across
  both poison values. Do not change delivery behavior or claim that hardware
  proved ordering from this run alone.
- Stop if the self-test is absent/fails, the diagnostic is not explicitly
  enabled, the mode changes, logs are incomplete, or the card is unstable.

The ladder resolution is one rung per odd 64th of the frame (32 rungs); it
cannot exclude reordering inside a rung or ordering across I420 planes.
Hardware execution and the decision about what it proves remain with the
parent/operator.
