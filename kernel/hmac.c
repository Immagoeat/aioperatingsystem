/* hmac.c - HMAC-SHA256 (RFC 2104/FIPS 198-1), built directly on top
 * of sha256.c. Used by tls.c for two things: the TLS 1.2 PRF (key
 * derivation from the pre-master secret) and the record-layer MAC
 * (integrity-protecting each encrypted TLS record, TLS 1.2's
 * MAC-then-encrypt construction for CBC cipher suites). */
#include "kernel.h"

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

void hmac_sha256(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t data_len, uint8_t out[32]) {
	uint8_t key_block[SHA256_BLOCK_SIZE];
	memset(key_block, 0, sizeof(key_block));

	if (key_len > SHA256_BLOCK_SIZE) {
		/* keys longer than one block are themselves hashed down first, per RFC 2104 */
		sha256(key, key_len, key_block);
	} else {
		memcpy(key_block, key, key_len);
	}

	uint8_t ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
	for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
		ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
		opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
	}

	/* inner = SHA256(ipad || data) */
	static uint8_t inner_buf[SHA256_BLOCK_SIZE + 16384]; /* generous - the largest single hmac_sha256 call here is over a TLS handshake transcript, comfortably under 16KB */
	uint32_t inner_len = SHA256_BLOCK_SIZE + data_len;
	memcpy(inner_buf, ipad, SHA256_BLOCK_SIZE);
	memcpy(inner_buf + SHA256_BLOCK_SIZE, data, data_len);
	uint8_t inner_hash[SHA256_DIGEST_SIZE];
	sha256(inner_buf, inner_len, inner_hash);

	/* outer = SHA256(opad || inner) */
	uint8_t outer_buf[SHA256_BLOCK_SIZE + SHA256_DIGEST_SIZE];
	memcpy(outer_buf, opad, SHA256_BLOCK_SIZE);
	memcpy(outer_buf + SHA256_BLOCK_SIZE, inner_hash, SHA256_DIGEST_SIZE);
	sha256(outer_buf, sizeof(outer_buf), out);
}

/* --- TLS 1.2 PRF (RFC 5246 section 5), built from HMAC-SHA256 (TLS
 * 1.2 fixed the PRF to a single configurable hash, unlike 1.0/1.1's
 * MD5+SHA1 combination - SHA-256 is the standard choice for the
 * RSA/AES-CBC cipher suite this client speaks). P_hash is an
 * iterated HMAC construction (A(i) = HMAC(secret, A(i-1)), output
 * chunks = HMAC(secret, A(i) || seed)) that can produce as much
 * keying material as needed by just running more iterations. */
void tls_prf(const uint8_t *secret, uint32_t secret_len, const char *label,
             const uint8_t *seed, uint32_t seed_len, uint8_t *out, uint32_t out_len) {
	uint32_t label_len = (uint32_t)strlen(label);
	static uint8_t label_seed[256];
	memcpy(label_seed, label, label_len);
	memcpy(label_seed + label_len, seed, seed_len);
	uint32_t label_seed_len = label_len + seed_len;

	uint8_t a[SHA256_DIGEST_SIZE];
	hmac_sha256(secret, secret_len, label_seed, label_seed_len, a); /* A(1) */

	uint32_t produced = 0;
	while (produced < out_len) {
		uint8_t a_and_seed[SHA256_DIGEST_SIZE + 256];
		memcpy(a_and_seed, a, SHA256_DIGEST_SIZE);
		memcpy(a_and_seed + SHA256_DIGEST_SIZE, label_seed, label_seed_len);

		uint8_t chunk[SHA256_DIGEST_SIZE];
		hmac_sha256(secret, secret_len, a_and_seed, SHA256_DIGEST_SIZE + label_seed_len, chunk);

		uint32_t take = out_len - produced;
		if (take > SHA256_DIGEST_SIZE) take = SHA256_DIGEST_SIZE;
		memcpy(out + produced, chunk, take);
		produced += take;

		uint8_t next_a[SHA256_DIGEST_SIZE];
		hmac_sha256(secret, secret_len, a, SHA256_DIGEST_SIZE, next_a); /* A(i+1) = HMAC(secret, A(i)) */
		memcpy(a, next_a, SHA256_DIGEST_SIZE);
	}
}
