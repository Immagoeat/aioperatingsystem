/* tls.c - a real (if narrowly scoped) TLS 1.2 client, built on the
 * primitives verified independently in bignum.c/rsa.c/aes.c/hmac.c/
 * ecc.c and the certificate parsing in x509.c.
 *
 * Two cipher suites, both AES-128-CBC + HMAC-SHA256 for the actual
 * record protection (no Galois-field authenticated-encryption math
 * needed - AES-GCM is out of scope, same reasoning as before), but
 * now with a real choice of key exchange:
 *
 *   - TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256 (P-256, RFC 4492/8422):
 *     offered first/preferred, and what most real HTTPS sites
 *     actually negotiate today. Forward-secret ephemeral key exchange,
 *     authenticated by the server's RSA signature over its ephemeral
 *     key (checked - see rsa_verify_pkcs1_sha256() in rsa.c).
 *   - TLS_RSA_WITH_AES_128_CBC_SHA256: the original, simpler suite
 *     this client started with - no elliptic-curve math, no forward
 *     secrecy, offered second as a fallback for servers that don't
 *     support ECDHE (older infrastructure, test/embedded targets).
 *
 * Either way this client correctly fails (not silently downgrades to
 * something insecure) against a server that supports neither.
 *
 * Certificate handling: extracts a real RSA public key from the
 * server's certificate (x509.c) and checks the certificate's subject
 * name against the hostname being connected to, but does NOT validate
 * the certificate's signature against a trusted root CA - there is no
 * root store here. This is real, honestly-scoped protection against
 * passive eavesdropping (the traffic is genuinely encrypted, and a
 * passive observer cannot read it), but weaker than a real browser's
 * TLS stack against an active attacker who can present their own
 * (unsigned-by-any-trusted-CA, but structurally valid) certificate for
 * the same hostname. Stated plainly rather than silently pretended
 * away - see x509.c's file comment for the same point in more detail. */
#include "kernel.h"

static void local_strcpy_bounded_tls(char *dst, const char *src, uint32_t n) {
	uint32_t i = 0;
	for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

/* Decodes a TLS alert description byte (RFC 5246 SS7.2) into text, so a
 * server-sent alert shows up as an actual reason instead of just
 * "connection closed" or a generic "didn't get what I expected"
 * message at whatever recv call happened to receive it. */
static const char *tls_alert_name(uint8_t description) {
	switch (description) {
		case 0: return "close_notify";
		case 10: return "unexpected_message";
		case 20: return "bad_record_mac";
		case 21: return "decryption_failed";
		case 22: return "record_overflow";
		case 30: return "decompression_failure";
		case 40: return "handshake_failure";
		case 41: return "no_certificate";
		case 42: return "bad_certificate";
		case 43: return "unsupported_certificate";
		case 44: return "certificate_revoked";
		case 45: return "certificate_expired";
		case 46: return "certificate_unknown";
		case 47: return "illegal_parameter";
		case 48: return "unknown_ca";
		case 49: return "access_denied";
		case 50: return "decode_error";
		case 51: return "decrypt_error";
		case 60: return "export_restriction";
		case 70: return "protocol_version";
		case 71: return "insufficient_security";
		case 80: return "internal_error";
		case 90: return "user_canceled";
		case 100: return "no_renegotiation";
		case 110: return "unsupported_extension";
		default: return "unknown";
	}
}

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20
#define TLS_CONTENT_ALERT              21
#define TLS_CONTENT_HANDSHAKE          22
#define TLS_CONTENT_APPLICATION_DATA   23

#define TLS_HANDSHAKE_CLIENT_HELLO         1
#define TLS_HANDSHAKE_SERVER_HELLO         2
#define TLS_HANDSHAKE_CERTIFICATE          11
#define TLS_HANDSHAKE_SERVER_KEY_EXCHANGE  12
#define TLS_HANDSHAKE_SERVER_HELLO_DONE    14
#define TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE  16
#define TLS_HANDSHAKE_FINISHED             20

#define TLS_VERSION_1_2 0x0303

/* Two cipher suites are offered, both ending up at the same AES-128-
 * CBC + HMAC-SHA256 record protection - see this file's top comment
 * for why AES-CBC/SHA256 rather than a GCM/AEAD suite. The difference
 * is entirely in key exchange: TLS_RSA_* (below) sends the pre-master
 * secret RSA-encrypted directly to the server (works with any RSA
 * cert, but no forward secrecy, and a growing number of real servers
 * have dropped support for it); TLS_ECDHE_RSA_* (P-256, RFC 4492/8422)
 * does an ephemeral Diffie-Hellman exchange on the P-256 curve
 * instead, authenticated by the server's RSA signature over its
 * ephemeral key (see ecc.c and rsa_verify_pkcs1_sha256() in rsa.c) -
 * this is what most real HTTPS sites actually negotiate today, and is
 * offered first/preferred (ClientHello lists it before the RSA suite,
 * and a server picks whichever of the offered suites it prefers - in
 * practice this means most real servers will now pick ECDHE). */
#define TLS_CIPHER_ECDHE_RSA_AES128_CBC_SHA256 0xC027
#define TLS_CIPHER_RSA_AES128_CBC_SHA256 0x003C

struct tls_conn {
	struct tcp_conn tcp;

	uint8_t client_random[32];
	uint8_t server_random[32];
	uint8_t master_secret[48];

	uint8_t client_mac_key[32];  /* HMAC-SHA256 key, client->server */
	uint8_t server_mac_key[32];  /* HMAC-SHA256 key, server->client */
	uint8_t client_write_key[16]; /* AES-128 key, client->server */
	uint8_t server_write_key[16]; /* AES-128 key, server->client */

	uint64_t write_seq_num; /* TLS record sequence number, client->server (part of the MAC input, not on the wire) */
	uint64_t read_seq_num;  /* server->client */

	/* running SHA-256 over the full handshake message transcript (all
	 * Handshake-layer messages, ChangeCipherSpec/alerts excluded per
	 * RFC 5246 7.4.9), needed for both sides' Finished message
	 * verification - accumulated as a growing buffer rather than a
	 * true streaming hash, since the whole transcript is well under a
	 * few KB and this avoids needing an incremental SHA-256 API */
	uint8_t transcript[8192];
	uint32_t transcript_len;

	bool handshake_done;
};

/* --- record layer plumbing --- */

static void transcript_append(struct tls_conn *tls, const uint8_t *data, uint32_t len) {
	if (tls->transcript_len + len > sizeof(tls->transcript)) return; /* handshake too large for this client's fixed buffer - a real failure, surfaced as a Finished-verification mismatch downstream rather than silently corrupting the transcript */
	memcpy(tls->transcript + tls->transcript_len, data, len);
	tls->transcript_len += len;
}

/* Sends one TLS record. `content_type` is a TLS_CONTENT_* value;
 * before the handshake's ChangeCipherSpec, records go out in the
 * clear (encrypt=false); after it, every record (including the
 * client's Finished message) must be MAC'd and AES-CBC encrypted, per
 * TLS 1.2's MAC-then-encrypt construction for CBC suites. */
static bool tls_send_record(struct tls_conn *tls, uint8_t content_type, const uint8_t *payload, uint32_t payload_len, bool encrypt) {
	static uint8_t record[2048];

	/* This client only ever sends small messages (handshake messages,
	 * one HTTP request) - 2048 bytes of headroom is genuinely enough
	 * for real usage, but check explicitly and fail safely rather than
	 * silently overflow record[]/to_encrypt[]/ciphertext[] if a caller
	 * ever asks for more than that. */
	if (payload_len > 1900) return false;

	if (!encrypt) {
		record[0] = content_type;
		record[1] = (uint8_t)(TLS_VERSION_1_2 >> 8);
		record[2] = (uint8_t)(TLS_VERSION_1_2);
		record[3] = (uint8_t)(payload_len >> 8);
		record[4] = (uint8_t)(payload_len);
		memcpy(record + 5, payload, payload_len);
		uint16_t wire_len = (uint16_t)(5 + payload_len);
		if (!tcp_send_segment(&tls->tcp, TCP_FLAG_PSH | TCP_FLAG_ACK, record, wire_len)) return false;
		/* tcp_send_segment() doesn't advance the connection's sequence
		 * number itself (see net_http_get() in net.c, which does the same
		 * by hand after its one send) - every byte actually put on the
		 * wire, TLS record header included, has to count here or the next
		 * record we send reuses a sequence number the peer has already
		 * seen and gets treated as a retransmission/duplicate. */
		tls->tcp.local_seq += wire_len;
		return true;
	}

	/* MAC input: seq_num(8) || type(1) || version(2) || length(2) || payload - RFC 5246 6.2.3.1 */
	uint8_t mac_input[8 + 5 + 2048];
	for (int i = 0; i < 8; i++) mac_input[i] = (uint8_t)(tls->write_seq_num >> (56 - i * 8));
	mac_input[8] = content_type;
	mac_input[9] = (uint8_t)(TLS_VERSION_1_2 >> 8);
	mac_input[10] = (uint8_t)(TLS_VERSION_1_2);
	mac_input[11] = (uint8_t)(payload_len >> 8);
	mac_input[12] = (uint8_t)(payload_len);
	memcpy(mac_input + 13, payload, payload_len);

	uint8_t mac[32];
	hmac_sha256(tls->client_mac_key, sizeof(tls->client_mac_key), mac_input, 13 + payload_len, mac);

	/* plaintext-to-encrypt = payload || MAC || padding (PKCS#7-style:
	 * pad byte value == number of padding bytes, per RFC 5246 6.2.3.2) */
	static uint8_t to_encrypt[2048];
	memcpy(to_encrypt, payload, payload_len);
	memcpy(to_encrypt + payload_len, mac, sizeof(mac));
	uint32_t unpadded_len = payload_len + sizeof(mac);
	uint32_t pad_len = 16 - (unpadded_len % 16);
	if (pad_len == 0) pad_len = 16; /* CBC always adds at least one full padding block's worth if already aligned - RFC 5246 requires at least 1 byte of padding */
	for (uint32_t i = 0; i < pad_len; i++) to_encrypt[unpadded_len + i] = (uint8_t)(pad_len - 1);
	uint32_t plaintext_len = unpadded_len + pad_len;

	/* explicit per-record IV (TLS 1.1+): a real, unpredictable-enough
	 * value prepended to the ciphertext - reuses the same PIT+RTC
	 * mixing auth.c/rsa.c already use, since there's no hardware RNG */
	uint8_t iv[16];
	struct rtc_time t;
	rtc_get_time(&t);
	uint32_t seed = timer_get_ticks() ^ ((uint32_t)t.hours << 16) ^ ((uint32_t)t.minutes << 8) ^ t.seconds ^ (uint32_t)tls->write_seq_num ^ 0x5A5A5A5Au;
	for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u + (uint32_t)i; iv[i] = (uint8_t)(seed >> 16); }

	static uint8_t ciphertext[2048];
	aes128_cbc_encrypt(tls->client_write_key, iv, to_encrypt, plaintext_len, ciphertext);

	uint32_t record_payload_len = 16 + plaintext_len; /* IV || ciphertext */
	record[0] = content_type;
	record[1] = (uint8_t)(TLS_VERSION_1_2 >> 8);
	record[2] = (uint8_t)(TLS_VERSION_1_2);
	record[3] = (uint8_t)(record_payload_len >> 8);
	record[4] = (uint8_t)(record_payload_len);
	memcpy(record + 5, iv, 16);
	memcpy(record + 5 + 16, ciphertext, plaintext_len);

	tls->write_seq_num++;
	uint16_t wire_len = (uint16_t)(5 + record_payload_len);
	if (!tcp_send_segment(&tls->tcp, TCP_FLAG_PSH | TCP_FLAG_ACK, record, wire_len)) return false;
	tls->tcp.local_seq += wire_len; /* see the matching comment in the unencrypted path above */
	return true;
}

#define TLS_RECV_TIMEOUT_TICKS 500

/* A TLS record can be up to 2^14 (16384) bytes of plaintext per RFC
 * 5246 6.2.1, and the ciphertext form (IV + padded MAC'd payload) can
 * run slightly larger still - real HTTPS response bodies routinely
 * produce records well over one TCP segment's worth of data (~1460
 * bytes), so unlike this client's own small outgoing messages
 * (handshake messages, one HTTP request - see tls_send_record()),
 * *incoming* records genuinely need real reassembly across multiple
 * TCP segments, not an assumption that one segment holds one record. */
#define TLS_MAX_RECORD_CIPHERTEXT (16384 + 16 + 32 + 16) /* payload + IV + MAC + max padding */
#define TLS_STREAM_BUF_SIZE (5 + TLS_MAX_RECORD_CIPHERTEXT)

/* A simple reassembly buffer over the raw TCP byte stream: segments
 * get appended as they arrive, and tls_recv_record() only consumes
 * bytes once a complete record (header + declared length) is present -
 * the same real stream-reassembly approach proven against a live
 * server (badssl.com) in a standalone test harness before this was
 * wired into the kernel's own TCP layer. */
struct tls_stream {
	uint8_t buf[TLS_STREAM_BUF_SIZE];
	uint32_t len;
};

static bool tls_stream_fill(struct tls_conn *tls, struct tls_stream *stream, uint32_t need, uint32_t deadline) {
	while (stream->len < need) {
		if (timer_get_ticks() >= deadline) return false;
		uint8_t flags;
		uint16_t seg_len;
		uint8_t seg[1600];
		if (!tcp_wait_segment(&tls->tcp, &flags, seg, &seg_len, sizeof(seg))) continue;
		tls->tcp.remote_seq += seg_len;
		tcp_send_segment(&tls->tcp, TCP_FLAG_ACK, NULL, 0);
		if (seg_len == 0) continue;
		if (stream->len + seg_len > sizeof(stream->buf)) return false; /* a genuinely oversized/malformed record - refuse rather than overflow */
		memcpy(stream->buf + stream->len, seg, seg_len);
		stream->len += seg_len;
	}
	return true;
}

static void tls_stream_consume(struct tls_stream *stream, uint32_t n) {
	memmove(stream->buf, stream->buf + n, stream->len - n);
	stream->len -= n;
}

/* Reads exactly one TLS record from the connection, decrypting and
 * MAC-checking it first if `encrypted` is true (i.e. after the
 * server's ChangeCipherSpec). One persistent stream buffer per
 * connection (not reset between calls) so a record that arrives split
 * across TCP segments - or multiple records that arrive coalesced in
 * one segment - are both handled correctly. */
static bool tls_recv_record(struct tls_conn *tls, uint8_t *out_content_type, uint8_t *out_payload, uint32_t *out_payload_len, uint32_t max_payload, bool encrypted) {
	static struct tls_stream stream;
	uint32_t deadline = timer_get_ticks() + TLS_RECV_TIMEOUT_TICKS;

	if (!tls_stream_fill(tls, &stream, 5, deadline)) return false;
	uint8_t content_type = stream.buf[0];
	uint8_t version_hi = stream.buf[1], version_lo = stream.buf[2];
	uint16_t record_len = (uint16_t)((stream.buf[3] << 8) | stream.buf[4]);
	if (!tls_stream_fill(tls, &stream, (uint32_t)5 + record_len, deadline)) return false;

	const uint8_t *record_payload = stream.buf + 5;

	if (!encrypted) {
		if (record_len > max_payload) { tls_stream_consume(&stream, 5 + record_len); return false; }
		memcpy(out_payload, record_payload, record_len);
		*out_payload_len = record_len;
		*out_content_type = content_type;
		tls_stream_consume(&stream, 5 + record_len);
		return true;
	}

	if (record_len < 16 + 16) { tls_stream_consume(&stream, 5 + record_len); return false; } /* must have at least an IV and one cipher block */
	const uint8_t *iv = record_payload;
	const uint8_t *ciphertext = record_payload + 16;
	uint32_t ciphertext_len = record_len - 16;
	if (ciphertext_len % 16 != 0) { tls_stream_consume(&stream, 5 + record_len); return false; }

	static uint8_t decrypted[TLS_MAX_RECORD_CIPHERTEXT];
	aes128_cbc_decrypt(tls->server_write_key, iv, ciphertext, ciphertext_len, decrypted);

	uint8_t pad_len = decrypted[ciphertext_len - 1];
	if ((uint32_t)pad_len + 1 > ciphertext_len) { tls_stream_consume(&stream, 5 + record_len); return false; }
	uint32_t unpadded_len = ciphertext_len - pad_len - 1;
	if (unpadded_len < 32) { tls_stream_consume(&stream, 5 + record_len); return false; } /* must have at least room for the MAC */

	uint32_t plaintext_len = unpadded_len - 32;
	const uint8_t *plaintext = decrypted;
	const uint8_t *received_mac = decrypted + plaintext_len;

	uint8_t mac_input[8 + 5 + TLS_MAX_RECORD_CIPHERTEXT];
	for (int i = 0; i < 8; i++) mac_input[i] = (uint8_t)(tls->read_seq_num >> (56 - i * 8));
	mac_input[8] = content_type;
	mac_input[9] = version_hi;
	mac_input[10] = version_lo;
	mac_input[11] = (uint8_t)(plaintext_len >> 8);
	mac_input[12] = (uint8_t)(plaintext_len);
	memcpy(mac_input + 13, plaintext, plaintext_len);

	uint8_t computed_mac[32];
	hmac_sha256(tls->server_mac_key, sizeof(tls->server_mac_key), mac_input, 13 + plaintext_len, computed_mac);

	uint8_t diff = 0;
	for (int i = 0; i < 32; i++) diff |= (uint8_t)(computed_mac[i] ^ received_mac[i]);

	tls->read_seq_num++;
	tls_stream_consume(&stream, 5 + record_len);

	if (diff != 0) return false; /* MAC mismatch - tampered or wrong key; refuse rather than trust unauthenticated data */

	if (plaintext_len > max_payload) return false;
	memcpy(out_payload, plaintext, plaintext_len);
	*out_payload_len = plaintext_len;
	*out_content_type = content_type;
	return true;
}

static void fill_random(uint8_t *out, uint32_t len, uint32_t extra_mix) {
	struct rtc_time t;
	rtc_get_time(&t);
	uint32_t seed = timer_get_ticks() ^ ((uint32_t)t.hours << 16) ^ ((uint32_t)t.minutes << 8) ^ t.seconds ^ extra_mix;
	for (uint32_t i = 0; i < len; i++) {
		seed = seed * 1103515245u + 12345u + i;
		out[i] = (uint8_t)(seed >> 16);
	}
}

bool tls_connect(struct tls_ctx *ctx, uint32_t remote_ip, uint16_t port, const char *hostname, char *out_error, uint32_t out_error_len) {
	static struct tls_conn tls;
	memset(&tls, 0, sizeof(tls));

	if (!tcp_connect(&tls.tcp, remote_ip, port)) {
		local_strcpy_bounded_tls(out_error, "TCP connection failed.", out_error_len);
		return false;
	}

	/* --- ClientHello --- */
	fill_random(tls.client_random, 32, 0x11111111u);

	uint8_t hello[512];
	uint32_t p = 0;
	hello[p++] = (uint8_t)(TLS_VERSION_1_2 >> 8); hello[p++] = (uint8_t)(TLS_VERSION_1_2);
	memcpy(hello + p, tls.client_random, 32); p += 32;
	hello[p++] = 0x00; /* session ID length: 0, no session resumption */
	hello[p++] = 0x00; hello[p++] = 0x04; /* cipher suites length: 2 bytes each, 2 suites */
	hello[p++] = (uint8_t)(TLS_CIPHER_ECDHE_RSA_AES128_CBC_SHA256 >> 8);
	hello[p++] = (uint8_t)(TLS_CIPHER_ECDHE_RSA_AES128_CBC_SHA256);
	hello[p++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256 >> 8);
	hello[p++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256);
	hello[p++] = 0x01; /* compression methods length: 1 */
	hello[p++] = 0x00; /* null compression */

	hello[p++] = 0x00; hello[p++] = 0x00; /* extensions length placeholder, filled below */
	uint32_t ext_len_pos = p - 2;

	/* Server Name Indication (SNI) extension - many real servers (name-
	 * based virtual hosting) require this to select the right
	 * certificate at all */
	uint32_t hostname_len = (uint32_t)strlen(hostname);
	uint32_t sni_len = 5 + hostname_len;
	hello[p++] = 0x00; hello[p++] = 0x00; /* extension type: server_name */
	hello[p++] = (uint8_t)((sni_len) >> 8); hello[p++] = (uint8_t)(sni_len); /* extension data length */
	hello[p++] = (uint8_t)((hostname_len + 3) >> 8); hello[p++] = (uint8_t)(hostname_len + 3); /* server_name_list length */
	hello[p++] = 0x00; /* name type: host_name */
	hello[p++] = (uint8_t)(hostname_len >> 8); hello[p++] = (uint8_t)(hostname_len);
	memcpy(hello + p, hostname, hostname_len); p += hostname_len;

	/* elliptic_curves/supported_groups (RFC 4492/8422) - required for a
	 * server to ever pick an ECDHE suite at all; P-256 (named curve 23)
	 * is the only one this client speaks (see ecc.c). Wire format
	 * confirmed against a real ClientHello (openssl s_client -msg). */
	hello[p++] = 0x00; hello[p++] = 0x0A; /* extension type: elliptic_curves */
	hello[p++] = 0x00; hello[p++] = 0x04; /* extension data length: 4 */
	hello[p++] = 0x00; hello[p++] = 0x02; /* named curve list length: 2 */
	hello[p++] = 0x00; hello[p++] = 0x17; /* secp256r1 (P-256) */

	/* ec_point_formats (RFC 4492) - declares this client only accepts
	 * the uncompressed point format, the only one ecc.c handles. */
	hello[p++] = 0x00; hello[p++] = 0x0B; /* extension type: ec_point_formats */
	hello[p++] = 0x00; hello[p++] = 0x02; /* extension data length: 2 */
	hello[p++] = 0x01; /* point format list length: 1 */
	hello[p++] = 0x00; /* uncompressed */

	/* signature_algorithms (RFC 5246 7.4.1.4.1) - without this, a
	 * server is entitled to assume the legacy default (SHA-1+RSA) when
	 * choosing how to sign ServerKeyExchange, which is exactly what a
	 * real server (httpbin.org) was observed doing before this
	 * extension was added - only SHA256+RSA is ever offered here since
	 * that's the only signature this client can check (see
	 * rsa_verify_pkcs1_sha256() in rsa.c). */
	hello[p++] = 0x00; hello[p++] = 0x0D; /* extension type: signature_algorithms */
	hello[p++] = 0x00; hello[p++] = 0x04; /* extension data length: 4 */
	hello[p++] = 0x00; hello[p++] = 0x02; /* supported_signature_algorithms list length: 2 */
	hello[p++] = 0x04; hello[p++] = 0x01; /* hash=SHA256(4), signature=RSA(1) */

	uint32_t extensions_len = p - ext_len_pos - 2;
	hello[ext_len_pos] = (uint8_t)(extensions_len >> 8);
	hello[ext_len_pos + 1] = (uint8_t)(extensions_len);

	uint8_t handshake_msg[520];
	handshake_msg[0] = TLS_HANDSHAKE_CLIENT_HELLO;
	handshake_msg[1] = (uint8_t)(p >> 16); handshake_msg[2] = (uint8_t)(p >> 8); handshake_msg[3] = (uint8_t)(p);
	memcpy(handshake_msg + 4, hello, p);
	uint32_t handshake_msg_len = 4 + p;

	transcript_append(&tls, handshake_msg, handshake_msg_len);
	if (!tls_send_record(&tls, TLS_CONTENT_HANDSHAKE, handshake_msg, handshake_msg_len, false)) {
		local_strcpy_bounded_tls(out_error, "Failed to send ClientHello.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* --- ServerHello, Certificate, ServerHelloDone --- these commonly
	 * arrive as several handshake messages, sometimes coalesced into
	 * one TLS record and sometimes not; read records until
	 * ServerHelloDone is seen, feeding each into the transcript and
	 * parsing out what's needed as it goes. */
	static uint8_t record_payload[16384];
	uint32_t record_payload_len;
	uint8_t content_type;

	struct x509_cert server_cert;
	bool got_server_hello = false, got_certificate = false, got_server_hello_done = false;
	bool got_server_key_exchange = false;
	uint16_t chosen_cipher_suite = 0;
	uint8_t server_ecdhe_pubkey[65]; /* uncompressed SEC1 point, only filled/used for the ECDHE suite */

	/* Handshake messages can span multiple records or several messages
	 * can share one record; accumulate into a flight buffer and walk
	 * it message-by-message. Sized generously for a real certificate
	 * chain (a leaf cert plus one or two intermediates, each a couple
	 * KB), not just a single small certificate. */
	static uint8_t flight[16384];
	uint32_t flight_len = 0;

	while (!got_server_hello_done) {
		if (!tls_recv_record(&tls, &content_type, record_payload, &record_payload_len, sizeof(record_payload), false)) {
			local_strcpy_bounded_tls(out_error, "Timed out waiting for the server's handshake response.", out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}
		if (content_type == TLS_CONTENT_ALERT) {
			char msg[96];
			const char *parts[] = { "Server sent alert: ", record_payload_len >= 2 ? tls_alert_name(record_payload[1]) : "malformed alert" };
			uint32_t mp = 0;
			for (int pi = 0; pi < 2; pi++) for (const char *p = parts[pi]; *p && mp < sizeof(msg) - 1; p++) msg[mp++] = *p;
			msg[mp] = '\0';
			local_strcpy_bounded_tls(out_error, msg, out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}
		if (content_type != TLS_CONTENT_HANDSHAKE) continue;
		if (flight_len + record_payload_len > sizeof(flight)) {
			local_strcpy_bounded_tls(out_error, "Server handshake too large for this client.", out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}
		memcpy(flight + flight_len, record_payload, record_payload_len);
		flight_len += record_payload_len;

		uint32_t fp = 0;
		while (fp + 4 <= flight_len) {
			uint8_t msg_type = flight[fp];
			uint32_t msg_len = ((uint32_t)flight[fp + 1] << 16) | ((uint32_t)flight[fp + 2] << 8) | flight[fp + 3];
			if (fp + 4 + msg_len > flight_len) break; /* incomplete message - wait for more records */

			transcript_append(&tls, flight + fp, 4 + msg_len);

			const uint8_t *body = flight + fp + 4;
			if (msg_type == TLS_HANDSHAKE_SERVER_HELLO) {
				if (msg_len < 2 + 32 + 1) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerHello.", out_error_len); return false; }
				memcpy(tls.server_random, body + 2, 32);

				/* version(2) || random(32) || session_id_len(1) ||
				 * session_id[...] || cipher_suite(2) || ... - walk past
				 * the variable-length session ID to reach the cipher
				 * suite the server actually chose (previously never
				 * checked at all - this client only ever offered one
				 * suite before ECDHE support existed, so it didn't
				 * matter yet). */
				uint32_t sess_id_len = body[34];
				uint32_t cs_pos = 35 + sess_id_len;
				if (cs_pos + 2 > msg_len) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerHello.", out_error_len); return false; }
				chosen_cipher_suite = (uint16_t)(((uint16_t)body[cs_pos] << 8) | body[cs_pos + 1]);
				if (chosen_cipher_suite != TLS_CIPHER_ECDHE_RSA_AES128_CBC_SHA256 && chosen_cipher_suite != TLS_CIPHER_RSA_AES128_CBC_SHA256) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Server chose a cipher suite this client didn't offer.", out_error_len);
					return false;
				}
				got_server_hello = true;
			} else if (msg_type == TLS_HANDSHAKE_SERVER_KEY_EXCHANGE) {
				/* Only sent for the ECDHE suite - RFC 4492/8422 wire
				 * format: curve_type(1) || named_curve(2) ||
				 * pubkey_len(1) || pubkey[...] ||
				 * SignatureAndHashAlgorithm(2) || sig_len(2) ||
				 * signature[...]. Verified against a real capture
				 * (openssl s_client -msg against a live ECDHE-RSA
				 * server) before this parsing was written. */
				if (msg_len < 1 + 2 + 1) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerKeyExchange.", out_error_len); return false; }
				uint8_t curve_type = body[0];
				uint16_t named_curve = (uint16_t)(((uint16_t)body[1] << 8) | body[2]);
				if (curve_type != 3 || named_curve != 23) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Server's ServerKeyExchange uses an unsupported curve (only P-256 is supported).", out_error_len);
					return false;
				}
				uint8_t pubkey_len = body[3];
				if (pubkey_len != 65 || 4 + 65u > msg_len) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Malformed ECDHE public key in ServerKeyExchange.", out_error_len);
					return false;
				}
				memcpy(server_ecdhe_pubkey, body + 4, 65);

				uint32_t sig_algo_pos = 4 + 65;
				if (sig_algo_pos + 4 > msg_len) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerKeyExchange signature header.", out_error_len); return false; }
				uint8_t hash_algo = body[sig_algo_pos], sig_algo = body[sig_algo_pos + 1];
				if (hash_algo != 4 || sig_algo != 1) { /* 4=SHA256, 1=RSA - the only combination this client checks against */
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Server signed ServerKeyExchange with an unsupported hash/signature algorithm.", out_error_len);
					return false;
				}
				uint32_t sig_len = (uint32_t)(((uint32_t)body[sig_algo_pos + 2] << 8) | body[sig_algo_pos + 3]);
				uint32_t sig_pos = sig_algo_pos + 4;
				if (sig_pos + sig_len != msg_len) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerKeyExchange signature length.", out_error_len); return false; }

				/* The signed content is client_random || server_random ||
				 * the ECDHE params exactly as sent on the wire (curve_type
				 * through the end of the public key) - RFC 8422 SS5.4. */
				uint8_t signed_content[32 + 32 + 1 + 2 + 1 + 65];
				memcpy(signed_content, tls.client_random, 32);
				memcpy(signed_content + 32, tls.server_random, 32);
				memcpy(signed_content + 64, body, 4 + 65);
				uint8_t signed_hash[32];
				sha256(signed_content, sizeof(signed_content), signed_hash);

				if (!got_certificate) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Server sent ServerKeyExchange before its Certificate.", out_error_len);
					return false;
				}
				if (!rsa_verify_pkcs1_sha256(server_cert.pubkey.modulus, server_cert.pubkey.modulus_len,
				                             server_cert.pubkey.exponent, server_cert.pubkey.exponent_len,
				                             body + sig_pos, sig_len, signed_hash)) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "ServerKeyExchange signature verification failed.", out_error_len);
					return false;
				}

				got_server_key_exchange = true;
			} else if (msg_type == TLS_HANDSHAKE_CERTIFICATE) {
				/* Certificate message: 3-byte total length, then a list
				 * of 3-byte-length-prefixed DER certificates - this
				 * client only needs the first (leaf/server) certificate */
				if (msg_len < 3 + 3) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed Certificate message.", out_error_len); return false; }
				uint32_t cert_len = ((uint32_t)body[3] << 16) | ((uint32_t)body[4] << 8) | body[5];
				if (6 + cert_len > msg_len) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed certificate length.", out_error_len); return false; }
				if (!x509_parse_certificate(body + 6, cert_len, &server_cert)) {
					tcp_close(&tls.tcp);
					local_strcpy_bounded_tls(out_error, "Could not parse the server's certificate (may use an unsupported key type - only RSA is supported).", out_error_len);
					return false;
				}
				got_certificate = true;
			} else if (msg_type == TLS_HANDSHAKE_SERVER_HELLO_DONE) {
				got_server_hello_done = true;
			}

			fp += 4 + msg_len;
		}
		/* shift any leftover partial message to the front for next time */
		memmove(flight, flight + fp, flight_len - fp);
		flight_len -= fp;
	}

	if (!got_server_hello || !got_certificate) {
		local_strcpy_bounded_tls(out_error, "Incomplete server handshake (missing ServerHello or Certificate).", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}
	bool using_ecdhe = (chosen_cipher_suite == TLS_CIPHER_ECDHE_RSA_AES128_CBC_SHA256);
	if (using_ecdhe && !got_server_key_exchange) {
		local_strcpy_bounded_tls(out_error, "Server chose ECDHE but never sent ServerKeyExchange.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* Real, but limited, hostname check - see this file's own top
	 * comment and x509.c's for exactly what this does and doesn't
	 * protect against (no CA chain validation happens here at all). */
	bool cn_matches = (strcmp(server_cert.subject_cn, hostname) == 0);
	if (!cn_matches) {
		local_strcpy_bounded_tls(out_error, "Certificate subject name does not match the requested hostname.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* --- ClientKeyExchange: derive the pre-master secret, either by
	 * RSA-encrypting a random value with the server's certificate key
	 * (TLS_RSA_*) or by an ECDHE exchange on P-256 (TLS_ECDHE_RSA_*) -
	 * see this file's cipher-suite comment near the top for why both
	 * exist. Either way, `pre_master_secret`/`pre_master_secret_len`
	 * end up holding the same thing from here on: the one shared value
	 * both sides now agree on that everything else (master secret,
	 * session keys) is derived from. */
	static uint8_t pre_master_secret[48];
	uint32_t pre_master_secret_len;

	uint8_t cke_msg[608];
	uint32_t cke_msg_len;

	if (using_ecdhe) {
		uint8_t ecdhe_seed[32];
		fill_random(ecdhe_seed, 32, 0x33333333u);

		uint8_t our_private_key[32], our_public_point[65];
		if (!ecc_p256_generate_keypair(ecdhe_seed, our_private_key, our_public_point)) {
			local_strcpy_bounded_tls(out_error, "Failed to generate an ECDHE key pair.", out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}

		uint8_t shared_secret[32];
		if (!ecc_p256_compute_shared_secret(our_private_key, server_ecdhe_pubkey, shared_secret)) {
			local_strcpy_bounded_tls(out_error, "ECDHE shared secret computation failed (the server's point may not be a valid P-256 point).", out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}
		memcpy(pre_master_secret, shared_secret, 32);
		pre_master_secret_len = 32;

		/* ClientKeyExchange for ECDHE (RFC 4492 SS5.7): just our
		 * uncompressed public point, 1-byte-length-prefixed. */
		uint8_t cke_body[66];
		cke_body[0] = 65;
		memcpy(cke_body + 1, our_public_point, 65);
		uint32_t cke_body_len = 66;

		cke_msg[0] = TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE;
		cke_msg[1] = (uint8_t)(cke_body_len >> 16); cke_msg[2] = (uint8_t)(cke_body_len >> 8); cke_msg[3] = (uint8_t)(cke_body_len);
		memcpy(cke_msg + 4, cke_body, cke_body_len);
		cke_msg_len = 4 + cke_body_len;
	} else {
		pre_master_secret[0] = (uint8_t)(TLS_VERSION_1_2 >> 8);
		pre_master_secret[1] = (uint8_t)(TLS_VERSION_1_2);
		fill_random(pre_master_secret + 2, 46, 0x22222222u);
		pre_master_secret_len = 48;

		static uint8_t encrypted_pms[512];
		if (!rsa_encrypt_pkcs1(server_cert.pubkey.modulus, server_cert.pubkey.modulus_len,
		                        server_cert.pubkey.exponent, server_cert.pubkey.exponent_len,
		                        pre_master_secret, pre_master_secret_len,
		                        encrypted_pms, server_cert.pubkey.modulus_len)) {
			local_strcpy_bounded_tls(out_error, "RSA encryption of the pre-master secret failed.", out_error_len);
			tcp_close(&tls.tcp);
			return false;
		}

		uint32_t modulus_len = server_cert.pubkey.modulus_len;
		uint8_t cke_body[600];
		cke_body[0] = (uint8_t)(modulus_len >> 8);
		cke_body[1] = (uint8_t)(modulus_len);
		memcpy(cke_body + 2, encrypted_pms, modulus_len);
		uint32_t cke_body_len = 2 + modulus_len;

		cke_msg[0] = TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE;
		cke_msg[1] = (uint8_t)(cke_body_len >> 16); cke_msg[2] = (uint8_t)(cke_body_len >> 8); cke_msg[3] = (uint8_t)(cke_body_len);
		memcpy(cke_msg + 4, cke_body, cke_body_len);
		cke_msg_len = 4 + cke_body_len;
	}

	transcript_append(&tls, cke_msg, cke_msg_len);
	if (!tls_send_record(&tls, TLS_CONTENT_HANDSHAKE, cke_msg, cke_msg_len, false)) {
		local_strcpy_bounded_tls(out_error, "Failed to send ClientKeyExchange.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* --- derive the master secret, then all session keys from it (RFC 5246 6.3/8.1) --- */
	uint8_t seed[64];
	memcpy(seed, tls.client_random, 32);
	memcpy(seed + 32, tls.server_random, 32);
	tls_prf(pre_master_secret, pre_master_secret_len, "master secret", seed, 64, tls.master_secret, sizeof(tls.master_secret));

	/* key_block = PRF(master_secret, "key expansion", server_random || client_random, needed_len)
	 * layout for this cipher suite: client_MAC(32) || server_MAC(32) || client_key(16) || server_key(16)
	 * (TLS_RSA_WITH_AES_128_CBC_SHA256 uses explicit IVs, so no IV material is derived here) */
	uint8_t key_seed[64];
	memcpy(key_seed, tls.server_random, 32);
	memcpy(key_seed + 32, tls.client_random, 32);
	uint8_t key_block[32 + 32 + 16 + 16];
	tls_prf(tls.master_secret, sizeof(tls.master_secret), "key expansion", key_seed, 64, key_block, sizeof(key_block));

	memcpy(tls.client_mac_key, key_block, 32);
	memcpy(tls.server_mac_key, key_block + 32, 32);
	memcpy(tls.client_write_key, key_block + 64, 16);
	memcpy(tls.server_write_key, key_block + 80, 16);

	/* --- ChangeCipherSpec + Finished --- */
	uint8_t ccs_payload[1] = { 0x01 };
	if (!tls_send_record(&tls, TLS_CONTENT_CHANGE_CIPHER_SPEC, ccs_payload, 1, false)) {
		local_strcpy_bounded_tls(out_error, "Failed to send ChangeCipherSpec.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	uint8_t transcript_hash[32];
	sha256(tls.transcript, tls.transcript_len, transcript_hash);
	uint8_t verify_data[12];
	tls_prf(tls.master_secret, sizeof(tls.master_secret), "client finished", transcript_hash, 32, verify_data, sizeof(verify_data));

	uint8_t finished_msg[16];
	finished_msg[0] = TLS_HANDSHAKE_FINISHED;
	finished_msg[1] = 0; finished_msg[2] = 0; finished_msg[3] = 12;
	memcpy(finished_msg + 4, verify_data, 12);
	transcript_append(&tls, finished_msg, sizeof(finished_msg));

	if (!tls_send_record(&tls, TLS_CONTENT_HANDSHAKE, finished_msg, sizeof(finished_msg), true)) {
		local_strcpy_bounded_tls(out_error, "Failed to send Finished.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* --- verify the server's ChangeCipherSpec + Finished --- */
	if (!tls_recv_record(&tls, &content_type, record_payload, &record_payload_len, sizeof(record_payload), false)) {
		local_strcpy_bounded_tls(out_error, "Did not receive the server's ChangeCipherSpec (timed out).", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}
	if (content_type == TLS_CONTENT_ALERT) {
		char msg[96];
		const char *parts[] = { "Server sent alert: ", record_payload_len >= 2 ? tls_alert_name(record_payload[1]) : "malformed alert" };
		uint32_t mp = 0;
		for (int pi = 0; pi < 2; pi++) for (const char *p = parts[pi]; *p && mp < sizeof(msg) - 1; p++) msg[mp++] = *p;
		msg[mp] = '\0';
		local_strcpy_bounded_tls(out_error, msg, out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}
	if (content_type != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
		local_strcpy_bounded_tls(out_error, "Did not receive the server's ChangeCipherSpec (unexpected message).", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	if (!tls_recv_record(&tls, &content_type, record_payload, &record_payload_len, sizeof(record_payload), true) ||
	    content_type != TLS_CONTENT_HANDSHAKE || record_payload_len != 16 || record_payload[0] != TLS_HANDSHAKE_FINISHED) {
		local_strcpy_bounded_tls(out_error, "Did not receive a valid server Finished message.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* the server's Finished check covers the transcript up to but not
	 * including the server's own Finished message - i.e. exactly what's
	 * been accumulated so far, since the client's Finished was already
	 * appended above before this point */
	uint8_t server_transcript_hash[32];
	sha256(tls.transcript, tls.transcript_len, server_transcript_hash);
	uint8_t expected_server_verify[12];
	tls_prf(tls.master_secret, sizeof(tls.master_secret), "server finished", server_transcript_hash, 32, expected_server_verify, sizeof(expected_server_verify));

	uint8_t diff = 0;
	for (int i = 0; i < 12; i++) diff |= (uint8_t)(expected_server_verify[i] ^ record_payload[4 + i]);
	if (diff != 0) {
		local_strcpy_bounded_tls(out_error, "Server Finished verification failed (handshake integrity check did not match).", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	tls.handshake_done = true;
	ctx->conn = &tls;
	return true;
}

bool tls_send(struct tls_ctx *ctx, const uint8_t *data, uint32_t len) {
	struct tls_conn *tls = (struct tls_conn *)ctx->conn;
	if (!tls->handshake_done) return false;
	/* application-data records are capped at 2^14 bytes by the spec;
	 * this client's callers (one HTTP request) never come close */
	return tls_send_record(tls, TLS_CONTENT_APPLICATION_DATA, data, len, true);
}

bool tls_recv(struct tls_ctx *ctx, uint8_t *out, uint32_t max_len, uint32_t *out_len) {
	struct tls_conn *tls = (struct tls_conn *)ctx->conn;
	if (!tls->handshake_done) return false;
	uint8_t content_type;
	bool ok = tls_recv_record(tls, &content_type, out, out_len, max_len, true);
	if (ok && content_type == TLS_CONTENT_ALERT) return false; /* server sent an alert (commonly close_notify at end-of-response, or a real error) - either way, no more application data is coming */
	return ok && content_type == TLS_CONTENT_APPLICATION_DATA;
}

void tls_close(struct tls_ctx *ctx) {
	struct tls_conn *tls = (struct tls_conn *)ctx->conn;
	tcp_close(&tls->tcp);
}
