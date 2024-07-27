# 001

Let's send millions of packets per-second.

Using only *one core* on a 5 year old bare metal linux box, I'm able to send ~6.1 million 100 byte UDP packets per-second.

To build:

`make`

To run the server:

`sudo ./server`

To run the client:

`sudo ./client`

You should see this on the server:

```
received delta 6119827
received delta 6117111
received delta 6065739
received delta 6010190
received delta 6103727
received delta 6115668
received delta 6109344
received delta 6113992
received delta 6098613
received delta 6126000
received delta 6127398
received delta 6112895
received delta 6115302
received delta 6119923
received delta 6115348
received delta 6119834
```

To reproduce this result, you'll need a 10G NIC and a 10G router between two bare metal linux machines running linux kernel 6.5+.

I'm using the Intel x540 T2 10G NIC ($160 USD each):

https://www.newegg.com/intel-x540t2/p/N82E16833106083

With a Netgear 10G router ($550 USD):

https://www.newegg.com/netgear-xs508m-100nas-7-x-10-gig-multi-gig-copper-ports-1-x-10g-1g-sfp-and-copper/p/N82E16833122954

You could also try this on Google Cloud, but I doubt you'll get the same performance as bare metal.
