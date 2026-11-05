## What was going on

The game keeps a 512 MB world in memory but runs under a 420 MB memory limit, so part of the world always sits in swap. Every 30th frame the main thread walks 2 MB of the world, as on entering an area it hasn't used for a while, and the walk faults those pages back in from swap.

## How to read it

1. **Where the frame went**: blocked 34.9 ms, of which 16.6 ms on disk I/O, plus 3.1 ms *running in the kernel* on page faults that read from disk. The game never asked to read a file, so disk waits on the main thread mean paging.
2. **What it was waiting for**: the disk group is 64 waits, the longest 4.6 ms, in `folio_wait_bit_common`. Open *This frame as a table, with stacks*: the stack runs `do_swap_page` ← `handle_mm_fault` ← `exc_page_fault`, a page fault waiting for a page to come back from swap. The 64 waits match the 64 major page faults the game itself counted in this frame.
3. **Timeline of the frame**: the main thread's lane is a run of short yellow waits, each ended by an interrupt (the small triangles above the lane).

## What this tells you

The frame was late because its own thread touched memory that had been pushed out to swap and had to wait 64 times for it to come back: 16.6 ms of disk and 3.1 ms of fault handling, in a frame 18.3 ms over budget. The cause is memory pressure; the remedies are on that side (more memory, a smaller working set, or keeping that data resident).

## What it can't tell you

- Why memory was short. The record doesn't see the 420 MB limit, who else used memory, or when these pages were pushed out.
- Mind the ranking. In this frame the disk group is first by a hair (16.6 ms against 15.9 ms of the usual wait for the workers); in many frames of this run it comes second. When the top group is the ordinary wait for the workers, look at the next one.
