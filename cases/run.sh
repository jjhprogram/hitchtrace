#!/usr/bin/env bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Run one variant of a hitchtrace case study: the minigame, the environment
# the variant sets up around it, and hitchtrace attached through the layer.
# Run as your normal user; only the tracer runs under sudo.
#
#   make && make layer layer-install && make -C cases
#   cases/run.sh VARIANT [SECONDS] [RUN_NAME]
#
# Variants:
#   baseline   the game alone on CPUs $GAME_CPUS
#   bgcpu      + bgload on the same CPUs: a background application that
#              wakes every BG_PERIOD_MS and burns BG_BURST_MS on BG_THREADS
#              threads (real-world-cases.md A/E2: a game losing its cores to another
#              process)
#   iomain     the game loads an ASSET_MB asset every ASSET_EVERY frames on
#              its main thread (real-world-cases.md C: a frame waiting on storage)
#   iothread   the same load done by a streaming thread that the main thread
#              waits for
#   mem        the game keeps a WORLD_MB world in memory and walks WALK_MB of
#              it every ASSET_EVERY frames, inside a cgroup limited to
#              MEM_MAX, so the walk faults pages back in from swap (real-world-cases.md
#              B: memory pressure)
#   memthp     the same with transparent huge pages asked for the world
#   alloc      the game allocates and fills ALLOC_MB of new memory every
#              ALLOC_EVERY frames, keeping the newest ALLOC_KEEP blocks, with
#              its unwalked world filling the cgroup to MEM_MAX: the kernel
#              has to make room first (real-world-cases.md B: reclaim)
#   compact    the game's memory grows by THP_MB of huge pages every
#              ALLOC_EVERY frames while frag holds the machine's free memory
#              in single pages, all but FRAG_LEAVE_MB (real-world-cases.md B2: huge
#              pages and compaction). Not limited to a cgroup: when the free
#              memory is used up the whole machine is short, so keep it brief
#   slack      the game's sleeps are made imprecise in bursts by a preloaded
#              library (real-world-cases.md F2: timers delivered late)
#   ipc        the game makes IPC_CALLS blocking calls per frame to a server
#              process on CPU SRV_CPU, where bgload runs too (real-world-cases.md D1: a
#              game waiting on wineserver)
#   cpuspike   the game burns BURN_MS of CPU on its main thread every
#              ASSET_EVERY frames: a negative control, not a kernel stall
#   squeeze    every SQ_PERIOD_MS, squeeze confines all the game's threads to
#              CPUs SQ_CPUS for SQ_BURST_MS, with nothing else running there
#              (real-world-cases.md A: a scheduler packing a game's threads onto too
#              few cores)
#   iodisk     iothread on a disk the game shares: the game and bgio, reading
#              in bursts, run in one cgroup whose reads from the data disk
#              are limited to DISK_BPS (real-world-cases.md C1: other I/O saturating a
#              slow disk). Needs the io controller delegated to the user
#              manager; DISK_BPS=none runs it without the limit
#
# Output, in cases/out/RUN_NAME/:
#   frames.jsonl   the game's own per-frame phase timings ("engine profiler")
#   hitch.jsonl    hitchtrace records
#   hitch.txt      the same, human-readable
#   bursts.jsonl   the ground truth where the environment supplies it: when
#                  bgload was burning CPU (bgcpu, ipc), when bgio was reading
#                  (iodisk), when the threads were confined (squeeze), which sleeps were made imprecise (slack). Elsewhere it is frames.jsonl: which
#                  frames loaded, walked or burned
#   manifest.json  what was run, on what
#
# For tools/catch: TOOL=delayacct|ftrace|perfetto|perfsched|perfoffcpu|offwaketime
# attaches that tool instead of hitchtrace, through tools/overhead/run.py
# --attach (sudo, so the credentials must be cached: sudo -v first); its
# output and tool.json go in the same directory. OUT_DIR=DIR writes there
# instead of cases/out/RUN_NAME. TOOL_CPUS=LIST keeps the tool's own process
# (hitchtrace's included) on those CPUs.
set -u

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
VARIANT=${1:?usage: cases/run.sh VARIANT [SECONDS] [RUN_NAME]}
SECS=${2:-30}
RUN=${3:-$VARIANT}
OUT=${OUT_DIR:-$ROOT/cases/out/$RUN}
TOOL=${TOOL:-ht}
TOOL_CPUS=${TOOL_CPUS:-}
LAYER=$ROOT/build/libVkLayer_hitchtrace.so

GAME_CPUS=${GAME_CPUS:-0-3}
GAME_ARGS=${GAME_ARGS:--w 3 -n 120}
# the game caps at 60 fps (16.67 ms); a hitch is a frame 25% over that
BUDGET_US=${BUDGET_US:-20833}
BG_THREADS=${BG_THREADS:-4}
BG_PERIOD_MS=${BG_PERIOD_MS:-700}
BG_BURST_MS=${BG_BURST_MS:-20}
ASSET_MB=${ASSET_MB:-8}
ASSET_EVERY=${ASSET_EVERY:-30}
PACK=$ROOT/build/cases/data/pack.bin
WORLD_MB=${WORLD_MB:-512}
WALK_MB=${WALK_MB:-2}
MEM_MAX=${MEM_MAX:-420M}
ALLOC_MB=${ALLOC_MB:-8}
ALLOC_EVERY=${ALLOC_EVERY:-15}
ALLOC_KEEP=${ALLOC_KEEP:-64}
THP_MB=${THP_MB:-24}
FRAG_LEAVE_MB=${FRAG_LEAVE_MB:-64}
SLACK_NS=${SLACK_NS:-8000000}
SRV_CPU=${SRV_CPU:-5}
IPC_CALLS=${IPC_CALLS:-30}
BURN_MS=${BURN_MS:-12}
SQ_CPUS=${SQ_CPUS:-0}
SQ_PERIOD_MS=${SQ_PERIOD_MS:-1500}
SQ_BURST_MS=${SQ_BURST_MS:-300}
DISK_BPS=${DISK_BPS:-200M}
DISK_SLICE=hitchdisk.slice
SCRATCH=$ROOT/build/cases/data/scratch.bin
# e.g. HITCHTRACE_ARGS=-U for user stacks
read -r -a TRACE_ARGS <<<"${HITCHTRACE_ARGS:-}"

case $VARIANT in
baseline|bgcpu|squeeze) ;;
iomain)   GAME_ARGS="$GAME_ARGS -s main -p $PACK -a $ASSET_MB -e $ASSET_EVERY" ;;
iothread) GAME_ARGS="$GAME_ARGS -s thread -p $PACK -a $ASSET_MB -e $ASSET_EVERY" ;;
mem)      GAME_ARGS="$GAME_ARGS -m $WORLD_MB -g $WALK_MB -e $ASSET_EVERY" ;;
memthp)   GAME_ARGS="$GAME_ARGS -m $WORLD_MB -g $WALK_MB -e $ASSET_EVERY -H" ;;
alloc)    GAME_ARGS="$GAME_ARGS -m $WORLD_MB -g 0 -A $ALLOC_MB -K $ALLOC_KEEP -e $ALLOC_EVERY" ;;
compact)  GAME_ARGS="$GAME_ARGS -A $THP_MB -K 1024 -H -e $ALLOC_EVERY" ;;
slack)    ;;
ipc)      GAME_ARGS="$GAME_ARGS -i $OUT/srv.sock -c $IPC_CALLS"
	  # more threads than the game's cores see: the server's one CPU is crowded
	  BG_THREADS=${IPC_BG_THREADS:-4}; BG_BURST_MS=${IPC_BG_BURST_MS:-25} ;;
cpuspike) GAME_ARGS="$GAME_ARGS -b $BURN_MS -e $ASSET_EVERY" ;;
iodisk)   # smaller loads, more often: alone they fit in the frame's slack
	  ASSET_MB=${IO_ASSET_MB:-2}; ASSET_EVERY=${IO_ASSET_EVERY:-15}
	  GAME_ARGS="$GAME_ARGS -s thread -p $PACK -a $ASSET_MB -e $ASSET_EVERY"
	  BG_THREADS=${IO_BG_THREADS:-4}; BG_PERIOD_MS=${IO_BG_PERIOD_MS:-1500}
	  BG_BURST_MS=${IO_BG_BURST_MS:-400} ;;
*) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac

# A slow disk shared with something else: one slice for the game and for
# bgio, its reads from the data disk limited. The user manager can only do
# that if it was delegated the io controller.
if [ "$VARIANT" = iodisk ] && [ "$DISK_BPS" != none ]; then
	ctl=/sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/cgroup.controllers
	if ! grep -qw io "$ctl" 2>/dev/null; then
		echo "iodisk needs the io controller delegated to your user manager." >&2
		echo "Once, as root, then log out and back in (or reboot):" >&2
		echo "  mkdir -p /etc/systemd/system/user@.service.d" >&2
		echo "  printf '[Service]\\nDelegate=cpu cpuset io memory pids\\n' \\" >&2
		echo "      > /etc/systemd/system/user@.service.d/delegate.conf" >&2
		echo "  systemctl daemon-reload" >&2
		echo "DISK_BPS=none runs the variant without the limit." >&2
		exit 2
	fi
fi

# What the game is started through: a memory-limited cgroup, a preload.
WRAP=()
case $VARIANT in
mem*|alloc) [ "$MEM_MAX" = none ] ||
	WRAP=(systemd-run --user --scope -q -p "MemoryMax=$MEM_MAX") ;;
iodisk) [ "$DISK_BPS" = none ] ||
	WRAP=(systemd-run --user --scope -q "--slice=$DISK_SLICE") ;;
slack) WRAP=(env "LD_PRELOAD=$ROOT/build/cases/slack.so" "SLACK_NS=$SLACK_NS"
	     "SLACK_LOG=$OUT/bursts.jsonl") ;;
esac
for f in build/hitchtrace build/cases/minigame build/cases/bgload \
	 build/cases/bgio build/cases/ipcserver build/cases/squeeze build/cases/frag \
	 build/cases/slack.so; do
	[ -e "$ROOT/$f" ] || { echo "missing $f: run make && make -C cases" >&2; exit 2; }
done
[ -f "$LAYER" ] || { echo "run 'make layer layer-install' first" >&2; exit 2; }
sudo -n "$ROOT/build/hitchtrace" --version >/dev/null 2>&1 ||
	{ echo "sudo -n build/hitchtrace must work (NOPASSWD rule)" >&2; exit 2; }

rm -rf -- "$OUT"
mkdir -p -- "$OUT"

# The asset pack: real blocks, not a sparse file, or reading it costs nothing.
case $VARIANT in
io*)
	if [ ! -f "$PACK" ]; then
		mkdir -p -- "$(dirname -- "$PACK")"
		dd if=/dev/zero of="$PACK" bs=1M count=256 oflag=direct status=none ||
			{ echo "cannot create $PACK" >&2; exit 1; }
	fi ;;
esac
if [ "$VARIANT" = iodisk ]; then
	if [ ! -f "$SCRATCH" ]; then
		dd if=/dev/zero of="$SCRATCH" bs=1M count=1024 oflag=direct status=none ||
			{ echo "cannot create $SCRATCH" >&2; exit 1; }
	fi
	if [ "$DISK_BPS" != none ]; then
		systemctl --user set-property --runtime "$DISK_SLICE" \
			"IOReadBandwidthMax=$(dirname -- "$PACK") $DISK_BPS" ||
			{ echo "cannot limit $DISK_SLICE" >&2; exit 1; }
	fi
fi

vmstat_mem() {
	grep -E '^(compact_stall|compact_fail|compact_success|thp_fault_alloc|thp_fault_fallback|allocstall_movable|pgmigrate_success) ' \
		/proc/vmstat | tr '\n' ' '
}

GAME_PID=
BG_PID=
SRV_PID=
FRAG_PID=
DA_OLD=
cleanup() {
	[ -n "$DA_OLD" ] && sudo -n sysctl -q -w "kernel.task_delayacct=$DA_OLD"
	[ -n "$FRAG_PID" ] && kill "$FRAG_PID" 2>/dev/null
	[ -n "$BG_PID" ] && kill "$BG_PID" 2>/dev/null
	[ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
	[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
}
trap cleanup EXIT

if [ "$VARIANT" = ipc ]; then
	taskset -c "$SRV_CPU" "$ROOT/build/cases/ipcserver" "$OUT/srv.sock" \
		2>"$OUT/server.log" &
	SRV_PID=$!
	sleep 0.3
fi

if [ "$VARIANT" = compact ]; then
	"$ROOT/build/cases/frag" -l "$FRAG_LEAVE_MB" >"$OUT/frag.out" 2>"$OUT/frag.log" &
	FRAG_PID=$!
	for _ in $(seq 100); do
		grep -q ready "$OUT/frag.out" 2>/dev/null && break
		sleep 0.2
	done
	grep -E 'DMA32|Normal' /proc/buddyinfo >>"$OUT/frag.log"
	echo "before: $(vmstat_mem)" >"$OUT/vmstat.log"
fi

# delay accounting covers only threads created while it is on: before the game
if [ "$TOOL" = delayacct ]; then
	DA_OLD=$(cat /proc/sys/kernel/task_delayacct)
	sudo -n sysctl -q -w kernel.task_delayacct=1 ||
		{ echo "cannot turn on delay accounting (sudo -v first)" >&2; DA_OLD=; exit 1; }
fi

# the game outlives the trace on both ends (another tool may take a while to
# start: the game gets time to spare and is stopped once the tool is done)
GAME_SECS=$((SECS + 4))
[ "$TOOL" = ht ] || GAME_SECS=$((SECS + 120))
# shellcheck disable=SC2086
HITCHTRACE=1 "${WRAP[@]}" taskset -c "$GAME_CPUS" "$ROOT/build/cases/minigame" \
	-d $GAME_SECS $GAME_ARGS -o "$OUT/frames.jsonl" 2>"$OUT/game.log" &
GAME_PID=$!
sleep 1.5
kill -0 "$GAME_PID" 2>/dev/null || { cat "$OUT/game.log" >&2; exit 1; }

PIN=()
[ -n "$TOOL_CPUS" ] && PIN=(taskset -c "$TOOL_CPUS")
if [ "$TOOL" = ht ]; then
	"${PIN[@]}" sudo -n "$ROOT/build/hitchtrace" -p "$GAME_PID" -x "$LAYER" \
		-m hitch_frame_mark -M hitch_present_end -b "$BUDGET_US" -d "$SECS" \
		"${TRACE_ARGS[@]}" \
		-o "$OUT/hitch.jsonl" >"$OUT/hitch.txt" 2>"$OUT/hitch.err" &
	TRACE_PID=$!
else
	"${PIN[@]}" sudo -n "$ROOT/tools/overhead/run.py" --attach "$TOOL" "$GAME_PID" "$OUT" "$SECS" \
		>"$OUT/attach.log" 2>&1 &
	TRACE_PID=$!
	for _ in $(seq 900); do
		# sudo runs as root: this user cannot signal it, even with kill -0
		[ -f "$OUT/tool.ready" ] || [ ! -d "/proc/$TRACE_PID" ] && break
		sleep 0.1
	done
	[ -f "$OUT/tool.ready" ] ||
		{ echo "$TOOL did not start:" >&2; tail -n 5 "$OUT/attach.log" >&2; exit 1; }
fi
sleep 1

if [ "$VARIANT" = bgcpu ] || [ "$VARIANT" = ipc ]; then
	# the game's own cores, or only the one its server runs on
	BG_CPUS=$GAME_CPUS
	[ "$VARIANT" = ipc ] && BG_CPUS=$SRV_CPU
	taskset -c "$BG_CPUS" "$ROOT/build/cases/bgload" -t "$BG_THREADS" \
		-p "$BG_PERIOD_MS" -b "$BG_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/bgload.log" &
	BG_PID=$!
fi

if [ "$VARIANT" = squeeze ]; then
	# the game's threads, not the taskset that started it
	"$ROOT/build/cases/squeeze" -P "$GAME_PID" -c "$SQ_CPUS" \
		-p "$SQ_PERIOD_MS" -b "$SQ_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/squeeze.log" &
	BG_PID=$!
fi

if [ "$VARIANT" = iodisk ]; then
	# off the game's cores: the disk is the only thing they share
	"${WRAP[@]}" taskset -c "${IO_BG_CPUS:-6-9}" \
		"$ROOT/build/cases/bgio" -R -f "$SCRATCH" -t "$BG_THREADS" \
		-p "$BG_PERIOD_MS" -b "$BG_BURST_MS" -d $((SECS - 2)) \
		-o "$OUT/bursts.jsonl" 2>"$OUT/bgio.log" &
	BG_PID=$!
fi

wait "$TRACE_PID"
[ "$TOOL" = ht ] || kill "$GAME_PID" 2>/dev/null
[ -n "$BG_PID" ] && wait "$BG_PID"
BG_PID=
wait "$GAME_PID"
GAME_PID=
[ -n "$SRV_PID" ] && wait "$SRV_PID" 2>/dev/null
SRV_PID=
if [ -n "$FRAG_PID" ]; then
	echo "after:  $(vmstat_mem)" >>"$OUT/vmstat.log"
	kill "$FRAG_PID" 2>/dev/null
	wait "$FRAG_PID" 2>/dev/null
	FRAG_PID=
fi

cat >"$OUT/manifest.json" <<EOF
{"variant": "$VARIANT", "tool": "$TOOL", "seconds": $SECS, "date": "$(date -Is)",
 "kernel": "$(uname -r)", "cpus_online": $(nproc --all),
 "game_cpus": "$GAME_CPUS", "game_args": "$GAME_ARGS", "budget_us": $BUDGET_US,
 "bgload": {"threads": $BG_THREADS, "period_ms": $BG_PERIOD_MS, "burst_ms": $BG_BURST_MS},
 "asset": {"mb": $ASSET_MB, "every_frames": $ASSET_EVERY},
 "memory": {"world_mb": $WORLD_MB, "walk_mb": $WALK_MB, "max": "$MEM_MAX"},
 "alloc": {"mb": $ALLOC_MB, "every_frames": $ALLOC_EVERY, "keep": $ALLOC_KEEP},
 "compact": {"thp_mb": $THP_MB, "frag_leave_mb": $FRAG_LEAVE_MB},
 "slack_ns": $SLACK_NS, "server_cpu": "$SRV_CPU", "ipc_calls": $IPC_CALLS,
 "burn_ms": $BURN_MS, "disk_bps": "$DISK_BPS",
 "squeeze": {"cpus": "$SQ_CPUS", "period_ms": $SQ_PERIOD_MS, "burst_ms": $SQ_BURST_MS},
 "hitchtrace_args": "${HITCHTRACE_ARGS:-}", "tool_cpus": "$TOOL_CPUS",
 "vulkan_device": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*deviceName\s*= //p' | head -1)",
 "mesa": "$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*driverInfo\s*= //p' | head -1)",
 "hitchtrace_commit": "$(git -C "$ROOT" describe --always --dirty)"}
EOF

if [ "$TOOL" = ht ]; then tail -n 2 "$OUT/hitch.err"; else tail -n 2 "$OUT/attach.log"; fi
echo "output in ${OUT#"$ROOT"/}"
