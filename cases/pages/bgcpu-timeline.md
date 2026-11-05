## What was going on

Every 0.7 s a background program, `bgload`, wakes four threads that each burn 20 ms of CPU, on the same four CPUs the game uses. It touches nothing the game uses; it only competes for the cores. 45 of this run's 98 hitches overlap one of its bursts.

## How to read it

1. **What it was waiting for**: the largest group is 14.1 ms of the main thread waiting on a lock, ended by `mg_worker0`. That much is in every frame: the main thread always waits for its workers. The line under it is what differs: `mg_worker0` was *kept off the CPU 6.0 ms, last preempted by bgload2*.
2. **The process's threads during this frame**: `mg_worker0` and `mg_worker1` each spent 6.0 ms ready to run but without a CPU, and each line names who took it: `bgload2` and `bgload1`. `mg_worker2`, which nobody preempted, ran 12.9 ms straight and finished first.
3. **Timeline of the frame**: on the two workers' lanes the orange stretches are those lost CPUs, while the main thread's lane stays in its lock wait until the last worker is done.
4. Compare [frame 1898], a hitch outside a burst: the same 13 ms wait for a worker, but that worker *was running* the whole time, and no other program is named anywhere in the record.

## What this tells you

The frame was late because two of the game's workers lost their CPUs to another program for about 6 ms each, and the main thread had to wait for them. The record names the program. The fix is outside the game's code: when and where that program runs (its priority, its CPU affinity, or not running it while playing).

## What it can't tell you

- Only the *last* thread to push each worker off its CPU is named. If several programs took turns, the others don't appear.
- The main thread's own 2.0 ms *waiting for a CPU* after it was woken names nobody: for a thread that was woken and found its CPU busy, the record has no occupant.
- `bgload` has no lane of its own, so what it was doing is outside the record.
