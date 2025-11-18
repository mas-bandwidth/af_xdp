# 008

According to https://www.fmad.io/blog/what-is-10g-line-rate I should be able to get ~14.8M 64 byte packets per-second on 10G ethernet.

To get a 64 byte packet at the ethernet layer we need to consider packet overhead:

1. 20 bytes for IPv4 header
2. 8 bytes for UDP header
3. 4 bytes for the ethernet frame check sequence at the end of the packet.

That's a total of 32 bytes of overhead per-packet.

Since we want 64 bytes on the wire per-packet, we need a UDP payload of _32 bytes_.

Results:

```
received delta 10933782
received delta 10957707
received delta 10927147
received delta 10953418
received delta 10955817
received delta 10956795
received delta 10952854
received delta 10928762
```

Very close!
