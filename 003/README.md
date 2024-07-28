# 003

The receive side is the bottleneck. Because the packets all come from one IP address, they all get the same hash of source and dest IP address, and are sent to the same receive queue.

To ensure that received packets are evenly distributed across RSS queues, in this version I _lie_ about the source IP address when I construct the raw packet to send over the AF_XDP socket.

Instead of sending the real client source address in the IPv4 header, I set it to 192.168.*.* where the low 16 bits are a counter that I increment with each packet sent, per-send thread.

This way received packets get load balanced across all RSS queues on the 10G NIC (hash of source and dest IP address), and it's much cheaper than buying hundreds of linux machines for testing :)

The results are now much better:

```
sent delta 5960640
sent delta 6087904
sent delta 6095892
sent delta 6096036
sent delta 6108820
sent delta 6091460
sent delta 6092058
sent delta 6097242
sent delta 6093232
sent delta 6095776
```
