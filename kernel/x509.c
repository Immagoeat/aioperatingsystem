/* x509.c - just enough X.509/ASN.1 DER parsing to pull an RSA public
 * key (and the certificate's subject common name) out of a TLS
 * server's certificate.
 *
 * Real, honest limitation stated plainly: this does NOT validate the
 * certificate's signature against a trusted root CA, and there is no
 * root CA store here at all. A genuine chain-of-trust validation (walk
 * the chain, verify each signature, check validity dates, check
 * revocation) is real, substantial additional work - a from-scratch
 * root store plus RSA/ECDSA signature verification against it - well
 * beyond "get a browser fetching pages over TLS" scope. What this
 * client actually does is check that the certificate's subject name
 * matches the hostname being connected to (so it's at least the right
 * *shape* of protection against being pointed at an unrelated
 * server's cert), extract a real RSA public key to actually encrypt
 * the pre-master secret with, and nothing more. This is meaningfully
 * weaker than a real browser's TLS stack - stated here rather than
 * silently pretended away - closer to TLS's confidentiality property
 * (an eavesdropper can't read the traffic) than its full authentication
 * property (this doesn't prove the server is who a trusted CA says it
 * is). */
#include "kernel.h"

/* ASN.1 DER tag bytes actually encountered walking a real X.509v3 cert
 * with an RSA key - not a general ASN.1 parser, just these. */
#define ASN1_INTEGER          0x02
#define ASN1_BIT_STRING       0x03
#define ASN1_OCTET_STRING     0x04
#define ASN1_OID              0x06
#define ASN1_UTF8_STRING      0x0C
#define ASN1_PRINTABLE_STRING 0x13
#define ASN1_SEQUENCE         0x30
#define ASN1_SET              0x31

struct asn1_cursor {
	const uint8_t *data;
	uint32_t len;
	uint32_t pos;
};

/* Reads one TLV (tag-length-value) header at the cursor, advancing
 * past the header so out_content_offset/out_content_len describe just
 * the value bytes. Handles DER's short-form and long-form length
 * encodings (long-form: high bit of the first length byte set, low 7
 * bits say how many following bytes make up the actual length,
 * big-endian) - both are common in real certificates depending on
 * field size. */
static bool asn1_read_tlv(struct asn1_cursor *c, uint8_t *out_tag, uint32_t *out_content_offset, uint32_t *out_content_len) {
	if (c->pos + 2 > c->len) return false;

	*out_tag = c->data[c->pos];
	uint8_t first_len_byte = c->data[c->pos + 1];
	uint32_t header_len = 2;
	uint32_t content_len;

	if (first_len_byte & 0x80) {
		uint32_t num_len_bytes = first_len_byte & 0x7F;
		if (num_len_bytes == 0 || num_len_bytes > 4) return false; /* indefinite-length (BER only, not valid DER) or unreasonably large */
		if (c->pos + 2 + num_len_bytes > c->len) return false;
		content_len = 0;
		for (uint32_t i = 0; i < num_len_bytes; i++) {
			content_len = (content_len << 8) | c->data[c->pos + 2 + i];
		}
		header_len = 2 + num_len_bytes;
	} else {
		content_len = first_len_byte;
	}

	if (c->pos + header_len + content_len > c->len) return false;

	*out_content_offset = c->pos + header_len;
	*out_content_len = content_len;
	c->pos += header_len + content_len;
	return true;
}

/* Steps into a constructed type's content (SEQUENCE/SET) as a fresh
 * sub-cursor scoped to just that content - the natural way to walk
 * ASN.1's nested structure without a general tree-building parser. */
static struct asn1_cursor asn1_enter(const struct asn1_cursor *parent, uint32_t offset, uint32_t len) {
	struct asn1_cursor sub;
	sub.data = parent->data;
	sub.len = offset + len;
	sub.pos = offset;
	return sub;
}

/* RSA public keys inside a certificate are themselves DER-encoded as
 * their own SEQUENCE { INTEGER modulus, INTEGER publicExponent },
 * wrapped in a BIT STRING inside SubjectPublicKeyInfo - this unwraps
 * that inner structure specifically. */
static bool parse_rsa_public_key(const uint8_t *data, uint32_t len, struct x509_pubkey *out) {
	struct asn1_cursor c = { data, len, 0 };
	uint8_t tag; uint32_t off, content_len;

	if (!asn1_read_tlv(&c, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false;
	struct asn1_cursor seq = asn1_enter(&c, off, content_len);

	if (!asn1_read_tlv(&seq, &tag, &off, &content_len) || tag != ASN1_INTEGER) return false;
	/* a leading 0x00 byte is standard DER practice to keep an
	 * otherwise-high-bit-set INTEGER unambiguously non-negative -
	 * strip it if present so the modulus is exactly the RSA key size */
	uint32_t mod_off = off, mod_len = content_len;
	if (mod_len > 0 && data[mod_off] == 0x00) { mod_off++; mod_len--; }
	if (mod_len > sizeof(out->modulus)) return false;
	memcpy(out->modulus, data + mod_off, mod_len);
	out->modulus_len = mod_len;

	if (!asn1_read_tlv(&seq, &tag, &off, &content_len) || tag != ASN1_INTEGER) return false;
	uint32_t exp_off = off, exp_len = content_len;
	if (exp_len > 0 && data[exp_off] == 0x00) { exp_off++; exp_len--; }
	if (exp_len > sizeof(out->exponent)) return false;
	memcpy(out->exponent, data + exp_off, exp_len);
	out->exponent_len = exp_len;

	return true;
}

/* Walks a Name (RDNSequence - SET OF RelativeDistinguishedName, each a
 * SET OF AttributeTypeAndValue) looking specifically for the Common
 * Name attribute (OID 2.5.4.3, DER-encoded as bytes 55 04 03) and
 * copies its string value out. Real X.509 names can have many
 * attribute types; this client only ever needs CN for a hostname
 * check, so nothing else is extracted. */
static void find_common_name(const uint8_t *data, uint32_t offset, uint32_t len, char *out, uint32_t out_max) {
	out[0] = '\0';
	struct asn1_cursor rdn_seq = { data, offset + len, offset };

	uint8_t tag; uint32_t off, content_len;
	while (asn1_read_tlv(&rdn_seq, &tag, &off, &content_len)) {
		if (tag != ASN1_SET) continue;
		struct asn1_cursor atv_set = asn1_enter(&rdn_seq, off, content_len);

		uint8_t atv_tag; uint32_t atv_off, atv_len;
		if (!asn1_read_tlv(&atv_set, &atv_tag, &atv_off, &atv_len) || atv_tag != ASN1_SEQUENCE) continue;
		struct asn1_cursor atv = asn1_enter(&atv_set, atv_off, atv_len);

		uint8_t oid_tag; uint32_t oid_off, oid_len;
		if (!asn1_read_tlv(&atv, &oid_tag, &oid_off, &oid_len) || oid_tag != ASN1_OID) continue;

		bool is_cn = (oid_len == 3 && data[oid_off] == 0x55 && data[oid_off + 1] == 0x04 && data[oid_off + 2] == 0x03);
		if (!is_cn) continue;

		uint8_t val_tag; uint32_t val_off, val_len;
		if (!asn1_read_tlv(&atv, &val_tag, &val_off, &val_len)) continue;
		if (val_tag != ASN1_UTF8_STRING && val_tag != ASN1_PRINTABLE_STRING) continue;

		uint32_t copy_len = val_len < out_max - 1 ? val_len : out_max - 1;
		memcpy(out, data + val_off, copy_len);
		out[copy_len] = '\0';
		return;
	}
}

bool x509_parse_certificate(const uint8_t *der, uint32_t der_len, struct x509_cert *out) {
	memset(out, 0, sizeof(*out));

	struct asn1_cursor c = { der, der_len, 0 };
	uint8_t tag; uint32_t off, content_len;

	/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue } */
	if (!asn1_read_tlv(&c, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false;
	struct asn1_cursor cert = asn1_enter(&c, off, content_len);

	/* TBSCertificate ::= SEQUENCE { [0] version, serialNumber, signature,
	 * issuer, validity, subject, subjectPublicKeyInfo, ... } */
	if (!asn1_read_tlv(&cert, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false;
	struct asn1_cursor tbs = asn1_enter(&cert, off, content_len);

	/* version is an explicitly-tagged [0] context-specific field
	 * (tag byte 0xA0) - optional in principle, but every real X.509v3
	 * cert includes it; skip over whatever's there without needing to
	 * decode it since this client doesn't need the version number itself */
	uint32_t save_pos = tbs.pos;
	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len)) return false;
	if (tag != 0xA0) tbs.pos = save_pos; /* no version field present - rewind, this TLV was actually serialNumber */

	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_INTEGER) return false; /* serialNumber */
	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* signature (algorithm identifier) */
	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* issuer */
	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* validity - not checked (no clock this client trusts for revocation-style checks anyway; see file comment on what's NOT validated) */

	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* subject */
	find_common_name(der, off, content_len, out->subject_cn, sizeof(out->subject_cn));

	if (!asn1_read_tlv(&tbs, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* subjectPublicKeyInfo */
	struct asn1_cursor spki = asn1_enter(&tbs, off, content_len);

	if (!asn1_read_tlv(&spki, &tag, &off, &content_len) || tag != ASN1_SEQUENCE) return false; /* algorithm identifier (rsaEncryption OID + NULL params) - not checked; if it's not actually RSA, the BIT STRING below simply won't parse as one and this returns false */
	if (!asn1_read_tlv(&spki, &tag, &off, &content_len) || tag != ASN1_BIT_STRING) return false; /* the actual key, DER-encoded, wrapped in a BIT STRING */

	/* a BIT STRING's first content byte is the "number of unused bits
	 * in the last byte" - always 0x00 for a byte-aligned DER encoding
	 * like an RSA key, so skip it */
	if (content_len < 1 || der[off] != 0x00) return false;
	if (!parse_rsa_public_key(der + off + 1, content_len - 1, &out->pubkey)) return false;

	return true;
}
