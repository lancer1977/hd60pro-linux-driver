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


# M239: the driver's own sentinel offsets and poison values.
#
# mz0380_raw_sentinel_offsets places four dwords at frame-4, frame-frame/16,
# frame-frame/4 and frame/2, and re-poisons them after every copy with 0xa5
# (bank 0) or 0x5a (bank 1). Three of those land inside the chroma planes, so a
# poison dword that survives into a delivered frame paints a small coloured
# blob at a FIXED position - which is what an isolated red dot looks like.
#
# Deterministic offsets make this an exact test rather than a heuristic: either
# those bytes hold poison in delivered frames or they do not.
POISON = (0xa5, 0x5a)


def sentinel_offsets(frame):
    return (frame - 4, frame - frame // 16, frame - frame // 4, frame // 2)


def poison_hits(buf):
    hits = []
    for off in sentinel_offsets(FRAME_BYTES):
        dword = buf[off:off + 4]
        if len(dword) == 4 and dword[0] in POISON and \
                dword[0] == dword[1] == dword[2] == dword[3]:
            hits.append((off, dword[0]))
    return hits


# M242: a fingerprint dense enough to identify a frame, cheap enough for 60 fps.
#
# The driver's raw_dup_content counter compares only each frame's first bytes,
# so it cannot tell a card repeating a whole picture from a driver re-taking a
# slot whose head happens to match. Those need opposite fixes. Sampling across
# the entire frame - luma and both chroma planes - makes a duplicate mean the
# whole picture repeated.
FINGERPRINT_STRIDE = 743


def fingerprint(buf):
    return hash(bytes(buf[::FINGERPRINT_STRIDE]))


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


def drift_slope(points):
    """Least-squares slope of mean luma against frame index.

    An eye cannot tell a slow ratchet from a stable picture over a couple of
    minutes, which is exactly the question left open after M233: the fast
    component of the brightening is gone, but "seems stable" is not a
    measurement. A slope in luma-per-1000-frames answers it, and at 60 fps
    1000 frames is under 17 seconds, so a few minutes of capture makes even a
    small drift unambiguous.
    """
    n = len(points)
    if n < 2:
        return 0.0
    sx = sum(i for i, _ in points)
    sy = sum(v for _, v in points)
    sxx = sum(i * i for i, _ in points)
    sxy = sum(i * v for i, v in points)
    denom = n * sxx - sx * sx
    if not denom:
        return 0.0
    return (n * sxy - sx * sy) / denom * 1000.0


def main():
    # Python block-buffers stdout when it is a pipe, so at roughly 55 bytes a
    # line the first flush needs about 74 lines - over a minute of capture
    # showing nothing at all, which reads exactly like a hung command. A live
    # trace is the whole point of --interval, so make it line buffered.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    # Piping this into head or tail closes the pipe early, and Python turns
    # that into a BrokenPipeError traceback that looks like a script fault.
    # Restore the default SIGPIPE behaviour so it just exits.
    try:
        import signal
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (ImportError, AttributeError, ValueError):
        pass
    src = sys.stdin.buffer
    # --interval N aggregates N frames per printed line instead of one line
    # per frame, so a long capture stays readable and the drift is visible.
    interval = 0
    if "--interval" in sys.argv:
        interval = int(sys.argv[sys.argv.index("--interval") + 1])
    bucket = []
    drift = []
    # 1800 frames is 30s at 60fps. A drift measurement does not need full rate
    # though - capturing at "-vf fps=2" moves 6 MB/s instead of 186 MB/s and
    # covers five minutes in 600 frames, which is a far better test of a slow
    # ratchet. Lower the threshold to match when capturing decimated.
    min_frames = 1800
    if "--min-frames" in sys.argv:
        min_frames = int(sys.argv[sys.argv.index("--min-frames") + 1])
    frames = []
    partials = []
    poisoned = []
    prints = []
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
        prints.append(fingerprint(buf))
        hits = poison_hits(buf)
        if hits:
            poisoned.append((idx, hits))
        drift.append((idx, mean))
        if interval:
            bucket.append((mean, lo, hi))
            if len(bucket) >= interval:
                bmean = sum(b[0] for b in bucket) / len(bucket)
                print("frames %6d-%6d  mean_y=%7.2f  min=%3d  max=%3d"
                      % (idx - len(bucket) + 1, idx, bmean,
                         min(b[1] for b in bucket),
                         max(b[2] for b in bucket)))
                bucket = []
            if note:
                print("  frame %5d %s" % (idx, note.strip()))
        else:
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
    # A drift slope needs enough frames to outlast scene motion. Below this
    # the windows overlap and the fit is dominated by whatever moved, which
    # is how a 60-frame sample reported "+47.9 per 1000 frames" while its own
    # first and last window were the same frames and differed by 0.00.
    DRIFT_MIN_FRAMES = min_frames
    # Run lengths of identical consecutive frames, and how far apart repeats of
    # the same picture sit. A slot-rotation fault repeats at the bank period;
    # a card genuinely re-sending a picture does not have to.
    runs = []
    cur = 1
    for a, b in zip(prints, prints[1:]):
        if a == b:
            cur += 1
        else:
            if cur > 1:
                runs.append(cur)
            cur = 1
    if cur > 1:
        runs.append(cur)
    dup_frames = sum(r - 1 for r in runs)

    last_seen = {}
    gaps = {}
    for i, fp in enumerate(prints):
        if fp in last_seen:
            g = i - last_seen[fp]
            gaps[g] = gaps.get(g, 0) + 1
        last_seen[fp] = i

    print("--- M242 duplicate frames (whole-frame fingerprint) ---")
    print("frames                     : %d" % len(prints))
    print("distinct pictures          : %d" % len(set(prints)))
    print("frames identical to previous: %d (%.1f%%)"
          % (dup_frames, 100.0 * dup_frames / max(len(prints) - 1, 1)))
    if runs:
        print("repeat run lengths         : max %d, mean %.2f over %d runs"
              % (max(runs), sum(runs) / len(runs), len(runs)))
    if gaps:
        top = sorted(gaps.items(), key=lambda kv: -kv[1])[:5]
        print("most common repeat spacing : %s"
              % ", ".join("%d frames x%d" % (g, n) for g, n in top))
    if not dup_frames:
        print("VERDICT: no whole-frame duplicates. Any raw_dup_content the driver")
        print("reports is head-collision only, not a repeated picture.")
    elif runs and max(runs) == 2 and gaps and \
            sorted(gaps.items(), key=lambda kv: -kv[1])[0][0] in (1, 4, 8):
        print("VERDICT: duplicates come in PAIRS at the bank period, which is the")
        print("driver re-delivering a slot the card has not rewritten - a")
        print("delivery-side fault, not the card repeating a picture.")
    else:
        print("VERDICT: duplicates are present. Read the run lengths and spacing")
        print("above: repeats at the 4-slot bank period point at slot rotation,")
        print("while long runs or irregular spacing point at the card genuinely")
        print("sending the same picture again.")
    print()
    print("--- M239 sentinel poison in delivered frames ---")
    print("frames carrying poison at a sentinel offset: %d of %d"
          % (len(poisoned), len(frames)))
    for pidx, hits in poisoned[:10]:
        print("  frame %5d: %s" % (pidx, ", ".join(
            "offset %d = 0x%02x" % (o, v) for o, v in hits)))
    if poisoned:
        print("VERDICT: the driver's own poison is reaching userspace. Three of")
        print("the four sentinel offsets sit in the chroma planes, so each one")
        print("paints a small coloured blob at a fixed position. The re-poison")
        print("after a copy is landing in a frame that is then delivered.")
    else:
        print("VERDICT: no poison at any sentinel offset - which is the expected")
        print("result since M239, not evidence about anything else. If coloured")
        print("dots are still VISIBLE with this at zero, they are not the driver")
        print("writing sentinels into frames; compare the same camera through")
        print("another capture path before blaming this driver.")
    print()
    print("--- M235 brightness drift ---")
    if len(drift) < DRIFT_MIN_FRAMES:
        print("not enough frames: %d, need %d (about %d seconds at 60 fps)."
              % (len(drift), DRIFT_MIN_FRAMES, DRIFT_MIN_FRAMES // 60))
        print("NO DRIFT VERDICT from this capture - a short sample of a moving")
        print("scene produces a slope that is scene motion, not brightening.")
    else:
        win = len(drift) // 4
        first = sum(v for _, v in drift[:win]) / win
        last = sum(v for _, v in drift[-win:]) / win
        slope = drift_slope(drift)
        print("mean luma first/last quarter : %.2f -> %.2f  (delta %+.2f, %d frames each)"
              % (first, last, last - first, win))
        print("slope                        : %+.3f luma per 1000 frames" % slope)

        # A slope alone cannot tell a steady ratchet from a picture that sits
        # flat and then jumps once. Fitting a line to a step function reports a
        # confident drift rate for something that is not drifting at all, which
        # is exactly what this script did before: it called a trace "drifting
        # at +7.091 per 1000 frames" while the mean held to within 0.03 for two
        # minutes either side of a single 2.6-luma step.
        nwin = 20
        size = max(len(drift) // nwin, 1)
        wins = [sum(v for _, v in drift[i:i + size]) / len(drift[i:i + size])
                for i in range(0, len(drift) - size + 1, size)]
        steps = [abs(b - a) for a, b in zip(wins, wins[1:])]
        biggest = max(steps) if steps else 0.0
        spread = max(wins) - min(wins) if wins else 0.0
        quiet = sorted(steps)[len(steps) // 2] if steps else 0.0

        print("largest window-to-window step: %.2f luma (total spread %.2f, median step %.2f)"
              % (biggest, spread, quiet))
        if spread < 0.5:
            print("DRIFT VERDICT: flat. No measurable brightness change.")
        elif biggest > spread * 0.4:
            print("DRIFT VERDICT: STEPPED, not drifting. The picture holds steady")
            print("and changes in discrete jumps - the biggest is %.2f luma of a"
                  % biggest)
            print("%.2f total spread, with a median window-to-window move of %.2f."
                  % (spread, quiet))
            print("That is something adjusting once per scene change, which is what")
            print("auto-exposure does. Ignore the slope above; a line fitted to a")
            print("step function reports a drift rate that nothing is doing.")
        else:
            print("DRIFT VERDICT: continuous drift at %+.3f per 1000 frames. The"
                  % slope)
            print("change is spread across the capture rather than concentrated in")
            print("a step, so it is a ratchet. Correlate with the rearms counter.")
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
