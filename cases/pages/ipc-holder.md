## What was going on

Every frame the game makes 30 blocking request-and-reply calls to `mg_server`, a separate process on CPU 5, the way a game under Wine calls wineserver. In bursts, `bgload` runs on that same CPU, so the server is ready to answer but cannot run.

## How to read it

1. **What it was waiting for**: a group of 30 waits, 8.9 ms in all and 5.5 ms for the longest, in `__skb_wait_for_more_packets` (waiting for a reply on a socket). It was ended by `mg_server`, which was *kept off the CPU 5.5 ms, last queued behind bgload2*. "Queued behind" means the server was woken and found its CPU taken, so it waited in line; it was never pushed off.
2. **Timeline of the frame**: after the frame limiter's sleep, the main thread's lane shows two long purple waits, 5.7 and 2.8 ms, then a comb of 28 short ones: two slow calls, then quick ones.
3. Compare [frame 2411], outside a burst: its 30 waits for the server add up to 1.3 ms, none longer than 0.2 ms, and the server was running rather than kept waiting.

## What this tells you

Two of the 30 calls were slow because the server process could not get its CPU, and the record names who had it: `bgload2`, a thread of the background program. The chain crosses into another process to say so. Neither the game's code nor the server's work was slow: the server was waiting to run, behind another program.

## What it can't tell you

- Who held the CPU before the last one. The record names the thread that was on the server's CPU when the server finally ran. In 4 of this run's 167 burst frames that was a kernel worker that ran just before the server, not `bgload`.
- The second slow call's holder. A group's chain follows its longest wait; the 2.8 ms call is counted in the group, but who held the CPU during it is not named.
- What the server did. `mg_server` is another process: it gets no lane, and its own waits are not recorded.
