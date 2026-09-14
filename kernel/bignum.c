/* bignum.c - arbitrary-precision unsigned integer arithmetic, just
 * enough for RSA public-key encryption (see rsa.c): modular
 * exponentiation, which is itself built from multiplication and
 * modular reduction.
 *
 * Numbers are stored as arrays of BIGNUM_WORDS 32-bit words,
 * little-endian by word index (word[0] is the least significant).
 * Fixed-size rather than dynamically sized - this is a freestanding
 * kernel with no heap allocator, and a fixed 4096-bit capacity (double
 * the largest RSA modulus this is expected to handle) is simpler and
 * safer than dynamic sizing for a component this security-sensitive.
 *
 * Schoolbook (not Karatsuba/Montgomery) multiplication and division -
 * O(n^2) instead of the faster algorithms real crypto libraries use,
 * which matters for performance at scale but not for correctness, and
 * this only ever runs once per TLS handshake (not a hot path). */
#include "kernel.h"

#define BIGNUM_WORDS 128 /* 128 * 32 = 4096 bits */

struct bignum {
	uint32_t word[BIGNUM_WORDS];
};

static void bn_zero(struct bignum *a) {
	memset(a->word, 0, sizeof(a->word));
}

static void bn_from_bytes(struct bignum *a, const uint8_t *bytes, uint32_t len) {
	bn_zero(a);
	/* bytes[] is big-endian (network/RSA convention: most significant
	 * byte first), so consume it from the end into increasing word
	 * indices (least significant word first) */
	int word_index = 0, shift = 0;
	for (int32_t i = (int32_t)len - 1; i >= 0 && word_index < BIGNUM_WORDS; i--) {
		a->word[word_index] |= (uint32_t)bytes[i] << shift;
		shift += 8;
		if (shift == 32) { shift = 0; word_index++; }
	}
}

/* Writes `a` out as big-endian bytes, zero-padded on the left to
 * exactly `out_len` bytes (the caller knows the expected modulus size -
 * RSA results must be left-padded to the modulus length, not just the
 * minimal encoding, since leading zero bytes are significant to the
 * protocol framing). */
static void bn_to_bytes(const struct bignum *a, uint8_t *out, uint32_t out_len) {
	for (uint32_t i = 0; i < out_len; i++) {
		uint32_t byte_pos_from_end = out_len - 1 - i;
		uint32_t word_index = byte_pos_from_end / 4;
		uint32_t byte_in_word = byte_pos_from_end % 4;
		uint8_t byte = (word_index < BIGNUM_WORDS) ? (uint8_t)(a->word[word_index] >> (byte_in_word * 8)) : 0;
		out[i] = byte;
	}
}

/* -1, 0, 1 as a compares to b */
static int bn_cmp(const struct bignum *a, const struct bignum *b) {
	for (int i = BIGNUM_WORDS - 1; i >= 0; i--) {
		if (a->word[i] != b->word[i]) return a->word[i] < b->word[i] ? -1 : 1;
	}
	return 0;
}

static bool bn_is_zero(const struct bignum *a) {
	for (int i = 0; i < BIGNUM_WORDS; i++) if (a->word[i] != 0) return false;
	return true;
}

/* out = a - b, assuming a >= b (the only case this module ever needs -
 * every subtraction here happens inside modular reduction, where the
 * subtrahend is already known not to exceed the minuend) */
static void bn_sub(struct bignum *out, const struct bignum *a, const struct bignum *b) {
	int64_t borrow = 0;
	for (int i = 0; i < BIGNUM_WORDS; i++) {
		int64_t diff = (int64_t)a->word[i] - (int64_t)b->word[i] - borrow;
		if (diff < 0) { diff += 0x100000000LL; borrow = 1; } else { borrow = 0; }
		out->word[i] = (uint32_t)diff;
	}
}

static void bn_shl1(struct bignum *a) {
	uint32_t carry = 0;
	for (int i = 0; i < BIGNUM_WORDS; i++) {
		uint32_t new_carry = a->word[i] >> 31;
		a->word[i] = (a->word[i] << 1) | carry;
		carry = new_carry;
	}
}

/* out = (a * b) mod m. Schoolbook long multiplication combined with
 * "add-and-reduce" modular reduction (binary long division style),
 * avoiding the need for a separate full-width multiply buffer wider
 * than BIGNUM_WORDS: reduces mod m after each bit, the standard
 * technique for modular multiplication without a bignum library's
 * general-purpose arbitrary-width product type. */
static void bn_mulmod(struct bignum *out, const struct bignum *a, const struct bignum *b, const struct bignum *m) {
	struct bignum result, base;
	bn_zero(&result);
	base = *a;

	/* process b's bits from least to most significant; for each set
	 * bit, add the correspondingly-shifted `a` (mod m) into the result */
	for (int word = 0; word < BIGNUM_WORDS; word++) {
		uint32_t b_word = b->word[word];
		for (int bit = 0; bit < 32; bit++) {
			if (b_word & (1u << bit)) {
				/* result = (result + base) mod m */
				struct bignum sum;
				uint64_t carry = 0;
				for (int i = 0; i < BIGNUM_WORDS; i++) {
					uint64_t s = (uint64_t)result.word[i] + base.word[i] + carry;
					sum.word[i] = (uint32_t)s;
					carry = s >> 32;
				}
				/* sum may have overflowed BIGNUM_WORDS (carry) or just
				 * exceeded m; either way, subtracting m once or twice
				 * brings it back in range since both operands were
				 * already < m before this addition */
				result = sum;
				if (carry || bn_cmp(&result, m) >= 0) bn_sub(&result, &result, m);
				if (bn_cmp(&result, m) >= 0) bn_sub(&result, &result, m);
			}
			/* base = (base * 2) mod m */
			bool overflow = (base.word[BIGNUM_WORDS - 1] & 0x80000000u) != 0;
			bn_shl1(&base);
			if (overflow || bn_cmp(&base, m) >= 0) bn_sub(&base, &base, m);
		}
	}

	*out = result;
}

/* out = (base^exp) mod m - square-and-multiply, the standard modular
 * exponentiation algorithm. This is the one operation rsa.c actually
 * needs: RSA public-key encryption is exactly this with exp = the
 * public exponent (almost always 65537) and m = the modulus. */
static void bn_modexp(struct bignum *out, const struct bignum *base, const struct bignum *exp, const struct bignum *m) {
	struct bignum result, b;
	bn_zero(&result);
	result.word[0] = 1; /* result = 1 */
	b = *base;

	/* reduce base mod m first, in case it's already >= m */
	while (bn_cmp(&b, m) >= 0) bn_sub(&b, &b, m);

	for (int word = 0; word < BIGNUM_WORDS; word++) {
		uint32_t e_word = exp->word[word];
		for (int bit = 0; bit < 32; bit++) {
			if (e_word & (1u << bit)) {
				bn_mulmod(&result, &result, &b, m);
			}
			bn_mulmod(&b, &b, &b, m);
		}
	}

	*out = result;
}

/* Public entry point: RSA-style modular exponentiation directly on
 * byte buffers (big-endian, RSA's wire format) rather than exposing
 * the whole bignum type - rsa.c is the only caller and only ever needs
 * this one operation. `modulus_len` bytes in and out (the RSA modulus
 * size - e.g. 256 for a 2048-bit key); `message`/`exponent` may be
 * shorter (they're zero-extended). Returns false if any input is
 * larger than this module's fixed 4096-bit capacity. */
bool bignum_modexp_bytes(const uint8_t *message, uint32_t message_len,
                          const uint8_t *exponent, uint32_t exponent_len,
                          const uint8_t *modulus, uint32_t modulus_len,
                          uint8_t *out, uint32_t out_len) {
	if (message_len > BIGNUM_WORDS * 4 || exponent_len > BIGNUM_WORDS * 4 ||
	    modulus_len > BIGNUM_WORDS * 4 || out_len > BIGNUM_WORDS * 4) {
		return false;
	}
	struct bignum m, base, exp, result;
	bn_from_bytes(&m, modulus, modulus_len);
	bn_from_bytes(&base, message, message_len);
	bn_from_bytes(&exp, exponent, exponent_len);

	if (bn_is_zero(&m)) return false;

	bn_modexp(&result, &base, &exp, &m);
	bn_to_bytes(&result, out, out_len);
	return true;
}
