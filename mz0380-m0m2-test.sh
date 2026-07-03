#!/usr/bin/env bash
#
# Staged bring-up test for the RE-corrected BAR0 mailbox + firmware model.
#
#   Stage M0 (read-only, safe): load probe-safe, dump the BAR0 mailbox and
#   extended BAR5 snapshot so we can see whether the low BAR0 mailbox region
#   responds pre-boot (i.e. is NOT stuck at 0xffffffff). This validates the
#   RE finding that the mailbox lives in BAR0 before committing to an upload.
#
#   Stage M2 (writes to the card): flip firmware_upload=1 and watch the
#   BEGIN/COMMIT mailbox handshake + boot result. Only run after M0 looks sane.
#
# Usage:
#   sudo ./mz0380-m0m2-test.sh m0     # read-only observability
#   sudo ./mz0380-m0m2-test.sh m2     # attempt firmware upload
#
set -u
cd "$(dirname "$0")"
MOD=./mz0380.ko
STAGE="${1:-m0}"

if [[ $EUID -ne 0 ]]; then
	echo "run as root (sudo $0 $STAGE)" >&2
	exit 1
fi

unload() { rmmod mz0380 2>/dev/null || true; }
load_deps() {
	# insmod does not resolve dependencies; pull in the v4l2/vb2/alsa stack first
	modprobe -a videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc \
		v4l2-dv-timings snd snd-pcm || { echo "modprobe deps failed" >&2; exit 1; }
}
dump() {
	echo "===== /proc/mz0380-state ====="
	cat /proc/mz0380-state 2>/dev/null
	echo "===== /proc/mz0380-snapshot (profile 6) ====="
	cat /proc/mz0380-snapshot 2>/dev/null
}

case "$STAGE" in
m0)
	echo "[M0] building..."; make -s || exit 1
	unload; load_deps
	echo "[M0] loading probe-safe, snapshot_profile=6 (no firmware, no DMA)..."
	insmod "$MOD" procfs_verbosity=2 snapshot_profile=6 || { echo "insmod failed"; dmesg | tail -20; exit 1; }
	sleep 1
	dump
	echo
	echo "[M0] KEY CHECK: in the state dump above, look at the bar0[...] mailbox"
	echo "     lines. If DOORBELL/STATUS/RESULT/PARAM read back as 0xffffffff,"
	echo "     the BAR0 low region is asleep pre-boot and upload will not work"
	echo "     yet. If they read structured values (0x0, small ints), the"
	echo "     mailbox is live and M2 can proceed."
	echo "[M0] leaving module loaded for inspection. 'sudo rmmod mz0380' when done."
	;;
m2)
	echo "[M2] building..."; make -s || exit 1
	unload; load_deps
	echo "[M2] loading with firmware_upload=1 (writes BAR0 mailbox + blob)..."
	dmesg -C 2>/dev/null || true
	insmod "$MOD" procfs_verbosity=2 snapshot_profile=6 firmware_upload=1 enable_video=1
	rc=$?
	sleep 2
	echo "===== dmesg ====="
	dmesg | grep -iE 'mz0380|firmware|BEGIN|COMMIT|boot|WARNING|Oops|BUG' \
		| grep -vE 'Modules linked in|Unloaded tainted' | tail -40
	echo "insmod rc=$rc"
	dump
	echo
	echo "[M2] PASS if dmesg shows a firmware version / boot success and a BAR0"
	echo "     read returns something other than 0xffffffff afterwards."
	;;
*)
	echo "unknown stage '$STAGE' (use m0 or m2)"; exit 1;;
esac
