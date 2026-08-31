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


BANDS = 18


def band_profile(buf):
    """Coarse luma profile down the frame, to see WHAT the black sits under.

    A constant fill boundary is not a race. 1920x540 I420 is 1555200 bytes,
    exactly half of a 1920x1080 I420 frame, so a half-height frame written into
    a full-height buffer lands as: 540 rows of picture, then its two chroma
    planes read as ~270 rows of flat ~128, then unwritten zeros. That predicts
    a FLAT MID-GREY BAND between the picture and the black, which a partially
    completed transfer would not produce - it would leave the previous frame's
    pixels or nothing at all there.

    So print the bands and let them decide, rather than inferring a mechanism
    from the boundary row alone.
    """
    y = buf[:Y_BYTES]
    rows_per = HEIGHT // BANDS
    out = []
    for b in range(BANDS):
        r0 = b * rows_per
        r1 = r0 + rows_per
        chunk = y[r0 * WIDTH:r1 * WIDTH:ROW_STRIDE]
        out.append((r0, r1, sum(chunk) / len(chunk), min(chunk), max(chunk)))
    return out


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
    # M234: limited-range video lives in 16..235 (luma) and 16..240 (chroma).
    # Anything outside that says the payload is FULL range, and the node
    # currently advertises LIM_RANGE unconditionally - which makes every player
    # expand it a second time and show a brighter, harsher picture.
    y_lo, y_hi = 255, 0
    c_lo, c_hi = 255, 0
    y_below16 = y_above235 = y_total = 0
    first_partial_buf = None
    idx = 0
    while True:
        buf = src.read(FRAME_BYTES)
        if len(buf) < FRAME_BYTES:
            break
        (mean, lo, hi), (umean, ulo, uhi), (vmean, vlo, vhi) = frame_stats(buf)
        y_lo = min(y_lo, lo)
        y_hi = max(y_hi, hi)
        c_lo = min(c_lo, ulo, vlo)
        c_hi = max(c_hi, uhi, vhi)
        ysample = buf[:Y_BYTES:STRIDE]
        y_total += len(ysample)
        y_below16 += sum(1 for b in ysample if b < 16)
        y_above235 += sum(1 for b in ysample if b > 235)
        blank = hi <= BLACK_MAX
        kind = classify_blank(buf) if blank else ""
        # The row profile is the expensive measurement, so it runs only on
        # frames that carry black at all - blanks and suspected partial fills.
        prof = fill_profile(buf) if lo <= BLACK_MAX else None
        partial = bool(prof) and not blank
        if partial:
            partials.append((idx, prof))
            if first_partial_buf is None:
                first_partial_buf = buf
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
        pidx, _ = partials[0]
        print("luma band profile of the first partial frame (frame %d):" % pidx)
        for r0, r1, mean, lo, hi in band_profile(first_partial_buf):
            flat = "  FLAT" if hi - lo <= 4 else ""
            print("  rows %4d-%4d  mean=%7.2f  min=%3d  max=%3d%s"
                  % (r0, r1 - 1, mean, lo, hi, flat))
        print()
        print("A flat band near 128 between the picture and the black means the")
        print("card wrote a HALF-HEIGHT frame: its chroma planes are being read as")
        print("luma rows, and the rest of the buffer was never written. A band that")
        print("holds the previous frame's picture instead means a partial transfer.")
        print()
        print("A partial fill that is picture on top and black to the BOTTOM is the")
        print("signature of copying a slot mid-write. Combined with black frames")
        print("whose chroma is 128, it says the card CLEARS the slot before filling")
        print("it: the clear wipes the end-of-frame poison, so 'landed' fires early")
        print("and the completeness test is measuring the wrong event.")
    means = [f[0] for f in frames]
    print()
    print("frames read       : %d" % len(frames))
    print()
    print("--- M234 range check (limited range is luma 16..235, chroma 16..240) ---")
    print("luma  min/max     : %d / %d" % (y_lo, y_hi))
    print("chroma min/max    : %d / %d" % (c_lo, c_hi))
    print("luma below 16     : %d of %d sampled (%.3f%%)"
          % (y_below16, y_total, 100.0 * y_below16 / max(y_total, 1)))
    print("luma above 235    : %d of %d sampled (%.3f%%)"
          % (y_above235, y_total, 100.0 * y_above235 / max(y_total, 1)))
    if y_hi > 235 or y_lo < 16:
        print("RANGE VERDICT: payload uses values OUTSIDE 16..235, so it is FULL")
        print("range. The node advertises V4L2_QUANTIZATION_LIM_RANGE, so every")
        print("player expands it again - brighter and harsher than the source.")
    else:
        print("RANGE VERDICT: payload stays inside 16..235, consistent with the")
        print("advertised LIM_RANGE. Do not change the quantization on this")
        print("evidence; look for the brightness difference elsewhere.")
    print()
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
