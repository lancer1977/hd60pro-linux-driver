#!/bin/bash
# Zero-spawn source check: is the HDMI source actually 1920x1080p60 right now?
#
# Two M210 starts were "spent" against a source that had settled at 1080p30.
# Both were rejected by the raw-only guard before any card command, so neither
# cost an encoder spawn - but each cost a load/unload cycle and a wrong guess
# about what the run had proved.  This answers the question first.
#
# It is safe to run as often as you like: the receiver measurement runs through
# QUERY_DV_TIMINGS, which mz0380_query_signal() serves live while the pipeline
# is stopped.  No SET_VIC is sent, so the 8-18 spawn budget is untouched.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

WANT_W=${WANT_W:-1920}
WANT_H=${WANT_H:-1080}
WANT_FPS=${WANT_FPS:-60}
KEEPLOADED=${KEEPLOADED:-0}
LOADED=0

cleanup() {
	[ "$LOADED" = 1 ] && [ "$KEEPLOADED" != 1 ] && \
		timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1
	return 0
}
trap cleanup EXIT INT TERM

if [ -e /dev/video0 ] && grep -q '^mz0380 ' /proc/modules 2>/dev/null; then
	echo "using the already-loaded module"
else
	# Plain load: no probe knobs, nothing armed, nothing started.
	./mz0380-live.sh load >/dev/null 2>&1 || { echo "load failed"; exit 1; }
	LOADED=1
fi

NODE=""
for name_file in /sys/class/video4linux/video*/name; do
	[ -r "$name_file" ] || continue
	# M240 renamed the node; accept the old name so this script works
	# against a module built before or after that change.
	case "$(cat "$name_file")" in
	"HD60 Pro HDMI capture"|"mz0380 H.264") ;;
	*) continue ;;
	esac
	node=${name_file%/name}
	NODE="/dev/${node##*/}"
	break
done
[ -n "$NODE" ] || { echo "video node not found"; exit 1; }

echo "--- QUERY_DV_TIMINGS on $NODE (live receiver read, no SET_VIC) ---"
v4l2-ctl -d "$NODE" --query-dv-timings 2>&1 | sed 's/^/  /'

echo
echo "--- what the receiver measured ---"
dmesg | grep -E 'MST3367 signal|live input' | tail -3 | sed 's/^/  /'

# hperiod is the line rate in hundreds of Hz and vperiod is Hz x10, so a
# 1080p60 source reads hper=674 vper=59x and 1080p30 reads hper=337 vper=299.
SIG=$(dmesg | grep 'MST3367 signal' | tail -1)
VPER=$(printf '%s\n' "$SIG" | sed -n 's/.*vper=\([0-9]\+\).*/\1/p')
GEOM=$(printf '%s\n' "$SIG" | sed -n 's/.*MST3367 signal: \([0-9]\+x[0-9]\+[pi]\).*/\1/p')

echo
if [ -z "$VPER" ]; then
	echo "NO MEASUREMENT: the receiver reported no lock at all."
	echo "Check the cable, the source's power state, and that the source is"
	echo "not asleep.  R55 in the log above is the receiver's lock byte."
	exit 1
fi

HZ=$(( (VPER + 5) / 10 ))
echo "measured: $GEOM at approximately ${HZ} Hz (vper=$VPER)"

if [ "$GEOM" = "${WANT_W}x${WANT_H}p" ] && [ "$HZ" = "$WANT_FPS" ]; then
	echo "PASS: the source is ${WANT_W}x${WANT_H}p${WANT_FPS}; a bounded run may be spent."
	exit 0
fi

echo "FAIL: the bounded raw discriminators require ${WANT_W}x${WANT_H}p${WANT_FPS}."
echo "Change the source's output mode and re-run this check.  Do not spend a"
echo "start until it passes - the guard will reject it anyway."
exit 1
