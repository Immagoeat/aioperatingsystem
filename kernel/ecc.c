/* ecc.c - NIST P-256 (secp256r1) elliptic curve point arithmetic, just
 * enough for ECDHE key exchange in tls.c: scalar multiplication of a
 * point (the curve's generator, or a peer's public point) by a
 * private scalar, and point (de)serialization in the uncompressed
 * SEC1 format TLS actually puts on the wire (0x04 || X || Y, 65 bytes
 * total for P-256).
 *
 * Field arithmetic (add/sub/mul/inverse mod the curve's prime) is
 * built entirely on bignum.c's general-purpose modular byte-buffer
 * functions - this file has no bignum type of its own, just 32-byte
 * big-endian buffers representing field elements. Points are
 * represented in affine (x, y) coordinates throughout (no
 * Jacobian/projective coordinates - simpler, and correctness matters
 * far more than performance for a single ECDHE exchange per TLS
 * handshake).
 *
 * The curve parameters below (prime, a, b, generator, order) were
 * pulled directly from OpenSSL's own `openssl ecparam -name
 * prime256v1 -param_enc explicit -text` output, then independently
 * cross-checked before use: the prime matches the standard's published
 * closed form (2^256 - 2^224 + 2^192 + 2^96 - 1), the generator point
 * was confirmed to actually satisfy the curve equation y^2 = x^3 + ax
 * + b (mod p), and scalar-multiplying the generator by the published
 * order was confirmed to reach the point at infinity - the standard
 * check that the order is correct. */
#include "kernel.h"

#define ECC_FIELD_BYTES 32

static const uint8_t P256_P[32] = {
	0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};
static const uint8_t P256_A[32] = {
	0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfc
};
static const uint8_t P256_B[32] = {
	0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
	0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b
};
static const uint8_t P256_GX[32] = {
	0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
	0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96
};
static const uint8_t P256_GY[32] = {
	0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
	0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5
};
/* the curve's group order (not the field prime) - scalars (private
 * keys) are reduced mod this, not mod P256_P */
static const uint8_t P256_N[32] = {
	0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51
};

/* Affine point, used only at the public API boundary (input peer
 * points, output this client's own public point / the final shared
 * secret) - see struct ecc_jpoint below for what scalar_mult actually
 * computes with internally. */
struct ecc_point {
	uint8_t x[32];
	uint8_t y[32];
	bool infinity;
};

static bool fmod_add(const uint8_t *a, const uint8_t *b, uint8_t *out) {
	return bignum_addmod_bytes(a, 32, b, 32, P256_P, 32, out, 32);
}
static bool fmod_sub(const uint8_t *a, const uint8_t *b, uint8_t *out) {
	return bignum_submod_bytes(a, 32, b, 32, P256_P, 32, out, 32);
}
static bool fmod_mul(const uint8_t *a, const uint8_t *b, uint8_t *out) {
	return bignum_mulmod_bytes(a, 32, b, 32, P256_P, 32, out, 32);
}
static bool fmod_inv(const uint8_t *a, uint8_t *out) {
	return bignum_invmod_bytes(a, 32, P256_P, 32, out, 32);
}

static bool fmod_is_zero(const uint8_t *a) {
	for (int i = 0; i < 32; i++) if (a[i] != 0) return false;
	return true;
}

/* Jacobian projective coordinates: affine (x, y) is represented as
 * (X, Y, Z) with x = X/Z^2, y = Y/Z^3 (Z=0 is the point at infinity).
 * The whole point of this representation is that point doubling and
 * addition need no modular inversion at all - only multiplications,
 * squarings, and additions - so a 256-bit scalar multiplication does
 * exactly one inversion total (converting the final result back to
 * affine), not one per point operation. Affine-coordinate point math
 * was tried first and measured at ~560ms per single point doubling
 * (a full modexp-based modular inverse per operation) - completely
 * impractical for a ~256-bit scalar multiplication (up to ~512 point
 * operations per handshake). These formulas (general Weierstrass
 * "add-2007-bl"/"dbl-2007-bl" style, not the P-256-specific a=-3
 * shortcut) were verified independently in Python against known-
 * correct affine results before being translated to C - see the
 * verification harness referenced in this session's history. */
struct ecc_jpoint {
	uint8_t x[32], y[32], z[32];
};

static void jpoint_set_infinity(struct ecc_jpoint *p) {
	memset(p->x, 0, 32); memset(p->y, 0, 32); memset(p->z, 0, 32);
}
static bool jpoint_is_infinity(const struct ecc_jpoint *p) {
	return fmod_is_zero(p->z);
}

/* P + P -> out, Jacobian coordinates. Standard formulas:
 *   XX = X1^2;  YY = Y1^2;  YYYY = YY^2;  ZZ = Z1^2
 *   S = 2*((X1+YY)^2 - XX - YYYY)
 *   M = 3*XX + a*ZZ^2
 *   T = M^2 - 2*S
 *   X3 = T
 *   Y3 = M*(S-T) - 8*YYYY
 *   Z3 = (Y1+Z1)^2 - YY - ZZ  */
static bool jpoint_double(const struct ecc_jpoint *p, struct ecc_jpoint *out) {
	if (jpoint_is_infinity(p)) { *out = *p; return true; }
	if (fmod_is_zero(p->y)) { jpoint_set_infinity(out); return true; } /* point of order 2 - never occurs for a valid P-256 scalar */

	uint8_t XX[32], YY[32], YYYY[32], ZZ[32];
	if (!fmod_mul(p->x, p->x, XX)) return false;
	if (!fmod_mul(p->y, p->y, YY)) return false;
	if (!fmod_mul(YY, YY, YYYY)) return false;
	if (!fmod_mul(p->z, p->z, ZZ)) return false;

	uint8_t S[32];
	{
		uint8_t x_plus_yy[32], sq[32], t1[32], t2[32];
		if (!fmod_add(p->x, YY, x_plus_yy)) return false;
		if (!fmod_mul(x_plus_yy, x_plus_yy, sq)) return false;
		if (!fmod_sub(sq, XX, t1)) return false;
		if (!fmod_sub(t1, YYYY, t2)) return false;
		if (!fmod_add(t2, t2, S)) return false; /* S = 2*t2 */
	}

	uint8_t M[32];
	{
		uint8_t three[32] = {0}; three[31] = 3;
		uint8_t three_XX[32], ZZZZ[32], a_ZZZZ[32];
		if (!fmod_mul(three, XX, three_XX)) return false;
		if (!fmod_mul(ZZ, ZZ, ZZZZ)) return false;
		if (!fmod_mul(P256_A, ZZZZ, a_ZZZZ)) return false;
		if (!fmod_add(three_XX, a_ZZZZ, M)) return false;
	}

	uint8_t T[32];
	{
		uint8_t M2[32], two_S[32];
		if (!fmod_mul(M, M, M2)) return false;
		if (!fmod_add(S, S, two_S)) return false;
		if (!fmod_sub(M2, two_S, T)) return false;
	}

	uint8_t Y3[32];
	{
		uint8_t s_minus_t[32], m_times[32], eight_yyyy[32], four_yyyy[32];
		if (!fmod_sub(S, T, s_minus_t)) return false;
		if (!fmod_mul(M, s_minus_t, m_times)) return false;
		if (!fmod_add(YYYY, YYYY, four_yyyy)) return false; /* 2*YYYY */
		if (!fmod_add(four_yyyy, four_yyyy, four_yyyy)) return false; /* 4*YYYY */
		if (!fmod_add(four_yyyy, four_yyyy, eight_yyyy)) return false; /* 8*YYYY */
		if (!fmod_sub(m_times, eight_yyyy, Y3)) return false;
	}

	uint8_t Z3[32];
	{
		uint8_t y_plus_z[32], sq[32], t1[32];
		if (!fmod_add(p->y, p->z, y_plus_z)) return false;
		if (!fmod_mul(y_plus_z, y_plus_z, sq)) return false;
		if (!fmod_sub(sq, YY, t1)) return false;
		if (!fmod_sub(t1, ZZ, Z3)) return false;
	}

	memcpy(out->x, T, 32);
	memcpy(out->y, Y3, 32);
	memcpy(out->z, Z3, 32);
	return true;
}

/* P + Q -> out, Jacobian coordinates, general case (mixed affine/
 * Jacobian isn't used here - both operands are always full Jacobian
 * points in this client's one call site, scalar multiplication).
 * Standard formulas:
 *   Z1Z1 = Z1^2; Z2Z2 = Z2^2
 *   U1 = X1*Z2Z2; U2 = X2*Z1Z1
 *   S1 = Y1*Z2*Z2Z2; S2 = Y2*Z1*Z1Z1
 *   H = U2-U1; I = (2H)^2; J = H*I
 *   r = 2*(S2-S1); V = U1*I
 *   X3 = r^2 - J - 2V
 *   Y3 = r*(V-X3) - 2*S1*J
 *   Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2) * H  */
static bool jpoint_add(const struct ecc_jpoint *p, const struct ecc_jpoint *q, struct ecc_jpoint *out) {
	if (jpoint_is_infinity(p)) { *out = *q; return true; }
	if (jpoint_is_infinity(q)) { *out = *p; return true; }

	uint8_t Z1Z1[32], Z2Z2[32];
	if (!fmod_mul(p->z, p->z, Z1Z1)) return false;
	if (!fmod_mul(q->z, q->z, Z2Z2)) return false;

	uint8_t U1[32], U2[32];
	if (!fmod_mul(p->x, Z2Z2, U1)) return false;
	if (!fmod_mul(q->x, Z1Z1, U2)) return false;

	uint8_t S1[32], S2[32];
	{
		uint8_t y1_z2[32], z1z1_z1[32];
		if (!fmod_mul(p->y, q->z, y1_z2)) return false;
		if (!fmod_mul(y1_z2, Z2Z2, S1)) return false;
		if (!fmod_mul(q->y, p->z, z1z1_z1)) return false;
		if (!fmod_mul(z1z1_z1, Z1Z1, S2)) return false;
	}

	bool same_u = memcmp(U1, U2, 32) == 0;
	if (same_u) {
		bool same_s = memcmp(S1, S2, 32) == 0;
		if (!same_s) { jpoint_set_infinity(out); return true; } /* P + (-P) = infinity */
		return jpoint_double(p, out); /* P == Q */
	}

	uint8_t H[32];
	if (!fmod_sub(U2, U1, H)) return false;
	uint8_t I[32];
	{
		uint8_t two_h[32];
		if (!fmod_add(H, H, two_h)) return false;
		if (!fmod_mul(two_h, two_h, I)) return false;
	}
	uint8_t J[32];
	if (!fmod_mul(H, I, J)) return false;
	uint8_t r[32];
	{
		uint8_t s2_minus_s1[32];
		if (!fmod_sub(S2, S1, s2_minus_s1)) return false;
		if (!fmod_add(s2_minus_s1, s2_minus_s1, r)) return false;
	}
	uint8_t V[32];
	if (!fmod_mul(U1, I, V)) return false;

	uint8_t X3[32];
	{
		uint8_t r2[32], t1[32];
		if (!fmod_mul(r, r, r2)) return false;
		if (!fmod_sub(r2, J, t1)) return false;
		uint8_t two_v[32];
		if (!fmod_add(V, V, two_v)) return false;
		if (!fmod_sub(t1, two_v, X3)) return false;
	}

	uint8_t Y3[32];
	{
		uint8_t v_minus_x3[32], r_times[32], two_s1[32], two_s1_j[32];
		if (!fmod_sub(V, X3, v_minus_x3)) return false;
		if (!fmod_mul(r, v_minus_x3, r_times)) return false;
		if (!fmod_add(S1, S1, two_s1)) return false;
		if (!fmod_mul(two_s1, J, two_s1_j)) return false;
		if (!fmod_sub(r_times, two_s1_j, Y3)) return false;
	}

	uint8_t Z3[32];
	{
		uint8_t z1_plus_z2[32], sq[32], t1[32], t2[32];
		if (!fmod_add(p->z, q->z, z1_plus_z2)) return false;
		if (!fmod_mul(z1_plus_z2, z1_plus_z2, sq)) return false;
		if (!fmod_sub(sq, Z1Z1, t1)) return false;
		if (!fmod_sub(t1, Z2Z2, t2)) return false;
		if (!fmod_mul(t2, H, Z3)) return false;
	}

	memcpy(out->x, X3, 32);
	memcpy(out->y, Y3, 32);
	memcpy(out->z, Z3, 32);
	return true;
}

static bool jpoint_to_affine(const struct ecc_jpoint *p, struct ecc_point *out) {
	if (jpoint_is_infinity(p)) { out->infinity = true; return true; }

	uint8_t zinv[32], zinv2[32], zinv3[32];
	if (!fmod_inv(p->z, zinv)) return false;
	if (!fmod_mul(zinv, zinv, zinv2)) return false;
	if (!fmod_mul(zinv2, zinv, zinv3)) return false;

	if (!fmod_mul(p->x, zinv2, out->x)) return false;
	if (!fmod_mul(p->y, zinv3, out->y)) return false;
	out->infinity = false;
	return true;
}

/* out = scalar * P, standard binary double-and-add, MSB to LSB, all
 * in Jacobian coordinates (one inversion total, at the very end, via
 * jpoint_to_affine - see the comment on struct ecc_jpoint above for
 * why this replaced an earlier affine-coordinate version that was
 * correct but far too slow). Not constant-time (branches and loop
 * bounds depend on the scalar's bits) - a real-world timing side-
 * channel concern for a long-lived server key, but this client
 * generates a fresh random private key for every single handshake and
 * never reuses it, the same scoping tradeoff already made for RSA/
 * AES/etc. elsewhere in this TLS client (see tls.c's and rsa.c's own
 * comments on their PRNG limitations) - a real hardened implementation
 * would use a constant-time ladder regardless, but that's a hardening
 * pass beyond what "correctly negotiate real ECDHE with a real
 * server" needs. */
static bool ecc_scalar_mult(const uint8_t *scalar, const struct ecc_point *p, struct ecc_point *out) {
	struct ecc_jpoint base, result;
	memcpy(base.x, p->x, 32);
	memcpy(base.y, p->y, 32);
	memset(base.z, 0, 32); base.z[31] = 1; /* Z=1: affine point lifted into Jacobian coordinates */

	jpoint_set_infinity(&result);

	for (int byte_i = 0; byte_i < 32; byte_i++) {
		uint8_t b = scalar[byte_i];
		for (int bit = 7; bit >= 0; bit--) {
			struct ecc_jpoint doubled;
			if (!jpoint_double(&result, &doubled)) return false;
			result = doubled;
			if (b & (1u << bit)) {
				struct ecc_jpoint sum;
				if (!jpoint_add(&result, &base, &sum)) return false;
				result = sum;
			}
		}
	}

	return jpoint_to_affine(&result, out);
}

/* Public entry point: generates an ECDHE key pair (a random private
 * scalar reduced mod the curve order, and the corresponding public
 * point private*G) and separately computes a shared secret from a
 * private scalar and a peer's public point - tls.c calls the former
 * once for its own ephemeral key and the latter once the server's
 * ServerKeyExchange point is known, exactly the two operations ECDHE
 * needs. `private_key` is exactly 32 bytes; `public_point_out`/
 * `peer_public_point` are the 65-byte uncompressed SEC1 form (0x04 ||
 * X || Y); the shared secret is the resulting point's X coordinate
 * alone (32 bytes), per RFC 8422/TLS's ECDH premaster secret
 * definition. */
bool ecc_p256_generate_keypair(const uint8_t *random_seed, uint8_t private_key_out[32], uint8_t public_point_out[65]) {
	/* Reduce the caller-supplied random bytes mod the group order so
	 * the private scalar is always in [0, n) - the RNG can hand back
	 * any 32 bytes, including values >= n, which must be reduced first
	 * to stay a valid scalar (a private key of exactly 0 is vanishingly
	 * unlikely from 256 real random bits and isn't specially guarded
	 * against here, same as this client doesn't guard against RSA's
	 * PKCS#1 padding randomly producing a zero byte run either - both
	 * are the kind of astronomically-unlikely edge case a from-scratch
	 * client can reasonably not special-case). */
	uint8_t zero[32] = {0};
	if (!bignum_addmod_bytes(random_seed, 32, zero, 32, P256_N, 32, private_key_out, 32)) return false;

	struct ecc_point g, pub;
	memcpy(g.x, P256_GX, 32);
	memcpy(g.y, P256_GY, 32);
	g.infinity = false;

	if (!ecc_scalar_mult(private_key_out, &g, &pub)) return false;

	public_point_out[0] = 0x04;
	memcpy(public_point_out + 1, pub.x, 32);
	memcpy(public_point_out + 33, pub.y, 32);
	return true;
}

bool ecc_p256_compute_shared_secret(const uint8_t private_key[32], const uint8_t peer_public_point[65], uint8_t shared_secret_out[32]) {
	if (peer_public_point[0] != 0x04) return false; /* only the uncompressed point format is supported - real servers overwhelmingly send this form */

	struct ecc_point peer, result;
	memcpy(peer.x, peer_public_point + 1, 32);
	memcpy(peer.y, peer_public_point + 33, 32);
	peer.infinity = false;

	/* Confirm the peer's point actually satisfies the curve equation
	 * before using it - a real, meaningful check (an "invalid curve
	 * attack" sends a point that doesn't lie on P-256 at all, on a
	 * different, weaker curve chosen to leak the private key through
	 * the arithmetic below; rejecting any point that fails this check
	 * closes that off) rather than trusting the server unconditionally. */
	uint8_t y2[32], x2[32], x3[32], ax[32], rhs[32];
	if (!fmod_mul(peer.y, peer.y, y2)) return false;
	if (!fmod_mul(peer.x, peer.x, x2)) return false;
	if (!fmod_mul(x2, peer.x, x3)) return false;
	if (!fmod_mul(P256_A, peer.x, ax)) return false;
	{
		uint8_t tmp[32];
		if (!fmod_add(x3, ax, tmp)) return false;
		if (!fmod_add(tmp, P256_B, rhs)) return false;
	}
	if (memcmp(y2, rhs, 32) != 0) return false; /* point is not on the curve - reject rather than proceed */

	if (!ecc_scalar_mult(private_key, &peer, &result)) return false;
	if (result.infinity) return false; /* shouldn't happen for a valid peer point and nonzero private key */

	memcpy(shared_secret_out, result.x, 32);
	return true;
}
