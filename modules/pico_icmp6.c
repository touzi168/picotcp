/*********************************************************************
   PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
   See LICENSE and COPYING for usage.

   .

   Authors: Kristof Roelants, Daniele Lacamera
 *********************************************************************/

#include "pico_config.h"
#include "pico_icmp6.h"
#include "pico_ipv6_nd.h"
#include "pico_eth.h"
#include "pico_device.h"
#include "pico_stack.h"
#include "pico_tree.h"
#include "pico_socket.h"

#define PICO_ICMP6_OPT_SRC_LINK_LAYER_ADDR 1
#define PICO_ICMP6_OPT_TGT_LINK_LAYER_ADDR 2
#define icmp6_dbg(...) do {} while(0)
/* #define icmp6_dbg dbg */

static struct pico_queue icmp6_in;
static struct pico_queue icmp6_out;

uint16_t pico_icmp6_checksum(struct pico_frame *f)
{
    struct pico_ipv6_hdr *ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    struct pico_icmp6_hdr *icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    struct pico_ipv6_pseudo_hdr pseudo;

    pseudo.src = ipv6_hdr->src;
    pseudo.dst = ipv6_hdr->dst;
    pseudo.len = long_be(f->transport_len);
    pseudo.nxthdr = PICO_PROTO_ICMP6;

    pseudo.zero[0] = 0;
    pseudo.zero[1] = 0;
    pseudo.zero[2] = 0;

    return pico_dualbuffer_checksum(&pseudo, sizeof(struct pico_ipv6_pseudo_hdr), icmp6_hdr, f->transport_len);
}

#ifdef PICO_SUPPORT_PING
static void pico_icmp6_ping_recv_reply(struct pico_frame *f);
#endif

static int pico_icmp6_process_in(struct pico_protocol *self, struct pico_frame *f)
{
    struct pico_icmp6_hdr *hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    IGNORE_PARAMETER(self);

    icmp6_dbg("Process IN, type = %d\n", hdr->type);

    switch (hdr->type)
    {
    case PICO_ICMP6_DEST_UNREACH:
        pico_ipv6_unreachable(f, hdr->code);
        break;

    case PICO_ICMP6_ECHO_REQUEST:
        icmp6_dbg("ICMP6: Received ECHO REQ\n");
        hdr->type = PICO_ICMP6_ECHO_REPLY;
        /* XXX these pointers and len should already be set correctly in pico_ipv6_process_in */
        /* Ugly, but the best way to get ICMP data size here. */
        f->transport_len = (uint16_t)(f->buffer_len - PICO_SIZE_IP6HDR);
        if (f->dev->eth)
            f->transport_len = (uint16_t)(f->transport_len - PICO_SIZE_ETHHDR);

        hdr->crc = 0;
        hdr->crc = short_be(pico_icmp6_checksum(f));

        f->net_hdr = f->transport_hdr - PICO_SIZE_IP6HDR;
        f->start = f->net_hdr;
        f->len = f->buffer_len;
        if (f->dev->eth)
            f->len -= PICO_SIZE_ETHHDR;

        pico_ipv6_rebound(f);
        break;

    case PICO_ICMP6_ECHO_REPLY:
#ifdef PICO_SUPPORT_PING
        pico_icmp6_ping_recv_reply(f);
#endif
        pico_frame_discard(f);
        break;

    default:
        return pico_ipv6_nd_recv(f); /* CAUTION -- Implies: pico_frame_discard in any case, keep in the default! */
    }
    return -1;
}

static int pico_icmp6_process_out(struct pico_protocol *self, struct pico_frame *f)
{
    IGNORE_PARAMETER(self);
    IGNORE_PARAMETER(f);
    return 0;
}

/* Interface: protocol definition */
struct pico_protocol pico_proto_icmp6 = {
    .name = "icmp6",
    .proto_number = PICO_PROTO_ICMP6,
    .layer = PICO_LAYER_TRANSPORT,
    .process_in = pico_icmp6_process_in,
    .process_out = pico_icmp6_process_out,
    .q_in = &icmp6_in,
    .q_out = &icmp6_out,
};

static int pico_icmp6_notify(struct pico_frame *f, uint8_t type, uint8_t code)
{
    struct pico_frame *notice = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    uint16_t len = 0;

    if (!f)
        return -1;

    ipv6_hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    len = (uint16_t)(short_be(ipv6_hdr->len) + PICO_SIZE_IP6HDR);
    switch (type)
    {
    case PICO_ICMP6_DEST_UNREACH:
        /* as much of invoking packet as possible without exceeding the minimum IPv6 MTU */
        if (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_DEST_UNREACH_SIZE + len > PICO_IPV6_MIN_MTU)
            len = PICO_IPV6_MIN_MTU - (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_DEST_UNREACH_SIZE);

        notice = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_DEST_UNREACH_SIZE + len));
        if (!notice) {
            pico_err = PICO_ERR_ENOMEM;
            return -1;
        }

        notice->payload = notice->transport_hdr + PICO_ICMP6HDR_DEST_UNREACH_SIZE;
        notice->payload_len = len;
        icmp6_hdr = (struct pico_icmp6_hdr *)notice->transport_hdr;
        icmp6_hdr->msg.err.dest_unreach.unused = 0;
        break;

    case PICO_ICMP6_TIME_EXCEEDED:
        /* as much of invoking packet as possible without exceeding the minimum IPv6 MTU */
        if (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_TIME_XCEEDED_SIZE + len > PICO_IPV6_MIN_MTU)
            len = PICO_IPV6_MIN_MTU - (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_TIME_XCEEDED_SIZE);

        notice = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_TIME_XCEEDED_SIZE + len));
        if (!notice) {
            pico_err = PICO_ERR_ENOMEM;
            return -1;
        }

        notice->payload = notice->transport_hdr + PICO_ICMP6HDR_TIME_XCEEDED_SIZE;
        notice->payload_len = len;
        icmp6_hdr = (struct pico_icmp6_hdr *)notice->transport_hdr;
        icmp6_hdr->msg.err.time_exceeded.unused = 0;
        break;

    default:
        return -1;
    }

    icmp6_hdr->type = type;
    icmp6_hdr->code = code;
    memcpy(notice->payload, f->net_hdr, notice->payload_len);
    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(notice, &ipv6_hdr->src, PICO_PROTO_ICMP6);
    return 0;
}

int pico_icmp6_port_unreachable(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_PORT);
}

int pico_icmp6_proto_unreachable(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADDR);
}

int pico_icmp6_dest_unreachable(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADDR);
}

int pico_icmp6_ttl_expired(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_TIME_EXCEEDED, PICO_ICMP6_TIMXCEED_INTRANS);
}

#ifdef PICO_SUPPORT_IPFILTER
int pico_icmp6_packet_filtered(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADMIN);
}
#endif

/* RFC 4861 $7.2.2: sending neighbor solicitations */
int pico_icmp6_neighbor_solicitation(struct pico_device *dev, struct pico_ip6 *dst, uint8_t type)
{
    struct pico_frame *sol = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_icmp6_opt_lladdr *opt = NULL;
    struct pico_ip6 daddr = {{ 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00 }};
    uint8_t i = 0;
    uint16_t len = 0;

    if (pico_ipv6_is_multicast(dst->addr))
        return -1;

    len = PICO_ICMP6HDR_NEIGH_SOL_SIZE;
    if (type != PICO_ICMP6_ND_DAD)
        len = (uint16_t)(len + 8);

    sol = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);
    if (!sol) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    sol->payload = sol->transport_hdr + len;
    sol->payload_len = 0;

    icmp6_hdr = (struct pico_icmp6_hdr *)sol->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_NEIGH_SOL;
    icmp6_hdr->code = 0;
    icmp6_hdr->msg.info.neigh_sol.unused = 0;
    icmp6_hdr->msg.info.neigh_sol.target = *dst;

    if (type != PICO_ICMP6_ND_DAD) {
        opt = (struct pico_icmp6_opt_lladdr *)icmp6_hdr->msg.info.neigh_sol.options;
        opt->type = PICO_ICMP6_OPT_SRC_LINK_LAYER_ADDR;
        opt->len = 1;
        memcpy(opt->addr.mac.addr, dev->eth->mac.addr, PICO_SIZE_ETH);
    }

    if (type == PICO_ICMP6_ND_SOLICITED || type == PICO_ICMP6_ND_DAD) {
        for (i = 1; i <= 3; ++i) {
            daddr.addr[PICO_SIZE_IP6 - i] = dst->addr[PICO_SIZE_IP6 - i];
        }
    } else {
        daddr = *dst;
    }

    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(sol, &daddr, PICO_PROTO_ICMP6);
    return 0;
}

/* RFC 4861 $7.2.4: sending solicited neighbor advertisements */
int pico_icmp6_neighbor_advertisement(struct pico_frame *f, struct pico_ip6 *target)
{
    struct pico_frame *adv = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_icmp6_opt_lladdr *opt = NULL;
    struct pico_ip6 dst = {{0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    adv = pico_proto_ipv6.alloc(&pico_proto_ipv6, PICO_ICMP6HDR_NEIGH_ADV_SIZE + 8);
    if (!adv) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    adv->payload = adv->transport_hdr + PICO_ICMP6HDR_NEIGH_ADV_SIZE + 8;
    adv->payload_len = 0;

    icmp6_hdr = (struct pico_icmp6_hdr *)adv->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_NEIGH_ADV;
    icmp6_hdr->code = 0;
    icmp6_hdr->msg.info.neigh_adv.target = *target;
    icmp6_hdr->msg.info.neigh_adv.rsor = long_be(0x60000000); /* !router && solicited && override */
    if (pico_ipv6_is_unspecified(ipv6_hdr->src.addr)) {
        /* solicited = clear && dst = all-nodes address (scope link-local) */
        icmp6_hdr->msg.info.neigh_adv.rsor ^= long_be(0x40000000);
    } else {
        /* solicited = set && dst = source of solicitation */
        dst = ipv6_hdr->src;
    }

    /* XXX if the target address is either an anycast address or a unicast
     * address for which the node is providing proxy service, or the target
     * link-layer Address option is not included, the Override flag SHOULD
     * be set to zero.
     */

    /* XXX if the target address is an anycast address, the sender SHOULD delay
     * sending a response for a random time between 0 and MAX_ANYCAST_DELAY_TIME seconds.
     */

    opt = (struct pico_icmp6_opt_lladdr *)icmp6_hdr->msg.info.neigh_adv.options;
    opt->type = PICO_ICMP6_OPT_TGT_LINK_LAYER_ADDR;
    opt->len = 1;
    memcpy(opt->addr.mac.addr, f->dev->eth->mac.addr, PICO_SIZE_ETH);

    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(adv, &dst, PICO_PROTO_ICMP6);
    return 0;
}

/* RFC 4861 $6.3.7: sending router solicitations */
int pico_icmp6_router_solicitation(struct pico_device *dev, struct pico_ip6 *src)
{
    struct pico_frame *sol = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_icmp6_opt_lladdr *lladdr = NULL;
    uint16_t len = 0;
    struct pico_ip6 daddr = {{ 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }};

    len = PICO_ICMP6HDR_ROUTER_SOL_SIZE;
    if (!pico_ipv6_is_unspecified(src->addr))
        len = (uint16_t)(len + 8);

    sol = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);
    if (!sol) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    sol->payload = sol->transport_hdr + len;
    sol->payload_len = 0;

    icmp6_hdr = (struct pico_icmp6_hdr *)sol->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_ROUTER_SOL;
    icmp6_hdr->code = 0;

    if (!pico_ipv6_is_unspecified(src->addr)) {
        lladdr = (struct pico_icmp6_opt_lladdr *)icmp6_hdr->msg.info.router_sol.options;
        lladdr->type = PICO_ICMP6_OPT_SRC_LINK_LAYER_ADDR;
        lladdr->len = 1;
        memcpy(lladdr->addr.mac.addr, dev->eth->mac.addr, PICO_SIZE_ETH);
    }

    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(sol, &daddr, PICO_PROTO_ICMP6);
    return 0;
}

/***********************/
/* Ping implementation */
/***********************/

#ifdef PICO_SUPPORT_PING
struct pico_icmp6_ping_cookie
{
    uint16_t id;
    uint16_t seq;
    uint16_t size;
    uint16_t err;
    int count;
    int interval;
    int timeout;
    pico_time timestamp;
    struct pico_ip6 dst;
    void (*cb)(struct pico_icmp6_stats*);
};

static int icmp6_cookie_compare(void *ka, void *kb)
{
    struct pico_icmp6_ping_cookie *a = ka, *b = kb;
    if (a->id < b->id)
        return -1;

    if (a->id > b->id)
        return 1;

    return (a->seq - b->seq);
}
PICO_TREE_DECLARE(IPV6Pings, icmp6_cookie_compare);

static int pico_icmp6_send_echo(struct pico_icmp6_ping_cookie *cookie)
{
    struct pico_frame *echo = NULL;
    struct pico_icmp6_hdr *hdr = NULL;

    echo = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_ECHO_REQUEST_SIZE + cookie->size));
    if (!echo) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    echo->payload = echo->transport_hdr + PICO_ICMP6HDR_ECHO_REQUEST_SIZE;
    echo->payload_len = cookie->size;

    hdr = (struct pico_icmp6_hdr *)echo->transport_hdr;
    hdr->type = PICO_ICMP6_ECHO_REQUEST;
    hdr->code = 0;
    hdr->msg.info.echo_request.id = short_be(cookie->id);
    hdr->msg.info.echo_request.seq = short_be(cookie->seq);
    /* XXX: Fill payload */
    hdr->crc = 0;
    hdr->crc = short_be(pico_icmp6_checksum(echo));
    pico_ipv6_frame_push(echo, &cookie->dst, PICO_PROTO_ICMP6);
    return 0;
}

static void pico_icmp6_ping_timeout(pico_time now, void *arg)
{
    struct pico_icmp6_ping_cookie *cookie = NULL;

    IGNORE_PARAMETER(now);

    cookie = (struct pico_icmp6_ping_cookie *)arg;
    if (pico_tree_findKey(&IPV6Pings, cookie)) {
        if (cookie->err == PICO_PING6_ERR_PENDING) {
            struct pico_icmp6_stats stats = {
                0
            };
            stats.dst = cookie->dst;
            stats.seq = cookie->seq;
            stats.time = 0;
            stats.size = cookie->size;
            stats.err = PICO_PING6_ERR_TIMEOUT;
            dbg(" ---- Ping6 timeout!!!\n");
            if (cookie->cb)
                cookie->cb(&stats);
        }

        pico_tree_delete(&IPV6Pings, cookie);
        PICO_FREE(cookie);
    }
}

static void pico_icmp6_next_ping(pico_time now, void *arg);
static inline void pico_icmp6_send_ping(struct pico_icmp6_ping_cookie *cookie)
{
    pico_icmp6_send_echo(cookie);
    cookie->timestamp = pico_tick;
    pico_timer_add((pico_time)(cookie->interval), pico_icmp6_next_ping, cookie);
    pico_timer_add((pico_time)(cookie->timeout), pico_icmp6_ping_timeout, cookie);
}

static void pico_icmp6_next_ping(pico_time now, void *arg)
{
    struct pico_icmp6_ping_cookie *cookie = NULL, *new = NULL;

    IGNORE_PARAMETER(now);

    cookie = (struct pico_icmp6_ping_cookie *)arg;
    if (pico_tree_findKey(&IPV6Pings, cookie)) {
        if (cookie->seq < (uint16_t)cookie->count) {
            new = PICO_ZALLOC(sizeof(struct pico_icmp6_ping_cookie));
            if (!new) {
                pico_err = PICO_ERR_ENOMEM;
                return;
            }

            memcpy(new, cookie, sizeof(struct pico_icmp6_ping_cookie));
            new->seq++;

            pico_tree_insert(&IPV6Pings, new);
            pico_icmp6_send_ping(new);
        }
    }
}

static void pico_icmp6_ping_recv_reply(struct pico_frame *f)
{
    struct pico_icmp6_ping_cookie *cookie = NULL, test = {
        0
    };
    struct pico_icmp6_hdr *hdr = NULL;

    hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    test.id  = short_be(hdr->msg.info.echo_reply.id);
    test.seq = short_be(hdr->msg.info.echo_reply.seq);
    cookie = pico_tree_findKey(&IPV6Pings, &test);
    if (cookie) {
        struct pico_icmp6_stats stats = {
            0
        };
        cookie->err = PICO_PING6_ERR_REPLIED;
        stats.dst = cookie->dst;
        stats.seq = cookie->seq;
        stats.size = cookie->size;
        stats.time = pico_tick - cookie->timestamp;
        stats.err = cookie->err;
        stats.ttl = ((struct pico_ipv6_hdr *)f->net_hdr)->hop;
        if(cookie->cb)
            cookie->cb(&stats);
    } else {
        dbg("Reply for seq=%d, not found.\n", test.seq);
    }
}

int pico_icmp6_ping(char *dst, int count, int interval, int timeout, int size, void (*cb)(struct pico_icmp6_stats *))
{
    static uint16_t next_id = 0x91c0;
    struct pico_icmp6_ping_cookie *cookie = NULL;

    if(!dst || !count || !interval || !timeout) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    cookie = PICO_ZALLOC(sizeof(struct pico_icmp6_ping_cookie));
    if (!cookie) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    if (pico_string_to_ipv6(dst, cookie->dst.addr) < 0) {
        pico_err = PICO_ERR_EINVAL;
        PICO_FREE(cookie);
        return -1;
    }

    cookie->seq = 1;
    cookie->id = next_id++;
    cookie->err = PICO_PING6_ERR_PENDING;
    cookie->size = (uint16_t)size;
    cookie->interval = interval;
    cookie->timeout = timeout;
    cookie->cb = cb;
    cookie->count = count;

    pico_tree_insert(&IPV6Pings, cookie);
    pico_icmp6_send_ping(cookie);
    return 0;
}

#endif
