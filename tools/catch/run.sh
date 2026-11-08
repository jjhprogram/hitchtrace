#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Does hitchtrace catch what other tools miss? Every case study, made rare,
# once with each tool, then scored by the rules in docs/catch-test.md.
# Run as your normal user: sudo asks for your password once, at the start.
#
#   make && make layer layer-install && make -C cases && make -C tools/overhead
#   tools/catch/run.sh [NAME]
#
# Environment: CASES, TOOLS (space-separated), SECS per run (120), RARE_S,
# seconds between injections (10). Output: build/catch/NAME/CASE-TOOL/, the
# directories cases/run.sh writes, and summary.md from score.py.
set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
NAME=${1:-catch-$(date +%Y%m%d-%H%M%S)}
OUTROOT=$ROOT/build/catch/$NAME
CASES=${CASES:-bgcpu iothread ipc mem cpuspike}
TOOLS=${TOOLS:-ht delayacct ftrace perfsched offwaketime}
SECS=${SECS:-120}
RARE_S=${RARE_S:-10}

# one injection every RARE_S: frames for the game's own injections (60 fps),
# milliseconds for bgload's bursts
export ASSET_EVERY=$((RARE_S * 60)) BG_PERIOD_MS=$((RARE_S * 1000))
# every tool's own process off the game's CPUs (0-3) and its server's (5)
export TOOL_CPUS=${TOOL_CPUS:-6-9}

[ -e "$OUTROOT" ] && { echo "$OUTROOT exists" >&2; exit 2; }
pgrep -x mg_main >/dev/null && { echo "a copy of the game is already running" >&2; exit 2; }
sudo -v || exit 1
mkdir -p -- "$OUTROOT"
cat >"$OUTROOT/plan.json" <<EOF
{"cases": "$CASES", "tools": "$TOOLS", "seconds": $SECS, "rare_s": $RARE_S,
 "date": "$(date -Is)", "commit": "$(git -C "$ROOT" describe --always --dirty)"}
EOF

n=0
for c in $CASES; do
	# a different order of the tools in each case, the same on every run
	for t in $(printf '%s\n' $TOOLS | shuf --random-source=<(yes "$c")); do
		n=$((n + 1))
		echo "[$n] $c with $t"
		sudo -v		# keep the credentials fresh for the tool's sudo -n
		if ! OUT_DIR=$OUTROOT/$c-$t TOOL=$t "$ROOT/cases/run.sh" "$c" "$SECS" "$c-$t" \
			>"$OUTROOT/$c-$t.log" 2>&1; then
			echo "    failed: $(tail -n 3 "$OUTROOT/$c-$t.log" | tr '\n' ' ')"
		fi
		sync
		sleep 2
	done
done

"$ROOT/tools/catch/score.py" "$OUTROOT"
