# How hitchtrace works

The developer's view: what each part of the code does, the state it keeps,
the rules it applies, and the record it writes. Why the design is what it is,
and how it compares with other tools, is argued in the paper (§3 to §5).
What it has been measured to do is in [evaluation.md](evaluation.md). How it
got this way is in [history.md](history.md), and what is still missing is in
[todo.md](todo.md).

## The parts

| File | Job |
|---|---|
| `src/layer/hitchtrace_layer.c` | Implicit Vulkan layer. Wraps `vkQueuePresentKHR` and calls `hitch_frame_mark(frame_id)` at present entry and `hitch_present_end(frame_id)` at present return. The markers are exported and never inlined, so a uprobe can sit on them. `frame_id` is one counter per process, counting presents. Inert unless `HITCHTRACE=1`; `HITCHTRACE_DISABLE=1` wins. 64-bit only. |
| `src/hitchtrace/hitchtrace.bpf.c` | All accounting, in the kernel: per-task state, wake chains, the root's frame, the gate, the record. |
| `src/hitchtrace/hitchtrace.h` | Types shared with user space: `enum ht_cause`, the hop and frame flags, `struct ht_record` and its parts. |
| `src/hitchtrace/hitchtrace.c` | Loader and front end: options, attach, ring-buffer reader, the human report and JSONL. |
| `src/hitchtrace/syms.c` | Kernel and user symbolization: kallsyms, ELF symbols, build-id and debuglink debuginfo, perf maps, through `/proc/PID/map_files` so container paths resolve. |

Viewers read the JSONL: `tools/hitchview.py` (one HTML page per run) and
`src/hitchtracegui/` (Qt).

## Frames

- **The root** is the thread that hits the frame marker. Its frame `N` runs
  from mark `N-1` to mark `N`. Other threads of the target process (`-p`)
  are context.
- **The present bracket.** Blocked time between `hitch_frame_mark` and
  `hitch_present_end` is `block_present`: the display pacing the app. It is
  opened only when the return probe is attached (`-M`, on by default with the
  layer). Otherwise a bracket that never closed would relabel every wait.
  The present-return probe is attached before the frame marker for the same
  reason.
- **Budget.** Fixed (`-b`, default 16,667 µs), or `--budget-mode median`: 1.5
  times the median of the last 19 frames, with a 2 ms floor, trusted after 8
  frames.
- **Gate.** A frame is a hitch if `frame_ns > budget_ns`. With `--share σ`,
  it is also required that `stall_ns ≥ σ × (frame_ns − budget_ns)`, where
  `stall_ns` is the root's time in every bucket except `oncpu` and
  `block_present`. `stall_ns` still counts the frame limiter's sleep and
  waits on threads that were running. The paper's "resolved gate"
  subtracts the latter offline; it is not in the kernel yet.
- **Within budget:** the root's frame state and the frame's interval buffer
  are reset, and nothing crosses to user space.

## Per-task state

`task_states` is task-local storage, created on first use and freed with
the task. It is kept for **every task on the machine**, not only the
target's, because a chain can run through any process (a server, wineserver)
before anyone knows it matters. That is most of hitchtrace's cost on other
processes. `struct task_state` holds:

- **Scheduling:** switch-out and switch-in times, the pending wakeup time,
  the state and `in_iowait` at switch-out, and the kernel (and optionally
  user) stack of a voluntary switch-out.
- **Off-CPU history:** cumulative runnable and blocked time, plus a ring of 32
  checkpoints (`stall_ckpt`) of those totals, each taken just before an
  off-CPU interval began. Intervals ending within 2 ms of the newest
  checkpoint share it, so the ring reaches back at least 64 ms. `stall_since()`
  reads off-CPU time since any moment from it.
- **The last blocked interval's cause and kernel stack**, which a wakee's link
  copies.
- **The last holder of its CPU:** set at a preemption to the task that took
  the CPU, or at the switch-in that ends a run-queue wait of at least
  100 µs to the task leaving the CPU (`preempt_queued`).
- **Wake chain:** `links[5]` of `struct hop_link`, plus `seq` (odd while being
  rewritten).
- **Classification inputs:** the syscall class while inside one, `in_present`,
  `gpu_depth`.
- **On-CPU kernel stall timers:** fault, major fault, reclaim, compaction,
  each cumulative with its start time; the time nested inside them; and their
  values when the frame opened.
- **For a root only:** the frame's start and id, its buckets
  (`cause_ns[HT_CAUSE_MAX]`), counts and flags, and the recent frame times
  for the median.

Other maps: `root_stalls` (the stall groups, one table per root, kept out of
task storage because the compiler will not inline a copy of the whole
table), `thread_slots` and `slot_next` (per-thread lines of the record),
`event_bufs` (the interval log, two buffers), `frame_state`, `irq_exit_nest`,
`kstacks` and `ustacks` (stack maps), `events` (the ring buffer) and `stats`.

## Hooks

| Program | Event | What it does |
|---|---|---|
| `on_switch` | `tp_btf/sched_switch` | Switch-out: time, state, iowait, stack; a preemption sets the victim's last holder. Switch-in: splits the off-CPU interval at the wakeup (blocked before it, run queue after it; a preempted task was runnable throughout), pushes a checkpoint, sets the last holder after a long queue wait. For target threads: their per-thread line and an interval-log entry. For the root: its buckets, stall groups and chain resolution. Also resets the irq-exit counter, which must be zero at any switch. |
| `on_waking`, `on_wakeup_new` | `tp_btf/sched_waking`, `sched_wakeup_new` | `record_wakeup()`: writes the wakee's `links[0]` from the waker as it is now, then copies the waker's `links[0..3]` into the wakee's `links[1..4]`. |
| `on_irq_exit*_enter/leave` | `fentry`/`fexit` `irq_exit_rcu`, `irq_exit` | A per-CPU nesting counter: a wakeup issued while it is nonzero came from interrupt exit (softirq or a ksoftirqd wake). Exit hooks attach before entry hooks, so attaching mid-interrupt can only produce an ignored unmatched decrement. |
| `on_sys_enter`, `on_sys_exit` | `raw_tp/sys_enter`, `sys_exit` | Syscall class of target threads: `futex`, `futex_waitv`, `futex_wait` → futex; `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`, `epoll_pwait(2)` → poll; `nanosleep`, `clock_nanosleep` → sleep. |
| `on_fence_*`, `on_syncobj_*` | `fentry`/`fexit` `dma_fence_wait_timeout`, `drm_syncobj_wait_ioctl`, `drm_syncobj_timeline_wait_ioctl` | `gpu_depth`. Never exercised: the VM has no GPU. |
| `on_fault_enter/leave` | `fentry`/`fexit` `handle_mm_fault` | Fault time; major if the return has `VM_FAULT_MAJOR`. |
| `on_reclaim_*`, `on_memcg_reclaim_*`, `on_compact_*` | `tp_btf` `mm_vmscan_direct_reclaim_*`, `mm_vmscan_memcg_reclaim_*`, `mm_compaction_begin/end` | Reclaim and compaction time. Memcg reclaim matters: a cgroup memory limit triggers only it. |
| `on_frame_mark` | uprobe on the frame marker | Closes the root's on-CPU tail, applies the carve-outs, computes frame time and budget, applies the gate, emits or resets. |
| `on_present_end` | uprobe on the present-return marker | Closes the present bracket. |

**Interrupt context at a wakeup.** Hardirq and NMI come from the per-CPU
preemption count, read through two weak typed ksyms (`__preempt_count`, and
`pcpu_hot` for kernels 6.2 to 6.14 where x86 moved it). The verifier prunes
the branch for the one that does not exist. Softirq on interrupt exit comes
from the nesting counter above. A wakeup from interrupt context carries no
task identity, since `current` is a bystander.

## The root's buckets

On the root, every interval is clipped to the frame window, and

    oncpu + oncpu_fault + oncpu_fault_major + oncpu_reclaim + oncpu_compact
      + runnable + runqueue + Σ block_* == frame_ns

`check_hitches.py` asserts this on every record.

| JSON name | Meaning |
|---|---|
| `oncpu` | running, outside the kernel stalls below (measured from switch-in to switch-out, not a remainder) |
| `oncpu_fault`, `oncpu_fault_major`, `oncpu_reclaim`, `oncpu_compact` | on-CPU time inside those kernel paths, carved out of `oncpu` at each switch-out and at frame end; each excludes what ran nested inside it, so the three carve the same time once |
| `runnable` | preempted: runnable, not running |
| `runqueue` | woken, waiting for a CPU |
| `block_present` … `block_other` | blocked, named by the first rule that applies, in this order: inside present → `block_present`; `gpu_depth` → `block_gpu`; `in_iowait` → `block_io`; syscall class → `block_futex`, `block_poll`, `block_timer`; woken from interrupt context → `block_timer`; woken by a task → `block_task`; else `block_other` |
| `resolved_wait_oncpu`, `resolved_inherited` | not part of the sum: `block_task` time split by what the waker was doing (below) |

"What was asked beats who answered": a futex wait ended by its timeout
interrupt is still `block_futex`. Present outranks the GPU because a FIFO
present may block on a fence.

## Wake chains

**The link.** At `sched_waking`, the wakee's `links[0]` describes the waker:
pid, tgid, comm; its runnable (`sched_ns`) and blocked time since the wakee
began to wait (from the waker's checkpoints); the cause and kernel stack of
its last blocked interval; and its last holder in that window (`p_*`, with
`HT_HOP_QUEUED` if it was queued rather than preempted). The waker's own
links are then copied in behind it. The copy is always four links, as the
verifier requires constant-size copies, and is trimmed logically. If the
waker's `seq` changed during the copy (another CPU woke the waker
meanwhile), the copy is discarded.

**Resolution.** When the root switches in after a blocked interval
`[lo, hi)` ended by a task, `resolve()` computes

    inherited  = min(hi − lo, waker's runnable + blocked time in the window)
    wait_oncpu = (hi − lo) − inherited

`wait_oncpu` is the root waiting for a thread that was running: the game's
own work, not a stall. For a stall group's longest interval it walks the
chain hop by hop, each hop's window narrowed to the previous hop's wait, up
to `--hops` (default 5).

**Where a chain stops.**
- At an interrupt or the idle task.
- At a waker that was running: off-CPU for less than `--min-stall`, 100 µs.
- At a waker that spent most of the wait runnable. The cause is then whoever
  held its CPU, so the hop names that holder instead of continuing to
  whoever last woke it.

**Hop flags** (`HT_HOP_*`; JSON names):
- `irq`, `softirq`, `irqexit`, `idle`: how the wait ended.
- `oncpu`: the waker was running.
- `trunc`: the chain continues past the last hop kept.
- `stale`: the waker's own wait ended before this wait began.
- `hist`: the waker's history was shorter than the wait, so the time is a
  lower bound.
- `preemptor`: on a group of preempted time, the task that took the CPU.
- `queued`: the holder held the CPU while the task was queued.

## Stall groups

The root's off-CPU intervals in a frame are grouped by `(cause, key_pid)`:

- blocked time by the thread that woke the root, or 0 for an interrupt;
- preempted time by the process that took the CPU;
- run-queue time in a group of its own.

There are seven slots and a catch-all eighth (`HT_STALL_OTHER`, `"other"`
in JSON). An interval of at least 100 µs takes a free slot, or displaces the
smallest group if it is longer than that group's whole total; the displaced
group and anything too short go into the catch-all. So the groups always add
up to the root's off-CPU time.

Each group keeps its total, its count, its longest interval and when it
began, its inherited time, the root's stack for that interval, and the chain
behind it. The front end ranks groups by total and prints those above 3% of
the frame. On-CPU kernel stalls are buckets, not groups, so they are never
ranked (see [todo.md](todo.md)).

## Interval log

With the timeline on (the default; `--no-timeline` turns it off), every
off-CPU interval of every thread of the target process is appended as it
ends to `event_bufs`: up to 2,048 `struct ht_event` of 72 bytes per frame.
Each entry is claimed with an atomic increment, since threads on several
CPUs append at once. The frame's epoch picks one of two buffers, so the
mark can send the frame that closed while the next is already logging.

For a hitch, the record is reserved and submitted first, then the buffer
(`HT_KIND_EVENTS`). The reader holds a record until its events arrive.
Sending the log first lost the interval of any thread the root woke on its
way into present.

## Per-thread lines

Each thread of the target gets a fixed slot, up to 32, holding:
- its own buckets;
- its longest off-CPU interval and that interval's cause and stack;
- the task that preempted it longest;
- if it was still off-CPU at frame end, how long it had been waiting
  (`open_ns`).

Slots are written by fixed index rather than at a running output index:
otherwise the unrolled copy loop exceeds the verifier's one-million-
instruction limit.

## Output

`struct ht_record` is 10,344 bytes. The JSONL file (`-o`) holds:

- a header: `{"type":"header","trace_start_ns":…,"pid":…,"budget_ns":…}`;
- one object per hitch:
  - `frame_id`, `root_pid`, `root_tgid`, `root_comm`;
  - `frame_start_ns`, `frame_end_ns`, `frame_ns`, `budget_ns`, `stall_ns`;
  - `cause_ns` (every bucket by name), `nstalls`, `flags`;
  - `stalls[]`, ranked. Each has `cause`, `ns`, `count`, `longest_ns`,
    `start_ns`, `inherited_ns`, `other`, `kstack`, `ustack`, `blocked_in`
    and `hops[]`; a hop has `pid`, `tgid`, `comm`, `ns`, `sched_ns`,
    `cause`, `flags`, `blocked_in`, `preemptor`;
  - `worst`, a copy of the largest group kept for older readers. Its `ns`
    is a group total; the longest single interval is `longest_ns`;
  - `threads[]`;
  - `events[]` (the interval log) and `events_dropped`;
  - `kstacks`: each kernel stack the record refers to, once;
- a summary at exit: `{"type":"summary",…}` with the counters of
  `enum ht_stat`.

The human report on stdout prints the same record: header, buckets,
resolution, groups with chains, thread lines.

Frame flags:
- `open_stall`: a stall was still open at frame end.
- `no_root_state`: the root's state was created mid-frame.
- `lost`: some accounting was dropped.
- `threads_full`: more than 32 threads.
- `events_full`: more than 2,048 intervals.

## Checks

- `tests/check_hitches.py`, on every hitchbench record:
  - the partition identity;
  - the groups summing to the root's off-CPU time;
  - per injector class, the cause and chain it must show (the `CLASS_SPECS`
    table in `tests/run_hitch_tests.sh`).
- `tests/check_timeline.py`: a thread's logged intervals, clipped to the
  frame, match its buckets to within 50 µs.
- `tests/test_check_hitches.py` tests the checker. It reads `enum ht_cause`
  from `src/hitchtrace/hitchtrace.h`, so a renamed bucket fails it.
- `hitchtrace.c` has a `static_assert` that every bucket has a JSON name.

## Constants

| What | Value | Where |
|---|---|---|
| Hops kept per group | 5 | `HT_MAX_HOPS` |
| Stall groups | 7 + 1 catch-all | `HT_MAX_STALLS` |
| Per-thread lines | 32 | `HT_MAX_THREADS` |
| Logged intervals per frame | 2,048 × 72 B | `HT_MAX_EVENTS` |
| Kernel stack depth | 32 frames | `HT_PERF_MAX_STACK` |
| Checkpoints, grain | 32, 2 ms | `HT_CKPT`, `HT_CKPT_GRAIN_NS` |
| Minimum stall, minimum queue wait for a holder | 100 µs | `--min-stall` |
| Median window, warm-up | 19, 8 frames | `HT_MEDIAN_WINDOW`, `HT_MEDIAN_MIN` |
