/* Sumi OS - networking: public interface of the IPv4 stack (kernel/net.c).
   All IPv4 addresses are 32-bit values in host order: 10.0.2.15 is 0x0A00020F.
   Every call blocks at most for the given timeout and returns >= 0 on success or one of
   the negative NET_E* codes; none of them panics when there is no adapter or no link. */
#pragma once
#include "kernel.h"

enum {
    NET_OK = 0,
    NET_ENODEV = -1,     /* no network adapter */
    NET_EDOWN = -2,      /* link is down */
    NET_ENOTCONF = -3,   /* no IP address yet (DHCP still running) */
    NET_ETIMEOUT = -4,
    NET_EUNREACH = -5,   /* an ICMP "destination unreachable" came back */
    NET_ENOROUTE = -6,   /* no gateway for an address outside the local network */
    NET_EREFUSED = -7,   /* TCP connection refused */
    NET_ERESET = -8,     /* TCP connection reset by the peer */
    NET_ENOTFOUND = -9,  /* the name does not exist */
    NET_EDNS = -10,      /* the DNS server did not give a usable answer */
    NET_EBUSY = -11,     /* out of sockets / queue full */
    NET_EINVAL = -12,    /* bad argument or stale handle */
    NET_ECLOSED = -13    /* the connection is closed */
};

typedef struct NetInfo {
    bool present, link, configured, dhcp;
    char adapter[48];
    u8 mac[6];
    u32 ip, mask, gw, dns;
    u32 lease_s;                 /* DHCP lease time in seconds (0 when static) */
    u32 rx_packets, rx_bytes, rx_dropped;
    u32 tx_packets, tx_bytes, tx_errors;
} NetInfo;

void net_init(void);             /* find the adapter and start the "net" thread; harmless without one */
bool net_up(void);               /* adapter present, link up and an address configured */
void net_info(NetInfo *out);
const char *net_strerror(int e);
char *net_ntoa(u32 ip, char *buf, size_t n);         /* "10.0.2.15" */
bool net_aton(const char *s, u32 *ip);               /* strict dotted quad */

/* ICMP echo: send one request and wait for the answer; *rtt_us gets the round trip in microseconds */
int net_ping(u32 ip, int seq, int timeout_ms, u32 *rtt_us);
/* DNS: A record for a host name (dotted quads are converted directly) */
int net_resolve(const char *name, u32 *ip);

/* UDP: local_port 0 picks a free one. Datagrams are at most 1472 bytes. */
int udp_open(u16 local_port);                        /* returns a handle */
int udp_sendto(int h, u32 ip, u16 port, const void *data, int n);
int udp_recvfrom(int h, void *buf, int n, u32 *ip, u16 *port, int timeout_ms);   /* returns the length */
int udp_close(int h);

/* TCP client. tcp_recv returns the number of bytes, 0 at end of stream, < 0 on error or timeout. */
int tcp_connect(u32 ip, u16 port, int timeout_ms);   /* returns a handle */
int tcp_send(int h, const void *data, int n, int timeout_ms);   /* returns the bytes queued */
int tcp_recv(int h, void *buf, int n, int timeout_ms);
int tcp_close(int h);
