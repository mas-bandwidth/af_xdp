# 005

According to this paper, the lowest latency for AF_XDP is obtained when:

1. Busy polling is disabled
2. Polling is disabled (calling poll from the program to do work)
3. Need wakeup is disabled

I've followed these recommendations from the first versions of the program, but now we're not actually trying to optimize away nanoseconds, we're trying to get close to line rate.

It's possible that some of these settings are reducing throughput.

In this version, I force to zero copy mode on the AF_XDP, to make sure that's active and enable the need wakeup. Theoretically, this should help.

Results:

```
```
