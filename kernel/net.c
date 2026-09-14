/* net.c - just enough of a network stack to prove real wired
 * connectivity: Ethernet framing, ARP, IPv4, UDP, and a DHCP client
 * that requests and receives a real lease from whatever DHCP server is
 * actually on the network (QEMU's user-mode network backend runs one
 * by default at 10.0.2.2, handing out addresses in 10.0.2.0/24).
 *
 * This is deliberately minimal - no TCP, no routing beyond "everything
 * non-local goes to the DHCP-provided gateway", no fragmentation. It's
 * scoped to what's needed to answer "does this NIC/driver actually
 * work" with a real, independently-checkable answer (a real leased IP
 * address), not to be a general-purpose stack. All multi-byte header
 * fields are big-endian ("network byte order"), per the relevant RFCs
 * (791 for IP, 768 for UDP, 826 for ARP, 2131 for DHCP) - see the
 * htons()/htonl() helpers below. */
#include "kernel.h"

static uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t htonl(uint32_t v) {
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define ntohs htons /* byte-swap is its own inverse */
#define ntohl htonl

static void local_strcpy_bounded(char *dst, const char *src, size_t n) {
	size_t i = 0;
	for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

static uint8_t my_mac[6];
static uint32_t my_ip;      /* 0 until a DHCP lease is obtained */
static uint32_t gateway_ip;
static uint32_t subnet_mask;
static bool have_lease = false;

/* --- Ethernet --- */
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV4 0x0800

struct eth_header {
	uint8_t  dest_mac[6];
	uint8_t  src_mac[6];
	uint16_t ethertype;
} __attribute__((packed));

static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* --- ARP (RFC 826) --- */
struct arp_packet {
	uint16_t htype, ptype;
	uint8_t  hlen, plen;
	uint16_t oper;
	uint8_t  sender_mac[6];
	uint32_t sender_ip;
	uint8_t  target_mac[6];
	uint32_t target_ip;
} __attribute__((packed));

#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY   2

/* --- IPv4 (RFC 791) --- */
struct ipv4_header {
	uint8_t  version_ihl; /* version in top nibble, header length (in 32-bit words) in bottom */
	uint8_t  dscp_ecn;
	uint16_t total_length;
	uint16_t id;
	uint16_t flags_fragment;
	uint8_t  ttl;
	uint8_t  protocol;
	uint16_t checksum;
	uint32_t src_ip;
	uint32_t dest_ip;
} __attribute__((packed));

#define IP_PROTO_UDP 17

/* --- UDP (RFC 768) --- */
struct udp_header {
	uint16_t src_port;
	uint16_t dest_port;
	uint16_t length;
	uint16_t checksum;
} __attribute__((packed));

static uint16_t ip_checksum(const void *data, uint32_t len) {
	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t sum = 0;
	for (uint32_t i = 0; i + 1 < len; i += 2) {
		sum += (uint32_t)((bytes[i] << 8) | bytes[i + 1]);
	}
	if (len & 1) sum += (uint32_t)(bytes[len - 1] << 8);
	while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

#define TX_SCRATCH_SIZE 1024
static uint8_t tx_scratch[TX_SCRATCH_SIZE];

static bool send_ethernet_frame(const uint8_t dest_mac[6], uint16_t ethertype, const void *payload, uint16_t payload_len) {
	if ((uint32_t)sizeof(struct eth_header) + payload_len > TX_SCRATCH_SIZE) return false;

	struct eth_header *eth = (struct eth_header *)tx_scratch;
	memcpy(eth->dest_mac, dest_mac, 6);
	memcpy(eth->src_mac, my_mac, 6);
	eth->ethertype = htons(ethertype);
	memcpy(tx_scratch + sizeof(struct eth_header), payload, payload_len);

	return e1000_send(tx_scratch, (uint16_t)(sizeof(struct eth_header) + payload_len));
}

static bool send_udp_packet(uint32_t dest_ip, const uint8_t dest_mac[6], uint16_t src_port, uint16_t dest_port, const void *data, uint16_t data_len) {
	uint16_t udp_len = (uint16_t)(sizeof(struct udp_header) + data_len);
	uint16_t ip_len = (uint16_t)(sizeof(struct ipv4_header) + udp_len);
	if ((uint32_t)sizeof(struct ipv4_header) + udp_len > TX_SCRATCH_SIZE - sizeof(struct eth_header)) return false;

	static uint8_t packet[TX_SCRATCH_SIZE];
	struct ipv4_header *ip = (struct ipv4_header *)packet;
	ip->version_ihl = 0x45; /* IPv4, 5 words (20 bytes, no options) */
	ip->dscp_ecn = 0;
	ip->total_length = htons(ip_len);
	ip->id = 0;
	ip->flags_fragment = 0;
	ip->ttl = 64;
	ip->protocol = IP_PROTO_UDP;
	ip->checksum = 0;
	ip->src_ip = my_ip; /* already in network order once assigned - see dhcp_apply_lease() */
	ip->dest_ip = dest_ip;
	ip->checksum = htons(ip_checksum(ip, sizeof(struct ipv4_header)));

	struct udp_header *udp = (struct udp_header *)(packet + sizeof(struct ipv4_header));
	udp->src_port = htons(src_port);
	udp->dest_port = htons(dest_port);
	udp->length = htons(udp_len);
	udp->checksum = 0; /* 0 = unused, valid per RFC 768 over IPv4 */

	memcpy(packet + sizeof(struct ipv4_header) + sizeof(struct udp_header), data, data_len);

	return send_ethernet_frame(dest_mac, ETHERTYPE_IPV4, packet, (uint16_t)(sizeof(struct ipv4_header) + udp_len));
}

/* --- DHCP (RFC 2131), just enough of it: DISCOVER -> OFFER -> REQUEST -> ACK --- */
struct dhcp_packet {
	uint8_t  op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint32_t ciaddr, yiaddr, siaddr, giaddr;
	uint8_t  chaddr[16];
	uint8_t  sname[64];
	uint8_t  file[128];
	uint32_t magic_cookie;
	uint8_t  options[64];
} __attribute__((packed));

#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5

static uint32_t dhcp_xid = 0x41555241u; /* "AURA" - fixed transaction ID; this driver never has more than one exchange in flight */

static void build_dhcp_base(struct dhcp_packet *pkt, uint8_t msg_type, uint32_t requested_ip) {
	memset(pkt, 0, sizeof(*pkt));
	pkt->op = DHCP_OP_REQUEST;
	pkt->htype = 1; /* Ethernet */
	pkt->hlen = 6;
	pkt->xid = htonl(dhcp_xid);
	memcpy(pkt->chaddr, my_mac, 6);
	pkt->magic_cookie = htonl(DHCP_MAGIC_COOKIE);

	int i = 0;
	pkt->options[i++] = 53; pkt->options[i++] = 1; pkt->options[i++] = msg_type; /* option 53: DHCP message type */
	if (requested_ip != 0) {
		pkt->options[i++] = 50; pkt->options[i++] = 4; /* option 50: requested IP address */
		memcpy(&pkt->options[i], &requested_ip, 4); i += 4;
	}
	pkt->options[i++] = 55; pkt->options[i++] = 3; pkt->options[i++] = 1; pkt->options[i++] = 3; pkt->options[i++] = 6; /* option 55: parameter request list (subnet mask, router, DNS) */
	pkt->options[i++] = 255; /* end */
}

/* Scans a received DHCP packet's options for a given tag, returning a
 * pointer to its value and writing the length to *out_len, or NULL if
 * not present / the option area is malformed. */
static const uint8_t *dhcp_find_option(const struct dhcp_packet *pkt, uint8_t tag, uint8_t *out_len) {
	const uint8_t *opt = pkt->options;
	const uint8_t *end = pkt->options + sizeof(pkt->options);
	while (opt < end && *opt != 255) {
		if (*opt == 0) { opt++; continue; } /* padding */
		uint8_t this_tag = opt[0];
		if (opt + 2 > end) break;
		uint8_t len = opt[1];
		if (opt + 2 + len > end) break;
		if (this_tag == tag) {
			*out_len = len;
			return opt + 2;
		}
		opt += 2 + len;
	}
	return NULL;
}

#define DHCP_TIMEOUT_TICKS 300 /* ~3 seconds at the 100Hz PIT tick rate */

/* Polls for a UDP packet on port 68 (DHCP client port), up to a
 * timeout, and hands back the parsed DHCP payload if one arrives.
 * Deliberately simple: no real IP/UDP header validation beyond length
 * and protocol/port, since on an isolated QEMU user-net segment the
 * only thing that will ever reply on port 68 is the DHCP server. */
static bool wait_for_dhcp_reply(struct dhcp_packet *out) {
	static uint8_t rx_buf[1600];
	uint32_t deadline = timer_get_ticks() + DHCP_TIMEOUT_TICKS;

	while (timer_get_ticks() < deadline) {
		uint16_t len = e1000_poll_receive(rx_buf, sizeof(rx_buf));
		if (len < sizeof(struct eth_header) + sizeof(struct ipv4_header) + sizeof(struct udp_header)) continue;

		struct eth_header *eth = (struct eth_header *)rx_buf;
		if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) continue;

		struct ipv4_header *ip = (struct ipv4_header *)(rx_buf + sizeof(struct eth_header));
		if (ip->protocol != IP_PROTO_UDP) continue;

		struct udp_header *udp = (struct udp_header *)((uint8_t *)ip + sizeof(struct ipv4_header));
		if (ntohs(udp->dest_port) != 68) continue;

		uint8_t *payload = (uint8_t *)udp + sizeof(struct udp_header);
		uint32_t payload_offset = (uint32_t)(payload - rx_buf);
		if (payload_offset + sizeof(struct dhcp_packet) > len) continue;

		memcpy(out, payload, sizeof(struct dhcp_packet));
		if (ntohl(out->xid) != dhcp_xid) continue;
		if (ntohl(out->magic_cookie) != DHCP_MAGIC_COOKIE) continue;

		return true;
	}
	return false;
}

static void format_ip(uint32_t ip_network_order, char *out /* at least 16 bytes */) {
	const uint8_t *b = (const uint8_t *)&ip_network_order;
	int pos = 0;
	for (int i = 0; i < 4; i++) {
		uint8_t v = b[i];
		if (v >= 100) out[pos++] = (char)('0' + v / 100);
		if (v >= 10) out[pos++] = (char)('0' + (v / 10) % 10);
		out[pos++] = (char)('0' + v % 10);
		if (i < 3) out[pos++] = '.';
	}
	out[pos] = '\0';
}

struct net_status net_status_cache;

bool net_init_and_request_lease(void) {
	memset(&net_status_cache, 0, sizeof(net_status_cache));

	if (!e1000_init()) {
		local_strcpy_bounded(net_status_cache.message, "No e1000 network controller found.", sizeof(net_status_cache.message));
		return false;
	}

	e1000_get_mac(my_mac);
	have_lease = false;
	my_ip = 0;

	/* DISCOVER, broadcast (no IP assigned yet, so everything - source
	 * IP 0.0.0.0, dest 255.255.255.255 - is broadcast at both layers) */
	struct dhcp_packet discover;
	build_dhcp_base(&discover, DHCP_MSG_DISCOVER, 0);
	my_ip = 0;
	if (!send_udp_packet(0xFFFFFFFFu, broadcast_mac, 68, 67, &discover, sizeof(discover))) {
		local_strcpy_bounded(net_status_cache.message, "Failed to send DHCP DISCOVER.", sizeof(net_status_cache.message));
		return false;
	}

	struct dhcp_packet offer;
	if (!wait_for_dhcp_reply(&offer)) {
		local_strcpy_bounded(net_status_cache.message, "No DHCP OFFER received (timed out).", sizeof(net_status_cache.message));
		return false;
	}

	uint8_t opt_len;
	const uint8_t *msg_type = dhcp_find_option(&offer, 53, &opt_len);
	if (!msg_type || *msg_type != DHCP_MSG_OFFER) {
		local_strcpy_bounded(net_status_cache.message, "Unexpected DHCP reply (not an OFFER).", sizeof(net_status_cache.message));
		return false;
	}

	uint32_t offered_ip = offer.yiaddr;
	uint32_t server_ip = offer.siaddr;
	const uint8_t *server_id_opt = dhcp_find_option(&offer, 54, &opt_len);
	if (server_id_opt && opt_len == 4) memcpy(&server_ip, server_id_opt, 4);

	/* REQUEST, still broadcast (per RFC 2131 - we don't have a
	 * confirmed IP yet, so the ACK also gets broadcast back) */
	struct dhcp_packet request;
	build_dhcp_base(&request, DHCP_MSG_REQUEST, offered_ip);
	if (!send_udp_packet(0xFFFFFFFFu, broadcast_mac, 68, 67, &request, sizeof(request))) {
		local_strcpy_bounded(net_status_cache.message, "Failed to send DHCP REQUEST.", sizeof(net_status_cache.message));
		return false;
	}

	struct dhcp_packet ack;
	if (!wait_for_dhcp_reply(&ack)) {
		local_strcpy_bounded(net_status_cache.message, "No DHCP ACK received (timed out).", sizeof(net_status_cache.message));
		return false;
	}

	msg_type = dhcp_find_option(&ack, 53, &opt_len);
	if (!msg_type || *msg_type != DHCP_MSG_ACK) {
		local_strcpy_bounded(net_status_cache.message, "DHCP request was not acknowledged (NAK or unexpected reply).", sizeof(net_status_cache.message));
		return false;
	}

	my_ip = ack.yiaddr;

	const uint8_t *mask_opt = dhcp_find_option(&ack, 1, &opt_len);
	if (mask_opt && opt_len == 4) memcpy(&subnet_mask, mask_opt, 4);

	const uint8_t *router_opt = dhcp_find_option(&ack, 3, &opt_len);
	if (router_opt && opt_len == 4) memcpy(&gateway_ip, router_opt, 4);
	else gateway_ip = server_ip;

	have_lease = true;

	net_status_cache.have_lease = true;
	memcpy(net_status_cache.mac, my_mac, 6);
	format_ip(my_ip, net_status_cache.ip_str);
	format_ip(subnet_mask, net_status_cache.subnet_str);
	format_ip(gateway_ip, net_status_cache.gateway_str);
	local_strcpy_bounded(net_status_cache.message, "DHCP lease obtained.", sizeof(net_status_cache.message));

	return true;
}

bool net_get_status(struct net_status *out) {
	*out = net_status_cache;
	return net_status_cache.have_lease;
}

/* --- ARP (RFC 826), actually used this time: DHCP never needed it
 * (everything there is broadcast), but reaching a specific IP with a
 * unicast Ethernet frame - which every TCP segment is - genuinely
 * requires resolving that IP to a MAC address first. A tiny one-entry
 * cache is all this needs: every real download this stack does talks
 * to exactly one peer at a time (see net_http_get()). */
static uint32_t arp_cache_ip;
static uint8_t arp_cache_mac[6];
static bool arp_cache_valid = false;

#define ARP_TIMEOUT_TICKS 200 /* ~2 seconds */

static bool arp_resolve(uint32_t ip, uint8_t out_mac[6]) {
	if (arp_cache_valid && arp_cache_ip == ip) {
		memcpy(out_mac, arp_cache_mac, 6);
		return true;
	}

	struct arp_packet req;
	req.htype = htons(1); /* Ethernet */
	req.ptype = htons(ETHERTYPE_IPV4);
	req.hlen = 6;
	req.plen = 4;
	req.oper = htons(ARP_OPER_REQUEST);
	memcpy(req.sender_mac, my_mac, 6);
	req.sender_ip = my_ip;
	memset(req.target_mac, 0, 6);
	req.target_ip = ip;

	if (!send_ethernet_frame(broadcast_mac, ETHERTYPE_ARP, &req, sizeof(req))) return false;

	static uint8_t rx_buf[1600];
	uint32_t deadline = timer_get_ticks() + ARP_TIMEOUT_TICKS;
	while (timer_get_ticks() < deadline) {
		uint16_t len = e1000_poll_receive(rx_buf, sizeof(rx_buf));
		if (len < sizeof(struct eth_header) + sizeof(struct arp_packet)) continue;

		struct eth_header *eth = (struct eth_header *)rx_buf;
		if (ntohs(eth->ethertype) != ETHERTYPE_ARP) continue;

		struct arp_packet *reply = (struct arp_packet *)(rx_buf + sizeof(struct eth_header));
		if (ntohs(reply->oper) != ARP_OPER_REPLY) continue;
		if (reply->sender_ip != ip) continue;

		memcpy(arp_cache_mac, reply->sender_mac, 6);
		arp_cache_ip = ip;
		arp_cache_valid = true;
		memcpy(out_mac, arp_cache_mac, 6);
		return true;
	}
	return false;
}

/* Resolves the MAC to actually send a packet to `dest_ip` toward: the
 * dest itself if it's on our own subnet, otherwise the gateway - the
 * one routing decision this stack makes (see the file comment: no
 * general routing table, just "local subnet or bust to the gateway",
 * which is all a single-homed client genuinely needs). */
static bool resolve_next_hop(uint32_t dest_ip, uint8_t out_mac[6]) {
	uint32_t next_hop = ((dest_ip ^ my_ip) & subnet_mask) == 0 ? dest_ip : gateway_ip;
	return arp_resolve(next_hop, out_mac);
}

/* --- TCP (RFC 793), just enough for one HTTP/1.0-style request/response:
 * a real three-way handshake, real sequence/ack tracking, sending one
 * data segment and receiving a (possibly multi-segment) reply, then a
 * real four-way close. No retransmission, no congestion control, no
 * out-of-order reassembly, no simultaneous connections - a single
 * client-initiated, single-outstanding-segment connection, which is
 * what one file download over a LAN/simple path actually needs. A
 * general-purpose TCP stack (the real kind with a retransmit queue and
 * a receive window that can reorder segments) is a much bigger project
 * than "make one HTTP GET work" calls for. */
struct tcp_header {
	uint16_t src_port, dest_port;
	uint32_t seq, ack;
	uint8_t  data_offset; /* top nibble = header length in 32-bit words */
	uint8_t  flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent_ptr;
} __attribute__((packed));

/* TCP_FLAG_* are declared in kernel.h alongside struct tcp_conn, which
 * tls.c also needs direct access to. */

#define IP_PROTO_TCP 6

/* IPv4 checksums (and TCP's) are computed over a "pseudo-header" that
 * isn't actually transmitted - src/dest IP, protocol, and length -
 * standing in for the fact that TCP/UDP don't carry their own IP
 * addresses but still need to detect a segment delivered to the wrong
 * one. Required for TCP (unlike UDP's optional checksum, which this
 * stack already sends as 0/unused). */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip, const void *tcp_data, uint16_t tcp_len) {
	struct { uint32_t src, dest; uint8_t zero, proto; uint16_t len; } __attribute__((packed)) pseudo;
	pseudo.src = src_ip;
	pseudo.dest = dest_ip;
	pseudo.zero = 0;
	pseudo.proto = IP_PROTO_TCP;
	pseudo.len = htons(tcp_len);

	uint32_t sum = 0;
	const uint8_t *p = (const uint8_t *)&pseudo;
	for (uint32_t i = 0; i + 1 < sizeof(pseudo); i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);

	const uint8_t *bytes = (const uint8_t *)tcp_data;
	for (uint32_t i = 0; i + 1 < tcp_len; i += 2) sum += (uint32_t)((bytes[i] << 8) | bytes[i + 1]);
	if (tcp_len & 1) sum += (uint32_t)(bytes[tcp_len - 1] << 8);

	while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

/* struct tcp_conn is declared in kernel.h. */

#define TCP_SCRATCH_SIZE 1600

bool tcp_send_segment(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len) {
	uint16_t tcp_len = (uint16_t)(sizeof(struct tcp_header) + data_len);
	if ((uint32_t)sizeof(struct ipv4_header) + tcp_len > TCP_SCRATCH_SIZE - sizeof(struct eth_header)) return false;

	static uint8_t packet[TCP_SCRATCH_SIZE];
	struct ipv4_header *ip = (struct ipv4_header *)packet;
	ip->version_ihl = 0x45;
	ip->dscp_ecn = 0;
	ip->total_length = htons((uint16_t)(sizeof(struct ipv4_header) + tcp_len));
	ip->id = 0;
	ip->flags_fragment = 0;
	ip->ttl = 64;
	ip->protocol = IP_PROTO_TCP;
	ip->checksum = 0;
	ip->src_ip = my_ip;
	ip->dest_ip = conn->remote_ip;
	ip->checksum = htons(ip_checksum(ip, sizeof(struct ipv4_header)));

	struct tcp_header *tcp = (struct tcp_header *)(packet + sizeof(struct ipv4_header));
	tcp->src_port = htons(conn->local_port);
	tcp->dest_port = htons(conn->remote_port);
	tcp->seq = htonl(conn->local_seq);
	tcp->ack = htonl((flags & TCP_FLAG_ACK) ? conn->remote_seq : 0);
	tcp->data_offset = (uint8_t)((sizeof(struct tcp_header) / 4) << 4);
	tcp->flags = flags;
	tcp->window = htons(4096);
	tcp->checksum = 0;
	tcp->urgent_ptr = 0;
	if (data_len > 0) memcpy((uint8_t *)tcp + sizeof(struct tcp_header), data, data_len);

	tcp->checksum = htons(tcp_checksum(my_ip, conn->remote_ip, tcp, tcp_len));

	return send_ethernet_frame(conn->remote_mac, ETHERTYPE_IPV4, packet, (uint16_t)(sizeof(struct ipv4_header) + tcp_len));
}

#define TCP_TIMEOUT_TICKS 500 /* ~5 seconds */

/* Waits for the next TCP segment from this specific connection's peer
 * (matching remote IP + both ports), handing back its flags/payload.
 * Deliberately doesn't handle out-of-order delivery - segments that
 * don't match the expected sequence number are treated as "keep
 * waiting", which is correct for the simple, low-latency LAN/direct
 * paths this stack is scoped to (see the file comment). */
bool tcp_wait_segment(struct tcp_conn *conn, uint8_t *out_flags, uint8_t *out_data, uint16_t *out_data_len, uint16_t max_data) {
	static uint8_t rx_buf[1600];
	uint32_t deadline = timer_get_ticks() + TCP_TIMEOUT_TICKS;

	while (timer_get_ticks() < deadline) {
		uint16_t len = e1000_poll_receive(rx_buf, sizeof(rx_buf));
		if (len < sizeof(struct eth_header) + sizeof(struct ipv4_header) + sizeof(struct tcp_header)) continue;

		struct eth_header *eth = (struct eth_header *)rx_buf;
		if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) continue;

		struct ipv4_header *ip = (struct ipv4_header *)(rx_buf + sizeof(struct eth_header));
		if (ip->protocol != IP_PROTO_TCP) continue;
		if (ip->src_ip != conn->remote_ip) continue;

		uint8_t ip_header_len = (uint8_t)((ip->version_ihl & 0x0F) * 4);
		struct tcp_header *tcp = (struct tcp_header *)((uint8_t *)ip + ip_header_len);
		if (ntohs(tcp->src_port) != conn->remote_port) continue;
		if (ntohs(tcp->dest_port) != conn->local_port) continue;

		uint32_t seg_seq = ntohl(tcp->seq);
		if (seg_seq != conn->remote_seq) continue; /* not the segment we're expecting next - see comment above */

		uint8_t tcp_header_len = (uint8_t)((tcp->data_offset >> 4) * 4);
		uint16_t ip_total_len = ntohs(ip->total_length);
		uint16_t tcp_payload_len = (uint16_t)(ip_total_len - ip_header_len - tcp_header_len);

		uint8_t *payload = (uint8_t *)tcp + tcp_header_len;
		uint32_t payload_offset = (uint32_t)(payload - rx_buf);
		if (payload_offset + tcp_payload_len > len) continue; /* truncated/corrupt capture */

		*out_flags = tcp->flags;
		uint16_t copy_len = tcp_payload_len < max_data ? tcp_payload_len : max_data;
		if (copy_len > 0) memcpy(out_data, payload, copy_len);
		*out_data_len = copy_len;
		return true;
	}
	return false;
}

bool tcp_connect(struct tcp_conn *conn, uint32_t remote_ip, uint16_t remote_port) {
	if (!resolve_next_hop(remote_ip, conn->remote_mac)) return false;

	conn->remote_ip = remote_ip;
	conn->remote_port = remote_port;
	conn->local_port = (uint16_t)(49152 + (timer_get_ticks() % 16384)); /* an ephemeral port, per the IANA dynamic-port range */
	conn->local_seq = 0x1000; /* arbitrary fixed ISN - no real security requirement here (this isn't defending against off-path spoofing), just needs to be consistent within one connection */

	if (!tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0)) return false;
	conn->local_seq++; /* SYN consumes one sequence number */

	/* the SYN-ACK's sequence number becomes what we expect next, but we
	 * don't know it before receiving it - so tcp_wait_segment()'s normal
	 * "seq must match remote_seq" filter can't apply to this one packet */
	static uint8_t rx_buf[1600];
	uint32_t deadline = timer_get_ticks() + TCP_TIMEOUT_TICKS;
	bool got_synack = false;
	while (timer_get_ticks() < deadline) {
		uint16_t len = e1000_poll_receive(rx_buf, sizeof(rx_buf));
		if (len < sizeof(struct eth_header) + sizeof(struct ipv4_header) + sizeof(struct tcp_header)) continue;
		struct eth_header *eth = (struct eth_header *)rx_buf;
		if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) continue;
		struct ipv4_header *ip = (struct ipv4_header *)(rx_buf + sizeof(struct eth_header));
		if (ip->protocol != IP_PROTO_TCP || ip->src_ip != remote_ip) continue;
		uint8_t ip_header_len = (uint8_t)((ip->version_ihl & 0x0F) * 4);
		struct tcp_header *tcp = (struct tcp_header *)((uint8_t *)ip + ip_header_len);
		if (ntohs(tcp->src_port) != remote_port || ntohs(tcp->dest_port) != conn->local_port) continue;
		if ((tcp->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) != (TCP_FLAG_SYN | TCP_FLAG_ACK)) continue;
		if (ntohl(tcp->ack) != conn->local_seq) continue; /* must ack our SYN specifically */

		conn->remote_seq = ntohl(tcp->seq) + 1; /* their SYN also consumes one sequence number */
		got_synack = true;
		break;
	}
	if (!got_synack) return false;

	if (!tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0)) return false;
	return true;
}

void tcp_close(struct tcp_conn *conn) {
	/* Best-effort: send our FIN and ACK whatever comes back, but don't
	 * loop indefinitely waiting for the peer's own FIN - the caller
	 * already has everything it needs (the HTTP response body) by the
	 * time this runs, so a slow/absent close from the peer shouldn't
	 * block it. */
	tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
	conn->local_seq++;

	uint8_t flags, data[512];
	uint16_t data_len;
	uint32_t deadline = timer_get_ticks() + 100; /* ~1 second, best-effort only */
	while (timer_get_ticks() < deadline) {
		if (tcp_wait_segment(conn, &flags, data, &data_len, sizeof(data))) {
			conn->remote_seq += data_len;
			if (flags & TCP_FLAG_FIN) conn->remote_seq++;
			tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
			if (flags & TCP_FLAG_FIN) return;
		}
	}
}

/* --- minimal HTTP/1.0 GET client ---
 *
 * Just enough to fetch one resource over plain HTTP (no TLS - a real
 * TLS stack, with certificate validation, is a substantial project on
 * its own and well beyond what this download system needs to be
 * genuine rather than fake) and save it to disk. No redirects, no
 * chunked transfer-encoding, no keep-alive - `Connection: close` is
 * requested explicitly so the server closes the connection once the
 * body is sent, which is how this code knows the download is done. */

/* Very small IPv4 "aa.bb.cc.dd" parser - net_http_get()'s only address
 * form (no DNS resolution here; see its own doc comment on why). */
static bool parse_ipv4(const char *s, uint32_t *out) {
	uint8_t octets[4];
	int octet = 0, value = -1;
	for (const char *p = s; ; p++) {
		if (*p >= '0' && *p <= '9') {
			if (value < 0) value = 0;
			value = value * 10 + (*p - '0');
			if (value > 255) return false;
		} else if (*p == '.' || *p == '\0') {
			if (value < 0 || octet >= 4) return false;
			octets[octet++] = (uint8_t)value;
			value = -1;
			if (*p == '\0') break;
		} else {
			return false;
		}
	}
	if (octet != 4) return false;
	*out = (uint32_t)octets[0] | ((uint32_t)octets[1] << 8) | ((uint32_t)octets[2] << 16) | ((uint32_t)octets[3] << 24);
	return true;
}

bool net_http_get(const char *host_ip_str, uint16_t port, const char *path, void *out_body, uint32_t max_body_len, uint32_t *out_body_len, char *out_error, uint32_t out_error_len) {
	*out_body_len = 0;

	if (!have_lease) {
		local_strcpy_bounded(out_error, "Not connected (no DHCP lease) - run netconnect first.", out_error_len);
		return false;
	}

	uint32_t host_ip;
	if (!parse_ipv4(host_ip_str, &host_ip)) {
		/* Honest limitation, not a bug: there's no DNS client here (RFC
		 * 1035 resolution is real additional protocol work, out of
		 * scope for "just enough to prove downloads work"), so only a
		 * literal dotted IPv4 address is accepted as a host - a real
		 * hostname must be resolved to an IP by other means first. */
		local_strcpy_bounded(out_error, "Only a literal IPv4 address is supported (no DNS client) - resolve the hostname first.", out_error_len);
		return false;
	}

	struct tcp_conn conn;
	if (!tcp_connect(&conn, host_ip, port)) {
		local_strcpy_bounded(out_error, "TCP connection failed (no route, no response, or connection refused).", out_error_len);
		return false;
	}

	char request[512];
	int req_len = 0;
	{
		const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host_ip_str, "\r\nConnection: close\r\nUser-Agent: auroraOS\r\n\r\n" };
		for (int i = 0; i < 5; i++) {
			for (const char *p = parts[i]; *p && req_len < (int)sizeof(request) - 1; p++) request[req_len++] = *p;
		}
	}

	if (!tcp_send_segment(&conn, TCP_FLAG_PSH | TCP_FLAG_ACK, request, (uint16_t)req_len)) {
		local_strcpy_bounded(out_error, "Failed to send HTTP request.", out_error_len);
		tcp_close(&conn);
		return false;
	}
	conn.local_seq += (uint32_t)req_len;

	/* Read segments until the peer closes (FIN) or a real timeout hits;
	 * ack each one as it arrives so the peer's own timers don't fire. */
	static uint8_t response[65536];
	uint32_t response_len = 0;
	uint8_t seg_data[1460];
	bool got_fin = false;
	uint32_t idle_deadline = timer_get_ticks() + TCP_TIMEOUT_TICKS;

	while (timer_get_ticks() < idle_deadline && !got_fin) {
		uint8_t flags;
		uint16_t seg_len;
		if (!tcp_wait_segment(&conn, &flags, seg_data, &seg_len, sizeof(seg_data))) continue;

		idle_deadline = timer_get_ticks() + TCP_TIMEOUT_TICKS; /* got something real - extend the timeout */
		conn.remote_seq += seg_len;
		if (flags & TCP_FLAG_FIN) { conn.remote_seq++; got_fin = true; }

		if (seg_len > 0 && response_len + seg_len <= sizeof(response)) {
			memcpy(response + response_len, seg_data, seg_len);
			response_len += seg_len;
		}

		tcp_send_segment(&conn, TCP_FLAG_ACK, NULL, 0);
	}

	tcp_close(&conn);

	if (response_len == 0) {
		local_strcpy_bounded(out_error, "No response received (timed out).", out_error_len);
		return false;
	}

	/* Split the status line + headers from the body at the first blank
	 * line (CRLF CRLF), per RFC 7230 - this is the one piece of real
	 * HTTP framing this client needs to understand to hand back just
	 * the body, not the whole raw response. */
	uint32_t body_start = 0;
	for (uint32_t i = 0; i + 3 < response_len; i++) {
		if (response[i] == '\r' && response[i + 1] == '\n' && response[i + 2] == '\r' && response[i + 3] == '\n') {
			body_start = i + 4;
			break;
		}
	}
	if (body_start == 0) {
		local_strcpy_bounded(out_error, "Malformed HTTP response (no header/body separator found).", out_error_len);
		return false;
	}

	/* A real (if minimal) status-line check: refuse to save an error
	 * page's body as if it were the requested file. No memcmp() in this
	 * kernel's minimal libc, so just compare the few bytes by hand. */
	bool starts_with_http = response_len >= 12;
	if (starts_with_http) {
		static const char prefix[7] = "HTTP/1.";
		for (int i = 0; i < 7; i++) {
			if (response[i] != (uint8_t)prefix[i]) { starts_with_http = false; break; }
		}
	}
	if (!starts_with_http || response[9] != '2') {
		char status_line[64];
		uint32_t sl_len = 0;
		while (sl_len < response_len && response[sl_len] != '\r' && sl_len < sizeof(status_line) - 1) {
			status_line[sl_len] = (char)response[sl_len];
			sl_len++;
		}
		status_line[sl_len] = '\0';
		local_strcpy_bounded(out_error, status_line[0] ? status_line : "Non-2xx HTTP response.", out_error_len);
		return false;
	}

	uint32_t body_len = response_len - body_start;
	if (body_len > max_body_len) body_len = max_body_len;
	memcpy(out_body, response + body_start, body_len);
	*out_body_len = body_len;
	return true;
}
