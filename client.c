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
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>

#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE

struct client_t
{
    int interface_index;
    struct xdp_program * program;
    bool attached_native;
    bool attached_skb;
    void * buffer;
    struct xsk_umem * umem;
    struct xsk_ring_prod send;
    struct xsk_ring_cons receive;
    struct xsk_ring_prod fill;
    struct xsk_ring_cons complete;
    struct xsk_socket * xsk;
    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;
};

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

    // allow unlimited locking of memory, so all memory needed for packet buffers can be locked.

    struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

    if ( setrlimit( RLIMIT_MEMLOCK, &rlim ) ) 
    {
        printf( "\nerror: could not setrlimit\n\n");
        return 1;
    }

    // allocate buffer for umem

    const int buffer_size = NUM_FRAMES * FRAME_SIZE;

    if ( posix_memalign( &client->buffer, getpagesize(), buffer_size ) ) 
    {
        printf( "\nerror: could not allocate buffer\n\n" );
        return 1;
    }

    // allocate umem

    ret = xsk_umem__create( &client->umem, client->buffer, buffer_size, &client->fill, &client->complete, NULL );
    if ( ret ) 
    {
        printf( "\nerror: could not create umem\n\n" );
        return 1;
    }

    // get the xks_map file handle

    struct client_map * map = bpf_object__find_map_by_name( xdp_program__bpf_obj( client->program ), "xsks_map" );
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

    ret = xsk_socket__create( &client->xsk, interface_name, queue_id, client->umem, &client->receive, &client->send, &xsk_config );
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

    // initialize umem frame allocation

    for ( int i = 0; i < NUM_FRAMES; i++ )
    {
        client->umem_frame_addr[i] = i * FRAME_SIZE;
    }

    client->umem_frame_free = NUM_FRAMES;

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
    bpf_shutdown( &client );
    fflush( stdout );
}

int main( int argc, char *argv[] )
{
    printf( "\n[client]\n" );

    signal( SIGINT,  interrupt_handler );
    signal( SIGTERM, clean_shutdown_handler );
    signal( SIGHUP,  clean_shutdown_handler );

    const char * interface_name = "enp8s0f0";   // vision 10G NIC

    const uint32_t server_address = 0xC0A8B77C; // 192.168.183.124

    const uint16_t server_port = 40000;

    if ( client_init( &client, interface_name ) != 0 )
    {
        cleanup();
        return 1;
    }

    while ( !quit )
    {
        usleep( 1000000 );
    }

    cleanup();

    printf( "\n" );

    return 0;
}


// todo: don't really understand the ring buffer operations yet. study the code...
/*
    // Stuff the receive path with buffers, we assume we have enough
    ret = xsk_ring_prod__reserve(&xsk_info->umem->fq,
                     XSK_RING_PROD__DEFAULT_NUM_DESCS,
                     &idx);

    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
        goto error_exit;

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i ++)
        *xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) =
            xsk_alloc_umem_frame(xsk_info);

    xsk_ring_prod__submit(&xsk_info->umem->fq,
                  XSK_RING_PROD__DEFAULT_NUM_DESCS);
*/
