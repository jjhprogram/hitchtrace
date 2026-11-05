## What was going on

A control case. Every 30th frame the main thread burns 12 ms of CPU in its own code, like a shader compile or a heavy script. Nothing else on the machine is in the way.

## How to read it

1. **Where the frame went**: the main thread was *running* 12.3 ms. In most hitches it runs under 1 ms.
2. **What it was waiting for**: only the usual wait for the workers (14.4 ms, grey: the worker was running the whole time) and a short timer sleep. No group is coloured, so nothing held the frame up.
3. **Timeline of the frame**: the main thread's lane is grey from about 2 ms to 14 ms, while the workers sit waiting for work.

## What this tells you

This hitch is the game's own CPU time, not a stall. hitchtrace's job here is to say so and not invent a cause, and it does: in 91% of these frames it blames no stall.

## What it can't tell you

What the main thread was running. Stacks are taken when a thread stops running, not while it runs. Use a CPU profiler (`perf`, or the engine's own profiler) on these frames.
