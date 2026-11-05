## What was going on

Every 30th frame the main thread asks its streaming thread, `mg_stream`, for an 8 MB asset and waits until it has arrived. The loading is asynchronous in name only: the frame depends on it. The asset is read from disk every time.

## How to read it

1. **Where the frame went**: the main thread was blocked 32.0 ms, mostly *waiting on a lock* (25.3 ms) plus a 6.5 ms timer sleep. "Lock" alone would send you looking for lock contention in the game's code.
2. **What it was waiting for** splits that lock time in two. 18.2 ms was ended by `mg_stream`, which *was itself waiting on disk I/O for 17.3 ms in blk_io_schedule*, ended by an interrupt (the disk finishing). 7.2 ms was ended by `mg_worker2`, which was running: the ordinary wait for the workers, drawn grey.
3. **Timeline of the frame**: `mg_stream`'s lane is yellow (disk) in eight reads from about 7 ms to 25 ms, and the main thread's lock wait ends right after the last one.
4. The 6.5 ms timer sleep is the frame limiter, which every frame has.

## What this tells you

The frame was late because it waited for a disk read made on another thread: 17 ms of disk in a frame 11.7 ms over budget. The cause is in the game's loading: start the read earlier, or don't make the frame wait for it.

## What it can't tell you

Why the disk took 17 ms. The chain ends at the disk's completion interrupt, so a slow disk, a busy disk and a large read all look the same. The game knows the read was 8 MB; the record doesn't. The `iodisk` recording shows the same picture caused by another program sharing the disk.
