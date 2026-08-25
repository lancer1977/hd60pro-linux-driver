#!/bin/bash
# M169: the spawn budget, counted across module reloads.
#
# Why this exists
# ---------------
# The card wedges for good somewhere in the 8-18 encoder-spawn range and the
# only reliable recovery is removing slot power. NEXT_SESSION_START's first
# open question is whether M157's vic_fast_kill=0 moves that cliff, and it
# names the evidence exactly:
#
#     "If a session passes 18 spawns on one power cycle without wedging,
#      that is the answer."
#
# Nothing could produce that number. The driver counts spawns only since
# insmod, and every hardware script in this tree rmmods and reinsmods, so each
# run starts the count at zero. dmesg would span the reloads - except that the
# same scripts open with `dmesg -C`. So the budget has been tracked by memory
# and by reading this file's prose, which is how a session ends up "12 spawns
# on top of whatever the current power cycle had already used".
#
# The tally lives in /run, which a tmpfs clears at every boot. That is the
# correct granularity by construction rather than by bookkeeping: the handoff
# records that a soft PC shutdown is enough to reset the card, so a host boot
# bounds a card power cycle. It does NOT survive a warm reboot correctly - a
# warm reboot may leave the card powered while /run is cleared - so `reset`
# exists for the case where you know better.
#
# Usage
#   ./mz0380-spawns.sh            show the running total (no root needed)
#   sudo ./mz0380-spawns.sh commit    add the loaded module's spawns to it
#   sudo ./mz0380-spawns.sh unloaded  mark a completed rmmod boundary
#   sudo ./mz0380-spawns.sh add 1     repair a known missed unloaded spawn
#   sudo ./mz0380-spawns.sh reset     start a new power cycle at zero
#
# `commit` is idempotent within a load and correct across loads: it adds only
# the delta since the last commit, and treats a count that went backwards as a
# fresh insmod. Hardware scripts call it while the module is still loaded,
# before their own rmmod.
set -u
cd "$(dirname "$0")"

TALLY=${MZ0380_TALLY:-/run/mz0380-spawn-tally}
STATE=${MZ0380_STATE:-/proc/mz0380-state}	# overridable so the arithmetic is testable

# tally file: two integers, "<total> <last_seen_this_load>"
read_tally() {
	if [ -r "$TALLY" ]; then
		read -r T L _ < "$TALLY" 2>/dev/null || { T=0; L=0; }
	else
		T=0; L=0
	fi
	case "$T" in ''|*[!0-9]*) T=0;; esac
	case "$L" in ''|*[!0-9]*) L=0;; esac
}

loaded_spawns() {
	# "  enc spawns : N since insmod (...)"
	[ -r "$STATE" ] || return 1
	sed -n 's/^ *enc spawns *: *\([0-9]\+\).*/\1/p' "$STATE" | head -1
}

# The tally normally lives in /run and so normally needs root - but say that
# because the write failed, not because of a uid test, so MZ0380_TALLY can point
# somewhere writable (which is also how the arithmetic below gets tested).
writable_or_die() {
	local dir
	dir=$(dirname "$TALLY")
	[ -w "$TALLY" ] || [ -w "$dir" ] || {
		echo "cannot write $TALLY - run as root, or set MZ0380_TALLY"
		exit 1
	}
}

verdict() {
	local n=$1
	if [ "$n" -ge 18 ]; then
		echo "  PAST 18 on this power cycle without a wedge - that is M157's answer:"
		echo "  vic_fast_kill=0 moved the cliff. Record it in RE_FINDINGS."
	elif [ "$n" -ge 8 ]; then
		echo "  INSIDE the historical 8-18 wedge range. Every further run is a gamble;"
		echo "  a wedge from here costs a mains-off cold boot."
	else
		echo "  Below 8 - the range where the card has never wedged."
	fi
}

case "${1:-show}" in
show)
	read_tally
	CUR=$(loaded_spawns || true)
	if [ -n "${CUR:-}" ]; then
		if [ "$CUR" -ge "$L" ]; then
			LIVE=$(( T + CUR - L ))
		else
			LIVE=$(( T + CUR ))	# module was reloaded since the last commit
		fi
		echo "spawns this power cycle: $LIVE  (committed $T, current load $CUR)"
	else
		LIVE=$T
		echo "spawns this power cycle: $LIVE  (committed; module not loaded)"
	fi
	verdict "$LIVE"
	;;
commit)
	writable_or_die
	CUR=$(loaded_spawns) || {
		echo "module not loaded - nothing to commit (run this BEFORE rmmod)"
		exit 1
	}
	read_tally
	if [ "$CUR" -ge "$L" ]; then
		T=$(( T + CUR - L ))
	else
		T=$(( T + CUR ))
	fi
	L=$CUR
	echo "$T $L" > "$TALLY"
	echo "spawns this power cycle: $T  (this load has spawned $CUR)"
	verdict "$T"
	;;
unloaded)
	[ ! -r "$STATE" ] || {
		echo "module is still loaded - commit it and rmmod before marking unloaded"
		exit 1
	}
	writable_or_die
	read_tally
	# A future module starts its own counter at zero. Keeping L from the old
	# module makes two consecutive one-spawn loads look like the same load.
	L=0
	echo "$T $L" > "$TALLY"
	echo "spawn tally marked at module-unload boundary: $T"
	;;
add)
	[ ! -r "$STATE" ] || {
		echo "module is loaded - use commit instead"
		exit 1
	}
	N=${2:-}
	case "$N" in ''|*[!0-9]*|0) echo "usage: $0 add <positive-count>" >&2; exit 1;; esac
	writable_or_die
	read_tally
	T=$(( T + N ))
	L=0
	echo "$T $L" > "$TALLY"
	echo "spawn tally repaired by +$N: $T"
	verdict "$T"
	;;
reset)
	writable_or_die
	echo "0 0" > "$TALLY"
	echo "spawn tally reset - declaring this a fresh power cycle"
	;;
*)
	echo "usage: $0 [show|commit|unloaded|add <count>|reset]" >&2
	exit 1
	;;
esac
