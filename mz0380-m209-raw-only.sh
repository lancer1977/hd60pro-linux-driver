#!/bin/bash
# Bounded M209 op02/op08 raw-bank discriminator.
#
# This is intentionally a one-shot hardware experiment.  It loads the driver
# in raw-only mode, queues eight V4L2 I420 buffers, and stops at the earlier of
# eight exact frames or two seconds after START_STREAMING.  It never enables
# h264_probe, and it treats any op04 registration or second SET_VIC as failure.
set -u
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

KEEPLOADED=${KEEPLOADED:-0}
CAPTURE_LOG=$(mktemp /tmp/mz0380-m209-v4l2.XXXXXX.log) || exit 1
RAW_OUT=$(mktemp /tmp/mz0380-m209.XXXXXX.i420) || exit 1
CAPTURE_PID=""

cleanup() {
	if [ -n "$CAPTURE_PID" ] && kill -0 "$CAPTURE_PID" 2>/dev/null; then
		kill -INT "$CAPTURE_PID" 2>/dev/null || true
		wait "$CAPTURE_PID" 2>/dev/null || true
	fi
	if [ "$KEEPLOADED" != 1 ]; then
		./mz0380-live.sh unload >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT INT TERM

echo "Loading the isolated M209 topology (no stream is started by load)..."
H264PROBE=0 RAWBANKS=1 POLLDRAIN=0 VICFW=7 WINSEQ=1 WINBUFS=1 \
	OP6=0 PERSIST=0 NOSG=0 ./mz0380-live.sh load || exit 1

NODE=""
for name_file in /sys/class/video4linux/video*/name; do
	[ -r "$name_file" ] || continue
	[ "$(cat "$name_file")" = "mz0380 H.264" ] || continue
	node=${name_file%/name}
	NODE="/dev/${node##*/}"
	break
done
[ -n "$NODE" ] || { echo "M209 video node not found"; exit 1; }

echo "Capturing from $NODE; deadline starts when the sole op06 is logged."
v4l2-ctl -d "$NODE" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
	--stream-mmap=8 --stream-count=8 --stream-to="$RAW_OUT" \
	>"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!

# STREAMON includes the Windows-order 1.9 s settle.  Wait for the actual kick,
# then enforce the requested two-second live window rather than charging that
# setup time against the discriminator.
START_DEADLINE=$(( $(date +%s%3N) + 7000 ))
while ! dmesg | grep -q 'stream start: START_STREAMING(op 0x06) fired'; do
	if ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
		break
	fi
	if [ "$(date +%s%3N)" -ge "$START_DEADLINE" ]; then
		break
	fi
	sleep 0.02
done

if dmesg | grep -q 'stream start: START_STREAMING(op 0x06) fired'; then
	LIVE_DEADLINE=$(( $(date +%s%3N) + 2000 ))
	while kill -0 "$CAPTURE_PID" 2>/dev/null; do
		dmesg | grep -q 'M209 raw discriminator SUCCESS' && break
		[ "$(date +%s%3N)" -ge "$LIVE_DEADLINE" ] && break
		sleep 0.02
	done
fi

if kill -0 "$CAPTURE_PID" 2>/dev/null; then
	kill -INT "$CAPTURE_PID" 2>/dev/null || true
fi
wait "$CAPTURE_PID" 2>/dev/null || true
CAPTURE_PID=""

SETVIC_COUNT=$(dmesg | grep -c 'stream start: SET_VIC(')
OP04_COUNT=$(dmesg | grep -c 'H.264 probe registered dedicated window1 ring')
SUCCESS=0
if dmesg | grep -q 'M209 raw discriminator SUCCESS' &&
   [ "$SETVIC_COUNT" -eq 1 ] && [ "$OP04_COUNT" -eq 0 ]; then
	SUCCESS=1
fi

echo
echo "M209 result:"
dmesg | grep -E 'M209 raw completion|M209 raw discriminator|M209 raw V4L2 totals' | tail -20
echo "  SET_VIC count : $SETVIC_COUNT (required: 1)"
echo "  op04 count    : $OP04_COUNT (required: 0)"
echo "  raw capture   : $RAW_OUT ($(stat -c %s "$RAW_OUT") bytes)"
echo "  v4l2-ctl log  : $CAPTURE_LOG"

if [ "$SUCCESS" -eq 1 ]; then
	echo "PASS: eight consecutive 0x2f7600 writes covered all op02/op08 slots."
	exit 0
fi

echo "INCONCLUSIVE/FAIL: the bounded success oracle was not met; inspect the lines above."
exit 1
