/* Sumi OS - networking: Intel e1000 (82540EM) driver and a small IPv4 stack.
   Ethernet II, ARP, IPv4 (no fragments), ICMP echo, UDP, DHCP client, DNS resolver, TCP client.

   Design: the adapter runs with all its interrupts masked. One kernel thread ("net") polls the
   receive ring every 10 ms and runs the protocol timers; application threads that wait for an
   answer also poll the ring themselves, so replies are not delayed by the polling period.
   All state is static (no allocation after start-up) and protected by one mutex that is only
   held for short, non-blocking sections. A thread holding a mutex is never killed by the
   scheduler, and waiting happens outside the mutex, so an application can die at any wait
   without leaving the stack locked; sockets remember their owner thread and the net thread
   reclaims the sockets of threads that no longer exist.
   Every length and offset taken from a received packet is checked against the frame size. */
#include "net.h"

/* ---------- helpers ---------- */
static inline u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline void wr16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
#define SEQ_LT(a, b)  ((i32)((u32)(a) - (u32)(b)) < 0)
#define SEQ_LEQ(a, b) ((i32)((u32)(a) - (u32)(b)) <= 0)
#define SEQ_GT(a, b)  ((i32)((u32)(a) - (u32)(b)) > 0)
#define TICKS(ms) ((u32)(ms) / (1000 / HZ))
static inline bool tick_due(u32 deadline) { return (i32)(ticks - deadline) >= 0; }

/* Internet checksum (one's complement sum of big-endian 16-bit words) */
static u32 csum_add(u32 sum, const u8 *p, u32 n) {
    while (n > 1) { sum += ((u32)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (u32)p[0] << 8;
    return sum;
}
static u16 csum_fin(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}
static u16 l4_csum(u32 src, u32 dst, u32 proto, const u8 *p, u32 len) {   /* with the IPv4 pseudo header */
    u32 sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
    return csum_fin(csum_add(sum, p, len));
}

static Mutex net_lock = { .name = "net" };
static u32 rng_state = 0x6D2B79F5;
static u32 net_rand(void) {
    u32 x = rng_state ^ (ticks * 2654435761u);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    if (!x) x = 0x1234567;
    return rng_state = x;
}

/* fine-grained time for round-trip measurements: the time stamp counter, calibrated against the tick */
static bool have_tsc;
static u32 tsc_per_us;
static u64 rdtsc(void) {
    if (!have_tsc) return 0;
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
static u32 elapsed_us(u64 tsc0, u32 tick0) {
    u32 dt = ticks - tick0;
    if (tsc_per_us && dt < 30 * HZ) return (u32)((rdtsc() - tsc0) / tsc_per_us);
    return dt * (1000000 / HZ);
}

/* The thread that owns a socket. A thread slot can be reused, so the identity is tid + address + start time. */
typedef struct Owner { int tid; Thread *t; u32 start; } Owner;
static void owner_set(Owner *o) { o->tid = cur_thread->tid; o->t = cur_thread; o->start = cur_thread->start_tick; }
static bool owner_alive(const Owner *o) {
    bool ok = false;
    u32 f = irq_save();
    if (o->tid >= 0 && o->tid < MAX_THREADS) {
        Thread *t = threads[o->tid];
        ok = t && t == o->t && t->state != T_ZOMBIE && t->start_tick == o->start;
    }
    irq_restore(f);
    return ok;
}
static void owner_wake(const Owner *o) {
    u32 f = irq_save();
    if (o->tid >= 0 && o->tid < MAX_THREADS) {
        Thread *t = threads[o->tid];
        if (t && t == o->t && t->start_tick == o->start && t != cur_thread) thread_wake(t);
    }
    irq_restore(f);
}

/* ---------- configuration and counters ---------- */
static struct { u32 ip, mask, gw, dns; bool dhcp; u32 lease_s; } cfg;
static struct { u32 rx_packets, rx_bytes, rx_dropped, tx_packets, tx_bytes, tx_errors; } stats;

/* ---------- Intel 82540EM (e1000) ---------- */
#define E_CTRL   0x0000
#define E_STATUS 0x0008
#define E_EERD   0x0014
#define E_FCAL   0x0028
#define E_FCAH   0x002C
#define E_FCT    0x0030
#define E_ICR    0x00C0
#define E_IMC    0x00D8
#define E_RCTL   0x0100
#define E_FCTTV  0x0170
#define E_TCTL   0x0400
#define E_TIPG   0x0410
#define E_RDBAL  0x2800
#define E_RDBAH  0x2804
#define E_RDLEN  0x2808
#define E_RDH    0x2810
#define E_RDT    0x2818
#define E_TDBAL  0x3800
#define E_TDBAH  0x3804
#define E_TDLEN  0x3808
#define E_TDH    0x3810
#define E_TDT    0x3818
#define E_MTA    0x5200
#define E_RAL    0x5400
#define E_RAH    0x5404

#define RX_N   64            /* ring sizes: the byte length of a ring must be a multiple of 128 */
#define TX_N   16
#define BUF_SZ 2048
#define ETH_HDR 14
#define ETH_MAX 1514         /* frame without the CRC */
#define IP_MAXPAY 1480

typedef struct RxDesc { u32 addr_lo, addr_hi; u16 length, csum; u8 status, errors; u16 special; } __attribute__((packed)) RxDesc;
typedef struct TxDesc { u32 addr_lo, addr_hi; u16 length; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) TxDesc;

static struct {
    bool present, link;
    volatile u8 *mmio;
    int bus, dev, fn;
    u8 mac[6];
    volatile RxDesc *rx;
    volatile TxDesc *tx;
    u8 *rxbuf, *txbuf;
    u32 rx_cur, tx_cur;
    u8 tx_used[TX_N];
} nic;

static inline u32 er(u32 reg) { return *(volatile u32 *)(nic.mmio + reg); }
static inline void ew(u32 reg, u32 v) { *(volatile u32 *)(nic.mmio + reg) = v; }

static bool e1000_find(void) {
    int maxbus = 0;
    for (int bus = 0; bus <= maxbus; bus++) for (int dev = 0; dev < 32; dev++) {
        u32 id = pci_read(bus, dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        int nfn = (pci_read(bus, dev, 0, 0x0C) & 0x00800000) ? 8 : 1;
        for (int fn = 0; fn < nfn; fn++) {
            if (fn) id = pci_read(bus, dev, fn, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            if (id == 0x100E8086) { nic.bus = bus; nic.dev = dev; nic.fn = fn; return true; }
            if ((pci_read(bus, dev, fn, 8) >> 16) == 0x0604) {          /* PCI bridge: scan the buses behind it too */
                int sub = (pci_read(bus, dev, fn, 0x18) >> 16) & 0xFF;
                if (sub > maxbus) maxbus = sub;
            }
        }
    }
    return false;
}

static bool mac_valid(const u8 *m) {
    if (m[0] & 1) return false;                                          /* multicast / broadcast */
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

static bool e1000_init(void) {
    if (!e1000_find()) return false;
    int b = nic.bus, d = nic.dev, f = nic.fn;
    u32 bar = pci_read(b, d, f, 0x10);
    if ((bar & 1) || (((bar >> 1) & 3) == 2 && pci_read(b, d, f, 0x14) != 0) || (bar & ~0xFu) == 0) {
        klog("net: e1000 found but its register window is not usable (BAR0 %08x)\n", bar);
        return false;
    }
    /* memory space + bus master on, legacy interrupt line off (the driver polls) */
    pci_write(b, d, f, 4, (pci_read(b, d, f, 4) & 0xFFFF) | 0x0406);
    nic.mmio = (volatile u8 *)(bar & ~0xFu);

    u8 *raw[4];
    raw[0] = kmalloc_k(RX_N * sizeof(RxDesc) + 16); raw[1] = kmalloc_k(TX_N * sizeof(TxDesc) + 16);
    raw[2] = kmalloc_k(RX_N * BUF_SZ + 16);         raw[3] = kmalloc_k(TX_N * BUF_SZ + 16);
    if (!raw[0] || !raw[1] || !raw[2] || !raw[3]) {
        for (int i = 0; i < 4; i++) kfree(raw[i]);
        klog("net: not enough memory for the e1000 rings\n");
        return false;
    }
    nic.rx = (volatile RxDesc *)(((u32)raw[0] + 15) & ~15u);
    nic.tx = (volatile TxDesc *)(((u32)raw[1] + 15) & ~15u);
    nic.rxbuf = (u8 *)(((u32)raw[2] + 15) & ~15u);
    nic.txbuf = (u8 *)(((u32)raw[3] + 15) & ~15u);
    memset((void *)nic.rx, 0, RX_N * sizeof(RxDesc));
    memset((void *)nic.tx, 0, TX_N * sizeof(TxDesc));

    ew(E_IMC, 0xFFFFFFFF);
    ew(E_RCTL, 0); ew(E_TCTL, 0);
    ew(E_CTRL, er(E_CTRL) | (1u << 26));                                 /* device reset */
    for (int i = 0; i < 200000; i++) { io_wait(); if (i >= 64 && !(er(E_CTRL) & (1u << 26))) break; }
    ew(E_IMC, 0xFFFFFFFF);                                               /* no interrupts, ever */
    (void)er(E_ICR);
    u32 ctrl = er(E_CTRL);
    ctrl |= (1u << 6) | (1u << 5);                                       /* set link up, auto speed detection */
    ctrl &= ~((1u << 3) | (1u << 7) | (1u << 27) | (1u << 28) | (1u << 30) | (1u << 31));
    ew(E_CTRL, ctrl);
    ew(E_FCAL, 0); ew(E_FCAH, 0); ew(E_FCT, 0); ew(E_FCTTV, 0);

    u32 ral = er(E_RAL), rah = er(E_RAH);
    u8 mac[6] = { (u8)ral, (u8)(ral >> 8), (u8)(ral >> 16), (u8)(ral >> 24), (u8)rah, (u8)(rah >> 8) };
    if (!mac_valid(mac)) {                                               /* not loaded: read the EEPROM words 0..2 */
        for (int w = 0; w < 3; w++) {
            u32 v = 0;
            ew(E_EERD, ((u32)w << 8) | 1);
            for (int i = 0; i < 100000; i++) { v = er(E_EERD); if (v & 0x10) break; io_wait(); }
            mac[w * 2] = (u8)(v >> 16); mac[w * 2 + 1] = (u8)(v >> 24);
        }
    }
    if (!mac_valid(mac)) { static const u8 fb[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }; memcpy(mac, fb, 6); }
    memcpy(nic.mac, mac, 6);
    ew(E_RAL, (u32)mac[0] | ((u32)mac[1] << 8) | ((u32)mac[2] << 16) | ((u32)mac[3] << 24));
    ew(E_RAH, (u32)mac[4] | ((u32)mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++) ew(E_MTA + i * 4, 0);

    for (int i = 0; i < RX_N; i++) { nic.rx[i].addr_lo = (u32)(nic.rxbuf + i * BUF_SZ); nic.rx[i].addr_hi = 0; nic.rx[i].status = 0; }
    ew(E_RDBAL, (u32)nic.rx); ew(E_RDBAH, 0); ew(E_RDLEN, RX_N * sizeof(RxDesc));
    ew(E_RDH, 0); ew(E_RDT, RX_N - 1);
    nic.rx_cur = 0;
    ew(E_TDBAL, (u32)nic.tx); ew(E_TDBAH, 0); ew(E_TDLEN, TX_N * sizeof(TxDesc));
    ew(E_TDH, 0); ew(E_TDT, 0);
    nic.tx_cur = 0;
    ew(E_TIPG, 10 | (8 << 10) | (6 << 20));
    ew(E_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));    /* enable, pad short packets */
    ew(E_RCTL, (1u << 1) | (1u << 15) | (1u << 26));                     /* enable, broadcast, strip CRC, 2048-byte buffers */
    nic.link = (er(E_STATUS) & 2) != 0;
    nic.present = true;
    return true;
}

/* Queue one frame (ETH_HDR.. bytes, no CRC). Drops it when the ring is full or the link is down. */
static int nic_send(const u8 *frame, u32 len) {
    if (!nic.present) return NET_ENODEV;
    if (len < ETH_HDR || len > ETH_MAX) { stats.tx_errors++; return NET_EINVAL; }
    if (!nic.link) { stats.tx_errors++; return NET_EDOWN; }
    u32 i = nic.tx_cur, next = (i + 1) % TX_N;
    if (next == (er(E_TDH) % TX_N) || (nic.tx_used[i] && !(nic.tx[i].status & 1))) { stats.tx_errors++; return NET_EBUSY; }
    u8 *buf = nic.txbuf + i * BUF_SZ;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }
    nic.tx[i].addr_lo = (u32)buf; nic.tx[i].addr_hi = 0;
    nic.tx[i].length = (u16)len; nic.tx[i].cso = 0; nic.tx[i].css = 0; nic.tx[i].special = 0;
    nic.tx[i].status = 0;
    nic.tx[i].cmd = 0x0B;                                                /* end of packet, insert CRC, report status */
    nic.tx_used[i] = 1;
    nic.tx_cur = next;
    ew(E_TDT, next);
    stats.tx_packets++; stats.tx_bytes += len;
    return 0;
}

/* ---------- Ethernet / ARP ---------- */
#define ETH_IP  0x0800
#define ETH_ARP 0x0806
#define ARP_N 8
enum { ARP_FREE = 0, ARP_WAIT, ARP_OK };
typedef struct ArpEnt {
    u8 state, tries;
    u8 mac[6];
    u32 ip;
    u32 stamp;               /* tick of the last request (ARP_WAIT) or of the last confirmation (ARP_OK) */
    u32 pend_len;            /* one frame waiting for the address (the newest wins) */
    u8 pend[ETH_MAX + 2];
} ArpEnt;
static ArpEnt arp_tab[ARP_N];
static const u8 mac_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static u8 txf[ETH_MAX + 6];  /* frame under construction (always used with the lock held) */
#define TXP (txf + ETH_HDR + 20)   /* where the IP payload goes */
static u16 ip_ident;

static void arp_send(int op, const u8 *dst_mac, const u8 *target_mac, u32 target_ip) {
    u8 f[ETH_HDR + 28];
    memcpy(f, dst_mac, 6); memcpy(f + 6, nic.mac, 6); wr16(f + 12, ETH_ARP);
    u8 *a = f + ETH_HDR;
    wr16(a, 1); wr16(a + 2, ETH_IP); a[4] = 6; a[5] = 4; wr16(a + 6, op);
    memcpy(a + 8, nic.mac, 6); wr32(a + 14, cfg.ip);
    memcpy(a + 18, target_mac, 6); wr32(a + 24, target_ip);
    nic_send(f, sizeof f);
}
static ArpEnt *arp_find(u32 ip) {
    for (int i = 0; i < ARP_N; i++) if (arp_tab[i].state != ARP_FREE && arp_tab[i].ip == ip) return &arp_tab[i];
    return NULL;
}
static ArpEnt *arp_new(u32 ip) {
    ArpEnt *e = NULL;
    for (int i = 0; i < ARP_N && !e; i++) if (arp_tab[i].state == ARP_FREE) e = &arp_tab[i];
    if (!e) {                                                            /* table full: recycle the oldest entry */
        e = &arp_tab[0];
        for (int i = 1; i < ARP_N; i++) if ((i32)(arp_tab[i].stamp - e->stamp) < 0) e = &arp_tab[i];
    }
    e->state = ARP_WAIT; e->ip = ip; e->tries = 0; e->stamp = ticks; e->pend_len = 0;
    memset(e->mac, 0, 6);
    return e;
}
static void arp_learn(u32 ip, const u8 *mac, bool create) {
    if (!ip || ip == 0xFFFFFFFF || !mac_valid(mac)) return;
    ArpEnt *e = arp_find(ip);
    if (!e) { if (!create) return; e = arp_new(ip); }
    memcpy(e->mac, mac, 6);
    e->state = ARP_OK; e->stamp = ticks;
    if (e->pend_len >= ETH_HDR && e->pend_len <= ETH_MAX) {
        memcpy(e->pend, mac, 6);
        nic_send(e->pend, e->pend_len);
    }
    e->pend_len = 0;
}
static void arp_input(const u8 *a, u32 len) {
    if (len < 28) return;
    if (rd16(a) != 1 || rd16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4) return;
    u32 op = rd16(a + 6), spa = rd32(a + 14), tpa = rd32(a + 24);
    const u8 *sha = a + 8;
    if (!memcmp(sha, nic.mac, 6)) return;
    bool for_us = cfg.ip && tpa == cfg.ip;
    if (spa != cfg.ip) arp_learn(spa, sha, for_us);
    if (op == 1 && for_us && mac_valid(sha)) arp_send(2, sha, sha, spa);
}
static void arp_timers(void) {
    for (int i = 0; i < ARP_N; i++) {
        ArpEnt *e = &arp_tab[i];
        if (e->state == ARP_WAIT && tick_due(e->stamp + HZ)) {
            if (++e->tries >= 3) { e->state = ARP_FREE; e->pend_len = 0; continue; }
            e->stamp = ticks;
            arp_send(1, mac_bcast, (const u8 *)"\0\0\0\0\0\0", e->ip);
        } else if (e->state == ARP_OK && tick_due(e->stamp + 300 * HZ)) e->state = ARP_FREE;
    }
}

/* ---------- IPv4 ---------- */
#define P_ICMP 1
#define P_TCP  6
#define P_UDP  17

/* Send the IP payload that was written to TXP. A frame whose next hop is not in the ARP table yet
   is parked in the ARP entry and leaves when the answer arrives. */
static int ip_output(u32 src, u32 dst, u32 proto, u32 plen) {
    if (plen > IP_MAXPAY) return NET_EINVAL;
    if (!nic.present) return NET_ENODEV;
    if (!nic.link) return NET_EDOWN;
    u8 *ip = txf + ETH_HDR;
    ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, 20 + plen); wr16(ip + 4, ip_ident++); wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = (u8)proto; wr16(ip + 10, 0); wr32(ip + 12, src); wr32(ip + 16, dst);
    wr16(ip + 10, csum_fin(csum_add(0, ip, 20)));
    memcpy(txf + 6, nic.mac, 6); wr16(txf + 12, ETH_IP);
    u32 flen = ETH_HDR + 20 + plen;
    if (dst == 0xFFFFFFFF || (cfg.ip && cfg.mask != 0xFFFFFFFF && dst == (cfg.ip | ~cfg.mask))) {
        memcpy(txf, mac_bcast, 6);
        return nic_send(txf, flen);
    }
    if (!cfg.ip) return NET_ENOTCONF;
    if ((dst >> 28) == 0xE || (dst >> 24) == 127 || (dst >> 24) == 0 || dst == cfg.ip) return NET_ENOROUTE;
    u32 hop = ((dst ^ cfg.ip) & cfg.mask) == 0 ? dst : cfg.gw;
    if (!hop || hop == cfg.ip) return NET_ENOROUTE;
    ArpEnt *e = arp_find(hop);
    if (e && e->state == ARP_OK) {
        memcpy(txf, e->mac, 6);
        return nic_send(txf, flen);
    }
    if (!e) { e = arp_new(hop); arp_send(1, mac_bcast, (const u8 *)"\0\0\0\0\0\0", hop); }
    memcpy(e->pend, txf, flen); e->pend_len = flen;
    return 0;
}

/* ---------- ICMP ---------- */
#define PING_N 4
#define PING_DATA 56
typedef struct PingSlot { bool used, done; int result; Owner owner; u32 ip; u16 id, seq; u64 tsc0; u32 tick0; u32 rtt_us; } PingSlot;
static PingSlot pings[PING_N];

static void icmp_input(u32 src, u32 dst, const u8 *p, u32 len) {
    if (len < 8 || csum_fin(csum_add(0, p, len)) != 0) { stats.rx_dropped++; return; }
    u32 type = p[0];
    if (type == 8 && p[1] == 0) {                                        /* echo request: answer with the same data */
        if (!cfg.ip || dst != cfg.ip || len > IP_MAXPAY) return;
        memcpy(TXP, p, len);
        TXP[0] = 0; TXP[2] = 0; TXP[3] = 0;
        wr16(TXP + 2, csum_fin(csum_add(0, TXP, len)));
        ip_output(cfg.ip, src, P_ICMP, len);
    } else if (type == 0) {                                              /* echo reply */
        u32 id = rd16(p + 4), seq = rd16(p + 6);
        for (int i = 0; i < PING_N; i++) {
            PingSlot *s = &pings[i];
            if (!s->used || s->done || s->id != id || s->seq != seq || s->ip != src) continue;
            s->rtt_us = elapsed_us(s->tsc0, s->tick0);
            s->result = 0; s->done = true;
            owner_wake(&s->owner);
        }
    } else if (type == 3 || type == 11) {                                /* unreachable / time exceeded: quotes our packet */
        if (len < 8 + 20 + 8) return;
        const u8 *q = p + 8;
        u32 ihl = (q[0] & 15) * 4;
        if ((q[0] >> 4) != 4 || ihl < 20 || 8 + ihl + 8 > len || q[9] != P_ICMP) return;
        const u8 *qi = q + ihl;
        if (qi[0] != 8) return;
        u32 id = rd16(qi + 4), seq = rd16(qi + 6), qdst = rd32(q + 16);
        for (int i = 0; i < PING_N; i++) {
            PingSlot *s = &pings[i];
            if (!s->used || s->done || s->id != id || s->seq != seq || s->ip != qdst) continue;
            s->result = NET_EUNREACH; s->done = true;
            owner_wake(&s->owner);
        }
    }
}

/* ---------- UDP ---------- */
#define UDP_N 4
#define UDP_QLEN 4
#define UDP_MAXDATA 1472
typedef struct UdpSock {
    bool used; u32 gen; Owner owner; u16 port;
    int q_head, q_count;
    struct { u32 ip; u16 port, len; u8 data[UDP_MAXDATA]; } q[UDP_QLEN];
} UdpSock;
static UdpSock udps[UDP_N];
static u32 udp_gen;
static void dhcp_input(u32 src, const u8 *p, u32 len);

static int udp_output(u32 src, u32 sport, u32 dst, u32 dport, const u8 *data, u32 n) {
    if (n > UDP_MAXDATA) return NET_EINVAL;
    u8 *u = TXP;
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, 8 + n); wr16(u + 6, 0);
    if (n) memcpy(u + 8, data, n);
    u16 c = l4_csum(src, dst, P_UDP, u, 8 + n);
    wr16(u + 6, c ? c : 0xFFFF);
    return ip_output(src, dst, P_UDP, 8 + n);
}
static void udp_input(u32 src, u32 dst, const u8 *p, u32 len) {
    if (len < 8) { stats.rx_dropped++; return; }
    u32 ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) { stats.rx_dropped++; return; }
    if (rd16(p + 6) != 0 && l4_csum(src, dst, P_UDP, p, ulen) != 0) { stats.rx_dropped++; return; }
    u32 sport = rd16(p), dport = rd16(p + 2), n = ulen - 8;
    if (dport == 68) { dhcp_input(src, p + 8, n); return; }
    if (!cfg.ip || n > UDP_MAXDATA) return;
    for (int i = 0; i < UDP_N; i++) {
        UdpSock *s = &udps[i];
        if (!s->used || s->port != dport) continue;
        if (s->q_count >= UDP_QLEN) { stats.rx_dropped++; return; }     /* queue full: drop */
        int slot = (s->q_head + s->q_count) % UDP_QLEN;
        s->q[slot].ip = src; s->q[slot].port = (u16)sport; s->q[slot].len = (u16)n;
        memcpy(s->q[slot].data, p + 8, n);
        s->q_count++;
        owner_wake(&s->owner);
        return;
    }
}

/* ---------- DHCP client (runs inside the net thread) ---------- */
enum { DH_IDLE = 0, DH_DISCOVER, DH_REQUEST, DH_BOUND, DH_RENEW, DH_STATIC };
static struct {
    int state, tries;
    u32 xid, start, next, interval;
    u32 offer_ip, server;
    u32 bound_tick, lease_ticks;
} dh;
#define FALLBACK_IP   0x0A00020F     /* 10.0.2.15: what QEMU's user network hands out */
#define FALLBACK_MASK 0xFFFFFF00
#define FALLBACK_GW   0x0A000202
#define FALLBACK_DNS  0x0A000203

static void dhcp_send(int type) {
    static u8 m[300];
    memset(m, 0, sizeof m);
    bool renew = dh.state == DH_RENEW;
    m[0] = 1; m[1] = 1; m[2] = 6; wr32(m + 4, dh.xid);
    wr16(m + 8, (ticks - dh.start) / HZ);
    if (renew) wr32(m + 12, cfg.ip); else wr16(m + 10, 0x8000);         /* ask for a broadcast answer */
    memcpy(m + 28, nic.mac, 6);
    wr32(m + 236, 0x63825363);
    u8 *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, nic.mac, 6); o += 6;
    if (type == 3 && !renew) {
        *o++ = 50; *o++ = 4; wr32(o, dh.offer_ip); o += 4;
        *o++ = 54; *o++ = 4; wr32(o, dh.server); o += 4;
    }
    *o++ = 12; *o++ = 4; memcpy(o, "sumi", 4); o += 4;
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
    if (renew) udp_output(cfg.ip, 68, dh.server, 67, m, sizeof m);
    else udp_output(0, 68, 0xFFFFFFFF, 67, m, sizeof m);
}
static void dhcp_begin(void) {
    dh.state = DH_DISCOVER; dh.xid = net_rand(); dh.start = ticks; dh.tries = 0;
    dh.interval = HZ; dh.next = ticks + dh.interval;
    dhcp_send(1);
}
static void dhcp_static(void) {
    cfg.ip = FALLBACK_IP; cfg.mask = FALLBACK_MASK; cfg.gw = FALLBACK_GW; cfg.dns = FALLBACK_DNS;
    cfg.dhcp = false; cfg.lease_s = 0;
    dh.state = DH_STATIC; dh.next = ticks + 60 * HZ;
    klog("net: no DHCP answer, using static 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3\n");
}
static void dhcp_timers(void) {
    switch (dh.state) {
    case DH_DISCOVER:
        if (tick_due(dh.start + 8 * HZ)) dhcp_static();
        else if (tick_due(dh.next)) { dh.interval = dh.interval < 4 * HZ ? dh.interval * 2 : 4 * HZ; dh.next = ticks + dh.interval; dhcp_send(1); }
        break;
    case DH_REQUEST:
        if (!tick_due(dh.next)) break;
        if (++dh.tries > 3) {
            if (cfg.ip) { dh.state = DH_STATIC; dh.next = ticks + 60 * HZ; }
            else if (tick_due(dh.start + 8 * HZ)) dhcp_static();
            else { u32 st = dh.start; dhcp_begin(); dh.start = st; }
        } else { dh.next = ticks + HZ; dhcp_send(3); }
        break;
    case DH_STATIC:                                                      /* keep looking for a server quietly */
        if (tick_due(dh.next)) { dh.xid = net_rand(); dh.start = ticks; dh.next = ticks + 60 * HZ; dhcp_send(1); }
        break;
    case DH_BOUND:
        if (tick_due(dh.bound_tick + dh.lease_ticks / 2)) { dh.state = DH_RENEW; dh.xid = net_rand(); dh.start = ticks; dh.next = ticks; }
        break;
    case DH_RENEW:
        if (tick_due(dh.bound_tick + dh.lease_ticks)) {                  /* lease ran out: start over */
            klog("net: DHCP lease expired\n");
            cfg.ip = 0; cfg.dhcp = false;
            dhcp_begin();
        } else if (tick_due(dh.next)) { dh.next = ticks + 30 * HZ; dhcp_send(3); }
        break;
    }
}
static void dhcp_input(u32 src, const u8 *p, u32 len) {
    if (dh.state == DH_IDLE || dh.state == DH_BOUND) return;
    if (len < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6) return;
    if (rd32(p + 4) != dh.xid || memcmp(p + 28, nic.mac, 6) || rd32(p + 236) != 0x63825363) return;
    u32 yi = rd32(p + 16), type = 0, mask = 0, gw = 0, dns = 0, lease = 0, server = 0;
    for (u32 i = 240; i < len; ) {
        u32 tag = p[i++];
        if (tag == 0) continue;
        if (tag == 255 || i >= len) break;
        u32 ol = p[i++];
        if (i + ol > len) break;
        const u8 *v = p + i;
        if (tag == 53 && ol >= 1) type = v[0];
        else if (tag == 1 && ol >= 4) mask = rd32(v);
        else if (tag == 3 && ol >= 4) gw = rd32(v);
        else if (tag == 6 && ol >= 4) dns = rd32(v);
        else if (tag == 51 && ol >= 4) lease = rd32(v);
        else if (tag == 54 && ol >= 4) server = rd32(v);
        i += ol;
    }
    if (!server) server = src;
    if (type == 2 && (dh.state == DH_DISCOVER || dh.state == DH_STATIC)) {           /* OFFER */
        if (!yi || (yi >> 24) == 0 || (yi >> 24) == 127 || (yi >> 28) >= 0xE) return;
        dh.offer_ip = yi; dh.server = server;
        dh.state = DH_REQUEST; dh.tries = 0; dh.next = ticks + HZ;
        dhcp_send(3);
    } else if (type == 5 && (dh.state == DH_REQUEST || dh.state == DH_RENEW)) {      /* ACK */
        if (!yi || (yi >> 24) == 0 || (yi >> 24) == 127 || (yi >> 28) >= 0xE) return;
        u32 inv = ~mask;
        if (!mask || (inv & (inv + 1)) != 0) mask = 0xFFFFFF00;         /* missing or not contiguous */
        if (gw && (((gw ^ yi) & mask) != 0 || gw == yi)) gw = 0;
        if (!lease) lease = 3600;
        if (lease > 30 * 86400u) lease = 30 * 86400u;
        if (lease < 60) lease = 60;
        bool changed = cfg.ip != yi || !cfg.dhcp;
        cfg.ip = yi; cfg.mask = mask; cfg.gw = gw; cfg.dns = dns; cfg.dhcp = true; cfg.lease_s = lease;
        dh.server = server; dh.state = DH_BOUND; dh.bound_tick = ticks; dh.lease_ticks = lease * HZ;
        if (changed) {
            char a[16], b[16], c[16];
            klog("net: DHCP %s gw %s dns %s (lease %u s)\n", net_ntoa(yi, a, sizeof a), net_ntoa(gw, b, sizeof b), net_ntoa(dns, c, sizeof c), lease);
        }
    } else if (type == 6 && (dh.state == DH_REQUEST || dh.state == DH_RENEW)) {      /* NAK */
        if (dh.state == DH_RENEW || cfg.dhcp) { cfg.ip = 0; cfg.dhcp = false; }
        if (cfg.ip) { dh.state = DH_STATIC; dh.next = ticks + 60 * HZ; } else dhcp_begin();
    }
}

/* ---------- TCP (client side only) ---------- */
#define TCP_N 8
#define TCP_RXBUF 32768
#define TCP_TXBUF 8192
#define TCP_MSS 1460
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TCP_RTO (1 * HZ)
#define TCP_RTO_MAX (8 * HZ)
enum { TS_FREE = 0, TS_SYN_SENT, TS_OPEN, TS_DEAD };
typedef struct TcpSock {
    int state, err;
    bool app_open;               /* the application still holds the handle */
    bool fin_rcvd, fin_queued, fin_sent, fin_acked, timer_on, probe;
    u32 gen; Owner owner;
    u32 rip; u16 rport, lport;
    u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, mss;
    u32 rcv_nxt, rcv_adv;        /* rcv_adv: the window we last told the peer */
    u32 rto, deadline, retries, close_tick;
    u32 rx_head, rx_len, tx_len;
    u8 rx[TCP_RXBUF];            /* ring: received, not yet read by the application */
    u8 tx[TCP_TXBUF];            /* linear: tx[0] is sequence number snd_una */
} TcpSock;
static TcpSock tcps[TCP_N];
static u32 tcp_gen;

static int tcp_raw(u32 dst, u32 lport, u32 rport, u32 seq, u32 ack, u32 flags, u32 wnd, const u8 *data, u32 n, bool mss_opt) {
    u8 *t = TXP;
    u32 hl = mss_opt ? 24 : 20;
    if (n > IP_MAXPAY - hl) return NET_EINVAL;
    wr16(t, lport); wr16(t + 2, rport); wr32(t + 4, seq); wr32(t + 8, ack);
    t[12] = (u8)((hl / 4) << 4); t[13] = (u8)flags; wr16(t + 14, wnd > 65535 ? 65535 : wnd); wr16(t + 16, 0); wr16(t + 18, 0);
    if (mss_opt) { t[20] = 2; t[21] = 4; wr16(t + 22, TCP_MSS); }
    if (n) memcpy(t + hl, data, n);
    wr16(t + 16, l4_csum(cfg.ip, dst, P_TCP, t, hl + n));
    return ip_output(cfg.ip, dst, P_TCP, hl + n);
}
static int tcp_xmit(TcpSock *s, u32 seq, u32 flags, const u8 *data, u32 n) {
    u32 wnd = TCP_RXBUF - s->rx_len;
    if (flags & TH_ACK) s->rcv_adv = wnd;
    return tcp_raw(s->rip, s->lport, s->rport, seq, (flags & TH_ACK) ? s->rcv_nxt : 0, flags, wnd, data, n, (flags & TH_SYN) != 0);
}
static void tcp_free(TcpSock *s) { s->state = TS_FREE; s->app_open = false; s->timer_on = false; }
/* The connection is over (err < 0: why). The slot stays until the application lets go of the handle. */
static void tcp_kill(TcpSock *s, int err, bool send_rst) {
    if (send_rst && s->state == TS_OPEN) tcp_xmit(s, s->snd_nxt, TH_RST | TH_ACK, NULL, 0);
    s->err = err; s->state = TS_DEAD; s->timer_on = false;
    if (s->app_open) owner_wake(&s->owner); else tcp_free(s);
}
/* Send whatever the peer's window allows; with ack_now at least one segment carrying an ACK leaves. */
static void tcp_output(TcpSock *s, bool ack_now) {
    if (s->state != TS_OPEN) return;
    bool sent = false;
    for (int burst = 0; burst < 8; burst++) {
        u32 off = s->snd_nxt - s->snd_una;
        if (off > s->tx_len) break;                                      /* the FIN is already out */
        u32 avail = s->tx_len - off;
        u32 wnd = s->snd_wnd ? s->snd_wnd : (s->probe ? 1 : 0);
        u32 room = wnd > off ? wnd - off : 0;
        u32 n = avail < room ? avail : room;
        if (n > s->mss) n = s->mss;
        bool fin = s->fin_queued && !s->fin_sent && n == avail;
        if (n == 0 && !fin) break;
        tcp_xmit(s, s->snd_nxt, TH_ACK | (n && n == avail ? TH_PSH : 0) | (fin ? TH_FIN : 0), s->tx + off, n);
        s->snd_nxt += n + (fin ? 1 : 0);
        if (fin) s->fin_sent = true;
        if (SEQ_GT(s->snd_nxt, s->snd_max)) s->snd_max = s->snd_nxt;
        if (!s->timer_on) { s->timer_on = true; s->deadline = ticks + s->rto; }
        sent = true;
    }
    if (!sent && ack_now) tcp_xmit(s, s->snd_nxt, TH_ACK, NULL, 0);
}
static void tcp_reply_rst(u32 src, u32 sport, u32 dport, u32 seq, u32 ack, u32 flags, u32 seglen) {
    if (flags & TH_RST) return;
    if (flags & TH_ACK) tcp_raw(src, dport, sport, ack, 0, TH_RST, 0, NULL, 0, false);
    else tcp_raw(src, dport, sport, 0, seq + seglen, TH_RST | TH_ACK, 0, NULL, 0, false);
}
static void tcp_input(u32 src, u32 dst, const u8 *seg, u32 len) {
    if (len < 20) { stats.rx_dropped++; return; }
    u32 doff = (seg[12] >> 4) * 4;
    if (doff < 20 || doff > len || l4_csum(src, dst, P_TCP, seg, len) != 0) { stats.rx_dropped++; return; }
    if (!cfg.ip || dst != cfg.ip) return;
    u32 sport = rd16(seg), dport = rd16(seg + 2), seq = rd32(seg + 4), ack = rd32(seg + 8), flags = seg[13], wnd = rd16(seg + 14);
    const u8 *data = seg + doff;
    u32 dlen = len - doff;
    TcpSock *s = NULL;
    for (int i = 0; i < TCP_N; i++) {
        TcpSock *c = &tcps[i];
        if ((c->state == TS_SYN_SENT || c->state == TS_OPEN) && c->lport == dport && c->rport == sport && c->rip == src) { s = c; break; }
    }
    if (!s) { tcp_reply_rst(src, sport, dport, seq, ack, flags, dlen + ((flags & TH_SYN) ? 1 : 0) + ((flags & TH_FIN) ? 1 : 0)); return; }

    if (s->state == TS_SYN_SENT) {
        if ((flags & TH_ACK) && ack != s->iss + 1) { tcp_reply_rst(src, sport, dport, seq, ack, flags, 0); return; }
        if (flags & TH_RST) { if (flags & TH_ACK) tcp_kill(s, NET_EREFUSED, false); return; }
        if ((flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) return;
        u32 mss = 536;
        for (u32 i = 20; i < doff; ) {                                   /* options: only the MSS matters */
            u32 kind = seg[i];
            if (kind == 0) break;
            if (kind == 1) { i++; continue; }
            if (i + 1 >= doff) break;
            u32 ol = seg[i + 1];
            if (ol < 2 || i + ol > doff) break;
            if (kind == 2 && ol == 4) mss = rd16(seg + i + 2);
            i += ol;
        }
        s->mss = mss < 64 ? 64 : mss > TCP_MSS ? TCP_MSS : mss;
        s->rcv_nxt = seq + 1;
        s->snd_una = s->snd_nxt = s->snd_max = ack;
        s->snd_wnd = wnd;
        s->state = TS_OPEN; s->timer_on = false; s->retries = 0; s->rto = TCP_RTO;
        tcp_xmit(s, s->snd_nxt, TH_ACK, NULL, 0);
        owner_wake(&s->owner);
        return;
    }

    /* established */
    if (flags & TH_RST) {
        u32 win = TCP_RXBUF - s->rx_len;
        if (seq == s->rcv_nxt || (SEQ_GT(seq, s->rcv_nxt) && SEQ_LT(seq, s->rcv_nxt + win))) tcp_kill(s, NET_ERESET, false);
        return;
    }
    if (flags & TH_SYN) { tcp_xmit(s, s->snd_nxt, TH_ACK, NULL, 0); return; }   /* e.g. a repeated SYN-ACK */
    if (!(flags & TH_ACK)) return;
    if (SEQ_GT(ack, s->snd_max)) { tcp_xmit(s, s->snd_nxt, TH_ACK, NULL, 0); return; }
    if (SEQ_GT(ack, s->snd_una)) {
        u32 acked = ack - s->snd_una;
        if (acked > s->tx_len) {                                         /* covers our FIN as well */
            if (s->fin_queued) s->fin_sent = s->fin_acked = true;
            acked = s->tx_len;
        }
        if (acked < s->tx_len) memmove(s->tx, s->tx + acked, s->tx_len - acked);
        s->tx_len -= acked;
        s->snd_una = ack;
        if (SEQ_LT(s->snd_nxt, s->snd_una)) s->snd_nxt = s->snd_una;
        s->retries = 0; s->rto = TCP_RTO;
        if (s->snd_una == s->snd_max) s->timer_on = false; else s->deadline = ticks + s->rto;
        owner_wake(&s->owner);
    }
    if (SEQ_LEQ(s->snd_una, ack)) { s->snd_wnd = wnd; if (!wnd) s->retries = 0; }

    bool ack_now = false;
    if (dlen > 0 || (flags & TH_FIN)) {
        ack_now = true;
        u32 off = s->rcv_nxt - seq;                                      /* bytes of this segment we already have */
        if (!SEQ_GT(seq, s->rcv_nxt) && off <= dlen && !s->fin_rcvd) {   /* in order (possibly overlapping) */
            const u8 *d = data + off;
            u32 n = dlen - off;
            u32 take = n;
            if (s->app_open) {
                u32 space = TCP_RXBUF - s->rx_len;
                if (take > space) take = space;
                u32 wpos = (s->rx_head + s->rx_len) % TCP_RXBUF;
                u32 first = TCP_RXBUF - wpos;
                if (first > take) first = take;
                memcpy(s->rx + wpos, d, first);
                if (take > first) memcpy(s->rx, d + first, take - first);
                s->rx_len += take;
            }                                                            /* after close: acknowledge and discard */
            s->rcv_nxt += take;
            if (take == n && (flags & TH_FIN)) { s->rcv_nxt++; s->fin_rcvd = true; }
            if (take || s->fin_rcvd) owner_wake(&s->owner);
        }
    }
    tcp_output(s, ack_now);
    if (!s->app_open && s->fin_rcvd && s->fin_acked) tcp_free(s);
}
static void tcp_timers(bool check_owners) {
    for (int i = 0; i < TCP_N; i++) {
        TcpSock *s = &tcps[i];
        if (s->state == TS_FREE) continue;
        if (s->app_open && check_owners && !owner_alive(&s->owner)) {    /* the application died: drop its connection */
            s->app_open = false;
            tcp_kill(s, NET_ECLOSED, true);
            continue;
        }
        if (s->state == TS_DEAD) { if (!s->app_open) tcp_free(s); continue; }
        if (!s->app_open) {
            if (s->fin_rcvd && s->fin_acked) { tcp_free(s); continue; }
            if (tick_due(s->close_tick + 4 * HZ)) { tcp_kill(s, NET_ECLOSED, true); continue; }
        }
        if (s->state == TS_SYN_SENT) {
            if (!tick_due(s->deadline)) continue;
            if (++s->retries > 5) { tcp_kill(s, NET_ETIMEOUT, false); continue; }
            s->rto = s->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2;
            s->deadline = ticks + s->rto;
            tcp_xmit(s, s->iss, TH_SYN, NULL, 0);
            continue;
        }
        if (s->tx_len == 0 && !(s->fin_queued && !s->fin_acked)) { s->timer_on = false; continue; }
        if (!s->timer_on) { s->timer_on = true; s->deadline = ticks + s->rto; continue; }
        if (!tick_due(s->deadline)) continue;
        if (++s->retries > 6) { tcp_kill(s, NET_ETIMEOUT, true); continue; }
        s->snd_nxt = s->snd_una;                                         /* go back and send again from the first unacknowledged byte */
        s->fin_sent = s->fin_acked;
        s->rto = s->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2;
        s->deadline = ticks + s->rto;
        s->probe = true;
        tcp_output(s, false);
        s->probe = false;
    }
}

/* ---------- receive path ---------- */
static void ip_input(const u8 *ip, u32 len) {
    if (len < 20 || (ip[0] >> 4) != 4) { stats.rx_dropped++; return; }
    u32 ihl = (ip[0] & 15) * 4, total = rd16(ip + 2);
    if (ihl < 20 || ihl > len || total < ihl || total > len) { stats.rx_dropped++; return; }
    if (csum_fin(csum_add(0, ip, ihl)) != 0) { stats.rx_dropped++; return; }
    if (rd16(ip + 6) & 0x3FFF) { stats.rx_dropped++; return; }          /* fragments are not supported */
    u32 src = rd32(ip + 12), dst = rd32(ip + 16), proto = ip[9];
    if (src == 0xFFFFFFFF || (src >> 28) == 0xE || (cfg.ip && (src == cfg.ip || src == (cfg.ip | ~cfg.mask)))) return;
    bool bcast = dst == 0xFFFFFFFF || (cfg.ip && dst == (cfg.ip | ~cfg.mask));
    if (cfg.ip) { if (dst != cfg.ip && !bcast) return; }
    else if (proto != P_UDP) return;                                     /* no address yet: only DHCP is of interest */
    const u8 *p = ip + ihl;
    u32 plen = total - ihl;
    if (proto == P_ICMP) icmp_input(src, dst, p, plen);
    else if (proto == P_UDP) udp_input(src, dst, p, plen);
    else if (proto == P_TCP) tcp_input(src, dst, p, plen);
}
static void eth_input(const u8 *f, u32 len) {
    if (len < ETH_HDR) { stats.rx_dropped++; return; }
    u32 type = rd16(f + 12);
    if (type == ETH_ARP) arp_input(f + ETH_HDR, len - ETH_HDR);
    else if (type == ETH_IP) ip_input(f + ETH_HDR, len - ETH_HDR);
}
/* Take received frames out of the ring (at most one ring's worth per call). Lock held. */
static void nic_poll(void) {
    if (!nic.present) return;
    for (int budget = 0; budget < RX_N; budget++) {
        u32 i = nic.rx_cur;
        u32 st = nic.rx[i].status;
        if (!(st & 1)) break;
        u32 len = nic.rx[i].length, err = nic.rx[i].errors;
        if ((st & 2) && !err && len >= ETH_HDR && len <= BUF_SZ) {
            stats.rx_packets++; stats.rx_bytes += len;
            eth_input(nic.rxbuf + i * BUF_SZ, len);
        } else stats.rx_dropped++;
        nic.rx[i].status = 0;
        ew(E_RDT, i);
        nic.rx_cur = (i + 1) % RX_N;
    }
}

/* ---------- the net thread ---------- */
static void net_thread(void *arg) {
    (void)arg;
    if (have_tsc) {                                                      /* calibrate the time stamp counter */
        sleep_ms(10);
        u32 t0 = ticks; u64 c0 = rdtsc();
        sleep_ms(50);
        u32 dt = ticks - t0; u64 dc = rdtsc() - c0;
        if (dt) tsc_per_us = (u32)(dc / ((u64)dt * (1000000 / HZ)));
    }
    u32 last_tick = ticks, slow = 0;
    bool first = true;
    for (;;) {
        mutex_lock(&net_lock);
        bool link = (er(E_STATUS) & 2) != 0;
        if (link != nic.link || first) {
            if (!first) klog("net: link %s\n", link ? "up" : "down");
            nic.link = link;
            if (link && !cfg.ip) dhcp_begin();
            first = false;
        }
        nic_poll();
        if (ticks != last_tick) {
            last_tick = ticks;
            bool owners = false;
            if (++slow >= 25) { slow = 0; owners = true; }               /* about four times a second */
            tcp_timers(owners);
            if (owners) {
                arp_timers();
                if (nic.link) dhcp_timers();
                for (int i = 0; i < UDP_N; i++) if (udps[i].used && !owner_alive(&udps[i].owner)) udps[i].used = false;
                for (int i = 0; i < PING_N; i++) if (pings[i].used && !owner_alive(&pings[i].owner)) pings[i].used = false;
            }
        }
        mutex_unlock(&net_lock);
        thread_wait(10);
    }
}

void net_init(void) {
    if (nic.present) return;
    if (!e1000_init()) { klog("net: no network adapter (Intel e1000 82540EM not found)\n"); return; }
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    (void)a; (void)b; (void)c;
    have_tsc = (d & (1 << 4)) != 0;
    rng_state ^= (u32)rdtsc() ^ rd32(nic.mac + 2) ^ (time_now() * 2246822519u);
    ip_ident = (u16)net_rand();
    klog("net: e1000 %02x:%02x:%02x:%02x:%02x:%02x at PCI %d:%d.%d, link %s\n", nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3],
         nic.mac[4], nic.mac[5], nic.bus, nic.dev, nic.fn, nic.link ? "up" : "down");
    if (!thread_create("net", net_thread, NULL, TF_KERNEL, 1)) {
        klog("net: cannot start the net thread - networking disabled\n");
        ew(E_RCTL, 0); ew(E_TCTL, 0);
        nic.present = false;
    }
}

/* ---------- public interface (called from application threads) ---------- */
static int ready_locked(void) {
    if (!nic.present) return NET_ENODEV;
    if (!nic.link) return NET_EDOWN;
    if (!cfg.ip) return NET_ENOTCONF;
    return 0;
}
/* Sleep for a moment while waiting for the network; returns false once the time is up. */
static bool wait_more(u32 start, int timeout_ms, int fast_polls) {
    u32 limit = timeout_ms <= 0 ? 0 : TICKS(timeout_ms + 9);
    if (ticks - start >= limit) return false;
    if (fast_polls > 0) thread_yield(); else thread_wait(10);
    return true;
}

bool net_up(void) { return nic.present && nic.link && cfg.ip != 0; }

void net_info(NetInfo *out) {
    memset(out, 0, sizeof *out);
    if (!nic.present) return;
    mutex_lock(&net_lock);
    out->present = true; out->link = nic.link; out->configured = cfg.ip != 0; out->dhcp = cfg.dhcp;
    snprintf(out->adapter, sizeof out->adapter, "Intel 82540EM (e1000) at PCI %d:%d.%d", nic.bus, nic.dev, nic.fn);
    memcpy(out->mac, nic.mac, 6);
    out->ip = cfg.ip; out->mask = cfg.mask; out->gw = cfg.gw; out->dns = cfg.dns; out->lease_s = cfg.lease_s;
    out->rx_packets = stats.rx_packets; out->rx_bytes = stats.rx_bytes; out->rx_dropped = stats.rx_dropped;
    out->tx_packets = stats.tx_packets; out->tx_bytes = stats.tx_bytes; out->tx_errors = stats.tx_errors;
    mutex_unlock(&net_lock);
}

const char *net_strerror(int e) {
    switch (e) {
    case NET_OK: return "ok";
    case NET_ENODEV: return "no network adapter";
    case NET_EDOWN: return "network link is down";
    case NET_ENOTCONF: return "network is not configured yet";
    case NET_ETIMEOUT: return "timed out";
    case NET_EUNREACH: return "destination unreachable";
    case NET_ENOROUTE: return "no route to host";
    case NET_EREFUSED: return "connection refused";
    case NET_ERESET: return "connection reset by peer";
    case NET_ENOTFOUND: return "host not found";
    case NET_EDNS: return "name server failure";
    case NET_EBUSY: return "out of network resources";
    case NET_EINVAL: return "invalid argument";
    case NET_ECLOSED: return "connection closed";
    }
    return "network error";
}

char *net_ntoa(u32 ip, char *buf, size_t n) {
    snprintf(buf, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}
bool net_aton(const char *s, u32 *ip) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit(*s)) return false;
        u32 o = 0; int nd = 0;
        while (isdigit(*s)) { o = o * 10 + (u32)(*s++ - '0'); if (++nd > 3 || o > 255) return false; }
        v = (v << 8) | o;
        if (part < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *ip = v;
    return true;
}

int net_ping(u32 ip, int seq, int timeout_ms, u32 *rtt_us) {
    if (!nic.present) return NET_ENODEV;
    mutex_lock(&net_lock);
    int rc = ready_locked();
    PingSlot *s = NULL;
    if (!rc) {
        for (int i = 0; i < PING_N && !s; i++) if (!pings[i].used) s = &pings[i];
        if (!s) rc = NET_EBUSY;
    }
    if (!rc) {
        memset(s, 0, sizeof *s);
        s->ip = ip; s->id = (u16)net_rand(); s->seq = (u16)seq;
        owner_set(&s->owner);
        u8 *p = TXP;
        p[0] = 8; p[1] = 0; wr16(p + 2, 0); wr16(p + 4, s->id); wr16(p + 6, s->seq);
        for (int i = 0; i < PING_DATA; i++) p[8 + i] = (u8)(0x20 + i);
        wr16(p + 2, csum_fin(csum_add(0, p, 8 + PING_DATA)));
        s->tsc0 = rdtsc(); s->tick0 = ticks;
        s->used = true;
        rc = ip_output(cfg.ip, ip, P_ICMP, 8 + PING_DATA);
        if (rc) s->used = false;
    }
    mutex_unlock(&net_lock);
    if (rc) return rc;
    u32 start = ticks;
    for (int polls = 300; ; polls--) {                                   /* poll briskly at first: local answers come within microseconds */
        mutex_lock(&net_lock);
        nic_poll();
        if (!s->used) rc = NET_ECLOSED;                                  /* cannot happen while we are alive */
        else if (s->done) { rc = s->result; if (rtt_us) *rtt_us = s->rtt_us; }
        else rc = 1;
        if (rc <= 0) s->used = false;
        mutex_unlock(&net_lock);
        if (rc <= 0) return rc;
        if (!wait_more(start, timeout_ms, ticks - start < 3 ? polls : 0)) break;
    }
    mutex_lock(&net_lock);
    rc = NET_ETIMEOUT;
    if (s->used && s->done) { rc = s->result; if (rtt_us) *rtt_us = s->rtt_us; }
    s->used = false;
    mutex_unlock(&net_lock);
    return rc;
}

/* --- UDP sockets --- */
#define H_MAKE(i, gen) ((int)(((gen) << 4) | (u32)(i)))
static UdpSock *udp_get(int h) {
    if (h <= 0 || (h & 15) >= UDP_N) return NULL;
    UdpSock *s = &udps[h & 15];
    return (s->used && s->gen == ((u32)h >> 4)) ? s : NULL;
}
static bool port_in_use(u32 port) {
    for (int i = 0; i < UDP_N; i++) if (udps[i].used && udps[i].port == port) return true;
    for (int i = 0; i < TCP_N; i++) if (tcps[i].state != TS_FREE && tcps[i].lport == port) return true;
    return port == 68;
}
static u16 pick_port(void) {
    for (int tries = 0; tries < 64; tries++) {
        u32 p = 49152 + net_rand() % 16000;
        if (!port_in_use(p)) return (u16)p;
    }
    return 0;
}
int udp_open(u16 local_port) {
    if (!nic.present) return NET_ENODEV;
    int h = NET_EBUSY;
    mutex_lock(&net_lock);
    u16 port = local_port ? local_port : pick_port();
    if (!port || port_in_use(port)) h = local_port ? NET_EINVAL : NET_EBUSY;
    else for (int i = 0; i < UDP_N; i++) {
        UdpSock *s = &udps[i];
        if (s->used) continue;
        s->gen = (++udp_gen & 0x3FFFFFF) ? (udp_gen & 0x3FFFFFF) : (udp_gen = 1);
        s->port = port; s->q_head = s->q_count = 0;
        owner_set(&s->owner);
        s->used = true;
        h = H_MAKE(i, s->gen);
        break;
    }
    mutex_unlock(&net_lock);
    return h;
}
int udp_sendto(int h, u32 ip, u16 port, const void *data, int n) {
    if (!nic.present) return NET_ENODEV;
    if (n < 0 || n > UDP_MAXDATA || (n && !data)) return NET_EINVAL;
    mutex_lock(&net_lock);
    UdpSock *s = udp_get(h);
    int rc = s ? ready_locked() : NET_EINVAL;
    if (!rc) rc = udp_output(cfg.ip, s->port, ip, port, data, (u32)n);
    mutex_unlock(&net_lock);
    return rc < 0 ? rc : n;
}
int udp_recvfrom(int h, void *buf, int n, u32 *ip, u16 *port, int timeout_ms) {
    if (!nic.present) return NET_ENODEV;
    if (n < 0 || (n && !buf)) return NET_EINVAL;
    u32 start = ticks;
    for (;;) {
        int rc = 1;
        mutex_lock(&net_lock);
        nic_poll();
        UdpSock *s = udp_get(h);
        if (!s) rc = NET_EINVAL;
        else if (s->q_count > 0) {
            int len = s->q[s->q_head].len;
            if (len > n) len = n;                                        /* a longer datagram is cut */
            memcpy(buf, s->q[s->q_head].data, len);
            if (ip) *ip = s->q[s->q_head].ip;
            if (port) *port = s->q[s->q_head].port;
            s->q_head = (s->q_head + 1) % UDP_QLEN; s->q_count--;
            rc = -1000 - len;                                            /* marks success: see below */
        } else if (!nic.link) rc = NET_EDOWN;
        mutex_unlock(&net_lock);
        if (rc <= -1000) return -1000 - rc;
        if (rc < 0) return rc;
        if (!wait_more(start, timeout_ms, 0)) return NET_ETIMEOUT;
    }
}
int udp_close(int h) {
    if (!nic.present) return NET_ENODEV;
    mutex_lock(&net_lock);
    UdpSock *s = udp_get(h);
    if (s) s->used = false;
    mutex_unlock(&net_lock);
    return s ? 0 : NET_EINVAL;
}

/* --- DNS resolver (runs in the caller's thread on top of a UDP socket) --- */
#define DNS_CACHE_N 8
#define DNS_NAME_MAX 253
static struct { char name[64]; u32 ip, expires; bool used; } dns_cache[DNS_CACHE_N];
static int dns_cache_next;

static int dns_build(u8 *q, u32 id, const char *name) {
    memset(q, 0, 12);
    wr16(q, id); q[2] = 0x01; q[5] = 1;                                  /* recursion desired, one question */
    int pos = 12;
    const char *p = name;
    while (*p) {
        const char *e = p;
        while (*e && *e != '.') {
            if (!isalnum(*e) && *e != '-' && *e != '_') return NET_EINVAL;
            e++;
        }
        int l = (int)(e - p);
        if (l < 1 || l > 63 || pos + l + 6 > 300) return NET_EINVAL;
        q[pos++] = (u8)l;
        memcpy(q + pos, p, l); pos += l;
        p = *e ? e + 1 : e;
    }
    if (pos == 12) return NET_EINVAL;
    q[pos++] = 0;
    wr16(q + pos, 1); wr16(q + pos + 2, 1);                              /* type A, class IN */
    return pos + 4;
}
static int dns_skip_name(const u8 *m, int len, int pos) {
    for (int guard = 0; guard < 128; guard++) {
        if (pos < 0 || pos >= len) return -1;
        u32 c = m[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return pos + 2 <= len ? pos + 2 : -1;    /* compression pointer ends the name */
        if (c & 0xC0) return -1;
        pos += 1 + (int)c;
    }
    return -1;
}
/* 0: *ip and *ttl filled in; 1: not the answer we wait for; < 0: definite failure */
static int dns_parse(const u8 *m, int len, u32 id, u32 *ip, u32 *ttl) {
    if (len < 12 || rd16(m) != id || !(m[2] & 0x80)) return 1;
    u32 rcode = m[3] & 15, qd = rd16(m + 4), an = rd16(m + 6);
    if (rcode == 3) return NET_ENOTFOUND;
    if (rcode != 0 || qd > 4) return NET_EDNS;
    int pos = 12;
    for (u32 i = 0; i < qd; i++) {
        pos = dns_skip_name(m, len, pos);
        if (pos < 0 || pos + 4 > len) return NET_EDNS;
        pos += 4;
    }
    for (u32 i = 0; i < an && i < 64; i++) {
        pos = dns_skip_name(m, len, pos);
        if (pos < 0 || pos + 10 > len) return NET_EDNS;
        u32 type = rd16(m + pos), cls = rd16(m + pos + 2), rdlen = rd16(m + pos + 8);
        *ttl = rd32(m + pos + 4);
        pos += 10;
        if (pos + (int)rdlen > len) return NET_EDNS;
        if (type == 1 && cls == 1 && rdlen == 4) { *ip = rd32(m + pos); return 0; }
        pos += (int)rdlen;                                               /* CNAME and others: keep looking */
    }
    return NET_ENOTFOUND;
}
int net_resolve(const char *name, u32 *ip) {
    if (!name || !ip) return NET_EINVAL;
    if (net_aton(name, ip)) return 0;
    if (!nic.present) return NET_ENODEV;
    size_t nl = strlen(name);
    if (nl && name[nl - 1] == '.') nl--;
    if (nl < 1 || nl > DNS_NAME_MAX) return NET_EINVAL;
    char host[DNS_NAME_MAX + 1];
    for (size_t i = 0; i < nl; i++) host[i] = (char)tolower(name[i]);
    host[nl] = 0;
    u8 q[320], r[512];
    int qlen = dns_build(q, 0, host);
    if (qlen < 0) return qlen;

    mutex_lock(&net_lock);
    int rc = ready_locked();
    u32 dns = cfg.dns;
    bool hit = false;
    if (!rc) for (int i = 0; i < DNS_CACHE_N; i++)
        if (dns_cache[i].used && !tick_due(dns_cache[i].expires) && !strcmp(dns_cache[i].name, host)) { *ip = dns_cache[i].ip; hit = true; }
    u32 id = net_rand() & 0xFFFF;
    mutex_unlock(&net_lock);
    if (rc) return rc;
    if (hit) return 0;
    if (!dns) return NET_EDNS;

    int h = udp_open(0);
    if (h < 0) return h;
    u32 addr = 0, ttl = 0;
    rc = NET_ETIMEOUT;
    for (int attempt = 0; attempt < 3 && rc == NET_ETIMEOUT; attempt++) {
        id = (id + 0x9E37) & 0xFFFF;
        wr16(q, id);
        int sr = udp_sendto(h, dns, 53, q, qlen);
        if (sr < 0) { rc = sr; break; }
        u32 start = ticks;
        for (;;) {
            int left = 2000 - (int)(ticks - start) * (1000 / HZ);
            u32 from = 0; u16 fport = 0;
            int n = left > 0 ? udp_recvfrom(h, r, sizeof r, &from, &fport, left) : NET_ETIMEOUT;
            if (n == NET_ETIMEOUT) break;
            if (n < 0) { rc = n; break; }
            if (from != dns || fport != 53) continue;
            int pr = dns_parse(r, n, id, &addr, &ttl);
            if (pr == 1) continue;
            rc = pr;
            break;
        }
    }
    udp_close(h);
    if (rc) return rc;
    *ip = addr;
    if (nl < sizeof dns_cache[0].name) {
        if (ttl < 10) ttl = 10;
        if (ttl > 600) ttl = 600;
        mutex_lock(&net_lock);
        int i = dns_cache_next; dns_cache_next = (dns_cache_next + 1) % DNS_CACHE_N;
        strlcpy(dns_cache[i].name, host, sizeof dns_cache[i].name);
        dns_cache[i].ip = addr; dns_cache[i].expires = ticks + ttl * HZ; dns_cache[i].used = true;
        mutex_unlock(&net_lock);
    }
    return 0;
}

/* --- TCP sockets --- */
static TcpSock *tcp_get(int h) {
    if (h <= 0 || (h & 15) >= TCP_N) return NULL;
    TcpSock *s = &tcps[h & 15];
    return (s->state != TS_FREE && s->app_open && s->gen == ((u32)h >> 4)) ? s : NULL;
}
int tcp_connect(u32 ip, u16 port, int timeout_ms) {
    if (!nic.present) return NET_ENODEV;
    if (!ip || !port) return NET_EINVAL;
    mutex_lock(&net_lock);
    int rc = ready_locked(), h = 0;
    TcpSock *s = NULL;
    if (!rc) {
        for (int i = 0; i < TCP_N && !s; i++) if (tcps[i].state == TS_FREE) s = &tcps[i];
        if (!s) for (int i = 0; i < TCP_N; i++) {                        /* none free: take one that is only finishing its close */
            TcpSock *c = &tcps[i];
            if (!c->app_open && (!s || (i32)(c->close_tick - s->close_tick) < 0)) s = c;
        }
        if (s && s->state != TS_FREE) tcp_kill(s, NET_ECLOSED, true);
        u16 lport = s ? pick_port() : 0;
        if (!s || !lport) rc = NET_EBUSY;
        else {
            s->err = 0;
            s->fin_rcvd = s->fin_queued = s->fin_sent = s->fin_acked = s->probe = false;
            s->gen = (++tcp_gen & 0x3FFFFFF) ? (tcp_gen & 0x3FFFFFF) : (tcp_gen = 1);
            owner_set(&s->owner);
            s->rip = ip; s->rport = port; s->lport = lport;
            s->iss = net_rand();
            s->snd_una = s->iss; s->snd_nxt = s->snd_max = s->iss + 1;
            s->snd_wnd = 0; s->mss = 536; s->rcv_nxt = 0; s->rcv_adv = 0;
            s->rto = TCP_RTO; s->retries = 0; s->deadline = ticks + s->rto; s->timer_on = true; s->close_tick = ticks;
            s->rx_head = s->rx_len = s->tx_len = 0;
            s->app_open = true;
            s->state = TS_SYN_SENT;
            h = H_MAKE((int)(s - tcps), s->gen);
            if (tcp_xmit(s, s->iss, TH_SYN, NULL, 0) == NET_ENOROUTE) { tcp_free(s); rc = NET_ENOROUTE; }
        }
    }
    mutex_unlock(&net_lock);
    if (rc) return rc;
    u32 start = ticks;
    for (;;) {
        mutex_lock(&net_lock);
        nic_poll();
        if (tcp_get(h) != s) rc = NET_ECLOSED;
        else if (s->state == TS_OPEN) rc = 0;
        else if (s->state == TS_DEAD) { rc = s->err ? s->err : NET_ECLOSED; tcp_free(s); }
        else if (!nic.link) { rc = NET_EDOWN; tcp_free(s); }
        else rc = 1;
        mutex_unlock(&net_lock);
        if (rc == 0) return h;
        if (rc < 0) return rc;
        if (!wait_more(start, timeout_ms, 0)) break;
    }
    mutex_lock(&net_lock);
    rc = NET_ETIMEOUT;
    if (tcp_get(h) == s) { if (s->state == TS_OPEN) rc = 0; else tcp_free(s); }
    mutex_unlock(&net_lock);
    return rc ? rc : h;
}
int tcp_send(int h, const void *data, int n, int timeout_ms) {
    if (!nic.present) return NET_ENODEV;
    if (n < 0 || (n && !data)) return NET_EINVAL;
    int done = 0;
    u32 start = ticks;
    for (;;) {
        int rc = 0;
        mutex_lock(&net_lock);
        nic_poll();
        TcpSock *s = tcp_get(h);
        if (!s) rc = NET_EINVAL;
        else if (s->state == TS_DEAD) rc = s->err ? s->err : NET_ECLOSED;
        else if (s->state != TS_OPEN || s->fin_queued) rc = NET_ECLOSED;
        else {
            u32 space = TCP_TXBUF - s->tx_len, take = (u32)(n - done);
            if (take > space) take = space;
            memcpy(s->tx + s->tx_len, (const u8 *)data + done, take);
            s->tx_len += take; done += (int)take;
            tcp_output(s, false);
        }
        mutex_unlock(&net_lock);
        if (rc < 0) return done ? done : rc;
        if (done >= n) return done;
        if (!wait_more(start, timeout_ms, 0)) return done ? done : NET_ETIMEOUT;
    }
}
int tcp_recv(int h, void *buf, int n, int timeout_ms) {
    if (!nic.present) return NET_ENODEV;
    if (n <= 0 || !buf) return NET_EINVAL;
    u32 start = ticks;
    for (;;) {
        int rc = -1000;                                                  /* -1000: nothing yet */
        mutex_lock(&net_lock);
        nic_poll();
        TcpSock *s = tcp_get(h);
        if (!s) rc = NET_EINVAL;
        else if (s->rx_len > 0) {
            u32 take = (u32)n < s->rx_len ? (u32)n : s->rx_len;
            u32 first = TCP_RXBUF - s->rx_head;
            if (first > take) first = take;
            memcpy(buf, s->rx + s->rx_head, first);
            if (take > first) memcpy((u8 *)buf + first, s->rx, take - first);
            s->rx_head = (s->rx_head + take) % TCP_RXBUF; s->rx_len -= take;
            /* tell the peer when the window has opened noticeably since the last ACK */
            u32 space = TCP_RXBUF - s->rx_len;
            if (s->state == TS_OPEN && !s->fin_rcvd && (space >= s->rcv_adv + 2 * TCP_MSS || (s->rcv_adv < TCP_MSS && space >= TCP_MSS)))
                tcp_output(s, true);
            rc = (int)take;
        }
        else if (s->fin_rcvd) rc = 0;
        else if (s->state == TS_DEAD) rc = s->err ? s->err : NET_ECLOSED;
        else if (s->state != TS_OPEN) rc = NET_ECLOSED;
        mutex_unlock(&net_lock);
        if (rc != -1000) return rc;
        if (!wait_more(start, timeout_ms, 0)) return NET_ETIMEOUT;
    }
}
int tcp_close(int h) {
    if (!nic.present) return NET_ENODEV;
    mutex_lock(&net_lock);
    TcpSock *s = tcp_get(h);
    if (s) {
        s->app_open = false;
        s->close_tick = ticks;
        if (s->state == TS_DEAD || s->state == TS_SYN_SENT) tcp_free(s);
        else if (s->rx_len > 0) tcp_kill(s, NET_ECLOSED, true);         /* unread data: abort instead of a graceful close */
        else {
            s->rx_len = 0;
            s->fin_queued = true;
            tcp_output(s, false);
            if (s->fin_rcvd && s->fin_acked) tcp_free(s);
        }
    }
    mutex_unlock(&net_lock);
    return s ? 0 : NET_EINVAL;
}
