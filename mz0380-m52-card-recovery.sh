#!/bin/bash
# M52: can a wedged card be recovered WITHOUT a mains-off cold boot?
#
# The wedge (hit after ~8-18 encoder spawns): the card's mailbox stops
# ACKing everything - CMD_INIT and even BEGIN_FW_DL time out (-110), so a
# firmware re-upload cannot even start. rmmod/insmod does not help. This
# script walks the escalating PCI-level resets the kernel offers for the
# device and tests the mailbox after each:
#
#   1. pm  reset  - D3hot -> D0 power-state bounce
#   2. bus reset  - secondary-bus (hot) reset on the upstream bridge;
#                   if the slot's PERST# is routed into the SoC's reset
#                   tree this reboots the card's ARM Linux
#   3. remove + rescan - full re-enumeration on top of a bus reset
#
# Verdict per stage comes from reloading the module and looking for the
# mailbox answering (CMD_INIT ok / fw READY / "already runs firmware").
#
# Decision table:
#   - "RECOVERED after <stage>"  -> cold boot NOT needed; use this stage
#     as the standard recovery (and consider automating it in the driver).
#   - all three fail             -> the SoC really does keep running through
#     PCIe resets; mains-off cold boot is the only recovery. (Then finding
#     a card-side reboot opcode is worth RE time - op 0xFF candidate.)
set -u
cd "$(dirname "$0")"

PCI=${PCI_ADDR:-0000:04:00.0}
SYS=/sys/bus/pci/devices/$PCI

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -e "$SYS" ] || { echo "no device at $PCI"; exit 1; }
make >/dev/null || { echo "build failed"; exit 1; }

unload() {
	fuser -k /dev/video0 2>/dev/null
	rmmod mz0380 2>/dev/null
	sleep 1
}

# Load the module and report whether the card's mailbox answered.
mailbox_alive() {
	dmesg -C
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 || return 1
	# upload+boot can take ~25 s; the failure path (-110) shows in ~10 s
	for i in $(seq 30); do
		if dmesg | grep -qE "already runs firmware|firmware.*READY|fw .*ready"; then
			return 0
		fi
		if dmesg | grep -qE "CMD_INIT got no answer|BEGIN_FW_DL command failed"; then
			return 1
		fi
		sleep 1
	done
	# no explicit marker either way - fall back to the state file
	grep -qi "state *: *ready" /proc/mz0380-state 2>/dev/null
}

try_stage() {
	local name="$1"; shift
	echo "=== stage: $name ==="
	unload
	"$@" || { echo "  ($name action failed)"; return 1; }
	sleep 2
	if mailbox_alive; then
		echo "RECOVERED after $name - card mailbox answers again"
		dmesg | grep -E "mz0380" | tail -5
		exit 0
	fi
	echo "  $name: still wedged"
	return 1
}

echo "reset methods offered: $(cat "$SYS"/reset_method 2>/dev/null)"

try_stage "pm-reset"  sh -c "echo pm  > '$SYS/reset_method' && echo 1 > '$SYS/reset'"
try_stage "bus-reset" sh -c "echo bus > '$SYS/reset_method' && echo 1 > '$SYS/reset'"
try_stage "remove+rescan" sh -c "echo 1 > '$SYS/remove'; sleep 2; echo 1 > /sys/bus/pci/rescan; sleep 3"

echo "ALL PCI RESETS FAILED - the SoC survives PCIe resets; mains-off cold boot is the only recovery"
exit 1
