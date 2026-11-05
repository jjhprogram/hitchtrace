## What was going on

Every 15th frame the main thread waits for `mg_stream` to read a 2 MB asset, as in `iothread-timeline`. Next to the game, another program, `bgio`, reads a large file in 400 ms bursts every 1.5 s. Both share a disk limited to 200 MB/s of reads, so they compete for it. With the other program idle a load takes about 2 ms; during one of its bursts, a median 57 ms.

## How to read it

1. **Every hitch in the run**: the tall dots, 50 to 120 ms, are loads that ran into a burst.
2. **What it was waiting for**: 63.7 ms of lock wait ended by `mg_stream`, which was *blocked 63.1 ms in blk_io_schedule*, ended by an interrupt. The frame was 65.4 ms over budget: the disk wait is the whole overrun.
3. **Timeline of the frame**: `mg_stream`'s lane is yellow for about 62 ms, in two reads.
4. Compare [frame 929], a load while the other program was idle: the same chain, but the wait on `mg_stream` is 2.6 ms and the frame only just misses its budget.
5. Look at [frame 990], the frame right after a slow load. There is nothing on disk in it, but `mg_worker0` ran 27.8 ms where the other workers ran about 15. After a long load the workers run slower for a few frames (most likely because the cores sat idle; this is not verified). The record shows them as running and cannot say they ran slower than usual.

## What this tells you

The streaming thread waited 63 ms for the disk. The record doesn't say why, but you can work it out: 2 MB in 63 ms is about 32 MB/s, far below what the same disk did a few seconds earlier in [frame 929]. That points at something else using the disk.

## What it can't tell you

Who else was using the disk. `bgio` appears nowhere in this run's records: it never woke or preempted a thread of the game; its own reads were simply served first, inside the kernel's block layer, which hitchtrace does not watch. A slow disk and a busy disk give the same record. Naming the competitor would need block-layer data: how long the game's request was queued, and whose requests were served meanwhile.
