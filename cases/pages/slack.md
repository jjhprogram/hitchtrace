## What was going on

The game caps itself at 60 fps by sleeping until the next frame is due (the frame limiter). In bursts, a preloaded library makes those sleeps imprecise: the kernel may wake the thread up to 8 ms late. This stands in for the Linux 7.0 timer regression in the case catalogue, where timers fired late.

## How to read it

1. **Where the frame went**: the main thread spent 11.1 ms in a timer sleep.
2. **What it was waiting for**: the sleep is the second group, 11.1 ms, ended by a hardware interrupt (the timer). The first is the usual wait for the workers, which were running almost the whole time.
3. Compare [frame 2367], a hitch outside the bursts: its timer sleep is 1.8 ms. Across this run the limiter normally sleeps about 2 ms.

## What this tells you

Only by comparison. The record says the main thread slept 11 ms in `do_nanosleep`, which is what a frame limiter does every frame, just longer. You see the problem only by noticing that other frames sleep much less. The game's own log knows the truth: in the bursts, the sleeps ended a median 8.1 ms after they were due.

## What it can't tell you

That the sleep was late. hitchtrace doesn't record how long a sleep asked for, so a late wake-up and an intended long sleep look the same, and a frame-limiter sleep reads as normal. Recording the requested wake-up time next to the actual one would make this case readable straight from the record.
