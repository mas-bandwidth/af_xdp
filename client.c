/*
    UDP client (userspace)

    Runs on Ubuntu 22.04 LTS 64bit with Linux Kernel 6.5+ *ONLY*

    Derived from https://github.com/xdp-project/xdp-tutorial/tree/master/advanced03-AF_XDP
*/

#include <memory.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <pthread.h>
#include <sched.h>

const char * INTERFACE_NAME = "enp8s0f0";

const uint8_t CLIENT_ETHERNET_ADDRESS[] = { 0xa0, 0x36, 0x9f, 0x68, 0xeb, 0x98 };

const uint8_t SERVER_ETHERNET_ADDRESS[] = { 0xa0, 0x36, 0x9f, 0x1e, 0x1a, 0xec };

const uint32_t CLIENT_IPV4_ADDRESS = 0xc0a8b779; // 192.168.183.121

const uint32_t SERVER_IPV4_ADDRESS = 0xc0a8b77c; // 192.168.183.124

const uint16_t SERVER_PORT = 40000;

const uint16_t CLIENT_PORT = 40000;

#define NUM_FRAMES 4096

#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE

#define INVALID_FRAME UINT64_MAX

struct client_t
{
    int interface_index;
    struct xdp_program * program;
    bool attached_native;
    bool attached_skb;
    void * buffer;
    struct xsk_umem * umem;
    struct xsk_ring_prod send_queue;
    struct xsk_ring_cons complete_queue;
    struct xsk_ring_cons receive_queue;
    struct xsk_ring_prod fill_queue;
    struct xsk_socket * xsk;
    uint64_t frames[NUM_FRAMES];
    uint32_t num_frames;
};

bool pin_thread_to_cpu( int cpu ) 
{
    int num_cpus = sysconf( _SC_NPROCESSORS_ONLN );
    if ( cpu < 0 || cpu >= num_cpus  )
        return false;

    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    CPU_SET( cpu, &cpuset );

    pthread_t current_thread = pthread_self();    

    pthread_setaffinity_np( current_thread, sizeof(cpu_set_t), &cpuset );
}

int client_init( struct client_t * client, const char * interface_name )
{
    // we can only run xdp programs as root

    if ( geteuid() != 0 ) 
    {
        printf( "\nerror: this program must be run as root\n\n" );
        return 1;
    }

    // find the network interface that matches the interface name
    {
        bool found = false;

        struct ifaddrs * addrs;
        if ( getifaddrs( &addrs ) != 0 )
        {
            printf( "\nerror: getifaddrs failed\n\n" );
            return 1;
        }

        for ( struct ifaddrs * iap = addrs; iap != NULL; iap = iap->ifa_next ) 
        {
            if ( iap->ifa_addr && ( iap->ifa_flags & IFF_UP ) && iap->ifa_addr->sa_family == AF_INET )
            {
                struct sockaddr_in * sa = (struct sockaddr_in*) iap->ifa_addr;
                if ( strcmp( interface_name, iap->ifa_name ) == 0 )
                {
                    printf( "found network interface: '%s'\n", iap->ifa_name );
                    client->interface_index = if_nametoindex( iap->ifa_name );
                    if ( !client->interface_index ) 
                    {
                        printf( "\nerror: if_nametoindex failed\n\n" );
                        return 1;
                    }
                    found = true;
                    break;
                }
            }
        }

        freeifaddrs( addrs );

        if ( !found )
        {
            printf( "\nerror: could not find any network interface matching '%s'", interface_name );
            return 1;
        }
    }

    // load the client_xdp program and attach it to the network interface

    printf( "loading client_xdp...\n" );

    client->program = xdp_program__open_file( "client_xdp.o", "client_xdp", NULL );
    if ( libxdp_get_error( client->program ) ) 
    {
        printf( "\nerror: could not load client_xdp program\n\n");
        return 1;
    }

    printf( "client_xdp loaded successfully.\n" );

    printf( "attaching client_xdp to network interface\n" );

    int ret = xdp_program__attach( client->program, client->interface_index, XDP_MODE_NATIVE, 0 );
    if ( ret == 0 )
    {
        client->attached_native = true;
    } 
    else
    {
        printf( "falling back to skb mode...\n" );
        ret = xdp_program__attach( client->program, client->interface_index, XDP_MODE_SKB, 0 );
        if ( ret == 0 )
        {
            client->attached_skb = true;
        }
        else
        {
            printf( "\nerror: failed to attach client_xdp program to interface\n\n" );
            return 1;
        }
    }

    // allow unlimited locking of memory, so all memory needed for packet buffers can be locked

    struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

    if ( setrlimit( RLIMIT_MEMLOCK, &rlim ) ) 
    {
        printf( "\nerror: could not setrlimit\n\n");
        return 1;
    }

    // pin to CPU 0

    pin_thread_to_cpu( 0 );

    // allocate buffer for umem

    const int buffer_size = NUM_FRAMES * FRAME_SIZE;

    if ( posix_memalign( &client->buffer, getpagesize(), buffer_size ) ) 
    {
        printf( "\nerror: could not allocate buffer\n\n" );
        return 1;
    }

    // allocate umem

    ret = xsk_umem__create( &client->umem, client->buffer, buffer_size, &client->fill_queue, &client->complete_queue, NULL );
    if ( ret ) 
    {
        printf( "\nerror: could not create umem\n\n" );
        return 1;
    }

    // get the xks_map file handle

    struct bpf_map * map = bpf_object__find_map_by_name( xdp_program__bpf_obj( client->program ), "xsks_map" );
    int xsk_map_fd = bpf_map__fd( map );
    if ( xsk_map_fd < 0 ) 
    {
        printf( "\nerror: no xsks map found\n\n" );
        return 1;
    }

    // create xsk socket

    struct xsk_socket_config xsk_config;

    memset( &xsk_config, 0, sizeof(xsk_config) );

    xsk_config.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_config.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_config.xdp_flags = 0;
    xsk_config.bind_flags = 0;
    xsk_config.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;

    int queue_id = 0;

    ret = xsk_socket__create( &client->xsk, interface_name, queue_id, client->umem, &client->receive_queue, &client->send_queue, &xsk_config );
    if ( ret )
    {
        printf( "\nerror: could not create xsk socket\n\n" );
        return 1;
    }

    ret = xsk_socket__update_xskmap( client->xsk, xsk_map_fd );
    if ( ret )
    {
        printf( "\nerror: could not update xskmap\n\n" );
        return 1;
    }

    // initialize frame allocator

    for ( int i = 0; i < NUM_FRAMES; i++ )
    {
        client->frames[i] = i * FRAME_SIZE;
    }

    client->num_frames = NUM_FRAMES;

    return 0;
}

void client_shutdown( struct client_t * client )
{
    assert( client );

    if ( client->program != NULL )
    {
        if ( client->attached_native )
        {
            xdp_program__detach( client->program, client->interface_index, XDP_MODE_NATIVE, 0 );
        }

        if ( client->attached_skb )
        {
            xdp_program__detach( client->program, client->interface_index, XDP_MODE_SKB, 0 );
        }

        xdp_program__close( client->program );

        xsk_socket__delete( client->xsk );

        xsk_umem__delete( client->umem );

        free( client->buffer );
    }
}

static struct client_t client;

volatile bool quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = true;
}

void clean_shutdown_handler( int signal )
{
    (void) signal;
    quit = true;
}

static void cleanup()
{
    client_shutdown( &client );
    fflush( stdout );
}

uint64_t client_alloc_frame( struct client_t * client )
{
    if ( client->num_frames == 0 )
        return INVALID_FRAME;
    client->num_frames--;
    uint64_t frame = client->frames[client->num_frames];
    client->frames[client->num_frames] = INVALID_FRAME;
    return frame;
}

void client_free_frame( struct client_t * client, uint64_t frame )
{
    assert( client->num_frames < NUM_FRAMES );
    client->frames[client->num_frames] = frame;
    client->num_frames++;
}

uint16_t ipv4_checksum( const void * data, size_t hdr_len )
{
    unsigned long sum = 0;

    const uint16_t * p = (const uint16_t*) data;

    while ( hdr_len > 1 )
    {
        sum += *p++;
        if ( sum & 0x80000000 )
        {
            sum = ( sum & 0xFFFF ) + ( sum >> 16 );
        }
        hdr_len -= 2;
    }

    while ( sum >> 16 )
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

void client_generate_packet( void * data, int payload_bytes )
{
    struct ethhdr * eth = data;
    struct iphdr  * ip  = data + sizeof( struct ethhdr );
    struct udphdr * udp = (void*) ip + sizeof( struct iphdr );

    // generate ethernet header

    memcpy( eth->h_dest, SERVER_ETHERNET_ADDRESS, ETH_ALEN );
    memcpy( eth->h_source, CLIENT_ETHERNET_ADDRESS, ETH_ALEN );
    eth->h_proto = ETH_P_IP;

    // generate ip header

    ip->ihl      = 5;
    ip->version  = 4;
    ip->tos      = 0x0;
    ip->id       = 0;
    ip->frag_off = htons(0x4000);
    ip->ttl      = 64;
    ip->tot_len  = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + payload_bytes );
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = CLIENT_IPV4_ADDRESS;
    ip->daddr    = SERVER_IPV4_ADDRESS;
    ip->check    = 0; 
    ip->check    = ipv4_checksum( ip, sizeof( struct iphdr ) );

    // generate udp header

    udp->source  = CLIENT_PORT;
    udp->dest    = SERVER_PORT;
    udp->len     = htons(sizeof(struct udphdr) + payload_bytes );
    udp->check   = 0;

    // generate udp payload

    uint8_t * payload = (void*) udp + sizeof( struct udphdr );

    for ( int i = 0; i < payload_bytes; i++ )
    {
        payload[i] = i;
    }
}

void client_update( struct client_t * client )
{
    // queue up packets in transmit queue

    // todo: grab all the frames and fill them with packets to send, send it all in one batch

    /*
        // Here we sent the packet out of the receive port. Note that
        // we allocate one entry and schedule it. Your design would be
        // faster if you do batch processing/transmission

        ret = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
        if (ret != 1) {
            // No more transmit slots, drop the packet
            return false;
        }

        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
        xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len = len;
        xsk_ring_prod__submit(&xsk->tx, 1);
        xsk->outstanding_tx++;
    */

    // send queued packets

    sendto( xsk_socket__fd( client->xsk ), NULL, 0, MSG_DONTWAIT, NULL, 0 );

    // mark completed send frames as free

    uint32_t complete_index;

    unsigned int completed = xsk_ring_cons__peek( &client->complete_queue, XSK_RING_CONS__DEFAULT_NUM_DESCS, &complete_index );

    if ( completed > 0 ) 
    {
        for ( int i = 0; i < completed; i++ )
        {
            client_free_frame( client, *xsk_ring_cons__comp_addr( &client->complete_queue, complete_index++ ) );
        }

        xsk_ring_cons__release( &client->complete_queue, completed );
    }
}

int main( int argc, char *argv[] )
{
    printf( "\n[client]\n" );

    signal( SIGINT,  interrupt_handler );
    signal( SIGTERM, clean_shutdown_handler );
    signal( SIGHUP,  clean_shutdown_handler );

    if ( client_init( &client, INTERFACE_NAME ) != 0 )
    {
        cleanup();
        return 1;
    }

    while ( !quit )
    {
        client_update( &client);
    }

    cleanup();

    printf( "\n" );

    return 0;
}
