#!/bin/bash
# Build the driver in a throwaway copy of the tree and report whether it
# actually compiled.
#
# WHY THIS EXISTS
#
# A build in the working tree is not always available: the hardware scripts run
# `make` as root, which leaves root-owned objects that make the next non-root
# build fail with "Operation not permitted" - so the obvious check gets skipped
# exactly when the tree is dirtiest.
#
# The workaround - build a copy in /tmp - then produced its own failure. Checking
# `ls mz0380.ko` after the build looks like a verification and is not: a stale
# .ko from an earlier run sits there and satisfies the test whether or not make
# succeeded. That is how a header declared in the wrong place shipped as
# "verified": it built on the DMA side, failed on the video side, and the check
# reported success from a leftover file.
#
# So this script does the two things that were missing. It starts from an empty
# directory, and it reports make's EXIT STATUS.
#
#   ./scripts/mz0380-build-check.sh          # build, print pass/fail
#   ./scripts/mz0380-build-check.sh -v       # also print the full log on failure
#
# Needs no root, touches no hardware, and leaves the working tree alone.
set -u
cd "$(dirname "$0")/.."

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

TMP=$(mktemp -d "${TMPDIR:-/tmp}/mz0380-buildcheck.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

# Copy only what kbuild needs. Starting from an empty directory is the point:
# nothing from a previous build can be mistaken for this one's output.
cp -r Makefile src "$TMP"/ || exit 1

LOG="$TMP/build.log"
( cd "$TMP" && make "$@" ) >"$LOG" 2>&1
RC=$?

# grep -c prints the count and still exits 1 when that count is zero, so the
# usual `|| echo 0` fallback appends a second number rather than supplying one.
WARN=$(grep -ci 'warning:' "$LOG" 2>/dev/null)
WARN=${WARN:-0}

if [ "$RC" -eq 0 ] && [ -f "$TMP/mz0380.ko" ]; then
	echo "BUILD OK   (make exit 0, $WARN warnings)"
	grep -i 'warning:' "$LOG" | head -10
	exit 0
fi

echo "BUILD FAILED (make exit $RC)"
grep -E 'error:|Error [0-9]' "$LOG" | head -20
[ "$VERBOSE" = 1 ] && { echo; echo "--- full log ---"; cat "$LOG"; }
exit 1
