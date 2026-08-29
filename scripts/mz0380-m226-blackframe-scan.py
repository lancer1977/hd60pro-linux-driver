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
C_BYTES = Y_BYTES // 4
FRAME_BYTES = Y_BYTES * 3 // 2
# A prime stride samples the whole plane without aligning to any row or block
# boundary, and keeps this fast enough to consume 60 fps in pure Python.
STRIDE = 997
# Rec.709 limited-range black is 16, not 0. Studio black plus a margin.
BLACK_MAX = 20


def plane_stats(plane):
    sample = plane[::STRIDE]
    return sum(sample) / len(sample), min(sample), max(sample)


def frame_stats(buf):
    y = buf[:Y_BYTES]
    u = buf[Y_BYTES:Y_BYTES + C_BYTES]
    v = buf[Y_BYTES + C_BYTES:Y_BYTES + 2 * C_BYTES]
    return plane_stats(y), plane_stats(u), plane_stats(v)


def classify_blank(buf):
    """Distinguish a black PICTURE from memory nothing wrote.

    Rec.709 studio black is Y=16 with both chroma planes at 128. A frame that
    is zero everywhere is not a picture at all - it is a slot the card never
    filled, which the poison sentinel test cannot detect because zeros differ
    from poison exactly the way real data does. The two call for opposite
    fixes, so name which one this is instead of reporting "blank".
    """
    if not any(buf):
        return "ALL-ZERO (slot never written; the landed test let it through)"
    y = buf[:Y_BYTES]
    u = buf[Y_BYTES:Y_BYTES + C_BYTES]
    v = buf[Y_BYTES + C_BYTES:Y_BYTES + 2 * C_BYTES]
    ymax = max(y[::STRIDE])
    umin, umax = min(u[::STRIDE]), max(u[::STRIDE])
    vmin, vmax = min(v[::STRIDE]), max(v[::STRIDE])
    if 8 <= ymax <= 20 and 120 <= umin and umax <= 136 \
            and 120 <= vmin and vmax <= 136:
        return "STUDIO BLACK (a real black picture the card painted)"
    return ("MIXED (y_max=%d u=%d..%d v=%d..%d - neither zeros nor studio black)"
            % (ymax, umin, umax, vmin, vmax))


def main():
    src = sys.stdin.buffer
    frames = []
    idx = 0
    while True:
        buf = src.read(FRAME_BYTES)
        if len(buf) < FRAME_BYTES:
            break
        (mean, lo, hi), (umean, _, _), (vmean, _, _) = frame_stats(buf)
        blank = hi <= BLACK_MAX
        kind = classify_blank(buf) if blank else ""
        frames.append((mean, lo, hi, blank, kind))
        print("frame %5d  mean_y=%7.2f  min=%3d  max=%3d  u=%6.2f v=%6.2f%s"
              % (idx, mean, lo, hi, umean, vmean,
                 "   <-- BLANK: " + kind if blank else ""))
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
        if len(gaps) >= 2:
            print("spacing (frames)  : min=%d max=%d mean=%.1f"
                  % (min(gaps), max(gaps), sum(gaps) / len(gaps)))
        elif gaps:
            # One gap is a distance, not a period. Saying "spacing min=max"
            # invites reading a cadence into a sample of two.
            print("spacing (frames)  : %d (single gap - NOT evidence of a period)"
                  % gaps[0])
        kinds = sorted({frames[i][4] for i in blanks})
        for k in kinds:
            print("blank kind        : %s" % k)
        print()
        zero = any(k.startswith("ALL-ZERO") for k in kinds)
        black = any(k.startswith("STUDIO BLACK") for k in kinds)
        if zero:
            print("VERDICT: at least one delivered frame was ALL ZEROS, which is not")
            print("a picture. The card never wrote that slot, and the sentinel test")
            print("cannot tell: it only checks that the poison is gone, and zeros")
            print("differ from poison exactly the way real data does. The fix belongs")
            print("in the completeness test, not in the content.")
        if black:
            print("VERDICT: at least one delivered frame was a real studio-black")
            print("picture (Y=16, chroma 128). The card painted it, the driver")
            print("forwarded it faithfully, and the question moves to why the card")
            print("emits blanks - not to the delivery path.")
        if not (zero or black):
            print("VERDICT: blank frames found, but they match neither zeros nor")
            print("studio black. Read the MIXED line above before theorising.")
    else:
        print()
        print("VERDICT: no blank frame was delivered in this capture. If the")
        print("flashing was visible during it, the black frames are NOT coming")
        print("from the driver's payload - look at the consumer or at frame")
        print("pacing next, and do not build a fix on this file's silence.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
