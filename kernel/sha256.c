/* sha256.c - SHA-256 (FIPS 180-4), a real cryptographic hash, not a
 * home-grown checksum. Used by auth.c to store a salted hash of the
 * user's password on disk rather than the password itself, so a
 * "password-locked" claim is actually true: reading the stored file
 * doesn't hand you the password back, and it's not an interchangeable
 * risk with a rolled-your-own algorithm.
 *
 * Textbook, unoptimized implementation, directly from the FIPS 180-4
 * specification's constants and round function - correctness matters
 * far more than speed for what this is used for (hashing a password a
 * handful of times per boot). Verified against the standard published
 * test vectors for "abc" and the empty string (see the git history for
 * the throwaway verification harness used to confirm this before it
 * was trusted for anything). */
#include "kernel.h"

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr32(uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* One-shot SHA-256 over `len` bytes of `data`, writing the 32-byte
 * digest to `out`. Handles the full Merkle-Damgard padding (the
 * trailing 0x80, zero padding, and the 64-bit bit-length suffix)
 * internally rather than exposing an incremental API, since every
 * caller here hashes a short, fully-buffered password + salt at once. */
void sha256(const void *data, uint32_t len, uint8_t out[32]) {
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t full_blocks = len / 64;
	for (uint32_t i = 0; i < full_blocks; i++) {
		sha256_transform(state, bytes + i * 64);
	}

	uint8_t tail[128]; /* room for up to 2 blocks: leftover data + 0x80 + zero padding + 8-byte length */
	uint32_t tail_len = len - full_blocks * 64;
	memcpy(tail, bytes + full_blocks * 64, tail_len);
	tail[tail_len] = 0x80;
	uint32_t after_marker = tail_len + 1;

	uint32_t pad_to = (after_marker <= 56) ? 56 : 120;
	memset(tail + after_marker, 0, pad_to - after_marker);

	uint64_t bit_len = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		tail[pad_to + i] = (uint8_t)(bit_len >> (56 - i * 8));
	}

	uint32_t tail_blocks = (pad_to + 8) / 64;
	for (uint32_t i = 0; i < tail_blocks; i++) {
		sha256_transform(state, tail + i * 64);
	}

	for (int i = 0; i < 8; i++) {
		out[i * 4 + 0] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(state[i]);
	}
}
