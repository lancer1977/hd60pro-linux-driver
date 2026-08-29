#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""M226: is the driver delivering black frames?

The flicker is flashing - intermittent black or blank pictures with a correct
one in between - and M225 has now been falsified by its own counter: the H.264
placeholder was withheld 19 times and the flashing was unchanged. The same run
recorded 0 dropped, 0 torn, 0 multi-slot and 0 placeholders delivered, so the
black frames are arriving through the ordinary delivery path as ordinary
frames.

That leaves a possibility nothing has tested. `mz0380_raw_probe_frame_landed`
decides a slot is complete by checking that the poison sentinels were
overwritten. A blank frame overwrites them exactly as a real one does, so the
test cannot tell a picture from a black rectangle - it only proves the card
wrote something. If the card emits an occasional blank into the raw bank, it is
delivered as a good frame, and it looks like this.

Reads I420 frames from stdin, so nothing hits the disk:

  ffmpeg -f v4l2 -input_format yuv420p -video_size 1920x1080 \\
         -i /dev/video0 -frames:v 600 -f rawvideo - 2>/dev/null \\
    | ./scripts/mz0380-m226-blackframe-scan.py

Prints per-frame luma and, at the end, the thing being asked: whether any
delivered frame was blank, and how the blanks were spaced.
"""
import sys

WIDTH, HEIGHT = 1920, 1080
Y_BYTES = WIDTH * HEIGHT
FRAME_BYTES = Y_BYTES * 3 // 2
# A prime stride samples the whole plane without aligning to any row or block
# boundary, and keeps this fast enough to consume 60 fps in pure Python.
STRIDE = 997
# Rec.709 limited-range black is 16, not 0. Studio black plus a margin.
BLACK_MAX = 20


def frame_stats(buf):
    y = buf[:Y_BYTES]
    sample = y[::STRIDE]
    n = len(sample)
    total = sum(sample)
    return total / n, min(sample), max(sample)


def main():
    src = sys.stdin.buffer
    frames = []
    idx = 0
    while True:
        buf = src.read(FRAME_BYTES)
        if len(buf) < FRAME_BYTES:
            break
        mean, lo, hi = frame_stats(buf)
        blank = hi <= BLACK_MAX
        frames.append((mean, lo, hi, blank))
        print("frame %5d  mean_y=%7.2f  min=%3d  max=%3d%s"
              % (idx, mean, lo, hi, "   <-- BLANK" if blank else ""))
        idx += 1

    if not frames:
        print("no complete frames read", file=sys.stderr)
        return 1

    blanks = [i for i, f in enumerate(frames) if f[3]]
    means = [f[0] for f in frames]
    print()
    print("frames read       : %d" % len(frames))
    print("luma mean range   : %.2f .. %.2f" % (min(means), max(means)))
    print("blank frames      : %d (max luma <= %d)" % (len(blanks), BLACK_MAX))
    if blanks:
        gaps = [b - a for a, b in zip(blanks, blanks[1:])]
        print("blank indices     : %s%s"
              % (blanks[:40], " ..." if len(blanks) > 40 else ""))
        if gaps:
            print("spacing (frames)  : min=%d max=%d mean=%.1f"
                  % (min(gaps), max(gaps), sum(gaps) / len(gaps)))
        print()
        print("VERDICT: the card is writing blank frames into the raw bank and")
        print("the sentinel 'landed' test passes them through as good frames.")
        print("The flashing is content the driver was handed, not a delivery bug.")
    else:
        print()
        print("VERDICT: no blank frame was delivered in this capture. If the")
        print("flashing was visible during it, the black frames are NOT coming")
        print("from the driver's payload - look at the consumer or at frame")
        print("pacing next, and do not build a fix on this file's silence.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
