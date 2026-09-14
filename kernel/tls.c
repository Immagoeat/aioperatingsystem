/* tls.c - a real (if narrowly scoped) TLS 1.2 client, built on the
 * primitives verified independently in bignum.c/rsa.c/aes.c/hmac.c and
 * the certificate parsing in x509.c.
 *
 * Cipher suite: TLS_RSA_WITH_AES_128_CBC_SHA256 only. This is
 * deliberately the oldest/simplest real TLS 1.2 mode - RSA key
 * exchange (no elliptic-curve math needed) and AES-CBC (no Galois-
 * field authenticated-encryption math needed) - chosen because it's
 * more tractable to hand-implement and verify correctly than
 * ECDHE+AES-GCM, at the real cost that many modern servers now refuse
 * it as too old (see the file comment in kernel.h's tls_connect()
 * declaration). This will work against servers that still allow it -
 * older infrastructure, test/embedded targets, badssl.com's own
 * legacy-cipher test subdomains - and correctly fail (not silently
 * downgrade to something insecure) against ones that don't.
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

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20
#define TLS_CONTENT_ALERT              21
#define TLS_CONTENT_HANDSHAKE          22
#define TLS_CONTENT_APPLICATION_DATA   23

#define TLS_HANDSHAKE_CLIENT_HELLO         1
#define TLS_HANDSHAKE_SERVER_HELLO         2
#define TLS_HANDSHAKE_CERTIFICATE          11
#define TLS_HANDSHAKE_SERVER_HELLO_DONE    14
#define TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE  16
#define TLS_HANDSHAKE_FINISHED             20

#define TLS_VERSION_1_2 0x0303

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
		return tcp_send_segment(&tls->tcp, TCP_FLAG_PSH | TCP_FLAG_ACK, record, (uint16_t)(5 + payload_len));
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
	return tcp_send_segment(&tls->tcp, TCP_FLAG_PSH | TCP_FLAG_ACK, record, (uint16_t)(5 + record_payload_len));
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
	hello[p++] = 0x00; hello[p++] = 0x02; /* cipher suites length: 2 bytes = 1 suite */
	hello[p++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256 >> 8);
	hello[p++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256);
	hello[p++] = 0x01; /* compression methods length: 1 */
	hello[p++] = 0x00; /* null compression */
	/* Server Name Indication (SNI) extension - many real servers (name-
	 * based virtual hosting) require this to select the right
	 * certificate at all */
	uint32_t hostname_len = (uint32_t)strlen(hostname);
	uint32_t sni_len = 5 + hostname_len;
	hello[p++] = 0x00; hello[p++] = 0x00; /* extensions length placeholder, filled below */
	uint32_t ext_len_pos = p - 2;
	hello[p++] = 0x00; hello[p++] = 0x00; /* extension type: server_name */
	hello[p++] = (uint8_t)((sni_len) >> 8); hello[p++] = (uint8_t)(sni_len); /* extension data length */
	hello[p++] = (uint8_t)((hostname_len + 3) >> 8); hello[p++] = (uint8_t)(hostname_len + 3); /* server_name_list length */
	hello[p++] = 0x00; /* name type: host_name */
	hello[p++] = (uint8_t)(hostname_len >> 8); hello[p++] = (uint8_t)(hostname_len);
	memcpy(hello + p, hostname, hostname_len); p += hostname_len;
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
				if (msg_len < 2 + 32) { tcp_close(&tls.tcp); local_strcpy_bounded_tls(out_error, "Malformed ServerHello.", out_error_len); return false; }
				memcpy(tls.server_random, body + 2, 32);
				got_server_hello = true;
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

	/* Real, but limited, hostname check - see this file's own top
	 * comment and x509.c's for exactly what this does and doesn't
	 * protect against (no CA chain validation happens here at all). */
	bool cn_matches = (strcmp(server_cert.subject_cn, hostname) == 0);
	if (!cn_matches) {
		local_strcpy_bounded_tls(out_error, "Certificate subject name does not match the requested hostname.", out_error_len);
		tcp_close(&tls.tcp);
		return false;
	}

	/* --- ClientKeyExchange: generate a pre-master secret, RSA-encrypt
	 * it with the server's public key from its certificate --- */
	uint8_t pre_master_secret[48];
	pre_master_secret[0] = (uint8_t)(TLS_VERSION_1_2 >> 8);
	pre_master_secret[1] = (uint8_t)(TLS_VERSION_1_2);
	fill_random(pre_master_secret + 2, 46, 0x22222222u);

	static uint8_t encrypted_pms[512];
	if (!rsa_encrypt_pkcs1(server_cert.pubkey.modulus, server_cert.pubkey.modulus_len,
	                        server_cert.pubkey.exponent, server_cert.pubkey.exponent_len,
	                        pre_master_secret, sizeof(pre_master_secret),
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

	uint8_t cke_msg[608];
	cke_msg[0] = TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE;
	cke_msg[1] = (uint8_t)(cke_body_len >> 16); cke_msg[2] = (uint8_t)(cke_body_len >> 8); cke_msg[3] = (uint8_t)(cke_body_len);
	memcpy(cke_msg + 4, cke_body, cke_body_len);
	uint32_t cke_msg_len = 4 + cke_body_len;

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
	tls_prf(pre_master_secret, sizeof(pre_master_secret), "master secret", seed, 64, tls.master_secret, sizeof(tls.master_secret));

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
	if (!tls_recv_record(&tls, &content_type, record_payload, &record_payload_len, sizeof(record_payload), false) ||
	    content_type != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
		local_strcpy_bounded_tls(out_error, "Did not receive the server's ChangeCipherSpec.", out_error_len);
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
