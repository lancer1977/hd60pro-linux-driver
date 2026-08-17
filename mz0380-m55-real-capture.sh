#!/bin/bash
# M55: REAL capture - the first attempt at the non-fake path with a source
# that actually transmits.
#
# M54 found one: a digital microscope drives the wire and the MST3367
# reached lock (detect 0x55=0xa3 LOCKED, htot=0x898=2200 = the horizontal
# total of 1080p). The DSLR still stays dark, which is the EDID blocker and
# is not what this script tests.
#
# Two behaviours seen there shape this run: lock is intermittent while the
# source settles (so detect is now polled, signal_poll_ms), and the vperiod
# counter reads saturated so no mode matches (so force_timings=1 streams a
# htotal=2200 lock as 1080p60).
#
# Decision table:
#   - "MST3367 signal:" or "force_timings: streaming as" then frames in the
#     capture file -> REAL CAPTURE WORKS end-to-end. Everything after this
#     is quality, not feasibility.
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
FRAMES=${1:-60}
LOCKWAIT=${2:-45}
make >/dev/null || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
insmod ./mz0380.ko firmware_upload=1 dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 dma_iova_remap=1 aic_on=1 \
	stream_nosg=0 force_timings=1 signal_poll_ms=4000 \
	|| { echo "insmod failed"; exit 1; }
echo "waiting for firmware upload + boot..."
sleep 25

# M55b: the source transmits around a plug/power event, not continuously -
# the one lock ever seen arrived ~30 s into a watch, exactly when the source
# was power-cycled, and lasted a single sample. So the detect window has to
# be OPEN while you cycle it, not before.
echo "=== 1. HPD edge, then watch while YOU power-cycle the source ==="
dmesg -C
echo "hpd 2 2000" > /proc/mz0380-hdmi
echo
echo "   >>> POWER-CYCLE (or re-plug) THE SOURCE NOW - you have ${LOCKWAIT}s <<<"
echo
echo "watch $LOCKWAIT" > /proc/mz0380-hdmi
dmesg | grep -E "detect|LOCKED|MATCHED|no table entry" | tail -25

echo
echo "=== 1b. can the driver name the mode? ==="
dmesg -C
v4l2-ctl -d /dev/video0 --query-dv-timings
dmesg | grep -E "MST3367 signal|force_timings|locked but unmatched"

echo
echo "=== 2. capture $FRAMES frames (power-cycle the source again if needed) ==="
rm -f /tmp/cap-m55.h264
dmesg -C
timeout 40 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count="$FRAMES" \
	--stream-to=/tmp/cap-m55.h264
SZ=$(stat -c %s /tmp/cap-m55.h264 2>/dev/null || echo 0)
echo "--- captured $SZ bytes ---"

echo "=== 3. what did the card do? ==="
dmesg | grep -E "stream start|stream stop|frame token|enc|no HDMI signal" | head -20
if [ "$SZ" -gt 0 ]; then
	echo "=== 4. does it look like H.264? (expect 00 00 00 01 NAL starts) ==="
	head -c 32 /tmp/cap-m55.h264 | od -An -tx1
	command -v ffprobe >/dev/null && ffprobe -v error -show_streams /tmp/cap-m55.h264 2>&1 | head -20
fi
