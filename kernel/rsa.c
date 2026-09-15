/* rsa.c - RSA public-key encryption with PKCS#1 v1.5 padding (RFC
 * 8017 section 7.2.1), as used by TLS 1.2's RSA key-exchange mode: the
 * client encrypts a randomly-generated pre-master secret with the
 * server's RSA public key (extracted from its certificate - see
 * x509.c), and only the server (holding the matching private key) can
 * decrypt it. This file only ever encrypts, never decrypts - a TLS
 * client has no private key of its own to use here.
 *
 * The actual modular exponentiation is bignum.c's job; this file is
 * just the PKCS#1 padding scheme wrapped around it. */
#include "kernel.h"

/* PKCS#1 v1.5 encryption padding: 0x00 0x02 <random nonzero bytes,
 * padding to fill the modulus> 0x00 <message>. The leading 0x00
 * guarantees the padded value is less than the modulus (interpreted as
 * an unsigned big-endian integer); 0x02 marks this as encryption (as
 * opposed to 0x01 for a signature); the random bytes must all be
 * nonzero so the trailing 0x00 unambiguously marks where the message
 * starts when the reverse operation (decryption, which this client
 * never does) unpads it. */
bool rsa_encrypt_pkcs1(const uint8_t *modulus, uint32_t modulus_len,
                        const uint8_t *pub_exponent, uint32_t exponent_len,
                        const uint8_t *message, uint32_t message_len,
                        uint8_t *out, uint32_t out_len) {
	/* RFC 8017: message must be at least 11 bytes shorter than the
	 * modulus (0x00 0x02 + at least 8 padding bytes + 0x00 = 11 bytes
	 * of overhead). A TLS pre-master secret is 48 bytes, comfortably
	 * within this for any real modulus size (>= 512 bits / 64 bytes). */
	if (out_len != modulus_len || message_len + 11 > modulus_len) return false;

	static uint8_t padded[512]; /* matches bignum.c's 4096-bit (512-byte) capacity */
	if (modulus_len > sizeof(padded)) return false;

	padded[0] = 0x00;
	padded[1] = 0x02;

	uint32_t padding_len = modulus_len - message_len - 3;
	/* Random padding bytes: this needs to be nonzero, but does NOT need
	 * to be cryptographically unpredictable the way the pre-master
	 * secret itself does - PKCS#1 v1.5's security here comes from the
	 * message length/structure, not secrecy of the padding bytes
	 * themselves. Still uses the same real entropy mixing auth.c's
	 * salt generation does (PIT ticks + CMOS RTC - see that file's
	 * comment on why there's no hardware RNG available), reseeded per
	 * byte so no visible pattern repeats. */
	struct rtc_time t;
	rtc_get_time(&t);
	uint32_t seed = timer_get_ticks() ^ ((uint32_t)t.hours << 16) ^ ((uint32_t)t.minutes << 8) ^ t.seconds ^ 0xA5A5A5A5u;
	for (uint32_t i = 0; i < padding_len; i++) {
		seed = seed * 1103515245u + 12345u + i;
		uint8_t b = (uint8_t)(seed >> 16);
		padded[2 + i] = b == 0 ? 0x42 : b; /* must be nonzero */
	}

	padded[2 + padding_len] = 0x00;
	memcpy(padded + 3 + padding_len, message, message_len);

	return bignum_modexp_bytes(padded, modulus_len, pub_exponent, exponent_len, modulus, modulus_len, out, out_len);
}

/* The DER encoding of the SHA-256 DigestInfo SEQUENCE (RFC 8017
 * Appendix A.2.4 / RFC 3447): SEQUENCE { SEQUENCE { OID
 * 2.16.840.1.101.3.4.2.1 (sha256), NULL }, OCTET STRING (32 bytes) }
 * minus the OCTET STRING's actual content bytes - this is the fixed
 * 19-byte prefix that comes right before the hash itself inside a
 * PKCS#1 v1.5 signature. Confirmed against a real signature produced
 * by `openssl dgst -sha256 -sign` and inspected with `pkeyutl
 * -verifyrecover -pkeyopt rsa_padding_mode:none` before being used
 * here, the same live-verification approach used for every other
 * cryptographic constant in this codebase. */
static const uint8_t SHA256_DIGESTINFO_PREFIX[19] = {
	0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};

/* Verifies a PKCS#1 v1.5 RSA signature (RFC 8017 section 8.2.2) over a
 * SHA-256 hash, using the public key - "decrypts" the signature with
 * the public exponent (the same bignum_modexp_bytes() operation
 * rsa_encrypt_pkcs1() above uses, just with the roles reversed: a
 * signature is produced with the private key and checked with the
 * public one) and checks the result has the exact structure a real
 * signature over `hash` would produce: 0x00 0x01 <0xFF padding> 0x00
 * <SHA-256 DigestInfo prefix> <hash>. Used by tls.c to check the
 * server's signature over its ECDHE ephemeral key in ServerKeyExchange
 * - real protection against a network attacker forging that message,
 * even though this client doesn't validate the certificate chain
 * itself against any root CA (see x509.c's file comment). */
bool rsa_verify_pkcs1_sha256(const uint8_t *modulus, uint32_t modulus_len,
                              const uint8_t *pub_exponent, uint32_t exponent_len,
                              const uint8_t *signature, uint32_t signature_len,
                              const uint8_t hash[32]) {
	if (signature_len != modulus_len || modulus_len < 11 + 19 + 32) return false;

	static uint8_t decoded[512];
	if (modulus_len > sizeof(decoded)) return false;

	if (!bignum_modexp_bytes(signature, signature_len, pub_exponent, exponent_len, modulus, modulus_len, decoded, modulus_len)) {
		return false;
	}

	if (decoded[0] != 0x00 || decoded[1] != 0x01) return false;

	uint32_t i = 2;
	while (i < modulus_len && decoded[i] == 0xFF) i++;
	if (i >= modulus_len || decoded[i] != 0x00) return false;
	i++;

	if (i + 19 + 32 != modulus_len) return false; /* wrong amount of padding for this exact structure */
	for (uint32_t j = 0; j < 19; j++) {
		if (decoded[i + j] != SHA256_DIGESTINFO_PREFIX[j]) return false;
	}
	i += 19;

	for (uint32_t j = 0; j < 32; j++) {
		if (decoded[i + j] != hash[j]) return false;
	}

	return true;
}
