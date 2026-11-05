## What was going on

Every 15th frame the game grows by 24 MB and asks for huge pages (2 MB pages instead of 4 KB ones). A helper program holds the machine's free memory as single scattered pages, so a huge page can only be made by moving other memory out of the way (compaction), again on the thread that asked. Once free memory runs out the kernel also reclaims, and sometimes gives up and hands out small pages instead.

## How to read it

1. **Where the frame went**: 33.7 ms running, almost all of it in the kernel. Hover the blue part, or open the table: compaction 22.1 ms, reclaim 5.8 ms, page faults 4.3 ms.
2. Compare [frame 1244]: compaction only 0.3 ms, but 6.7 ms of page faults and 5.7 ms of other running. There the kernel gave up on huge pages and handed out small ones: the game's own counter shows 6,144 page faults instead of a dozen.
3. **What it was waiting for**: in both, only the usual wait for the workers. The memory time is on the CPU, so it isn't listed there.

## What this tells you

In this frame, asking for huge pages cost 22 ms of compaction. In [frame 1244] it cost the extra page faults of falling back to small pages. Either way the frame paid for the huge-page request, and the record shows which way.

## What it can't tell you

- That compaction failed, or that the faults were small pages standing in for huge ones. The record has the time, not the outcome; the game's fault counter shows it.
- A small sample: 42 slow allocations from one 27 s run, on a machine pushed to the edge of its memory.
