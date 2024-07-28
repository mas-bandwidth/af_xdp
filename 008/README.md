# 008

According to https://www.fmad.io/blog/what-is-10g-line-rate I should be able to get ~14.8M 64 byte packets per-second on wire as the 10gbps standard line rate.

To get a 64 byte packet at the ethernet layer:

1. 20 bytes for IPv4 header
2. 8 bytes for UDP header
3. 4 bytes for the ethernet frame check sequence at the end of the packet.

This gives us a UDP payload size of *32 bytes* which expands to the standard 64 byte ethernet packet on the wire.

Results:

```
sent delta 10695364
sent delta 10685704
sent delta 10684636
sent delta 10689440
sent delta 10688332
sent delta 10687644
sent delta 10681188
sent delta 10687948
sent delta 10686944
sent delta 10690228
sent delta 10689076
sent delta 10690476
sent delta 10685715
sent delta 10689797
```

With a smaller packet size I can send much more packets per-second one just one core. This is a pretty good result, especially using only one core!

I'm not exactly sure why we can't hit the 14.8M packets number. Does anybody have any idea what I am missing? 

What do I have to do to be able to hit 14.8M packets per-second on a 10G NIC with AF_XDP?

Please email glenn@mas-bandwidth.com if you know!
