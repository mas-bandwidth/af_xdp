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

#define INVALID_FRAME      UINT64_MAX

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

    // allocate buffer for umem

    const int buffer_size = NUM_FRAMES * FRAME_SIZE;

    if ( posix_memalign( &client->buffer, getpagesize(), buffer_size ) ) 
    {
        printf( "\nerror: could not allocate buffer\n\n" );
        return 1;
    }

    // allocate umem

    ret = xsk_umem__create( &client->umem, client->buffer, buffer_size, &client->fill, &client->complete_queue, NULL );
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
    if ( clients->num_frames == 0 )
        return INVALID_UMEM_FRAME;
    client->num_frames--;
    uint64_t frame = client->frames[client->num_frames];
    client->frames[client->num_frames] = INVALID_UMEM_FRAME;
    return frame;
}

void client_free_frame( struct client_t * client, uint64_t frame )
{
    assert( client->num_frames < NUM_FRAMES );
    client->frames[client->num_frames] = frame;
    client->num_frames++;
}

void client_update()
{
    // queue up packets in transmit queue

    // ...

    // send queued packets

    sendto( xsk_socket__fd( client->xsk ), NULL, 0, MSG_DONTWAIT, NULL, 0 );

    // mark completed transmit buffers as free

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
        client_update();

        usleep( 1000000 );
    }

    cleanup();

    printf( "\n" );

    return 0;
}










#if 0

/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>

#include <bpf/bpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>

#include "../common/common_params.h"
#include "../common/common_user_bpf_xdp.h"
#include "../common/common_libbpf.h"

#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE      64
#define INVALID_UMEM_FRAME UINT64_MAX

static struct xdp_program *prog;
int xsk_map_fd;
bool custom_xsk = false;
struct config cfg = {
    .ifindex   = -1,
};

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};
struct stats_record {
    uint64_t timestamp;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
};
struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;

    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;

    uint32_t outstanding_tx;

    struct stats_record stats;
    struct stats_record prev_stats;
};

static inline __u32 xsk_ring_prod__free(struct xsk_ring_prod *r)
{
    r->cached_cons = *r->consumer + r->size;
    return r->cached_cons - r->cached_prod;
}

static const char *__doc__ = "AF_XDP kernel bypass example\n";

static const struct option_wrapper long_options[] = {

    {{"help",    no_argument,       NULL, 'h' },
     "Show help", false},

    {{"dev",     required_argument, NULL, 'd' },
     "Operate on device <ifname>", "<ifname>", true},

    {{"skb-mode",    no_argument,       NULL, 'S' },
     "Install XDP program in SKB (AKA generic) mode"},

    {{"native-mode", no_argument,       NULL, 'N' },
     "Install XDP program in native mode"},

    {{"auto-mode",   no_argument,       NULL, 'A' },
     "Auto-detect SKB or native mode"},

    {{"force",   no_argument,       NULL, 'F' },
     "Force install, replacing existing program on interface"},

    {{"copy",        no_argument,       NULL, 'c' },
     "Force copy mode"},

    {{"zero-copy",   no_argument,       NULL, 'z' },
     "Force zero-copy mode"},

    {{"queue",   required_argument, NULL, 'Q' },
     "Configure interface receive queue for AF_XDP, default=0"},

    {{"poll-mode",   no_argument,       NULL, 'p' },
     "Use the poll() API waiting for packets to arrive"},

    {{"quiet",   no_argument,       NULL, 'q' },
     "Quiet mode (no output)"},

    {{"filename",    required_argument, NULL,  1  },
     "Load program from <file>", "<file>"},

    {{"progname",    required_argument, NULL,  2  },
     "Load program from function <name> in the ELF file", "<name>"},

    {{0, 0, NULL,  0 }, NULL, false}
};

static bool global_exit;

static struct xsk_umem_info *configure_xsk_umem(void *buffer, uint64_t size)
{
    struct xsk_umem_info *umem;
    int ret;

    umem = calloc(1, sizeof(*umem));
    if (!umem)
        return NULL;

    ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
                   NULL);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    umem->buffer = buffer;
    return umem;
}

static uint64_t xsk_alloc_umem_frame(struct xsk_socket_info *xsk)
{
    uint64_t frame;
    if (xsk->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
    return frame;
}

static void xsk_free_umem_frame(struct xsk_socket_info *xsk, uint64_t frame)
{
    assert(xsk->umem_frame_free < NUM_FRAMES);

    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static uint64_t xsk_umem_free_frames(struct xsk_socket_info *xsk)
{
    return xsk->umem_frame_free;
}

static struct xsk_socket_info *xsk_configure_socket(struct config *cfg,
                            struct xsk_umem_info *umem)
{
    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    uint32_t idx;
    int i;
    int ret;
    uint32_t prog_id;

    xsk_info = calloc(1, sizeof(*xsk_info));
    if (!xsk_info)
        return NULL;

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.xdp_flags = cfg->xdp_flags;
    xsk_cfg.bind_flags = cfg->xsk_bind_flags;
    xsk_cfg.libbpf_flags = (custom_xsk) ? XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD: 0;
    ret = xsk_socket__create(&xsk_info->xsk, cfg->ifname,
                 cfg->xsk_if_queue, umem->umem, &xsk_info->rx,
                 &xsk_info->tx, &xsk_cfg);
    if (ret)
        goto error_exit;

    if (custom_xsk) {
        ret = xsk_socket__update_xskmap(xsk_info->xsk, xsk_map_fd);
        if (ret)
            goto error_exit;
    } else {
        /* Getting the program ID must be after the xdp_socket__create() call */
        if (bpf_xdp_query_id(cfg->ifindex, cfg->xdp_flags, &prog_id))
            goto error_exit;
    }

    /* Initialize umem frame allocation */
    for (i = 0; i < NUM_FRAMES; i++)
        xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

    xsk_info->umem_frame_free = NUM_FRAMES;

    /* Stuff the receive path with buffers, we assume we have enough */
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

    return xsk_info;

error_exit:
    errno = -ret;
    return NULL;
}

static void complete_tx(struct xsk_socket_info *xsk)
{
    unsigned int completed;
    uint32_t idx_cq;

    if (!xsk->outstanding_tx)
        return;

    sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

    /* Collect/free completed TX buffers */
    completed = xsk_ring_cons__peek(&xsk->umem->cq,
                    XSK_RING_CONS__DEFAULT_NUM_DESCS,
                    &idx_cq);

    if (completed > 0) {
        for (int i = 0; i < completed; i++)
            xsk_free_umem_frame(xsk,
                        *xsk_ring_cons__comp_addr(&xsk->umem->cq,
                                      idx_cq++));

        xsk_ring_cons__release(&xsk->umem->cq, completed);
        xsk->outstanding_tx -= completed < xsk->outstanding_tx ?
            completed : xsk->outstanding_tx;
    }
}

#endif
