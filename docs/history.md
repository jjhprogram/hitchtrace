# History

How hitchtrace got to its current design, and what found each change. Dates
are when the work was done; several commits were made later, on 2026-11-05.
The current design is in [design.md](design.md).

## Before the prototype (2026-09-07 to 2026-09-21)

- **Research and positioning** (a claude.ai design conversation,
  2026-09-07 to 2026-09-19).
  - **Verdict:** close prior work exists, so the novelty is narrow but
    real. Every piece exists somewhere; nobody has combined them for
    unmodified Linux games.
  - **Defensible contributions:**
    - frame-scoped accumulation with the commit/discard decision at frame
      end, which ftrace's declarative language cannot express;
    - cause-gated emission;
    - online wake chains for outlier frames;
    - measurements.
  - **Conceded:** that eBPF reduces data movement (the founding premise of
    BPF tracing), selective capture (flight recorders), the chain-walk
    algorithm (Giraldeau & Dagenais), and the frame marker (Nsight).
  - The paper's background, motivation and related work carry this
    positioning now.
- **Implementation plan** (2026-09-19). A 12-week plan with USDT probes in
  the layer, a separate C++ daemon, per-CPU edge rings, and a user-space
  chain walk over a dumped window.
- **Plan review** (2026-09-21). 43 issues, five independent reviews, each
  major finding put to a refutation pass. Taken up in the prototype:
  - chains copied forward online in task storage, not edge rings;
  - per-root accounting instead of a per-process sum;
  - a frame marker in the layer, since the loader export is bypassed by
    volk and all of Proton;
  - interrupt detection from the preemption count and an `irq_exit_rcu`
    counter;
  - memcg reclaim hooks;
  - one C front end;
  - perf rebuilt with BPF skeletons;
  - delay accounting read from `*_delay_total`.

  The review's points about real games are in
  [bare-metal-plan.md](bare-metal-plan.md).
- **Prototype** (2026-09-21, pushed at `65c66fc`). The root's partition,
  cause taxonomy, present bracket, cause gate, one chain behind the root's
  single longest wait, per-thread lines, hitchbench and its checker, and the
  Vulkan layer.

## Records that pointed at the wrong stall (about 2026-09-21)

A test Vulkan game with seven injectors (vkroids, since deleted), traced in
four configurations:
- The buckets were exact.
- In 15 of 44 hitches (34%), the chain explained the wrong wait. Each time
  it was the frame limiter's sleep, which beat the real cause because:
  - preemption was never a candidate;
  - on-CPU fault time was never a candidate;
  - an I/O stall split into 132 short waits loses to one contiguous sleep.
- The waker's inherited time came from its last off-CPU interval only.

A vkcube control on lavapipe showed what the cause gate is for: 97% of its
hitch time was `block_present`, and `--share 0.5` cut 284 hitches to 69.

## Credit the waker's whole time off-CPU (2026-10-06, `ea5d1a4`)

Found by case 1 (`bgcpu`, first run `cases/out/bgcpu-confirm`):
- The chain credited a median 40% of the waking worker's real delay, since a
  preempted worker loses its CPU several times a frame.
- A hop said "preempted" without saying by whom.

The fix:
- Each task keeps the checkpoint ring of its cumulative off-CPU time.
- A link records the waker's off-CPU time since the wakee began to wait.
- A hop mostly kept off the CPU ends the chain and names the preemptor.

Results (`bgcpu-fixed`):
- The waker's delay credited rose from a median 40% to 100%.
- `bgload` was named on the first hop in 90% of burst frames; this was
  impossible before.

## Stall groups (2026-10-06, `4ca8a3b`)

Explaining only the longest single wait failed when time was lost in many
short waits, or to preemption. The root's intervals are now grouped by cause
and by who ended them, and each group carries its own chain. On the same
hitchbench records (`tests/rank_rules.py`), the injected cause ranked first:

| Class | Injected frames | Longest single wait | Largest group |
|---|---|---|---|
| `thousand_cuts` (40 sleeps of 0.5 ms) | 16 | 0 | 16 |
| `preempt` | 17 | 0 | 17 |
| `io` | 18 | 13 | 13 |
| the other six classes | 100 | 100 | 100 |
| **Total** | **151** | **113 (75%)** | **146 (97%)** |

- In case 3 (`mem`), the swap-in arrives as 64 waits of under 0.7 ms each.
  The old rule picked it in 8 to 15% of frames; the groups pick it in every
  frame, ranked by stall time.
- The record grew from 7,056 to 10,336 bytes.
- The JSON keeps a `worst` key for older readers, now the largest group.

## The interval log and hitchview (2026-10-06 and 07, `503311a`, `af839cc`)

- Records gained every off-CPU interval of every thread of the process (the
  timeline), sent right after the record.
- `tests/check_timeline.py` showed the log and the buckets agree to within
  50 µs on 2,294 thread lines from the game and 511 from hitchbench.
- Cost: a median 16 to 22 intervals per hitch, about 1.5 KB. No measurable
  slowdown in an interleaved A/B test.
- `tools/hitchview.py` draws them.

A page where a person judges the cause was chosen over the idea case 2
raised: ranking each group against a baseline of normal frames.

## Case studies 2 to 9 (2026-10-06 to 07)

Storage, the shared disk, memory, late timers, the server, the CPU spike,
one core, new memory, huge pages. Results are in `cases/README.md`. What
they could not show became items in [todo.md](todo.md):
- that a sleep ended late;
- who else was using the disk;
- which CPU, and whether other cores sat idle;
- on-CPU kernel stalls ranked as groups.

## Reclaim inside a page fault counted once (2026-10-07, committed `73f10de`)

Found by case 8 (`alloc`). Fault time included the reclaim that ran inside
the fault, so reclaim read about 1.7 ms against 10.9 ms of "fault" per
record. Now a fault's time excludes reclaim and compaction nested in it, and
reclaim excludes nested compaction. Fault time became the same 2.3 ms in
slow and fast allocations; the rest is reclaim.

## Evaluation harnesses (2026-11-03 to 05)

- `tools/overhead`, the cost pilot. An earlier pilot
  (`build/overhead/pilot-20261103-103426`) was spoiled by harness bugs,
  listed in `tools/overhead/README.md`.
- `tools/catch`, the rare-hitch test. Its protocol was fixed before the
  runs; four changes found in check runs are recorded in
  `tools/catch/README.md`.
- The delayacct figures from the overhead pilot turned out invalid. A
  thread is delay-accounted only if `kernel.task_delayacct` was on when the
  thread was created.

## Name who held a queued task's CPU (2026-11-05, `be6b7be`)

Found by the catch test: `ipc`'s culprit was named in 0 of 33 hitches. The
server was asleep when `bgload` took its CPU, so it waited in the run queue
without being preempted, and no event named anyone. At the switch-in ending
a run-queue wait of at least 100 µs, hitchtrace now records the task leaving
the CPU as the last holder (`queued`).

| Rerun | Culprit named | Noise hitches naming it |
|---|---|---|
| Catch test, `ipc` (`catch-ipc-holder`) | 29 of 29 | 0 of 88 |
| Case 5 (`ipc-holder`) | 163 of 167 | 1 |

hitchbench gained the `server_queued` class.

## Comparison tools and the paper (2026-11-06 to 08)

- perf `--off-cpu` (perf rebuilt with BPF skeletons) and Perfetto (as a
  flight recorder) were added to both harnesses, with rules written before
  their runs.
- The paper moved from the USENIX draft to the lab's IEEEtran layout, then
  became a shorter draft with background and motivation separated and four
  claim-based evaluation questions.

## Repository cleanup (2026-11-09)

- **chaingraph removed** (`a594b58`), with its tests, `chaingraph-svg.sh`,
  the FlameGraph submodule, and its option in the harnesses. It was the
  whole-run wake-chain profiler the repository started with. The last
  commit with it is `b114afa`. The measured overhead pilot and catch run
  include a chaingraph option; the docs and paper leave those rows out.
- **Sources moved** (`e4f0c13`):
  - the tool, symbolizer, layer and viewer under `src/`;
  - Hitch Lab to `cases/hitchlab/`;
  - the Qt viewer renamed from hitchexplorer to hitchtracegui.
- **Docs consolidated.** These were deleted from the tree, last present in
  `e4f0c13`:
  - the September plan (`docs/hitchtrace-implementation-plan.md`);
  - its review (`docs/hitchtrace-plan-review.md`);
  - the design-conversation summary (`docs/design-conversation-summary.md`).

  Run `git show e4f0c13:docs/<file>` to read them. What still applied went
  to [bare-metal-plan.md](bare-metal-plan.md), [todo.md](todo.md) and
  [real-world-cases.md](real-world-cases.md). Four exported Claude Code
  transcripts from 2026-09-30 were deleted too; they were never in git.

## A torn wake-chain copy (2026-11-17)

Found by reading the code, not by a run. The copy of the waker's chain
compared `seq` before and after, but did not check that it was even to
begin with. A copy taken while another CPU was rewriting the waker's links,
and checked before that rewrite finished, passed with half-old, half-new
links. The waker's `depth` was also read after the check, so it could
belong to a newer chain than the links copied. Both are fixed: an odd `seq`
discards the copy, `depth` is read inside the window, and compiler barriers
hold the order on both sides. The effect was confined to chain hops past
the first, and needed a waker woken by another CPU between setting its
state and `schedule()`, while issuing a wakeup itself.
