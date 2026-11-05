## What was going on

The game's world fills its 420 MB memory limit. Every 15th frame the main thread allocates and fills 8 MB of new memory. With the limit reached, the kernel first has to push something else out to swap before it can hand over new pages, and it does that work on the thread that asked (direct reclaim). Without the limit the allocation takes about 3.5 ms; under it, the slow ones take a median 13.6 ms.

## How to read it

1. **Where the frame went**: the main thread ran 14.5 ms, most of it *in the kernel* (the blue part of the bar). Hover it, or open *This frame as a table, with stacks*: 9.5 ms reclaim, 2.4 ms page faults.
2. **What it was waiting for**: only the usual 13.0 ms wait for the workers. Reclaim is time *on* the CPU, so it isn't in this list. Read the bar above the list, not just its top entry.
3. Compare [frame 929], an allocation that needed no room made: 2.0 ms of page faults and no reclaim.

## What this tells you

The frame was late because the kernel spent 9.5 ms freeing memory before the game could have its own. The 2.4 ms of page faults is what any 8 MB allocation costs here. The cause is memory pressure at the moment of allocating.

## What it can't tell you

- What was pushed out, where it went, or why memory was full: the record has the time spent reclaiming, not what was reclaimed.
- When in the frame it happened. The timeline shows the main thread simply running; the record has the amount of kernel time, not its position.
