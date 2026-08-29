#!/bin/bash
# Regenerate every disassembly / strings dump the RE work needs, into re-dump/.
#
# Idempotent and cheap to re-run. Nothing here touches the card or needs root -
# it only reads binaries and writes text files. Run it once and the analysis
# files are all in one place:
#
#     bash mz0380-re-dump.sh
#
# Outputs (re-dump/):
#   ep.txt              card PCIe endpoint driver - mailbox dispatch, MSI,
#                       store_channel_done, pcie_set_outbound
#   vpl_dmac.txt        card DMA controller - StartTail/ISRTail, profile[dst]
#   vcm.txt / .str      video_capture_mgr - the process that writes
#                       channel_done and consumes mailbox opcode 0x50
#   tinyvenc5.txt/.str  the H.264 encoder process
#   yuan_ioctrl.txt/.str the I2C/GPIO servicer
#   win64.txt           Windows driver MZ0380.X64.SYS (2017, v1.1.0.177)
#   win64-v195.txt      Windows driver v1.1.0.195, from the hd60-trace capture
set -u
cd "$(dirname "$0")/.."

ARM="--triple=armv7-linux-gnueabi"
OUT=re-dump
FWTREE=$OUT/fw
ONCARD=windowsDriver/hd60-trace/artifacts/oncard-binaries
WINART=windowsDriver/hd60-trace/artifacts

mkdir -p $OUT

command -v llvm-objdump >/dev/null || { echo "need llvm-objdump"; exit 1; }

# The on-card userspace binaries (tinyvenc5 etc) are only in the firmware blob,
# not in the hd60-trace capture, so unpack it once. It is a gzip'd tar of the
# card's embedded-Linux root.
if [ ! -d $FWTREE/yuan_demo_sdi ]; then
	mkdir -p $FWTREE
	if [ -r /usr/lib/firmware/mz0380/MZ0380.HD.HEX ]; then
		tar xzf /usr/lib/firmware/mz0380/MZ0380.HD.HEX -C $FWTREE \
			&& echo "unpacked firmware -> $FWTREE"
	elif [ -r $WINART/MZ0380.HD.firmware.tar ]; then
		tar xf $WINART/MZ0380.HD.firmware.tar -C $FWTREE \
			&& echo "unpacked firmware tar -> $FWTREE"
	else
		echo "WARNING: no firmware blob found; skipping tinyvenc5"
	fi
fi

FW=$FWTREE/yuan_demo_sdi

# dump <output-basename> <binary> <triple-or-empty>
dump() {
	local out=$1 bin=$2 triple=${3-}
	[ -r "$bin" ] || { echo "skip $out (no $bin)"; return; }
	llvm-objdump -d $triple "$bin" > "$OUT/$out.txt" 2>&1
	strings -a "$bin" > "$OUT/$out.str" 2>/dev/null
	printf '%-16s %8s lines  <- %s\n' "$out" "$(wc -l < "$OUT/$out.txt")" "$bin"
}

echo "=== card side (ARM) ==="
dump ep          $ONCARD/ep.ko              "$ARM"
dump vcm         $ONCARD/video_capture_mgr  "$ARM"
dump yuan_ioctrl $ONCARD/yuan_ioctrl        "$ARM"
dump vpl_dmac    $FW/drivers/vpl_dmac.ko    "$ARM"
dump tinyvenc5   $FW/tinyvenc5              "$ARM"

echo "=== host side (x86-64 PE) ==="
dump win64       $WINART/e60MZ0380.X64.SYS
dump win64-v195  $WINART/e60MZ0380.X64.v195.SYS

echo
echo "done -> $OUT/"
ls -l $OUT/*.txt 2>/dev/null
