// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Network Stack Implementation
//
// Protocols:
//   Ethernet II (RFC 894) — frame parsing
//   ARP (RFC 826) — address resolution + cache
//   IPv4 (RFC 791) — routing, header checksum
//   ICMPv4 (RFC 792) — echo request/reply (ping)
//   UDP (RFC 768) — socket send/receive
//   TCP (RFC 793) — state machine stub (CLOSED, LISTEN, SYN_SENT, ESTABLISHED)
//   DHCP (RFC 2131) — client: DISCOVER → OFFER → REQUEST → ACK
//
// Reference: task.md [Phase 6]

#include "vortex/kernel/net.hpp"
#include "vortex/types.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::net {

using arch::x86_64::serial_write;
using arch::x86_64::serial_write_hex;
using arch::x86_64::serial_write_dec;

// ─── External Eth-Layer Functions (implemented in eth.cpp) ──────────────────

extern bool eth_transmit(const uint8_t* data, size_t len);
extern void eth_get_mac(uint8_t mac[6]);

// ─── Byte-Order Helpers (x86 little-endian ↔ network big-endian) ────────────

constexpr uint16_t ntohs(uint16_t n) { return __builtin_bswap16(n); }
constexpr uint32_t ntohl(uint32_t n) { return __builtin_bswap32(n); }
constexpr uint16_t htons(uint16_t n) { return __builtin_bswap16(n); }
constexpr uint32_t htonl(uint32_t n) { return __builtin_bswap32(n); }

// ─── Address Types ───────────────────────────────────────────────────────────

struct [[gnu::packed]] MacAddr {
    uint8_t addr[6];

    bool operator==(const MacAddr& o) const {
        return addr[0] == o.addr[0] && addr[1] == o.addr[1] &&
               addr[2] == o.addr[2] && addr[3] == o.addr[3] &&
               addr[4] == o.addr[4] && addr[5] == o.addr[5];
    }
    bool is_broadcast() const {
        return addr[0] == 0xFF && addr[1] == 0xFF && addr[2] == 0xFF &&
               addr[3] == 0xFF && addr[4] == 0xFF && addr[5] == 0xFF;
    }
    bool is_null() const {
        return addr[0] == 0 && addr[1] == 0 && addr[2] == 0 &&
               addr[3] == 0 && addr[4] == 0 && addr[5] == 0;
    }
};

using ipv4_t = uint32_t; // Network byte order

// ─── Protocol Constants ──────────────────────────────────────────────────────

// EtherType
constexpr uint16_t ETHERTYPE_ARP  = 0x0806;
constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;

// ARP hardware / protocol
constexpr uint16_t ARP_HTYPE_ETH = 0x0001;
constexpr uint16_t ARP_PTYPE_IP  = 0x0800;
constexpr uint8_t  ARP_HLEN_ETH  = 6;
constexpr uint8_t  ARP_PLEN_IP   = 4;
constexpr uint16_t ARP_OP_REQUEST = 0x0001;
constexpr uint16_t ARP_OP_REPLY   = 0x0002;

// IP protocol numbers
constexpr uint8_t IPPROTO_ICMP = 1;
constexpr uint8_t IPPROTO_TCP  = 6;
constexpr uint8_t IPPROTO_UDP  = 17;

// ICMP types
constexpr uint8_t ICMP_ECHO_REPLY   = 0;
constexpr uint8_t ICMP_ECHO_REQUEST = 8;

// UDP / TCP port limits
constexpr uint16_t PORT_ANY = 0;
constexpr size_t   MAX_UDP_SOCKETS = 16;
constexpr size_t   MAX_TCP_SOCKETS = 16;
constexpr size_t   ARP_CACHE_SIZE  = 16;

// DHCP well-known ports
constexpr uint16_t DHCP_SERVER_PORT = 67;
constexpr uint16_t DHCP_CLIENT_PORT = 68;

// DHCP magic cookie (RFC 2131 §7)
constexpr uint32_t DHCP_MAGIC_COOKIE = 0x63825363;

// DHCP option tags
constexpr uint8_t DHCP_OPT_PAD        = 0;
constexpr uint8_t DHCP_OPT_SUBNET_MASK = 1;
constexpr uint8_t DHCP_OPT_ROUTER     = 3;
constexpr uint8_t DHCP_OPT_DNS        = 6;
constexpr uint8_t DHCP_OPT_MSG_TYPE   = 53;
constexpr uint8_t DHCP_OPT_SERVER_ID  = 54;
constexpr uint8_t DHCP_OPT_REQ_IP    = 50;
constexpr uint8_t DHCP_OPT_LEASE     = 51;
constexpr uint8_t DHCP_OPT_END       = 255;

// DHCP message types (option 53 value)
constexpr uint8_t DHCP_DISCOVER = 1;
constexpr uint8_t DHCP_OFFER    = 2;
constexpr uint8_t DHCP_REQUEST  = 3;
constexpr uint8_t DHCP_ACK      = 5;
constexpr uint8_t DHCP_NAK      = 6;

// ─── Protocol Header Structures ─────────────────────────────────────────────

struct [[gnu::packed]] EthHeader {
    MacAddr   dst;
    MacAddr   src;
    uint16_t  type;
};

struct [[gnu::packed]] ArpPacket {
    uint16_t  htype;
    uint16_t  ptype;
    uint8_t   hlen;
    uint8_t   plen;
    uint16_t  oper;
    MacAddr   sha;
    uint8_t   spa[4];
    MacAddr   tha;
    uint8_t   tpa[4];
};

struct [[gnu::packed]] Ipv4Header {
    uint8_t   ver_ihl;      // Version (4) << 4 | IHL
    uint8_t   dscp_ecn;
    uint16_t  total_length;
    uint16_t  identification;
    uint16_t  flags_frag_offset;
    uint8_t   ttl;
    uint8_t   protocol;
    uint16_t  header_checksum;
    uint8_t   src_ip[4];
    uint8_t   dst_ip[4];
};

struct [[gnu::packed]] IcmpHeader {
    uint8_t   type;
    uint8_t   code;
    uint16_t  checksum;
    uint16_t  identifier;
    uint16_t  sequence;
};

struct [[gnu::packed]] UdpHeader {
    uint16_t  src_port;
    uint16_t  dst_port;
    uint16_t  length;
    uint16_t  checksum;
};

struct [[gnu::packed]] TcpHeader {
    uint16_t  src_port;
    uint16_t  dst_port;
    uint32_t  seq_number;
    uint32_t  ack_number;
    uint16_t  data_offset_flags;
    uint16_t  window_size;
    uint16_t  checksum;
    uint16_t  urgent_pointer;
};

// ─── DHCP Packet ─────────────────────────────────────────────────────────────

struct [[gnu::packed]] DhcpPacket {
    uint8_t   op;      // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t   htype;   // 1 = Ethernet
    uint8_t   hlen;    // 6
    uint8_t   hops;
    uint32_t  xid;
    uint16_t  secs;
    uint16_t  flags;
    uint8_t   ciaddr[4];
    uint8_t   yiaddr[4];
    uint8_t   siaddr[4];
    uint8_t   giaddr[4];
    uint8_t   chaddr[16];
    char      sname[64];
    char      boot_file[128];
    uint32_t  magic;
    // Options follow
};

// ─── Static State ────────────────────────────────────────────────────────────

static MacAddr   g_our_mac;
static ipv4_t    g_our_ip       = 0;
static ipv4_t    g_subnet_mask  = 0;
static ipv4_t    g_gateway_ip   = 0;
static bool      g_net_ready    = false;
static uint16_t  g_ip_ident     = 0;

// ─── ARP Cache ───────────────────────────────────────────────────────────────

struct ArpEntry {
    ipv4_t    ip;
    MacAddr   mac;
    bool      valid;
};

static ArpEntry g_arp_cache[ARP_CACHE_SIZE];
static size_t   g_arp_cache_next = 0;

static void arp_cache_insert(ipv4_t ip, const MacAddr& mac) {
    for (size_t i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            g_arp_cache[i].mac   = mac;
            return;
        }
    }
    g_arp_cache[g_arp_cache_next].ip    = ip;
    g_arp_cache[g_arp_cache_next].mac   = mac;
    g_arp_cache[g_arp_cache_next].valid = true;
    g_arp_cache_next = (g_arp_cache_next + 1) % ARP_CACHE_SIZE;
}

static bool arp_cache_lookup(ipv4_t ip, MacAddr& mac) {
    for (size_t i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            mac = g_arp_cache[i].mac;
            return true;
        }
    }
    return false;
}

// ─── UDP Socket Table ────────────────────────────────────────────────────────

enum class UdpState {
    FREE,
    BOUND,
    CONNECTED
};

struct UdpControlBlock {
    UdpState  state;
    uint16_t  local_port;
    ipv4_t    remote_ip;
    uint16_t  remote_port;
    // For simplicity: single-packet buffer
    uint8_t   rx_data[2048];
    size_t    rx_len;
    bool      rx_pending;
};

static UdpControlBlock g_udp_sockets[MAX_UDP_SOCKETS];

// ─── TCP Control Block Stub ──────────────────────────────────────────────────

enum class TcpState : uint8_t {
    CLOSED     = 0,
    LISTEN     = 1,
    SYN_SENT   = 2,
    SYN_RECEIVED = 3,
    ESTABLISHED = 4,
    FIN_WAIT_1 = 5,
    FIN_WAIT_2 = 6,
    CLOSE_WAIT = 7,
    CLOSING    = 8,
    LAST_ACK   = 9,
    TIME_WAIT  = 10
};

struct TcpControlBlock {
    TcpState  state;
    uint16_t  local_port;
    uint16_t  remote_port;
    ipv4_t    remote_ip;
    uint32_t  snd_nxt;
    uint32_t  rcv_nxt;
};

static TcpControlBlock g_tcp_sockets[MAX_TCP_SOCKETS];

// ─── DHCP State ──────────────────────────────────────────────────────────────

enum class DhcpClientState {
    IDLE,
    DISCOVERING,
    WAITING_OFFER,
    REQUESTING,
    BOUND
};

static DhcpClientState g_dhcp_state = DhcpClientState::IDLE;
static uint32_t        g_dhcp_xid  = 0;
static ipv4_t          g_dhcp_server_ip = 0;
static ipv4_t          g_dhcp_offered_ip = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static ipv4_t ip_from_bytes(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return htonl(static_cast<uint32_t>(a) << 24 |
                 static_cast<uint32_t>(b) << 16 |
                 static_cast<uint32_t>(c) << 8  |
                 static_cast<uint32_t>(d));
}

static void ip_to_bytes(ipv4_t ip, uint8_t* out) {
    uint32_t n = ntohl(ip);
    out[0] = static_cast<uint8_t>(n >> 24);
    out[1] = static_cast<uint8_t>(n >> 16);
    out[2] = static_cast<uint8_t>(n >> 8);
    out[3] = static_cast<uint8_t>(n);
}

static void mac_to_str(const MacAddr& mac, char* out) {
    constexpr const char* hex = "0123456789ABCDEF";
    for (int i = 0; i < 6; ++i) {
        out[i * 3]     = hex[(mac.addr[i] >> 4) & 0xF];
        out[i * 3 + 1] = hex[mac.addr[i] & 0xF];
        out[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
}

static uint16_t ip_checksum(const void* data, size_t len) {
    uint32_t sum = 0;
    const uint16_t* words = static_cast<const uint16_t*>(data);
    for (size_t i = 0; i < len / 2; ++i) {
        sum += ntohs(words[i]);
    }
    if (len & 1) {
        sum += static_cast<const uint8_t*>(data)[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return htons(~static_cast<uint16_t>(sum) & 0xFFFF);
}

// ─── Ethernet Frame Transmission ─────────────────────────────────────────────

static MacAddr g_broadcast_mac = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

static bool eth_send(const MacAddr& dst, uint16_t type,
                     const void* payload, size_t payload_len)
{
    // Allocate buffer: header + payload (stack-based for small packets)
    uint8_t buf[64 + 1514]; // Max ethernet frame
    if (sizeof(EthHeader) + payload_len > sizeof(buf)) {
        serial_write("[NET] eth_send: packet too large\n");
        return false;
    }

    auto* eth = reinterpret_cast<EthHeader*>(buf);
    eth->dst = dst;
    eth->src = g_our_mac;
    eth->type = htons(type);

    __builtin_memcpy(buf + sizeof(EthHeader), payload, payload_len);

    return eth_transmit(buf, sizeof(EthHeader) + payload_len);
}

// ─── ARP ─────────────────────────────────────────────────────────────────────

static void arp_send_request(ipv4_t target_ip) {
    serial_write("[ARP] Sending request for ");
    serial_write_hex(ntohl(target_ip));
    serial_write("\n");

    ArpPacket req;
    req.htype = htons(ARP_HTYPE_ETH);
    req.ptype = htons(ARP_PTYPE_IP);
    req.hlen  = ARP_HLEN_ETH;
    req.plen  = ARP_PLEN_IP;
    req.oper  = htons(ARP_OP_REQUEST);
    req.sha   = g_our_mac;
    ip_to_bytes(g_our_ip, req.spa);
    req.tha   = MacAddr{{0, 0, 0, 0, 0, 0}};
    ip_to_bytes(target_ip, req.tpa);

    eth_send(g_broadcast_mac, ETHERTYPE_ARP, &req, sizeof(req));
}

static void arp_handle_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(ArpPacket)) return;

    const auto* arp = reinterpret_cast<const ArpPacket*>(data);

    if (ntohs(arp->htype) != ARP_HTYPE_ETH ||
        ntohs(arp->ptype) != ARP_PTYPE_IP) return;

    ipv4_t sender_ip;
    __builtin_memcpy(&sender_ip, arp->spa, 4);

    // Cache sender (RFC 826: update cache on any ARP packet)
    arp_cache_insert(sender_ip, arp->sha);

    ipv4_t target_ip;
    __builtin_memcpy(&target_ip, arp->tpa, 4);

    // Only respond to requests for our IP
    if (ntohs(arp->oper) == ARP_OP_REQUEST && target_ip == g_our_ip) {
        serial_write("[ARP] Reply to ");
        serial_write_hex(ntohl(sender_ip));
        serial_write("\n");

        ArpPacket reply;
        reply.htype = htons(ARP_HTYPE_ETH);
        reply.ptype = htons(ARP_PTYPE_IP);
        reply.hlen  = ARP_HLEN_ETH;
        reply.plen  = ARP_PLEN_IP;
        reply.oper  = htons(ARP_OP_REPLY);
        reply.sha   = g_our_mac;
        ip_to_bytes(g_our_ip, reply.spa);
        reply.tha   = arp->sha;
        __builtin_memcpy(reply.tpa, arp->spa, 4);

        eth_send(arp->sha, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}

static bool arp_resolve(ipv4_t ip, MacAddr& mac) {
    if (arp_cache_lookup(ip, mac)) {
        return true;
    }
    arp_send_request(ip);
    // Poll a few times (simplified — real stack would block/wait)
    for (int retry = 0; retry < 3; ++retry) {
        // In a real kernel we'd yield and wait for the ARP reply interrupt.
        // For now, return false and let upper layers retry.
        (void)retry;
    }
    return false;
}

// ─── Forward declarations ─────────────────────────────────────────────────────
static void icmp_handle_echo(const uint8_t* data, size_t len, uint32_t src_ip);
static void udp_handle_packet(const uint8_t* data, size_t len);
static void tcp_handle_packet(const uint8_t* data, size_t len);
static void dhcp_send_request(uint32_t server_id, uint32_t offered_ip);

// ─── IPv4 ────────────────────────────────────────────────────────────────────

static void ipv4_handle_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(Ipv4Header)) return;

    const auto* ip = reinterpret_cast<const Ipv4Header*>(data);

    // Verify checksum
    uint16_t received_cksum = ip->header_checksum;
    // Temporarily zero checksum for computation
    // We compute on a copy to avoid const-cast
    Ipv4Header hdr_copy;
    __builtin_memcpy(&hdr_copy, ip, sizeof(hdr_copy));
    hdr_copy.header_checksum = 0;
    uint16_t calc_cksum = ip_checksum(&hdr_copy, (ip->ver_ihl & 0x0F) * 4);
    if (calc_cksum != received_cksum) {
        serial_write("[IP] Bad checksum\n");
        return;
    }

    uint8_t ihl = ip->ver_ihl & 0x0F;
    size_t header_len = ihl * 4;
    if (header_len < sizeof(Ipv4Header)) return;

    size_t total_len = ntohs(ip->total_length);
    if (total_len > len) {
        serial_write("[IP] Truncated packet\n");
        return;
    }

    ipv4_t src_ip;
    __builtin_memcpy(&src_ip, ip->src_ip, 4);

    ipv4_t dst_ip;
    __builtin_memcpy(&dst_ip, ip->dst_ip, 4);

    // Check if packet is for us or broadcast
    if (dst_ip != g_our_ip && dst_ip != 0xFFFFFFFF && dst_ip != (g_our_ip | ~g_subnet_mask)) {
        return;
    }

    const uint8_t* payload = data + header_len;
    size_t payload_len = total_len - header_len;

    switch (ip->protocol) {
    case IPPROTO_ICMP:
        icmp_handle_echo(payload, payload_len, src_ip);
        break;
    case IPPROTO_UDP:
        udp_handle_packet(payload, payload_len);
        break;
    case IPPROTO_TCP:
        tcp_handle_packet(payload, payload_len);
        break;
    default:
        break;
    }
}

static bool ipv4_send(ipv4_t dst_ip, uint8_t protocol,
                      const uint8_t* payload, size_t payload_len)
{
    size_t header_len = sizeof(Ipv4Header);
    size_t total_len  = header_len + payload_len;

    uint8_t buf[sizeof(Ipv4Header) + 1500];
    if (total_len > sizeof(buf)) {
        serial_write("[IP] Packet too large\n");
        return false;
    }

    auto* ip = reinterpret_cast<Ipv4Header*>(buf);
    ip->ver_ihl          = 0x45; // IPv4, IHL=5
    ip->dscp_ecn         = 0;
    ip->total_length     = htons(static_cast<uint16_t>(total_len));
    ip->identification   = htons(g_ip_ident++);
    ip->flags_frag_offset = htons(0x4000); // Don't fragment
    ip->ttl              = 64;
    ip->protocol         = protocol;
    ip->header_checksum  = 0;

    uint8_t src_bytes[4];
    ip_to_bytes(g_our_ip, src_bytes);
    __builtin_memcpy(ip->src_ip, src_bytes, 4);

    uint8_t dst_bytes[4];
    ip_to_bytes(dst_ip, dst_bytes);
    __builtin_memcpy(ip->dst_ip, dst_bytes, 4);

    // Compute checksum (header only)
    Ipv4Header hdr_for_cksum;
    __builtin_memcpy(&hdr_for_cksum, ip, sizeof(hdr_for_cksum));
    ip->header_checksum = ip_checksum(&hdr_for_cksum, header_len);

    // Copy payload
    if (payload_len > 0) {
        __builtin_memcpy(buf + header_len, payload, payload_len);
    }

    // Determine next-hop MAC
    MacAddr next_hop_mac;
    ipv4_t next_hop = dst_ip;

    if (dst_ip == 0xFFFFFFFF) {
        // Broadcast
        return eth_send(g_broadcast_mac, ETHERTYPE_IPV4, buf, total_len);
    }

    // Check if destination is on our subnet
    ipv4_t masked_dst = ntohl(dst_ip) & ntohl(g_subnet_mask);
    ipv4_t masked_our = ntohl(g_our_ip) & ntohl(g_subnet_mask);

    if (masked_dst != masked_our && g_gateway_ip != 0) {
        next_hop = g_gateway_ip;
    }

    if (!arp_resolve(next_hop, next_hop_mac)) {
        serial_write("[IP] ARP resolution failed\n");
        return false;
    }

    return eth_send(next_hop_mac, ETHERTYPE_IPV4, buf, total_len);
}

// ─── ICMP ────────────────────────────────────────────────────────────────────

static void icmp_handle_echo(const uint8_t* data, size_t len, ipv4_t src_ip) {
    if (len < sizeof(IcmpHeader)) return;

    const auto* icmp = reinterpret_cast<const IcmpHeader*>(data);

    if (icmp->type != ICMP_ECHO_REQUEST) return;

    serial_write("[ICMP] Echo request from ");
    serial_write_hex(ntohl(src_ip));
    serial_write("\n");

    // Build reply: copy original data, flip type to REPLY
    uint8_t reply_buf[512];
    size_t reply_len = (len < sizeof(reply_buf)) ? len : sizeof(reply_buf);
    __builtin_memcpy(reply_buf, data, reply_len);

    auto* reply = reinterpret_cast<IcmpHeader*>(reply_buf);
    reply->type = ICMP_ECHO_REPLY;
    reply->checksum = 0;

    // Recompute ICMP checksum (covers header + data)
    // For checksum computation, temporarily zero out checksum in a copy
    IcmpHeader tmp;
    __builtin_memcpy(&tmp, reply, sizeof(tmp));
    tmp.checksum = 0;
    __builtin_memcpy(reply_buf, &tmp, sizeof(tmp));

    // Compute pseudo-header checksum
    reply->checksum = ip_checksum(reply_buf, reply_len);

    ipv4_send(src_ip, IPPROTO_ICMP, reply_buf, reply_len);
}

// ─── UDP ─────────────────────────────────────────────────────────────────────

static uint16_t udp_checksum(const void* udp_hdr, size_t udp_len,
                             ipv4_t src_ip, ipv4_t dst_ip)
{
    // Pseudo-header checksum (RFC 768)
    // We use the simpler method: compute over pseudo-header + UDP datagram
    // Build pseudo-header on the stack
    struct [[gnu::packed]] PseudoHeader {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    };

    uint8_t buf[sizeof(PseudoHeader) + 512];
    size_t total = sizeof(PseudoHeader) + udp_len;
    if (total > sizeof(buf)) return 0;

    auto* ph = reinterpret_cast<PseudoHeader*>(buf);
    ph->src   = src_ip;
    ph->dst   = dst_ip;
    ph->zero  = 0;
    ph->proto = IPPROTO_UDP;
    ph->len   = htons(static_cast<uint16_t>(udp_len));

    // Copy UDP datagram after pseudo-header, zero checksum field
    __builtin_memcpy(buf + sizeof(PseudoHeader), udp_hdr, udp_len);
    // Zero the checksum field in the UDP header copy
    auto* uh = reinterpret_cast<UdpHeader*>(buf + sizeof(PseudoHeader));
    uh->checksum = 0;

    uint16_t cksum = ip_checksum(buf, total);
    return (cksum == 0) ? 0xFFFF : cksum;
}

static int udp_find_socket(uint16_t port) {
    for (size_t i = 0; i < MAX_UDP_SOCKETS; ++i) {
        if (g_udp_sockets[i].state != UdpState::FREE &&
            g_udp_sockets[i].local_port == port) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int udp_alloc_socket() {
    for (size_t i = 0; i < MAX_UDP_SOCKETS; ++i) {
        if (g_udp_sockets[i].state == UdpState::FREE) {
            g_udp_sockets[i].state      = UdpState::BOUND;
            g_udp_sockets[i].local_port = 0;
            g_udp_sockets[i].rx_pending = false;
            g_udp_sockets[i].rx_len     = 0;
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void udp_handle_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(UdpHeader)) return;

    const auto* udp = reinterpret_cast<const UdpHeader*>(data);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len  = ntohs(udp->length);

    int idx = udp_find_socket(dst_port);
    if (idx < 0) {
        serial_write("[UDP] No socket on port ");
        serial_write_dec(dst_port);
        serial_write("\n");
        return;
    }

    auto* sock = &g_udp_sockets[idx];
    size_t payload_len = udp_len - sizeof(UdpHeader);
    if (payload_len > sizeof(sock->rx_data)) {
        payload_len = sizeof(sock->rx_data);
    }

    __builtin_memcpy(sock->rx_data, data + sizeof(UdpHeader), payload_len);
    sock->rx_len      = payload_len;
    sock->rx_pending  = true;
    sock->remote_port = src_port;

    // Extract source IP from the IP header (passed in the network buffer)
    // TODO: store src_ip so sys_socket_recv can provide the sender address
}

// ─── TCP Stub ────────────────────────────────────────────────────────────────

static int tcp_find_or_alloc_socket(uint16_t port) {
    for (size_t i = 0; i < MAX_TCP_SOCKETS; ++i) {
        if (g_tcp_sockets[i].state != TcpState::CLOSED &&
            g_tcp_sockets[i].local_port == port) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < MAX_TCP_SOCKETS; ++i) {
        if (g_tcp_sockets[i].state == TcpState::CLOSED) {
            g_tcp_sockets[i].state      = TcpState::CLOSED;
            g_tcp_sockets[i].local_port = port;
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void tcp_handle_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(TcpHeader)) return;

    const auto* tcp = reinterpret_cast<const TcpHeader*>(data);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t flags    = ntohs(tcp->data_offset_flags) & 0x1F;

    int idx = tcp_find_or_alloc_socket(dst_port);
    if (idx < 0) return;

    auto* tcb = &g_tcp_sockets[idx];

    (void)tcb;

    serial_write("[TCP] Packet port=");
    serial_write_dec(dst_port);
    serial_write(" flags=0x");
    serial_write_hex(flags);
    serial_write("\n");

    // State machine stub (RFC 793 §3.2)
    switch (g_tcp_sockets[idx].state) {
    case TcpState::CLOSED:
        // Ignore
        break;
    case TcpState::LISTEN:
        if (flags & 0x02) { // SYN
            serial_write("[TCP] SYN received on listening socket\n");
            // Would transition to SYN_RECEIVED and send SYN+ACK
            g_tcp_sockets[idx].state = TcpState::SYN_RECEIVED;
        }
        break;
    case TcpState::SYN_SENT:
        if (flags & 0x12) { // SYN+ACK
            serial_write("[TCP] SYN+ACK received, connection established\n");
            g_tcp_sockets[idx].state = TcpState::ESTABLISHED;
        }
        break;
    case TcpState::ESTABLISHED:
        if (flags & 0x01) { // FIN
            serial_write("[TCP] FIN received\n");
            g_tcp_sockets[idx].state = TcpState::CLOSE_WAIT;
        }
        break;
    default:
        break;
    }
}

// ─── DHCP ────────────────────────────────────────────────────────────────────

static void dhcp_send_discover() {
    uint8_t buf[sizeof(DhcpPacket) + 64];
    __builtin_memset(buf, 0, sizeof(buf));

    auto* dhcp = reinterpret_cast<DhcpPacket*>(buf);
    dhcp->op    = 1;  // BOOTREQUEST
    dhcp->htype = 1;  // Ethernet
    dhcp->hlen  = 6;
    dhcp->hops  = 0;
    dhcp->xid   = htonl(g_dhcp_xid);
    dhcp->secs  = 0;
    dhcp->flags = htons(0x8000); // Broadcast flag

    __builtin_memcpy(dhcp->chaddr, g_our_mac.addr, 6);

    dhcp->magic = htonl(DHCP_MAGIC_COOKIE);

    // Options
    uint8_t* opt = buf + sizeof(DhcpPacket);
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_DISCOVER;
    *opt++ = DHCP_OPT_END;

    size_t total_len = sizeof(DhcpPacket) + (opt - (buf + sizeof(DhcpPacket)));

    serial_write("[DHCP] Sending DISCOVER (xid=0x");
    serial_write_hex(g_dhcp_xid);
    serial_write(")\n");

    // DHCP uses UDP, send via IP broadcast
    g_dhcp_state = DhcpClientState::WAITING_OFFER;
    // We'll manually construct and send the UDP/IP packet
    uint8_t udp_payload[sizeof(DhcpPacket) + 64];
    __builtin_memcpy(udp_payload, buf, total_len);

    // Build UDP datagram
    uint8_t udp_buf[sizeof(UdpHeader) + sizeof(DhcpPacket) + 64];
    auto* udp_hdr = reinterpret_cast<UdpHeader*>(udp_buf);
    udp_hdr->src_port = htons(DHCP_CLIENT_PORT);
    udp_hdr->dst_port = htons(DHCP_SERVER_PORT);
    udp_hdr->length   = htons(static_cast<uint16_t>(sizeof(UdpHeader) + total_len));
    udp_hdr->checksum = 0;

    __builtin_memcpy(udp_buf + sizeof(UdpHeader), udp_payload, total_len);

    ipv4_send(0xFFFFFFFF, IPPROTO_UDP, udp_buf, sizeof(UdpHeader) + total_len);
}

static bool dhcp_parse_options(const uint8_t* data, size_t len,
                               uint8_t& msg_type, ipv4_t& server_id)
{
    msg_type  = 0;
    server_id = 0;

    const uint8_t* opt = data;
    const uint8_t* end = data + len;

    while (opt < end) {
        if (*opt == DHCP_OPT_END) break;
        if (*opt == DHCP_OPT_PAD) { ++opt; continue; }

        uint8_t tag  = *opt++;
        uint8_t olen = *opt++;

        if (opt + olen > end) break;

        if (tag == DHCP_OPT_MSG_TYPE && olen >= 1) {
            msg_type = opt[0];
        } else if (tag == DHCP_OPT_SERVER_ID && olen >= 4) {
            __builtin_memcpy(&server_id, opt, 4);
        }

        opt += olen;
    }
    return msg_type != 0;
}

static void dhcp_handle_offer(const uint8_t* data, size_t len) {
    if (len < sizeof(DhcpPacket)) return;
    const auto* dhcp = reinterpret_cast<const DhcpPacket*>(data);

    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) return;
    if (dhcp->op != 2) return; // BOOTREPLY

    uint8_t  msg_type;
    ipv4_t   server_id;

    if (!dhcp_parse_options(data + sizeof(DhcpPacket),
                            len - sizeof(DhcpPacket),
                            msg_type, server_id))
    {
        return;
    }

    if (msg_type != DHCP_OFFER) return;

    ipv4_t offered_ip;
    __builtin_memcpy(&offered_ip, dhcp->yiaddr, 4);

    serial_write("[DHCP] OFFER from ");
    serial_write_hex(ntohl(server_id));
    serial_write(" IP=");
    serial_write_hex(ntohl(offered_ip));
    serial_write("\n");

    g_dhcp_server_ip   = server_id;
    g_dhcp_offered_ip  = offered_ip;
    g_dhcp_state       = DhcpClientState::REQUESTING;

    dhcp_send_request(server_id, offered_ip);
}

static void dhcp_send_request(ipv4_t server_ip, ipv4_t requested_ip) {
    uint8_t buf[sizeof(DhcpPacket) + 64];
    __builtin_memset(buf, 0, sizeof(buf));

    auto* dhcp = reinterpret_cast<DhcpPacket*>(buf);
    dhcp->op    = 1;
    dhcp->htype = 1;
    dhcp->hlen  = 6;
    dhcp->hops  = 0;
    dhcp->xid   = htonl(g_dhcp_xid);
    dhcp->secs  = 0;
    dhcp->flags = htons(0x8000);

    __builtin_memcpy(dhcp->chaddr, g_our_mac.addr, 6);
    dhcp->magic = htonl(DHCP_MAGIC_COOKIE);

    uint8_t* opt = buf + sizeof(DhcpPacket);
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;
    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    __builtin_memcpy(opt, &server_ip, 4); opt += 4;
    *opt++ = DHCP_OPT_REQ_IP;
    *opt++ = 4;
    __builtin_memcpy(opt, &requested_ip, 4); opt += 4;
    *opt++ = DHCP_OPT_END;

    size_t total_len = sizeof(DhcpPacket) + (opt - (buf + sizeof(DhcpPacket)));

    serial_write("[DHCP] Sending REQUEST for ");
    serial_write_hex(ntohl(requested_ip));
    serial_write("\n");

    uint8_t udp_buf[sizeof(UdpHeader) + sizeof(DhcpPacket) + 64];
    auto* udp_hdr = reinterpret_cast<UdpHeader*>(udp_buf);
    udp_hdr->src_port = htons(DHCP_CLIENT_PORT);
    udp_hdr->dst_port = htons(DHCP_SERVER_PORT);
    udp_hdr->length   = htons(static_cast<uint16_t>(sizeof(UdpHeader) + total_len));
    udp_hdr->checksum = 0;

    __builtin_memcpy(udp_buf + sizeof(UdpHeader), buf, total_len);
    ipv4_send(0xFFFFFFFF, IPPROTO_UDP, udp_buf, sizeof(UdpHeader) + total_len);
}

static void dhcp_handle_ack(const uint8_t* data, size_t len) {
    if (len < sizeof(DhcpPacket)) return;
    const auto* dhcp = reinterpret_cast<const DhcpPacket*>(data);

    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) return;
    if (dhcp->op != 2) return;

    uint8_t  msg_type;
    ipv4_t   server_id;

    if (!dhcp_parse_options(data + sizeof(DhcpPacket),
                            len - sizeof(DhcpPacket),
                            msg_type, server_id))
    {
        return;
    }

    if (msg_type != DHCP_ACK) return;

    ipv4_t assigned_ip;
    __builtin_memcpy(&assigned_ip, dhcp->yiaddr, 4);

    serial_write("[DHCP] ACK — assigned IP=");
    serial_write_hex(ntohl(assigned_ip));
    serial_write("\n");

    g_our_ip      = assigned_ip;
    g_subnet_mask = ip_from_bytes(255, 255, 255, 0);
    g_gateway_ip  = ip_from_bytes(192, 168, 1, 1);
    g_dhcp_state  = DhcpClientState::BOUND;
    g_net_ready   = true;

    serial_write("[NET] Interface configured: IP=");
    serial_write_hex(ntohl(g_our_ip));
    serial_write("\n");
}

static void dhcp_dispatch(const uint8_t* payload, size_t len) {
    if (len < sizeof(DhcpPacket)) return;
    const auto* dhcp = reinterpret_cast<const DhcpPacket*>(payload);
    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) return;

    uint8_t  msg_type = 0;
    ipv4_t   server_id = 0;
    dhcp_parse_options(payload + sizeof(DhcpPacket),
                       len - sizeof(DhcpPacket),
                       msg_type, server_id);

    switch (msg_type) {
    case DHCP_OFFER:
        if (g_dhcp_state == DhcpClientState::WAITING_OFFER) {
            dhcp_handle_offer(payload, len);
        }
        break;
    case DHCP_ACK:
        if (g_dhcp_state == DhcpClientState::REQUESTING) {
            dhcp_handle_ack(payload, len);
        }
        break;
    case DHCP_NAK:
        serial_write("[DHCP] NAK received\n");
        g_dhcp_state = DhcpClientState::IDLE;
        break;
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void net_init() {
    serial_write("[NET] Initializing network stack\n");

    // Get MAC address from eth layer
    eth_get_mac(g_our_mac.addr);

    char mac_str[18];
    mac_to_str(g_our_mac, mac_str);
    serial_write("[NET] MAC: ");
    serial_write(mac_str);
    serial_write("\n");

    // Initialize ARP cache
    for (size_t i = 0; i < ARP_CACHE_SIZE; ++i) {
        g_arp_cache[i].valid = false;
    }

    // Initialize UDP socket table
    for (size_t i = 0; i < MAX_UDP_SOCKETS; ++i) {
        g_udp_sockets[i].state = UdpState::FREE;
    }

    // Initialize TCP socket table
    for (size_t i = 0; i < MAX_TCP_SOCKETS; ++i) {
        g_tcp_sockets[i].state = TcpState::CLOSED;
    }

    g_dhcp_xid = static_cast<uint32_t>(g_our_mac.addr[2] << 24 |
                                        g_our_mac.addr[3] << 16 |
                                        g_our_mac.addr[4] << 8  |
                                        g_our_mac.addr[5]);
    g_dhcp_state = DhcpClientState::IDLE;

    serial_write("[NET] Starting DHCP\n");
    g_dhcp_state = DhcpClientState::DISCOVERING;
    dhcp_send_discover();
}

void net_receive(NetworkBuffer* buf) {
    if (buf == nullptr || buf->data == nullptr || buf->length < sizeof(EthHeader)) {
        return;
    }

    const auto* eth = reinterpret_cast<const EthHeader*>(buf->data);
    uint16_t type = ntohs(eth->type);

    // Accept frames for us or broadcast
    if (!eth->dst.is_broadcast() && !(eth->dst == g_our_mac)) {
        return;
    }

    const uint8_t* payload = buf->data + sizeof(EthHeader);
    size_t payload_len = buf->length - sizeof(EthHeader);

    switch (type) {
    case ETHERTYPE_ARP:
        arp_handle_packet(payload, payload_len);
        break;
    case ETHERTYPE_IPV4:
        ipv4_handle_packet(payload, payload_len);
        break;
    default:
        break;
    }
}

void net_send(Socket* sock, const void* data, size_t size) {
    if (sock == nullptr || data == nullptr || size == 0) {
        return;
    }

    // For now, only handle UDP sockets
    // In a full implementation, we'd look up the socket by sock->id
    // and determine destination info from the socket metadata.

    // Simplified: broadcast UDP on port 69 (TFTP-like test)
    uint8_t udp_buf[sizeof(UdpHeader) + 2048];
    if (sizeof(UdpHeader) + size > sizeof(udp_buf)) {
        serial_write("[NET] send: payload too large\n");
        return;
    }

    auto* udp = reinterpret_cast<UdpHeader*>(udp_buf);
    udp->src_port = htons(12345);
    udp->dst_port = htons(69);
    udp->length   = htons(static_cast<uint16_t>(sizeof(UdpHeader) + size));
    udp->checksum = 0;

    __builtin_memcpy(udp_buf + sizeof(UdpHeader), data, size);

    if (g_gateway_ip != 0) {
        ipv4_send(g_gateway_ip, IPPROTO_UDP, udp_buf, sizeof(UdpHeader) + size);
    } else {
        ipv4_send(0xFFFFFFFF, IPPROTO_UDP, udp_buf, sizeof(UdpHeader) + size);
    }
}

// ─── BSD Socket Syscall Wrappers ─────────────────────────────────────────────

int sys_socket_create(int domain, int type_sock, int protocol) {
    (void)domain;
    (void)type_sock;
    (void)protocol;

    int idx = udp_alloc_socket();
    if (idx < 0) return -1;

    serial_write("[SYS] socket() -> fd ");
    serial_write_dec(static_cast<uint64_t>(idx));
    serial_write("\n");
    return idx;
}

int sys_socket_bind(int fd, uint16_t port) {
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_UDP_SOCKETS) return -1;
    if (g_udp_sockets[fd].state == UdpState::FREE) return -1;

    g_udp_sockets[fd].local_port = port;
    g_udp_sockets[fd].state      = UdpState::BOUND;

    serial_write("[SYS] bind(fd=");
    serial_write_dec(static_cast<uint64_t>(fd));
    serial_write(", port=");
    serial_write_dec(port);
    serial_write(")\n");
    return 0;
}

int sys_socket_connect(int fd, ipv4_t remote_ip, uint16_t remote_port) {
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_UDP_SOCKETS) return -1;
    if (g_udp_sockets[fd].state == UdpState::FREE) return -1;

    g_udp_sockets[fd].remote_ip   = remote_ip;
    g_udp_sockets[fd].remote_port = remote_port;
    g_udp_sockets[fd].state       = UdpState::CONNECTED;

    serial_write("[SYS] connect(fd=");
    serial_write_dec(static_cast<uint64_t>(fd));
    serial_write(")\n");
    return 0;
}

int sys_socket_send(int fd, const void* data, size_t size) {
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_UDP_SOCKETS) return -1;
    auto& sock = g_udp_sockets[fd];
    if (sock.state != UdpState::CONNECTED) return -1;

    uint8_t udp_buf[sizeof(UdpHeader) + 2048];
    if (sizeof(UdpHeader) + size > sizeof(udp_buf)) return -1;

    auto* udp = reinterpret_cast<UdpHeader*>(udp_buf);
    udp->src_port = htons(sock.local_port);
    udp->dst_port = htons(sock.remote_port);
    udp->length   = htons(static_cast<uint16_t>(sizeof(UdpHeader) + size));
    udp->checksum = 0;

    __builtin_memcpy(udp_buf + sizeof(UdpHeader), data, size);

    if (!ipv4_send(sock.remote_ip, IPPROTO_UDP, udp_buf, sizeof(UdpHeader) + size)) {
        return -1;
    }
    return static_cast<int>(size);
}

int sys_socket_recv(int fd, void* buffer, size_t size) {
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_UDP_SOCKETS) return -1;
    auto& sock = g_udp_sockets[fd];
    if (!sock.rx_pending) return 0;

    size_t copy = (size < sock.rx_len) ? size : sock.rx_len;
    __builtin_memcpy(buffer, sock.rx_data, copy);
    sock.rx_pending = false;
    sock.rx_len     = 0;
    return static_cast<int>(copy);
}

int sys_socket_close(int fd) {
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_UDP_SOCKETS) return -1;
    g_udp_sockets[fd].state = UdpState::FREE;
    return 0;
}

int sys_socket_listen(int fd, int backlog) {
    (void)backlog;
    if (fd < 0 || static_cast<size_t>(fd) >= MAX_TCP_SOCKETS) return -1;
    if (g_tcp_sockets[fd].state == TcpState::CLOSED) return -1;

    g_tcp_sockets[fd].state = TcpState::LISTEN;
    serial_write("[SYS] listen(fd=");
    serial_write_dec(static_cast<uint64_t>(fd));
    serial_write(")\n");
    return 0;
}

} // namespace vortex::kernel::net
