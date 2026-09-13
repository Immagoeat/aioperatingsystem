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
