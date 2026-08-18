#!/bin/bash
# M55: real HDMI capture. The normal path requires a coherent receiver lock
# and uses BT1120 -> SSM -> H.264. NOSG=1 remains available only as an
# explicit card-side fake-frame/DMA diagnostic.
#
# M54 found one: a digital microscope drives the wire and the MST3367
# reached lock (detect 0x55=0xa3 LOCKED, htot=0x898=2200 = the horizontal
# total of 1080p). The DSLR still stays dark, which is the EDID blocker and
# is not what this script tests.
#
# Lock can be intermittent while the source settles, so timings are queried
# throughout the source power-cycle window. Unmatched/torn counters are never
# coerced to a guessed video mode.
#
# Decision table:
#   - "MST3367 signal:" followed by valid H.264 NAL units with NOSG=0 -> real
#     capture works end-to-end.
#   - query-dv-timings keeps returning ENOLCK -> the source dropped lock;
#     power-cycle it, or raise signal_poll_ms.
#   - timings fine but 0 bytes captured -> the encoder armed but no frame
#     completed: compare with the nosg path (m50), which does deliver, and
#     look for 'stream start' + enc_stat lines in dmesg.
#   - frames arrive but decode to garbage -> the per-mode receiver config
#     (M10 step 2, still unrecovered) is wrong for this source's mode.
set -u
cd "$(dirname "$0")"

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

find_mz_video_node() {
	local name_file node

	for name_file in /sys/class/video4linux/video*/name; do
		[ -r "$name_file" ] || continue
		[ "$(cat "$name_file")" = "mz0380 H.264" ] || continue
		node=${name_file%/name}
		echo "/dev/${node##*/}"
		return 0
	done
	return 1
}

VIDEO_NODE=

# M60: leave the machine as we found it. An insmod'd module left behind kept
# the card armed between runs and made the next run's "reload" a no-op against
# stale code.
cleanup() {
	local node=${VIDEO_NODE:-}

	[ -n "$node" ] || node=$(find_mz_video_node 2>/dev/null || true)
	[ -z "$node" ] || fuser -k "$node" 2>/dev/null
	rmmod mz0380 2>/dev/null && echo "(module unloaded)"
}
trap cleanup EXIT INT TERM
# M60: the NOSG diagnostic respawns the encoder for every frame and the card
# wedges after roughly 8-18 spawns. Real capture keeps one encoder alive for
# the whole stream, but retain the conservative default for comparable tests.
FRAMES=${1:-6}
LOCKWAIT=${2:-45}
# Real capture is the default. NOSG=1 deliberately bypasses HDMI/BT1120 and
# must never be used to decide whether source pixels are reaching the card.
NOSG=${NOSG:-0}
make >/dev/null || { echo "build failed"; exit 1; }

VIDEO_NODE=$(find_mz_video_node 2>/dev/null || true)
[ -z "$VIDEO_NODE" ] || fuser -k "$VIDEO_NODE" 2>/dev/null
rmmod mz0380 2>/dev/null
VIDEO_NODE=
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C
insmod ./mz0380.ko firmware_upload=1 dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 dma_iova_remap=1 aic_on=1 \
	stream_nosg="$NOSG" force_timings=0 signal_poll_ms=4000 \
	set_buf_opcode="${SETBUF:-2}" vic_fw="${VICFW:-5}" \
	|| { echo "insmod failed"; exit 1; }
echo "waiting for firmware upload + boot..."
sleep 25

VIDEO_NODE=$(find_mz_video_node) || {
	echo "mz0380 video node did not register"
	exit 1
}
echo "using $VIDEO_NODE"

if ! dmesg | grep -q '4 stream buffers x'; then
	echo "DMA stream buffers were not armed; capture cannot work."
	dmesg | grep -E 'IOVA|IOMMU|stream buffer|DMA setup' | tail -20
	exit 1
fi

# Property 201 is persistent card state. Do not let a previous Windows/Linux
# session leave the VIC routed away from the only physical HDMI connector.
v4l2-ctl -d "$VIDEO_NODE" --set-input=0 || {
	echo "could not select the HDMI input route"
	exit 1
}

# The source transmits around a plug/power event. QUERY_DV_TIMINGS itself must
# run during that window; the old script blocked in procfs watch and queried
# only after the transient lock had already disappeared.
if [ "$NOSG" = 1 ]; then
	echo "=== 1. NOSG=1: skipping receiver lock (card-side diagnostic only) ==="
else
	echo "=== 1. HPD edge, then poll timings while YOU power-cycle the source ==="
	dmesg -C
	echo "hpd 2 2000" > /proc/mz0380-hdmi
	echo
	echo "   >>> POWER-CYCLE (or re-plug) THE SOURCE NOW - you have ${LOCKWAIT}s <<<"
	echo
	TIMINGS=/tmp/mz0380-m55-timings.txt
	TIMING_ERR=/tmp/mz0380-m55-timings.err
	rm -f "$TIMINGS" "$TIMING_ERR"
	LOCKED=0
	DEADLINE=$((SECONDS + LOCKWAIT))
	while [ "$SECONDS" -lt "$DEADLINE" ]; do
		if v4l2-ctl -d "$VIDEO_NODE" --query-dv-timings >"$TIMINGS" 2>"$TIMING_ERR"; then
			LOCKED=1
			break
		fi
		sleep 0.2
	done

	if [ "$LOCKED" -ne 1 ]; then
		echo "No coherent HDMI timing was locked during the ${LOCKWAIT}s window."
		cat "$TIMING_ERR"
		dmesg | grep -E "MST3367|detect|lock|timing|unmatched" | tail -40
		exit 2
	fi

	echo "=== 1b. coherent mode detected ==="
	cat "$TIMINGS"
	dmesg | grep -E "MST3367 signal|coherent but unsupported|timing" | tail -20
	# M70: show the EDID push + read-back verdict from bring-up before any
	# later dmesg -C erases it. READ-BACK OK = a real store holds our EDID.
	echo "--- EDID delivery verdict ---"
	dmesg | grep -iE "EDID push|EDID READ-BACK|EDID read-back|EDID write failed|EDID mux" | tail -5
fi

echo
echo "=== 2. capture $FRAMES frames - POWER-CYCLE THE SOURCE ONCE STREAMING STARTS ==="
# M58: the nosg path delivers raw NV12, not H.264 - name the file for what it
# actually holds so the check below is not nonsense (the M57 run "failed"
# ffprobe purely because raw NV12 was written to a .h264 name).
if [ "$NOSG" = 1 ]; then CAP=/tmp/cap-m55.nv12; else CAP=/tmp/cap-m55.h264; fi
rm -f "$CAP"
dmesg -C

# M66: the detection burst is long gone by the time the encoder is armed.
# Arming costs ~4s (SET_VIC 2s + SET_ENC_PARAMS 2s), and the source only
# transmits for roughly 300ms after a power-cycle, so a capture started
# "while lock is live" is guaranteed to record silence. The burst has to
# arrive AFTER the encoder is running - hence the prompt below, and a window
# long enough to cycle the source two or three times.
#
# The concurrent watch is read-only and correlates what the receiver saw with
# what the encoder produced: without it, "0 bytes" cannot distinguish "the
# source never transmitted" from "it did and the card dropped the frames".
CAPWAIT=${CAPWAIT:-60}

# M68: give the source a REASON to transmit while the encoder is armed.
#
# The previous run answered the open question: the watch reported 55=03
# no-lock, unchanged, for the whole 60s window - so the card was not dropping
# frames, the source was simply silent. HPD is raised once during bring-up and
# then never moves, so a source that has already given up has no reason to
# retry. M48/M49 proved this source reacts to a hotplug EDGE, so pulse HPD
# repeatedly across the capture window and watch between the pulses.
#
# The writes are serialised on purpose: /proc/mz0380-hdmi runs "watch" inline,
# so a concurrent hpd write would just block behind it. Alternating in one
# background shell gives edges at roughly t+7s, t+21s and t+35s.
# M74: WATCH=0 runs the capture with NO concurrent receiver I2C at all.
# The watch samples the MST3367 every 250ms and, until M74, re-armed
# auto-position on every lock transition - i.e. the diagnostic was poking
# acquisition registers on a receiver that was mid-capture. Writes are now
# suppressed while streaming, but a clean control run with zero I2C traffic
# is still the only way to prove the observer is not the problem.
WATCH=${WATCH:-1}
WATCH_PID=
if [ "$WATCH" = 1 ]; then
	( sleep 6
	  for _ in 1 2 3; do
		echo "hpd 2 1000"  > /proc/mz0380-hdmi 2>/dev/null
		echo "watch 12"    > /proc/mz0380-hdmi 2>/dev/null
	  done ) &
	WATCH_PID=$!
else
	echo "   (WATCH=0: no HPD pulses, no receiver polling during capture)"
fi

echo
echo "   >>> The driver now pulses HPD 3x during this window by itself.    <<<"
echo "   >>> ALSO power-cycle the source a couple of times over ${CAPWAIT}s   <<<"
echo "   >>> - whichever makes it transmit, the encoder is already armed.  <<<"
echo

timeout "$CAPWAIT" v4l2-ctl -d "$VIDEO_NODE" --stream-mmap --stream-count="$FRAMES" \
	--stream-to="$CAP"
SZ=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
[ -z "$WATCH_PID" ] || wait "$WATCH_PID" 2>/dev/null
echo "--- captured $SZ bytes ---"

echo "--- did the receiver see the source while the encoder was running? ---"
dmesg | grep -E "detect 55=|MST3367 signal" | tail -12

echo "=== 3. what did the card do? ==="
dmesg | grep -E "stream start|stream stop|frame token|enc|no HDMI signal" | head -20
# M70: the per-buffer poison scan is the "did H.264 bytes land without a
# completion" measurement - it has been printed at every stop and filtered
# out by the grep above this whole time.
echo "--- MST3367 output stage (is the receiver clocking BT1120 out?) ---"
dmesg | grep -E "output stage" | tail -6
echo "--- buffer poison scan (pages touched = card wrote data) ---"
dmesg | grep -E "stop buf\[" | tail -8
dmesg | grep -E "encoder spawns|entering the range|SET_AIC\(on=0\)" | tail -3

# M59: a wall of SET_VIC ret=-110 is the known wedge, not a capture bug. The
# mailbox stops answering after enough encoder spawns and ONLY a mains-off
# cold boot clears it - rmmod/insmod and firmware re-upload do not. Say so
# here, because every later result in this run is meaningless once it hits.
if [ "$(dmesg | grep -c 'ret=-110')" -gt 2 ]; then
	echo
	echo "!!! CARD WEDGED: SET_VIC is timing out (-110). The mailbox is dead."
	echo "!!! Shut down, switch the PSU off at the mains, then retry."
	echo "!!! uptime: $(uptime -p) - a cold boot resets this counter."
fi
if [ "$SZ" -gt 0 ] && [ "$NOSG" = 1 ]; then
	echo "=== 4. NOSG diagnostic frame (card-generated; not HDMI pixels) ==="
	echo "The NV12 payload proves only the fake-frame encoder/DMA path."
	head -c 64 "$CAP" | od -An -tx1
elif [ "$SZ" -gt 0 ]; then
	echo "=== 4. does it look like H.264? (expect 00 00 00 01 NAL starts) ==="
	head -c 32 "$CAP" | od -An -tx1
	command -v ffprobe >/dev/null && ffprobe -v error -show_streams "$CAP" 2>&1 | head -20
fi
