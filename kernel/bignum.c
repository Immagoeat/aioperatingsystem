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

/* --- width-bounded variants of bn_cmp()/bn_sub() above, plus a
 * bounded bn_shl1 (there was never an unbounded version - it was only
 * ever called from bn_mulmod_w() below), used once the modulus's real
 * significant word count is known. Every value handled here is always
 * < 2*m (an
 * invariant bn_mulmod_w()'s reduction step maintains), so every word
 * above that count is provably always zero for every operand these
 * ever see - skipping them is a pure performance win with no behavior
 * change, not an approximation. This exists because BIGNUM_WORDS is a
 * fixed 4096-bit capacity sized for RSA, but P-256 ECDHE (ecc.c) only
 * ever needs 256 bits: without this, a single ECDHE point operation
 * was measured costing 16x more schoolbook-multiply work than its
 * actual operand size needs - the same "always walk the full fixed
 * capacity" mistake bn_modexp() itself had before it was bounded to
 * the exponent's real bit length (see bn_bit_length() below), just in
 * the word-width dimension instead of the exponent dimension. */
static int bn_cmp_w(const struct bignum *a, const struct bignum *b, int words) {
	for (int i = words - 1; i >= 0; i--) {
		if (a->word[i] != b->word[i]) return a->word[i] < b->word[i] ? -1 : 1;
	}
	return 0;
}

static void bn_sub_w(struct bignum *out, const struct bignum *a, const struct bignum *b, int words) {
	int64_t borrow = 0;
	for (int i = 0; i < words; i++) {
		int64_t diff = (int64_t)a->word[i] - (int64_t)b->word[i] - borrow;
		if (diff < 0) { diff += 0x100000000LL; borrow = 1; } else { borrow = 0; }
		out->word[i] = (uint32_t)diff;
	}
}

static void bn_shl1_w(struct bignum *a, int words) {
	uint32_t carry = 0;
	for (int i = 0; i < words; i++) {
		uint32_t new_carry = a->word[i] >> 31;
		a->word[i] = (a->word[i] << 1) | carry;
		carry = new_carry;
	}
}

/* Index (0-based) of the highest set bit in `a`, or -1 if `a` is zero. */
static int bn_bit_length(const struct bignum *a) {
	for (int word = BIGNUM_WORDS - 1; word >= 0; word--) {
		uint32_t w = a->word[word];
		if (w != 0) {
			int bit = 31;
			while (!(w & (1u << bit))) bit--;
			return word * 32 + bit;
		}
	}
	return -1;
}

/* How many words are actually significant for a modulus of the given
 * bit length - i.e. every operand this modulus's arithmetic ever
 * touches (always kept < m by the reduction steps below) is
 * guaranteed zero above this many words. Used to bound bn_mulmod()'s
 * inner loops (see the comment above bn_cmp_w() etc.) to real work
 * instead of the fixed 4096-bit BIGNUM_WORDS capacity. */
static int bn_sig_words(int modulus_bit_length) {
	if (modulus_bit_length < 0) return 1; /* zero modulus - degenerate, but must return at least 1 to keep callers' loops well-defined */
	int words = modulus_bit_length / 32 + 1;
	return words > BIGNUM_WORDS ? BIGNUM_WORDS : words;
}

/* out = (a * b) mod m. Schoolbook long multiplication combined with
 * "add-and-reduce" modular reduction (binary long division style),
 * avoiding the need for a separate full-width multiply buffer wider
 * than BIGNUM_WORDS: reduces mod m after each bit, the standard
 * technique for modular multiplication without a bignum library's
 * general-purpose arbitrary-width product type. `words` bounds every
 * inner loop to the modulus's real significant word count (see
 * bn_sig_words()) instead of always walking the full fixed-capacity
 * BIGNUM_WORDS - a pure performance improvement, not a behavior
 * change, since every word above that bound is provably always zero
 * for values kept < m. */
static void bn_mulmod_w(struct bignum *out, const struct bignum *a, const struct bignum *b, const struct bignum *m, int words) {
	struct bignum result, base;
	bn_zero(&result);
	base = *a;

	/* process b's bits from least to most significant; for each set
	 * bit, add the correspondingly-shifted `a` (mod m) into the result */
	for (int word = 0; word < words; word++) {
		uint32_t b_word = b->word[word];
		for (int bit = 0; bit < 32; bit++) {
			if (b_word & (1u << bit)) {
				/* result = (result + base) mod m */
				struct bignum sum;
				uint64_t carry = 0;
				for (int i = 0; i < words; i++) {
					uint64_t s = (uint64_t)result.word[i] + base.word[i] + carry;
					sum.word[i] = (uint32_t)s;
					carry = s >> 32;
				}
				/* sum may have overflowed the `words`-word range or just
				 * exceeded m; either way, subtracting m once or twice
				 * brings it back in range since both operands were
				 * already < m before this addition */
				result = sum;
				if (carry || bn_cmp_w(&result, m, words) >= 0) bn_sub_w(&result, &result, m, words);
				if (bn_cmp_w(&result, m, words) >= 0) bn_sub_w(&result, &result, m, words);
			}
			/* base = (base * 2) mod m */
			bool overflow = (base.word[words - 1] & 0x80000000u) != 0;
			bn_shl1_w(&base, words);
			if (overflow || bn_cmp_w(&base, m, words) >= 0) bn_sub_w(&base, &base, m, words);
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

	int words = bn_sig_words(bn_bit_length(m));

	/* reduce base mod m first, in case it's already >= m */
	while (bn_cmp_w(&b, m, words) >= 0) bn_sub_w(&b, &b, m, words);

	int top_bit = bn_bit_length(exp);
	for (int i = 0; i <= top_bit; i++) {
		int word = i / 32, bit = i % 32;
		if (exp->word[word] & (1u << bit)) {
			bn_mulmod_w(&result, &result, &b, m, words);
		}
		if (i < top_bit) bn_mulmod_w(&b, &b, &b, m, words); /* skip the last, unused squaring of b */
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

/* out = a + b, assuming the true sum fits in BIGNUM_WORDS (the only
 * case bn_addmod_bytes() below needs - both operands are already
 * reduced mod m before this runs). */
static void bn_add(struct bignum *out, const struct bignum *a, const struct bignum *b) {
	uint64_t carry = 0;
	for (int i = 0; i < BIGNUM_WORDS; i++) {
		uint64_t s = (uint64_t)a->word[i] + b->word[i] + carry;
		out->word[i] = (uint32_t)s;
		carry = s >> 32;
	}
}

/* The general-purpose modular add/sub/mul/modexp entry points below
 * (as opposed to bignum_modexp_bytes() above, which is RSA-specific in
 * spirit even though the underlying math is the same) exist for
 * ecc.c's P-256 field arithmetic: elliptic curve point operations need
 * modular addition, subtraction, multiplication, and inversion over
 * the curve's prime field, not just the one modexp operation RSA
 * needs. Modular inverse is computed via Fermat's little theorem
 * (a^(p-2) mod p, valid since P-256's field modulus is prime) rather
 * than a separate extended-Euclidean-algorithm implementation, so it
 * reuses bn_modexp() instead of adding a whole new algorithm. */

bool bignum_addmod_bytes(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len,
                          const uint8_t *modulus, uint32_t modulus_len, uint8_t *out, uint32_t out_len) {
	if (a_len > BIGNUM_WORDS * 4 || b_len > BIGNUM_WORDS * 4 || modulus_len > BIGNUM_WORDS * 4 || out_len > BIGNUM_WORDS * 4) {
		return false;
	}
	struct bignum m, ba, bb, sum;
	bn_from_bytes(&m, modulus, modulus_len);
	bn_from_bytes(&ba, a, a_len);
	bn_from_bytes(&bb, b, b_len);
	if (bn_is_zero(&m)) return false;
	while (bn_cmp(&ba, &m) >= 0) bn_sub(&ba, &ba, &m);
	while (bn_cmp(&bb, &m) >= 0) bn_sub(&bb, &bb, &m);

	bn_add(&sum, &ba, &bb);
	if (bn_cmp(&sum, &m) >= 0) bn_sub(&sum, &sum, &m);
	bn_to_bytes(&sum, out, out_len);
	return true;
}

bool bignum_submod_bytes(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len,
                          const uint8_t *modulus, uint32_t modulus_len, uint8_t *out, uint32_t out_len) {
	if (a_len > BIGNUM_WORDS * 4 || b_len > BIGNUM_WORDS * 4 || modulus_len > BIGNUM_WORDS * 4 || out_len > BIGNUM_WORDS * 4) {
		return false;
	}
	struct bignum m, ba, bb, diff;
	bn_from_bytes(&m, modulus, modulus_len);
	bn_from_bytes(&ba, a, a_len);
	bn_from_bytes(&bb, b, b_len);
	if (bn_is_zero(&m)) return false;
	while (bn_cmp(&ba, &m) >= 0) bn_sub(&ba, &ba, &m);
	while (bn_cmp(&bb, &m) >= 0) bn_sub(&bb, &bb, &m);

	if (bn_cmp(&ba, &bb) >= 0) {
		bn_sub(&diff, &ba, &bb);
	} else {
		/* a - b when a < b: (a + m) - b, since 0 <= a,b < m guarantees
		 * a + m - b is both correct mod m and non-negative */
		struct bignum tmp;
		bn_add(&tmp, &ba, &m);
		bn_sub(&diff, &tmp, &bb);
	}
	bn_to_bytes(&diff, out, out_len);
	return true;
}

bool bignum_mulmod_bytes(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len,
                          const uint8_t *modulus, uint32_t modulus_len, uint8_t *out, uint32_t out_len) {
	if (a_len > BIGNUM_WORDS * 4 || b_len > BIGNUM_WORDS * 4 || modulus_len > BIGNUM_WORDS * 4 || out_len > BIGNUM_WORDS * 4) {
		return false;
	}
	struct bignum m, ba, bb, result;
	bn_from_bytes(&m, modulus, modulus_len);
	bn_from_bytes(&ba, a, a_len);
	bn_from_bytes(&bb, b, b_len);
	if (bn_is_zero(&m)) return false;
	int words = bn_sig_words(bn_bit_length(&m));
	while (bn_cmp_w(&ba, &m, words) >= 0) bn_sub_w(&ba, &ba, &m, words);
	while (bn_cmp_w(&bb, &m, words) >= 0) bn_sub_w(&bb, &bb, &m, words);

	bn_mulmod_w(&result, &ba, &bb, &m, words);
	bn_to_bytes(&result, out, out_len);
	return true;
}

/* out = a^-1 mod modulus (modular multiplicative inverse), via
 * Fermat's little theorem: a^(p-2) mod p == a^-1 mod p when p is
 * prime and a is not a multiple of p - both true for every inverse
 * ecc.c ever needs (P-256's field modulus, and separately its group
 * order, are both prime; a is always a nonzero field/scalar value). */
bool bignum_invmod_bytes(const uint8_t *a, uint32_t a_len, const uint8_t *modulus, uint32_t modulus_len, uint8_t *out, uint32_t out_len) {
	if (a_len > BIGNUM_WORDS * 4 || modulus_len > BIGNUM_WORDS * 4 || out_len > BIGNUM_WORDS * 4) {
		return false;
	}
	struct bignum m, base, exp, two, result;
	bn_from_bytes(&m, modulus, modulus_len);
	bn_from_bytes(&base, a, a_len);
	if (bn_is_zero(&m)) return false;

	bn_zero(&two);
	two.word[0] = 2;
	bn_sub(&exp, &m, &two); /* exp = m - 2 */

	bn_modexp(&result, &base, &exp, &m);
	bn_to_bytes(&result, out, out_len);
	return true;
}
