# 007

It looks on the client side that only 8 cores are actually doing work, even though I have 32 cores that should be sending packets. 

```
glenn@vision:~$ top

top - 11:19:43 up 3 days, 22:45,  4 users,  load average: 33.58, 15.51, 18.62
Tasks: 858 total,   9 running, 849 sleeping,   0 stopped,   0 zombie
%Cpu(s):  0.0 us,  0.3 sy, 79.9 ni,  0.0 id,  0.0 wa,  0.0 hi, 19.8 si,  0.0 st
MiB Mem :  64256.0 total,  57773.8 free,   4429.9 used,   2052.3 buff/cache
MiB Swap:  20479.5 total,  20479.5 free,      0.0 used.  56411.4 avail Mem

    PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND
  31687 root      26   6  801864 529920   4352 S  2568   0.8  15:19.50 client
     15 root      20   0       0      0      0 R  79.1   0.0 483:13.15 ksoftirqd/0
     25 root      20   0       0      0      0 R  79.1   0.0   5170:11 ksoftirqd/1
     31 root      20   0       0      0      0 R  79.1   0.0 495:56.73 ksoftirqd/2
     43 root      20   0       0      0      0 R  79.1   0.0 480:38.81 ksoftirqd/4
     37 root      20   0       0      0      0 R  78.8   0.0 483:12.97 ksoftirqd/3
     49 root      20   0       0      0      0 R  78.8   0.0 480:23.82 ksoftirqd/5
     55 root      20   0       0      0      0 R  78.8   0.0 483:30.50 ksoftirqd/6
     61 root      20   0       0      0      0 R  78.8   0.0 483:19.91 ksoftirqd/7
  31721 glenn     26   6   23852   4864   3328 R   0.7   0.0   0:00.17 top
  26527 root      20   0       0      0      0 I   0.3   0.0   0:00.81 kworker/u263:2-events_unbound
  28694 root      20   0       0      0      0 I   0.3   0.0   0:01.11 kworker/u264:5-events_power_efficient
      1 root      20   0  166608  11404   8084 S   0.0   0.0   0:04.71 systemd
      2 root      20   0       0      0      0 S   0.0   0.0   0:00.15 kthreadd
      3 root      20   0       0      0      0 S   0.0   0.0   0:00.00 pool_workqueue_release
```

Is this the bottleneck?

It's not the RSS queues, those settings are fine. 

Is there something on the transmit side that steers transmitted packets to specific CPUs?

Some googling and I find "XPS (Transmit Packet Steering)"

Is this active during AF_XDP sends? My assumption is that each queue n is pinned to CPU core n, and work will be done there.

If instead, sends are being done on different cores to where we queue up the packets, this could explain the perf difference between my setup, and the expected line rate.

https://github.com/xdp-project/xdp-cpumap-tc



...