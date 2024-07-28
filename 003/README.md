# 002

_(Work in progress...)_

Let's send even more packets.

By using *two cores* I'm able to send ~12.2 million 100 byte UDP packets per-second.

Going any further requires a NIC faster than 10G, but it _should_ scale linearly as you add more cores up to the maximum line rate of your NIC.

To ensure that the packets are evenly distributed across RSS queues on the server, I lie about the source IP address when I construct the raw packet to send over the AF_XDP socket. This way received packets get load balanced across all RSS queues on the 10G NIC (hash of source and dest IP address), and it's much cheaper than buying hundreds of linux machines for testing :)

To build, first make sure you have Linux setup according to the instructions here: https://mas-bandwidth.com/xdp-for-game-programmers/

Next, edit the source to specify your network interface, and your own ethernet addresses and IP addresses for your client and server (use ifconfig to see them).

In client.c:

```c
const char * INTERFACE_NAME = "enp8s0f0";

const uint8_t CLIENT_ETHERNET_ADDRESS[] = { 0xa0, 0x36, 0x9f, 0x68, 0xeb, 0x98 };

const uint8_t SERVER_ETHERNET_ADDRESS[] = { 0xa0, 0x36, 0x9f, 0x1e, 0x1a, 0xec };

const uint32_t CLIENT_IPV4_ADDRESS = 0xc0a8b779; // 192.168.183.121

const uint32_t SERVER_IPV4_ADDRESS = 0xc0a8b77c; // 192.168.183.124
```

In server.c:

```c
const char * INTERFACE_NAME = "enp8s0f0";
```

Next, we want real CPU cores, not hyperthread cores on the same CPU.

Disable hyperthreading on both your client and server Linux boxes:

```console
echo off | sudo tee /sys/devices/system/cpu/smt/control
```

Build:

`make`

Run the server:

`sudo ./server`

Run the client:

`sudo ./client`

You should see this on the server:

```
received delta 12119827
...
```
