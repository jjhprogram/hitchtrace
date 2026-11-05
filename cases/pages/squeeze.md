## What was going on

Every 1.5 s, for 300 ms, all of the game's threads are allowed to run only on CPU 0, while CPUs 1 to 3 sit idle. This stands in for a scheduler that packs a game's threads onto too few cores, or for a wrong affinity mask. Nothing else runs on CPU 0: the threads only get in each other's way.

## How to read it

1. **What it was waiting for**: 45.5 ms of lock wait ended by `mg_worker0`, which was *kept off the CPU 30.8 ms, last preempted by mg_worker1*, another of the game's own threads.
2. **The process's threads during this frame**: each worker ran about 15 ms, its usual amount, and spent 23 to 31 ms ready to run but without a CPU, each pushed aside by another thread of the game.
3. **Timeline of the frame**: the worker lanes take turns. Wherever one is grey (running), the others are orange. Their running times add up to 44.8 ms in a 46.7 ms frame: one core's worth.
4. Compare [frame 645], outside a squeeze: workers also wait for CPUs there, but they also run side by side.

## What this tells you

The work didn't get heavier, and no other program took the cores: the game's threads were queued behind each other. That points at where the threads are allowed to run, or at the scheduler, not at the game's code.

## What it can't tell you

Which CPU the threads were on, or that CPUs 1 to 3 were idle. The record has no CPU numbers, and that is the fact that separates a bad placement from a machine that is simply full.
