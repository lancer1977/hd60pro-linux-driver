#!/bin/bash
# Load the driver and LEAVE IT LOADED, so OBS (or any V4L2 app) can open the
# node and you can watch what the card does. Companion to the m55 harness,
# which always unloads on exit.
#
#   sudo ./mz0380-live.sh load      # build, smoke-test, insmod, wait, print node
#   sudo ./mz0380-live.sh status    # what is the card doing right now
#   sudo ./mz0380-live.sh watch     # follow the driver's log until Ctrl-C
#   sudo ./mz0380-live.sh unload    # kill users of the node, rmmod, tidy up
#
# Takes the same env knobs as m55 (POLLDRAIN VICFW VICB0 MSTB1 MSTB2 MSTB5
# B0LATE KICKOP KICKREP OP6KICK WINSEQ INTX SETBUF ... plus EXTRA="p=v ...").
# POLLDRAIN defaults to 20 here, because without it the node hands over
# nothing at all - the completion event this card should raise has never fired
# in this project's history, so frames are delivered by polling or not at all.
#
# READ THIS BEFORE YOU WATCH OBS
#
#   * The card delivers ONE frame per stream. Expect a single picture and then
#     a frozen preview - that is the current state of the hardware, not OBS
#     failing. Nothing you do in OBS changes it.
#   * That one frame is the card's own "NO SIGNAL" splash: video black with a
#     text band, strictly monochrome. It is not your source.
#   * EVERY start/stop in OBS costs one encoder spawn, and the card wedges
#     somewhere around 8-18 spawns per power cycle. OBS reconnects on its own
#     when a source stalls, so it can burn the entire budget unattended.
#     Set the OBS source to "Deactivate when not showing" OFF and do NOT leave
#     it retrying. `status` prints the spawn count so you can watch the budget.
#   * When the card wedges, every command returns -110 and only a power cycle
#     AT MAINS clears it (slot standby survives a soft power-off).
set -u
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

ACTION=${1:-load}

find_node() {
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

do_unload() {
	local node
	node=$(find_node 2>/dev/null || true)
	if [ -n "$node" ]; then
		fuser -k "$node" 2>/dev/null && echo "killed the process holding $node"
		sleep 1
	fi
	if rmmod mz0380 2>/dev/null; then
		echo "module unloaded"
	elif [ -d /sys/module/mz0380 ]; then
		echo "rmmod FAILED - state=$(cat /sys/module/mz0380/initstate 2>/dev/null)"
		echo "if that says 'going' the module is wedged; power-cycle at mains."
		return 1
	else
		echo "module was not loaded"
	fi
	# m55 runs make as root and leaves root-owned objects that break the next
	# non-root build with a confusing "Operation not permitted".
	rm -f ./*.o
	return 0
}

case "$ACTION" in
unload)
	do_unload
	exit $?
	;;

status)
	if [ ! -d /sys/module/mz0380 ]; then echo "not loaded"; exit 1; fi
	node=$(find_node 2>/dev/null || echo "(no node)")
	echo "node          : $node"
	echo "initstate     : $(cat /sys/module/mz0380/initstate 2>/dev/null)"
	echo "users         : $(cat /sys/module/mz0380/refcnt 2>/dev/null)"
	echo
	echo "--- receiver / signal (last few) ---"
	dmesg | grep -E 'MST3367 signal|detect 55=' | tail -5
	echo
	echo "--- encoder spawns this session (budget is ~8-18 per power cycle) ---"
	printf '  SET_VIC sent    : %s\n' "$(dmesg | grep -c 'stream start: SET_VIC')"
	printf '  frames delivered: %s\n' "$(dmesg | grep -c 'inferred H.264 length=')"
	printf '  command timeouts: %s\n' "$(dmesg | grep -c 'ret=-110')"
	echo
	echo "--- last driver lines ---"
	dmesg | grep mz0380 | tail -12
	;;

watch)
	echo "following the driver log - Ctrl-C to stop (module stays loaded)"
	dmesg -w | grep --line-buffered mz0380
	;;

load)
	make >/dev/null || { echo "build failed"; exit 1; }

	# M85: never leave a module loaded that cannot unload cleanly. An oops in
	# the exit path wedges it in MODULE_STATE_GOING, which no rmmod clears -
	# and this script deliberately leaves the module in place, so the check
	# matters more here than in m55. SKIPSMOKE=1 to bypass.
	if [ "${SKIPSMOKE:-0}" != 1 ]; then
		state=$(cat /sys/module/mz0380/initstate 2>/dev/null || true)
		if [ "$state" = going ]; then
			echo "mz0380 is wedged in MODULE_STATE_GOING - power-cycle at mains"
			exit 1
		fi
		./mz0380-m85-unload-smoke.sh >/tmp/mz0380-smoke.log 2>&1 || {
			echo "load/unload smoke test FAILED - see /tmp/mz0380-smoke.log"
			tail -20 /tmp/mz0380-smoke.log
			exit 1
		}
		echo "(load/unload smoke test clean)"
	fi

	do_unload >/dev/null 2>&1
	sleep 1
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm

	# M89: pass a knob ONLY when the caller set it, so the driver's own default
	# governs otherwise. Hardcoding "${VAR:-<literal>}" here silently overrides
	# the module default the moment the two drift apart.
	OPTARGS=""
	add_opt() { [ -z "$2" ] || OPTARGS="$OPTARGS $1=$2"; }
	add_opt vic_fw          "${VICFW:-}"
	add_opt vic_out_format  "${VICM:-}"
	add_opt win_seq         "${WINSEQ:-}"
	add_opt irq_intx        "${INTX:-}"
	add_opt enc_sub         "${ENCSUB:-}"
	add_opt win_start_op6   "${OP6:-}"
	add_opt win_bufs_first  "${WINBUFS:-}"
	add_opt set_buf_op8     "${OP8:-}"
	add_opt probe_windows   "${PROBEWIN:-}"
	add_opt mst_win_output  "${MSTOUT:-}"
	add_opt mst_ad          "${MSTAD:-}"
	add_opt vic_in_w        "${VICINW:-}"
	add_opt vic_in_h        "${VICINH:-}"
	add_opt vic_in_fmt      "${VICINFMT:-}"
	add_opt vic_b0          "${VICB0:-}"
	add_opt mst_b1          "${MSTB1:-}"
	add_opt mst_b2          "${MSTB2:-}"
	add_opt mst_b5          "${MSTB5:-}"
	add_opt mst_b0_late     "${B0LATE:-}"
	add_opt set_buf_opcode  "${SETBUF:-}"
	add_opt rx_strap        "${RXSTRAP:-}"
	add_opt poll_drain_credit "${POLLCREDIT:-}"
	add_opt op6_kick_ms     "${OP6KICK:-}"
	add_opt kick_opcode     "${KICKOP:-}"
	add_opt kick_repeat     "${KICKREP:-}"
	add_opt stream_without_signal "${NOSRC:-}"
	# The one knob this script does default, because the node delivers nothing
	# without it. Override with POLLDRAIN=0 to see the pre-M112 behaviour.
	add_opt poll_drain_ms   "${POLLDRAIN:-20}"

	dmesg -C
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 aic_on=1 \
		stream_nosg="${NOSG:-0}" force_timings=0 signal_poll_ms=4000 \
		$OPTARGS ${EXTRA:-} \
		|| { echo "insmod failed"; exit 1; }
	echo "insmod:$OPTARGS ${EXTRA:-}"

	echo "waiting for the card to finish booting its own flash image..."
	sleep 25

	NODE=$(find_node 2>/dev/null || true)
	if [ -z "$NODE" ]; then
		echo "no video node appeared - driver log:"
		dmesg | grep mz0380 | tail -20
		exit 1
	fi

	echo
	dmesg | grep -E 'MST3367 signal|firmware|IRQ' | tail -6
	echo
	echo "=================================================================="
	echo " node: $NODE   (module stays loaded until you unload it)"
	echo "=================================================================="
	echo
	echo " In OBS: Video Capture Device (V4L2)  ->  device $NODE"
	echo "   - turn OFF 'Deactivate when not showing'"
	echo "   - if the preview stalls, do NOT let it retry in a loop:"
	echo "     every retry is one encoder spawn out of a budget of ~8-18."
	echo
	echo " Expect ONE frame, then a frozen preview, showing the card's own"
	echo " monochrome NO SIGNAL splash. That is the hardware's current"
	echo " behaviour, not OBS misbehaving."
	echo
	echo "   sudo ./mz0380-live.sh status    # signal, spawns, last log lines"
	echo "   sudo ./mz0380-live.sh watch     # follow the log live"
	echo "   sudo ./mz0380-live.sh unload    # when you are done"
	;;

*)
	echo "usage: $0 {load|status|watch|unload}"
	exit 1
	;;
esac
