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


ROW_STRIDE = 31


def fill_profile(buf):
    """Where does the picture stop and black begin?

    The tear guard assumes the card writes a frame in one ascending pass and
    puts its sentinel at the last dword, so the sentinel can only read as
    written once the transfer reached the end. That assumption dies if the card
    CLEARS the slot before filling it: the clear overwrites the poison at the
    end of the buffer, "landed" fires on the clear, and the copy takes a frame
    that is still being filled.

    If that is what happens, a partially black frame is not noise scattered
    through the picture - it is picture down to some row and black from there
    to the bottom. Measure the boundary instead of assuming it.

    Returns (first_black_row, black_rows, filled_fraction), or None if the
    frame has no black suffix at all.
    """
    y = buf[:Y_BYTES]
    row = HEIGHT - 1
    while row >= 0:
        line = y[row * WIDTH:(row + 1) * WIDTH:ROW_STRIDE]
        if max(line) > BLACK_MAX:
            break
        row -= 1
    black_rows = HEIGHT - 1 - row
    if black_rows == 0:
        return None
    return row + 1, black_rows, (row + 1) / HEIGHT


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
    chroma_neutral = (120 <= umin and umax <= 136
                      and 120 <= vmin and vmax <= 136)
    if 8 <= ymax <= 20 and chroma_neutral:
        return "STUDIO BLACK (Rec.709 limited-range black picture)"
    if ymax < 8 and chroma_neutral:
        # Y at zero with neutral chroma is full-range black. Someone wrote
        # those chroma bytes - unwritten memory reads 0, not 128 - but that
        # does not say WHO or why: a painted black picture and a buffer
        # cleared to black before filling are byte-identical. The partial-fill
        # profile is what separates them, so do not resolve it here.
        return "FULL-RANGE BLACK (Y=0, chroma 128 - painted black OR a cleared slot)"
    return ("MIXED (y_max=%d u=%d..%d v=%d..%d - neither zeros nor studio black)"
            % (ymax, umin, umax, vmin, vmax))


def main():
    src = sys.stdin.buffer
    frames = []
    partials = []
    idx = 0
    while True:
        buf = src.read(FRAME_BYTES)
        if len(buf) < FRAME_BYTES:
            break
        (mean, lo, hi), (umean, _, _), (vmean, _, _) = frame_stats(buf)
        blank = hi <= BLACK_MAX
        kind = classify_blank(buf) if blank else ""
        # The row profile is the expensive measurement, so it runs only on
        # frames that carry black at all - blanks and suspected partial fills.
        prof = fill_profile(buf) if lo <= BLACK_MAX else None
        partial = bool(prof) and not blank
        if partial:
            partials.append((idx, prof))
        note = ""
        if blank:
            note = "   <-- BLANK: " + kind
        elif partial:
            note = ("   <-- PARTIAL: filled to row %d/%d (%.0f%%), %d black rows"
                    % (prof[0], HEIGHT, prof[2] * 100, prof[1]))
        frames.append((mean, lo, hi, blank, kind))
        print("frame %5d  mean_y=%7.2f  min=%3d  max=%3d  u=%6.2f v=%6.2f%s"
              % (idx, mean, lo, hi, umean, vmean, note))
        idx += 1

    if not frames:
        print("no complete frames read", file=sys.stderr)
        return 1

    blanks = [i for i, f in enumerate(frames) if f[3]]
    print()
    print("partial fills     : %d" % len(partials))
    for pidx, prof in partials[:20]:
        print("  frame %5d filled to row %d/%d (%.1f%%), %d black rows to the bottom"
              % (pidx, prof[0], HEIGHT, prof[2] * 100, prof[1]))
    if partials:
        print()
        print("A partial fill that is picture on top and black to the BOTTOM is the")
        print("signature of copying a slot mid-write. Combined with black frames")
        print("whose chroma is 128, it says the card CLEARS the slot before filling")
        print("it: the clear wipes the end-of-frame poison, so 'landed' fires early")
        print("and the completeness test is measuring the wrong event.")
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
        full = any(k.startswith("FULL-RANGE BLACK") for k in kinds)
        if full:
            print("VERDICT: blank frames are full-range black - Y=0 with chroma 128.")
            print("Those chroma bytes were written by something; unwritten memory")
            print("reads 0. Whether the card painted a black picture or cleared the")
            print("slot before filling it is decided by the partial-fill count above:")
            print("partial fills that run to the BOTTOM mean a clear, and the")
            print("completeness test is firing on it. None means the card really is")
            print("emitting black pictures.")
        if not (zero or black or full):
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
