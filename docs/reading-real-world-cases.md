# What hitchtrace would say about the documented stutters

For each case in [real-world-cases.md](real-world-cases.md): what
hitchtrace's record would show, how to read it back to the root cause, and
what it leaves for the developer to find some other way.

**How sure each entry is.** None of these real cases has been run under
hitchtrace yet (that is [bare-metal-plan.md](bare-metal-plan.md)). Each entry
says which of three grounds it stands on:

- **From case N.** A case study in [cases/README.md](../cases/README.md)
  reproduces the mechanism. The record shown is a real one from that run,
  abridged.
- **Expected.** Worked out from [design.md](design.md), not run. Records
  marked *illustrative* are written by hand in the report's format; their
  names and numbers only show the shape.
- **Out of scope.** The cause is not something the kernel sees. The entry
  says what the record shows instead, which is still a result: it rules the
  kernel out.

The catalogue's own caveats carry over: several figures, issue numbers and
quotes in it are not yet checked against their sources.

## How to read a record

A real record (case 2, `iothread`, frame 1619), abridged:

```
hitch frame 1619  34.7 ms  (budget 20.8 ms, over by 13.8 ms)  root mg_main
  oncpu      0.3 ms   runqueue 0.8 ms
  blocked   33.5 ms  futex 26.5 | timer 6.8 | present 0.2
  resolved   waiting on a running task 8.6 ms, inherited stalls 18.1 ms
  stalls     off-CPU time by cause and by who ended it, largest first
     18.8 ms  futex    blocked in __futex_wait
              <- mg_stream              blocked 18.0 ms in blk_io_schedule (2.0 ms of it kept off the CPU)
                <- [softirq]            woken from a softirq
      7.7 ms  futex    blocked in __futex_wait
              <- mg_worker0             was running (waiting for work) 0.0 ms
      6.8 ms  timer    blocked in do_nanosleep
              <- [hardirq]              woken by an interrupt
  threads
    mg_stream    oncpu 0.8  runqueue 2.0  blocked 23.8 futex 7.8 | io 16.0   largest 12.0 ms in blk_io_schedule
```

The same five steps apply to every case below.

1. **The header** says how much time there is to explain: here 13.8 ms
   over budget.
2. **Running or waiting?** The bucket lines add up to the frame.
   - Mostly `oncpu` with nothing in brackets: the game's own CPU work, or
     the same work running slower. hitchtrace's answer is "not a kernel
     stall": go to a CPU profiler (case 6).
   - `oncpu` with `fault`, `fault_major`, `reclaim` or `compact` in
     brackets: the kernel doing memory work on the game's thread (cases 3,
     8, 9). These are **not** ranked as stall groups, so read them here,
     not from the list below.
3. **If waiting, pick the stall group that matters.** Judge it by the time
   it inherited, not by its total. Skip groups whose first hop says "was
   running" (the game waiting on its own work), the frame limiter's
   `timer` group, and `present` (the display's pacing).
4. **Follow that group's chain to its last hop.** The last hop says what
   kind of cause it is.
   - `[hardirq]` or `[softirq]` after a block on I/O: a device, usually
     the disk.
   - "kept off the CPU …, last preempted by X" or "… last queued behind
     X": X had the CPU. X's name is the culprit.
   - "blocked … in *function*": a kernel path, named by the function.
   - "kept off the CPU …" with no name after it: the CPU went idle while
     the thread waited (the idle task is never named as a holder).
   - `[at least]` after a time: the waker's history was shorter than the
     wait, so the time is a lower bound.
5. **The thread lines** give the same breakdown for every thread of the
   game, with its longest wait and who preempted it longest. Use them when
   the main thread waited on several threads.

Two habits avoid the known traps.

- **Rank by stall time, not total.** Every frame has about 10 ms of waiting
  for the workers and 6 ms of limiter sleep, which can outrank a short real
  stall ([cases/README.md](../cases/README.md), case 2).
- **Read the bucket line before the stall list.** On-CPU kernel stalls
  never appear in the list (case 8).

## A. The scheduler

### A1. Wakeups delayed 200–660 ms on a hybrid Intel CPU (scx #3739)

*Expected. The symptom, threads runnable while cores sit idle, is
reproduced by case 7 (`squeeze`).*

Illustrative record, for a game whose render thread was the one held back:

```
hitch frame 8812  702.4 ms  (budget 16.7 ms, over by 685.7 ms)  root GameThread
  blocked  700.1 ms  futex 698.0 | present 0.4
  stalls
    698.0 ms  futex    blocked in __futex_wait
              <- RenderThread           kept off the CPU 661.6 ms, last queued behind data-loop.0
```

**Reading it.** The main thread waited on the render thread. The render
thread had been woken but was not run for 661 ms: it sat in a run queue.
The last task on its CPU was `data-loop.0`, a thread that runs for
microseconds at a time. A thread kept waiting two thirds of a second behind
something that light is not a busy machine. The scheduler put it on the
wrong CPU and never moved it. Repeating the session under the default
scheduler confirms it.

If the CPU had just gone idle when the wait ended, the hop names no holder
at all ("kept off the CPU 661.6 ms"). That reads the same way: runnable,
with nothing in the way.

**What it leaves.**
- **Which CPU, and whether other cores were idle.** Not recorded. That is
  the fact that separates a placement bug from a full machine, and the one
  the #3739 reporter needed (case 7; [todo.md](todo.md)).
- **Only the last holder** is kept, not the one that held the CPU longest
  ([todo.md](todo.md)).
- **If the starved thread is the main thread itself,** its wait appears as
  a `runqueue` group that names nobody. Holders are named on chain hops
  only.

### A2. Multi-second pauses in Helldivers 2 under scx_lavd (scx #234)

*Expected.* The same shape as A1, at seconds rather than hundreds of
milliseconds: one or more game threads "kept off the CPU" for most of a
multi-second frame. A frame that long can also fill the interval log
(`events_full` flag) and mark chain times `[at least]`.

**Reading it.** The game's threads were ready to run for seconds and were
not run. The cause is the scheduler's decision, which hitchtrace does not
see. It narrows the question from "why does the game freeze?" to "why did
the scheduler not run these threads?". The scheduler's own state answers
that: here, scx_lavd's debug dump.

### A3. A single-threaded game kept on E-cores (scx #3259)

*Expected; mostly out of scope.* Every frame is slow, and the record
shows the main thread running longer than the game's work needs, with no
stall (like case 6). A run-queue group appears only once all the E-cores
are busy.

**Reading it.** "Running longer, waiting for nothing" means the CPU itself
was slower: a smaller core, or a lower clock. hitchtrace cannot tell which,
since it records neither the CPU nor its frequency ([todo.md](todo.md)).
Pinning the game to a P-core and comparing settles it.

### A4. The scx_lavd 1.1.3 regression (scx #3750, #3791)

*Expected.* The same signature as A2. Here hitchtrace is useful for the
comparison: run both versions and count the frames whose top stall is a
thread "kept off the CPU". The holders name what got the CPU instead.

### A5. GameMode and the CPU governor

*Out of scope: CPU frequency.* As in A3, the record says the work ran
longer and nothing waited. Case 2b saw the same thing after long disk
loads: workers ran 50% slower with no stall in sight. That outcome points
at frequency or thermals, and a governor A/B test confirms it. Frequency
is not recorded ([todo.md](todo.md)).

### A6. Task graphs (Igalia's LAVD work)

*From case 1 (`bgcpu`).* Min's model is a chain of threads, each waking
the next, where a delay to one in the middle reaches the end. That is
exactly what a chain records:

```
     21.4 ms  futex    blocked in __futex_wait
              <- mg_worker2   kept off the CPU 10.6 ms, last preempted by bgload0
```

**Reading it.** Follow the hops until one was runnable rather than
blocked: that is where the delay entered the graph. Its preemptor or
holder is the cause. In case 1 the chain named the background program in
90% of affected frames.

## B. Memory

### B1. Swap, zram and swappiness on the Steam Deck (CryoUtilities vs SteamOS)

*From case 3 (`mem`).* A real record of a frame that touched swapped-out
memory:

```
  oncpu      3.1 ms  (fault_major)   runqueue 1.0 ms
  blocked   35.4 ms  futex 18.8 | io 14.1 | timer 2.3 | present 0.2
     14.1 ms  io       blocked in folio_wait_bit_common, 64 times, longest 3.7 ms
              <- [softirq]              woken from a softirq
```

The group's kernel stack is
`io_schedule <- folio_wait_bit_common <- do_swap_page <- handle_mm_fault <- exc_page_fault`.

**Reading it.** 64 waits on I/O, each one a page fault waiting for a page
to come back from swap (`do_swap_page`), plus 3 ms on the CPU inside major
faults. The wait count matched the kernel's own major-fault count in 99%
of frames. Swap-in is the cause of the stall.

To settle the folklore, run the same session under each setting
(swappiness, zram, swap size) and count frames whose overrun is mostly
swap-in. That turns contradictory advice into a measurement.

**What it leaves.**
- **Why memory was short:** the record can't say, and can't see who else
  used the memory.
- **zram looks different.** A swap-in from zram is decompression on the
  faulting thread, so it should show as on-CPU `fault_major` time rather
  than an I/O wait. Not yet tested.

### B2. Huge pages and compaction (Arch Wiki vs CryoUtilities)

*From cases 8 (`alloc`) and 9 (`compact`).* Real records:

```
  oncpu     14.5 ms  (fault 2.4, reclaim 9.5)                 # case 8
  oncpu     33.7 ms  (fault 4.3, reclaim 5.8, compact 22.1)   # case 9, huge pages mostly made
  oncpu     13.5 ms  (fault 6.7, reclaim 0.9, compact 0.3)    # case 9, huge pages not made
```

**Reading it.** The bracket says which kernel memory work the frame paid
for:
- **`compact`:** making room for huge pages.
- **`reclaim`:** freeing memory before handing it out.
- **Large `fault` with little `compact`:** probably compaction failed and
  the kernel fell back to many small pages.

If `compact` dominates with transparent huge pages on, the huge-page
setting is costing frames. If `reclaim` dominates, memory is short whatever
the page size.

**What it leaves.**
- **These are not ranked.** In every case-8 and case-9 record, the top
  stall group was the ordinary wait for the workers ([todo.md](todo.md)).
- **Compaction's outcome is not recorded,** so "failed, fell back to small
  pages" is an inference.

### B3. `vm.max_map_count` crashes

*Out of scope.* The failure is a crash or a hang at start-up, not a slow
frame. Where it does slow frames, threads wait on the address-space lock:
blocked in an `rwsem` function, woken by the game thread that held it. The
stack names the lock; the map-count limit behind it is not visible.

## C. Storage

### C1. Steam downloads freezing the system on I/O wait (steam-for-linux #6073)

*From case 2b (`iodisk`).* A real record from a frame that loaded an asset
while another program read from the same disk:

```
hitch frame 2144  86.2 ms  (budget 20.8 ms, over by 65.4 ms)  root mg_main
     63.7 ms  futex    blocked in __futex_wait
              <- mg_stream         blocked 63.1 ms in blk_io_schedule
                <- [softirq]       woken from a softirq
```

**Reading it.** The game did nothing unusual. Its streaming thread waited
63 ms for the disk, in two reads, and the disk interrupt ended the wait.
The storage answered slowly; the game's code is not at fault.

**What it leaves.** **Who else was using the disk.** In all 54 such records
nothing names the other program, and nothing can: it never woke or
preempted a game thread, it just had its requests served first. A slow
disk and a busy disk give the same record. Block-layer data taken only
while a game thread waits on I/O would close this ([todo.md](todo.md)).

The knock-on is visible too. If Steam's own threads also take the game's
cores, they appear as preemptors, as in A6.

### C2. Stutter after an hour with the Steam overlay off (steam-for-linux #5727)

*Expected, and the most useful kind of case for hitchtrace.* The mechanism
was never found, so there is no record to predict. Instead, the first
record answers the first question that was never answered. Read it with
steps 2 to 4:

- **Running:** the game's own work got slower. Use a CPU profiler, or
  suspect frequency (A5).
- **Waiting on a game thread that was running:** the same, one thread
  further on.
- **Waiting on a thread of the Steam client or the compositor:** the chain
  names it, with why that thread was late.
- **Waiting on the kernel:** a memory or I/O bucket says which.

Comparing records from the first hour with records from later shows what
changed.

## D. Wine and Proton

### D1. Wine's synchronisation: wineserver, esync, fsync, ntsync

*Partly from case 5 (`ipc`), which stands in for the wineserver round
trip.* A real record:

```
     10.7 ms  task     blocked in __skb_wait_for_more_packets, 30 times, longest 10.0 ms
              <- mg_server        kept off the CPU 10.0 ms, last queued behind bgload0
```

**Reading it.** 30 waits in one frame, each ended by the same thread of
another process: that is a round-trip pattern. When the server itself was
kept off the CPU, the hop says by whom. Each synchronisation path leaves a
different signature:

| Path | Group | Ended by |
|---|---|---|
| wineserver round trip | `task`, blocked reading from wineserver | `wineserver` |
| esync (eventfd) | `poll` | the game thread that signalled |
| fsync (`futex_waitv`) | `futex` | the game thread that signalled |
| ntsync (ioctl on `/dev/ntsync`) | `other`, blocked in the ntsync driver | the game thread that signalled |

A large count of waits ended by `wineserver` is the overhead that esync,
fsync and ntsync were built to remove. Once a game is on ntsync, the record
still says who signalled, but the wait is filed under `other` and only the
stack says "ntsync" ([todo.md](todo.md)).

### D2. Elden Ring and vkd3d-proton (2022)

*Mostly out of scope.* The causes were in the translation layer and on the
GPU: command pools freed and reallocated each frame, and memory cleared
before use.

**Reading it.** On the CPU side the render thread runs longer, so the
record says "running, not a kernel stall", as in case 6. That directs the
developer to a CPU profiler on the render thread, which would show the
allocator. Any wait for the GPU appears as `gpu` or `present`, and says
only that the GPU was slow. The GPU-side bucket has never run: the
development VM has no GPU.

### D3. Elden Ring's frame-rate drop on skeleton revival (`VKD3D_CONFIG=no_upload_hvv`)

*Out of scope: GPU memory traffic.* If anything, longer `present` or `gpu`
waits. The record points to a GPU tool and no further.

## E. GPU power and capture tools

### E1. amdgpu clock ramp (DPM)

*Out of scope.* The GPU idles between frames and is slow to speed back up.
On the CPU this shows as a longer wait in `present` or `gpu`. The record
says the frame waited on the GPU, which rules the kernel and CPU out.
Forcing the power profile, an A/B test, confirms the cause.

### E2. Capture tools taking time from the game (OBS, GPU Screen Recorder)

*From case 1 (`bgcpu`).* The encoder's threads compete for the game's
cores, and the record names them the way case 1 names `bgload`: as a
preemptor on a chain hop ("last preempted by …"), or on a thread line
("preempted by … for … ms").

**Reading it.** The preemptor's name is the capture tool's thread. If
capture is done by reading frames back from the GPU, the cost lands in
`present` or `gpu` instead, as in E1.

## F. Timers and firmware

### F1. AMD fTPM stalls (kernel Bugzilla 216989)

*Expected; partly out of scope.* The kernel's random-number thread asked
the firmware TPM for data, and the firmware wrote to flash, stalling for
1–2 s. What the record shows depends on where that time goes:

- **If the kernel thread held a CPU** that a game thread was waiting for,
  a chain hop names it ("last preempted by hwrng"), and the reading is
  direct.
- **If the firmware stopped the CPU outright,** the kernel cannot see it.
  The time appears as ordinary running, or waiting, of whatever was on
  that CPU. The record then shows a 1–2 s frame with no stall big enough
  to explain it.

"No stall, and the work did not grow" is itself the pointer to something
below the kernel. The kernel log and disabling the fTPM confirm it.

### F2. The Linux 7.0 timer regression (April 2026)

*From case 4 (`slack`).* Late timer interrupts make the frame limiter
oversleep. The record measures the sleep exactly: the `timer` group,
"blocked in do_nanosleep", 9.5 ms where it is usually 6.

**Reading it.** Only by comparison. The developer knows the limiter's
target, so a timer group longer than that target is an overshoot. If every
timer-woken wait in the game runs long, suspect the kernel's timers.

**What it leaves.** **The overshoot itself.** A sleep asked to be 6 ms that
took 14 looks the same as one asked to be 14, because the requested
duration is not recorded. This is the first gap in [todo.md](todo.md).
Ranking by stall time ignores timer sleeps, so it is blind here too.

### F3. Steam Deck OLED Wi-Fi power saving

*Out of scope.* The stutter is in a Remote Play stream, not in the game's
frames. For a game that waits on the network, the waits show as `poll`,
ended by the network card's interrupt (`[softirq]`): a slow network, with
no reason given.

## Split locks: God of War, Fallout 76, Elden Ring Nightreign

*Expected; not reproduced, since the VM does not expose split-lock
detection.* When a game thread does a split lock, the kernel sleeps it
10 ms ("misery mode", Linux 5.19 and later), then makes it wait on a
semaphore if another core holds the split-lock slot.

Expected record, illustrative:

```
     40.3 ms  timer    blocked in split_lock_warn, 4 times, longest 10.1 ms
              <- [softirq]              woken from a softirq
```

**Reading it.** Read it by the stack, not the bucket. The sleep is not
inside a system call and is ended by a timer, so hitchtrace's rules file it
under `timer`, next to the frame limiter. The catalogue expected `other`;
this needs checking on hardware with split-lock detection.

The kernel stack names the split-lock handler, which identifies the cause
outright. The kernel log's `#AC: … took a split_lock trap` line confirms
it, and `kernel.split_lock_mitigate=0` removes it.

A second thread waiting on the semaphore would show a `task` wait ended by
the kernel worker that releases it.

## Shader compilation (DXVK and others)

*From case 6 (`cpuspike`); out of scope as a cause, but correctly
classified.* There are two shapes:

- **Compiled on the render thread:** the record shows more `oncpu` and no
  stall, as in case 6.
- **Compiled on a worker** (DXVK's compiler threads) while the render
  thread waits: a `futex` group whose hop says the waker "was running".
  That is the game waiting on its own work, not a stall.

**Reading it.** "Running" or "waiting on a running thread" both say: not
the kernel. In the second shape the waker's name says which thread to
point the CPU profiler at. That is the most hitchtrace can do for the most
famous stutter class, and it is the right answer.

## Summary

| Case | Where it shows in the record | Gets to the root cause? | Missing |
|---|---|---|---|
| A1 scx #3739 | hop "kept off the CPU", holder | localises: the scheduler held a thread back | CPU number, idle cores, longest holder |
| A2 scx #234 | same, seconds long | localises to the scheduler | the scheduler's reasoning |
| A3 scx #3259 | `oncpu` up, no stall | no: says "slower CPU" | CPU, frequency |
| A4 scx 1.1.3 | as A2 | compares versions | as A2 |
| A5 GameMode | `oncpu` up, no stall | no | frequency |
| A6 task graphs | multi-hop chain | yes (case 1) | longest holder |
| B1 swap/zram | `io` group with `do_swap_page`; `fault_major` | yes (case 3) | why memory was short |
| B2 THP/compaction | `compact`, `reclaim`, `fault` in brackets | yes, if the bucket line is read (cases 8, 9) | ranking; compaction outcome |
| B3 map count | (crash, not a frame) | no | — |
| C1 Steam downloads | chain to the disk interrupt | half: "the disk was slow" (case 2b) | who else used the disk |
| C2 overlay off | unknown in advance | triage: running or waiting, on whom | — |
| D1 Wine sync | many waits ended by one waker | yes for wineserver (case 5) | ntsync filed under `other` |
| D2 Elden Ring | `oncpu`, `gpu`/`present` | no: points to CPU and GPU profilers | GPU-side causes |
| D3 BAR upload | `gpu`/`present` | no | GPU-side causes |
| E1 amdgpu DPM | `present`/`gpu` | no: "the GPU was slow" | GPU-side causes |
| E2 capture tools | preemptor named | yes (case 1) | — |
| F1 fTPM | holder `hwrng`, or unexplained time | partly | firmware time |
| F2 Linux 7.0 timers | `timer` group longer than usual | only by comparison (case 4) | requested sleep length |
| F3 Wi-Fi power save | `poll` ended by the network interrupt | no (not the game's frames) | — |
| Split locks | `timer` group, split-lock stack | yes, by the stack | a bucket of its own |
| Shader compilation | `oncpu`, or a wait on a running thread | correctly "not the kernel" (case 6) | what the CPU ran |

Where hitchtrace reaches the cause, it is because the cause is a wait or a
CPU taken by something the kernel can name. Where it stops short, the
missing piece is almost always one of the open items in
[todo.md](todo.md):
- the requested sleep;
- the CPU and its idle siblings;
- the block layer;
- frequency.

Where it is out of scope, the record still says which side the problem is
on, which is the first thing a developer needs.
