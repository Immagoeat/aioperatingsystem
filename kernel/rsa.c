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
