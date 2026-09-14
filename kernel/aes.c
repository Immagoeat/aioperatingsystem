/* aes.c - AES-128 (FIPS 197), block encryption only (a TLS client only
 * ever encrypts its own outgoing records and decrypts the server's
 * incoming ones - "decrypt" for AES-CBC is a real, different set of
 * operations (InvSubBytes/InvShiftRows/InvMixColumns), also
 * implemented here since TLS needs both directions).
 *
 * Textbook implementation directly from the FIPS 197 tables and round
 * structure - not the fastest possible (real libraries use hardware
 * AES-NI instructions or bitsliced/table-based tricks for
 * performance and side-channel resistance), but this is not a hot
 * path (a handful of 16-byte blocks per HTTP request) and correctness
 * matters far more than speed. Verified against the official FIPS 197
 * Appendix B test vector before being trusted for anything - see the
 * git history for the throwaway verification harness. */
#include "kernel.h"

static const uint8_t aes_sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t aes_inv_sbox[256];
static bool aes_inv_sbox_ready = false;

static void aes_build_inv_sbox(void) {
	for (int i = 0; i < 256; i++) aes_inv_sbox[aes_sbox[i]] = (uint8_t)i;
	aes_inv_sbox_ready = true;
}

static const uint8_t aes_rcon[11] = { 0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

#define AES_ROUNDS 10
#define AES_KEY_WORDS 4 /* AES-128: 4 32-bit words = 16 bytes */

struct aes_ctx {
	uint32_t round_key[(AES_ROUNDS + 1) * 4]; /* 11 round keys of 4 words each */
};

static uint32_t aes_sub_word(uint32_t w) {
	uint8_t b0 = aes_sbox[(w >> 24) & 0xFF];
	uint8_t b1 = aes_sbox[(w >> 16) & 0xFF];
	uint8_t b2 = aes_sbox[(w >> 8) & 0xFF];
	uint8_t b3 = aes_sbox[w & 0xFF];
	return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
}

static uint32_t aes_rot_word(uint32_t w) {
	return (w << 8) | (w >> 24);
}

static void aes_key_expansion(struct aes_ctx *ctx, const uint8_t key[16]) {
	uint32_t *w = ctx->round_key;
	for (int i = 0; i < AES_KEY_WORDS; i++) {
		w[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
		       ((uint32_t)key[i * 4 + 2] << 8) | key[i * 4 + 3];
	}
	for (int i = AES_KEY_WORDS; i < (AES_ROUNDS + 1) * 4; i++) {
		uint32_t temp = w[i - 1];
		if (i % AES_KEY_WORDS == 0) {
			temp = aes_sub_word(aes_rot_word(temp)) ^ ((uint32_t)aes_rcon[i / AES_KEY_WORDS] << 24);
		}
		w[i] = w[i - AES_KEY_WORDS] ^ temp;
	}
}

/* GF(2^8) multiplication, used by MixColumns/InvMixColumns - the
 * standard "xtime" (multiply-by-2, reducing mod the AES polynomial
 * 0x11b when the top bit would overflow) peasant-multiplication
 * algorithm, direct from the FIPS 197 specification. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
	uint8_t result = 0;
	for (int i = 0; i < 8; i++) {
		if (b & 1) result ^= a;
		bool hi = (a & 0x80) != 0;
		a = (uint8_t)(a << 1);
		if (hi) a ^= 0x1b;
		b >>= 1;
	}
	return result;
}

/* state is a 4x4 byte matrix stored column-major (state[col*4+row]),
 * per FIPS 197's own convention - matches how bytes are loaded
 * in/out of a 16-byte block below. */

static void aes_add_round_key(uint8_t state[16], const uint32_t round_key[4]) {
	for (int col = 0; col < 4; col++) {
		state[col * 4 + 0] ^= (uint8_t)(round_key[col] >> 24);
		state[col * 4 + 1] ^= (uint8_t)(round_key[col] >> 16);
		state[col * 4 + 2] ^= (uint8_t)(round_key[col] >> 8);
		state[col * 4 + 3] ^= (uint8_t)(round_key[col]);
	}
}

static void aes_sub_bytes(uint8_t state[16]) {
	for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
}

static void aes_inv_sub_bytes(uint8_t state[16]) {
	for (int i = 0; i < 16; i++) state[i] = aes_inv_sbox[state[i]];
}

static void aes_shift_rows(uint8_t state[16]) {
	uint8_t tmp[16];
	memcpy(tmp, state, 16);
	/* row r is cyclically left-shifted by r positions; state is
	 * column-major so row r, column c lives at [c*4+r] */
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			state[c * 4 + r] = tmp[((c + r) % 4) * 4 + r];
		}
	}
}

static void aes_inv_shift_rows(uint8_t state[16]) {
	uint8_t tmp[16];
	memcpy(tmp, state, 16);
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			state[((c + r) % 4) * 4 + r] = tmp[c * 4 + r];
		}
	}
}

static void aes_mix_columns(uint8_t state[16]) {
	for (int c = 0; c < 4; c++) {
		uint8_t *col = &state[c * 4];
		uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
		col[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
		col[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
		col[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
		col[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
	}
}

static void aes_inv_mix_columns(uint8_t state[16]) {
	for (int c = 0; c < 4; c++) {
		uint8_t *col = &state[c * 4];
		uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
		col[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
		col[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
		col[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
		col[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
	}
}

static void aes_encrypt_block(const struct aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]) {
	uint8_t state[16];
	memcpy(state, in, 16);

	aes_add_round_key(state, &ctx->round_key[0]);
	for (int round = 1; round < AES_ROUNDS; round++) {
		aes_sub_bytes(state);
		aes_shift_rows(state);
		aes_mix_columns(state);
		aes_add_round_key(state, &ctx->round_key[round * 4]);
	}
	aes_sub_bytes(state);
	aes_shift_rows(state);
	aes_add_round_key(state, &ctx->round_key[AES_ROUNDS * 4]);

	memcpy(out, state, 16);
}

static void aes_decrypt_block(const struct aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]) {
	uint8_t state[16];
	memcpy(state, in, 16);

	aes_add_round_key(state, &ctx->round_key[AES_ROUNDS * 4]);
	for (int round = AES_ROUNDS - 1; round > 0; round--) {
		aes_inv_shift_rows(state);
		aes_inv_sub_bytes(state);
		aes_add_round_key(state, &ctx->round_key[round * 4]);
		aes_inv_mix_columns(state);
	}
	aes_inv_shift_rows(state);
	aes_inv_sub_bytes(state);
	aes_add_round_key(state, &ctx->round_key[0]);

	memcpy(out, state, 16);
}

/* --- CBC mode (RFC 3602-style, as used by TLS 1.2's CBC cipher suites) --- */

void aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *plaintext, uint32_t len, uint8_t *out) {
	if (!aes_inv_sbox_ready) aes_build_inv_sbox(); /* harmless if encrypt-only is ever used without decrypt, but keeps one init path for both */
	struct aes_ctx ctx;
	aes_key_expansion(&ctx, key);

	uint8_t prev[16];
	memcpy(prev, iv, 16);

	for (uint32_t offset = 0; offset < len; offset += 16) {
		uint8_t block[16];
		for (int i = 0; i < 16; i++) block[i] = plaintext[offset + i] ^ prev[i];
		aes_encrypt_block(&ctx, block, out + offset);
		memcpy(prev, out + offset, 16);
	}
}

void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *ciphertext, uint32_t len, uint8_t *out) {
	if (!aes_inv_sbox_ready) aes_build_inv_sbox();
	struct aes_ctx ctx;
	aes_key_expansion(&ctx, key);

	uint8_t prev[16];
	memcpy(prev, iv, 16);

	for (uint32_t offset = 0; offset < len; offset += 16) {
		uint8_t decrypted[16];
		aes_decrypt_block(&ctx, ciphertext + offset, decrypted);
		for (int i = 0; i < 16; i++) out[offset + i] = (uint8_t)(decrypted[i] ^ prev[i]);
		memcpy(prev, ciphertext + offset, 16);
	}
}
