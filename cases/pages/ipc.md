## What was going on

Every frame the game makes 30 blocking request-and-reply calls to `mg_server`, a separate process on CPU 5, the way a game under Wine calls wineserver. In bursts, `bgload` runs on that same CPU, so the server is ready to answer but cannot run.

## How to read it

1. **What it was waiting for**: a group of 30 waits, 7.9 ms in all and 7.0 ms for the longest, in `__skb_wait_for_more_packets` (waiting for a reply on a socket). It was ended by `mg_server`, which was *kept off the CPU 7.0 ms*.
2. **Timeline of the frame**: the main thread's lane starts with one long purple wait, then a comb of short ones: one slow call, then 29 quick ones.
3. Compare [frame 1003], outside a burst: the waits for the server there add up to about 2 ms, with no single long one.

## What this tells you

One of the 30 calls took 7 ms because the server process could not get its CPU. The chain crosses into another process to say so. Neither the game's code nor the server's work was slow: the server was waiting to run.

## What it can't tell you

- Who had the server's CPU. The server was not pushed off its CPU; it was woken and found the CPU already busy. hitchtrace names the thread that pushes a thread off a CPU, not the one occupying a CPU when a thread wakes, so `bgload` is not named here.
- What the server did. `mg_server` is another process: it gets no lane, and its own waits are not recorded.
