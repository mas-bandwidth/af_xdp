# 008

Try reducing to the gold standard of 64 byte packets on the wire at the ethernet level.

According to https://www.fmad.io/blog/what-is-10g-line-rate I should be able to get ~14.8M 64 byte packets per-second on wire as the 10gbps standard line rate.

To get a 64 byte packet at the ethernet layer:

1. 20 bytes for IPv4 header
2. 8 bytes for UDP header
3. 4 bytes for the ethernet frame check sequence

This gives us a UDP payload size of *32 bytes* which will result in the standard 64 byte ethernet packet.

