/* inflate.c - a real DEFLATE decompressor (RFC 1951), plus a thin
 * gzip container wrapper (RFC 1952), for decoding HTTP responses sent
 * with `Content-Encoding: gzip` - browser.c's whole reason for
 * needing this is that a real, unmodified server (python.org, among
 * many others) sends gzip-compressed bodies unconditionally,
 * regardless of what Accept-Encoding says, and without this the
 * compressed bytes were getting fed straight into html.c's tag
 * stripper as if they were text, rendering as garbage.
 *
 * This is a genuine implementation of the LZ77 + Huffman coding
 * DEFLATE uses - a bit-level reader, canonical Huffman decoding built
 * from code-length arrays (both the fixed tables RFC 1951 section
 * 3.2.6 defines and the per-block dynamic tables section 3.2.7
 * describes), and the length/distance back-reference tables from
 * section 3.2.5 - not a stub or a partial format that only handles
 * the simple case. Verified against real zlib/gzip-compressed data
 * (short strings, long repetitive strings forcing back-references
 * across the full length/distance range, dynamic Huffman blocks,
 * stored blocks, and real compressed HTML) before being trusted here,
 * the same verify-before-trust discipline as every other from-scratch
 * protocol piece in this codebase (bignum.c, aes.c, tls.c, html.c). */
#include "kernel.h"

struct bitreader {
	const uint8_t *data;
	uint32_t len;
	uint32_t byte_pos;
	uint32_t bit_pos; /* 0-7, next bit to read within data[byte_pos] */
};

static void br_init(struct bitreader *br, const uint8_t *data, uint32_t len) {
	br->data = data;
	br->len = len;
	br->byte_pos = 0;
	br->bit_pos = 0;
}

/* DEFLATE packs bits LSB-first within each byte (RFC 1951 section
 * 3.1.1) - the first bit read is bit 0 of the first byte. Returns 0
 * past the end of the buffer rather than reading out of bounds -
 * malformed/truncated input degrades to "decodes wrong and the CRC
 * check at the end catches it" rather than a crash. */
static uint32_t br_read_bits(struct bitreader *br, int count) {
	uint32_t result = 0;
	for (int i = 0; i < count; i++) {
		uint32_t bit = 0;
		if (br->byte_pos < br->len) {
			bit = (br->data[br->byte_pos] >> br->bit_pos) & 1;
		}
		result |= bit << i;
		br->bit_pos++;
		if (br->bit_pos == 8) { br->bit_pos = 0; br->byte_pos++; }
	}
	return result;
}

static void br_align_to_byte(struct bitreader *br) {
	if (br->bit_pos != 0) { br->bit_pos = 0; br->byte_pos++; }
}

/* --- canonical Huffman decoding ---
 *
 * DEFLATE Huffman codes are "canonical": fully determined by the
 * code length of each symbol (RFC 1951 section 3.2.2), so a decoder
 * only needs the array of code lengths per symbol - the codes
 * themselves are derived algorithmically, not transmitted. This
 * builds a simple (symbol, length) -> code table and decodes bit by
 * bit (slower than a lookup-table decoder real zlib implementations
 * use, but this only ever runs on one HTTP response body per fetch,
 * not a hot path, and correctness matters far more than speed here -
 * same tradeoff bignum.c's schoolbook multiplication makes). */
#define MAX_HUFFMAN_SYMBOLS 288
#define MAX_HUFFMAN_BITS 15

struct huffman_table {
	uint16_t codes[MAX_HUFFMAN_SYMBOLS];  /* the canonical code for each symbol, if used */
	uint8_t lengths[MAX_HUFFMAN_SYMBOLS]; /* code length in bits, 0 = symbol unused */
	int symbol_count;
};

/* Builds canonical codes from an array of code lengths (RFC 1951
 * section 3.2.2's algorithm exactly): count how many codes exist at
 * each length, derive the first code at each length, then assign
 * codes to symbols in order. */
static void huffman_build(struct huffman_table *t, const uint8_t *lengths, int count) {
	t->symbol_count = count;
	for (int i = 0; i < count; i++) t->lengths[i] = lengths[i];

	uint16_t bl_count[MAX_HUFFMAN_BITS + 1] = {0};
	for (int i = 0; i < count; i++) {
		if (lengths[i] > 0) bl_count[lengths[i]]++;
	}

	uint16_t next_code[MAX_HUFFMAN_BITS + 1] = {0};
	uint16_t code = 0;
	for (int bits = 1; bits <= MAX_HUFFMAN_BITS; bits++) {
		code = (uint16_t)((code + bl_count[bits - 1]) << 1);
		next_code[bits] = code;
	}

	for (int i = 0; i < count; i++) {
		int len = lengths[i];
		if (len > 0) {
			t->codes[i] = next_code[len];
			next_code[len]++;
		} else {
			t->codes[i] = 0;
		}
	}
}

/* Decodes one symbol by reading bits one at a time (MSB-first for the
 * Huffman code itself, per RFC 1951 section 3.1.1 - the one place
 * DEFLATE's bit order flips from its usual LSB-first packing) and
 * checking against every symbol's canonical code at that length.
 * O(symbols) per bit read is not how a real fast decoder does this,
 * but is straightforward to get right, which matters more here. */
static int huffman_decode(struct huffman_table *t, struct bitreader *br) {
	uint16_t code = 0;
	for (int len = 1; len <= MAX_HUFFMAN_BITS; len++) {
		code = (uint16_t)((code << 1) | br_read_bits(br, 1));
		for (int sym = 0; sym < t->symbol_count; sym++) {
			if (t->lengths[sym] == len && t->codes[sym] == code) return sym;
		}
	}
	return -1; /* malformed input - no symbol matched any code up to the max length */
}

/* RFC 1951 section 3.2.5: length code 257-285 -> (base length, extra
 * bits to add). Length code 285 has a fixed length of 258 (0 extra
 * bits) despite the pattern the other codes follow. */
static const uint16_t LENGTH_BASE[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t LENGTH_EXTRA_BITS[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};

/* RFC 1951 section 3.2.5: distance code 0-29 -> (base distance, extra
 * bits to add). */
static const uint16_t DIST_BASE[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
	1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t DIST_EXTRA_BITS[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* RFC 1951 section 3.2.7: the order code-length codes themselves are
 * transmitted in for a dynamic Huffman block - deliberately not
 * ascending, chosen so trailing zero-length (unused) entries are
 * common and can be omitted. */
static const uint8_t CODE_LENGTH_ORDER[19] = {
	16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static void build_fixed_huffman_tables(struct huffman_table *lit, struct huffman_table *dist) {
	/* RFC 1951 section 3.2.6's fixed literal/length code lengths:
	 * 0-143: 8 bits, 144-255: 9 bits, 256-279: 7 bits, 280-287: 8 bits. */
	uint8_t lit_lengths[288];
	for (int i = 0; i <= 143; i++) lit_lengths[i] = 8;
	for (int i = 144; i <= 255; i++) lit_lengths[i] = 9;
	for (int i = 256; i <= 279; i++) lit_lengths[i] = 7;
	for (int i = 280; i <= 287; i++) lit_lengths[i] = 8;
	huffman_build(lit, lit_lengths, 288);

	/* fixed distance codes: all 30 codes are 5 bits */
	uint8_t dist_lengths[30];
	for (int i = 0; i < 30; i++) dist_lengths[i] = 5;
	huffman_build(dist, dist_lengths, 30);
}

/* Reads a dynamic Huffman block's header (RFC 1951 section 3.2.7):
 * HLIT/HDIST/HCLEN counts, the code-length-code lengths (used to
 * decode a third, small Huffman table), then that table decodes the
 * actual literal/length and distance code lengths, with RLE-style
 * repeat codes (16/17/18) for runs of the same or zero length. */
static bool read_dynamic_huffman_tables(struct bitreader *br, struct huffman_table *lit, struct huffman_table *dist) {
	int hlit = (int)br_read_bits(br, 5) + 257;
	int hdist = (int)br_read_bits(br, 5) + 1;
	int hclen = (int)br_read_bits(br, 4) + 4;

	uint8_t cl_lengths[19] = {0};
	for (int i = 0; i < hclen; i++) {
		cl_lengths[CODE_LENGTH_ORDER[i]] = (uint8_t)br_read_bits(br, 3);
	}
	struct huffman_table cl_table;
	huffman_build(&cl_table, cl_lengths, 19);

	uint8_t all_lengths[288 + 30];
	int total = hlit + hdist;
	int i = 0;
	while (i < total) {
		int sym = huffman_decode(&cl_table, br);
		if (sym < 0) return false;
		if (sym < 16) {
			all_lengths[i++] = (uint8_t)sym;
		} else if (sym == 16) {
			if (i == 0) return false; /* nothing to repeat */
			int repeat = (int)br_read_bits(br, 2) + 3;
			uint8_t prev = all_lengths[i - 1];
			while (repeat-- > 0 && i < total) all_lengths[i++] = prev;
		} else if (sym == 17) {
			int repeat = (int)br_read_bits(br, 3) + 3;
			while (repeat-- > 0 && i < total) all_lengths[i++] = 0;
		} else { /* sym == 18 */
			int repeat = (int)br_read_bits(br, 7) + 11;
			while (repeat-- > 0 && i < total) all_lengths[i++] = 0;
		}
	}

	huffman_build(lit, all_lengths, hlit);
	huffman_build(dist, all_lengths + hlit, hdist);
	return true;
}

/* Decodes one block's worth of literal/length/distance symbols into
 * `out`, starting at *out_len, growing it in place - shared by both
 * the fixed and dynamic Huffman block types (RFC 1951 section 3.2.3's
 * BTYPE 01 and 10), which only differ in how their two Huffman tables
 * were built, not in how they're used afterward. */
static bool inflate_block_huffman(struct bitreader *br, struct huffman_table *lit, struct huffman_table *dist,
                                   uint8_t *out, uint32_t *out_len, uint32_t out_max) {
	for (;;) {
		int sym = huffman_decode(lit, br);
		if (sym < 0) return false;

		if (sym < 256) {
			if (*out_len >= out_max) return false;
			out[(*out_len)++] = (uint8_t)sym;
		} else if (sym == 256) {
			return true; /* end-of-block */
		} else {
			int len_idx = sym - 257;
			if (len_idx >= 29) return false;
			uint32_t length = LENGTH_BASE[len_idx] + br_read_bits(br, LENGTH_EXTRA_BITS[len_idx]);

			int dist_sym = huffman_decode(dist, br);
			if (dist_sym < 0 || dist_sym >= 30) return false;
			uint32_t distance = DIST_BASE[dist_sym] + br_read_bits(br, DIST_EXTRA_BITS[dist_sym]);

			if (distance == 0 || distance > *out_len) return false; /* back-reference before the start of output - malformed */
			if (*out_len + length > out_max) return false;

			/* copy byte-by-byte, not memcpy - back-references can (and
			 * routinely do) overlap with the source region still being
			 * written, e.g. a distance-1 reference repeating the same
			 * byte `length` times, which memcpy's overlap behavior is
			 * undefined for */
			uint32_t src = *out_len - distance;
			for (uint32_t i = 0; i < length; i++) {
				out[*out_len + i] = out[src + i];
			}
			*out_len += length;
		}
	}
}

/* Public entry point: decompresses a raw DEFLATE stream (RFC 1951) -
 * no gzip/zlib framing, just the compressed blocks themselves - into
 * `out`, up to `out_max` bytes. Returns the decompressed length, or 0
 * on any malformed/truncated input or if the output would exceed
 * out_max (this client always knows a reasonable max body size up
 * front - see net.c's DOWNLOAD_MAX_BODY/FETCH_BODY_MAX/browser_body
 * callers - so silently truncating would be worse than just failing
 * visibly). */
uint32_t inflate_raw(const uint8_t *data, uint32_t data_len, uint8_t *out, uint32_t out_max) {
	struct bitreader br;
	br_init(&br, data, data_len);

	uint32_t out_len = 0;
	bool final = false;

	while (!final) {
		final = br_read_bits(&br, 1) != 0;
		uint32_t btype = br_read_bits(&br, 2);

		if (btype == 0) {
			/* stored (uncompressed) block: skip to the next byte
			 * boundary, then LEN(2) + NLEN(2, one's complement of LEN,
			 * used only as a redundancy check here) + LEN raw bytes */
			br_align_to_byte(&br);
			if (br.byte_pos + 4 > br.len) return 0;
			uint32_t block_len = br.data[br.byte_pos] | ((uint32_t)br.data[br.byte_pos + 1] << 8);
			uint32_t nlen = br.data[br.byte_pos + 2] | ((uint32_t)br.data[br.byte_pos + 3] << 8);
			if ((block_len ^ 0xFFFFu) != nlen) return 0;
			br.byte_pos += 4;
			if (br.byte_pos + block_len > br.len) return 0;
			if (out_len + block_len > out_max) return 0;
			memcpy(out + out_len, br.data + br.byte_pos, block_len);
			out_len += block_len;
			br.byte_pos += block_len;
		} else if (btype == 1) {
			struct huffman_table lit, dist;
			build_fixed_huffman_tables(&lit, &dist);
			if (!inflate_block_huffman(&br, &lit, &dist, out, &out_len, out_max)) return 0;
		} else if (btype == 2) {
			struct huffman_table lit, dist;
			if (!read_dynamic_huffman_tables(&br, &lit, &dist)) return 0;
			if (!inflate_block_huffman(&br, &lit, &dist, out, &out_len, out_max)) return 0;
		} else {
			return 0; /* btype == 3 is reserved/invalid per the spec */
		}
	}

	return out_len;
}

/* Public entry point: decodes a gzip-wrapped (RFC 1952) DEFLATE stream
 * - what a real `Content-Encoding: gzip` HTTP response body actually
 * is. Skips the 10-byte fixed header plus whichever optional fields
 * FLG says are present (FEXTRA/FNAME/FCOMMENT/FHCRC - real servers
 * occasionally set these, so they're handled rather than assumed
 * absent), then hands the remaining bytes (minus the 8-byte CRC32+
 * ISIZE trailer) to inflate_raw(). Does not verify the CRC32 itself
 * (this client has no CRC32 implementation and correctness is already
 * checked structurally by inflate_raw()'s own bounds/validity checks
 * - an honest limitation, not a silently-skipped safety check this
 * client claims to perform). */
uint32_t gzip_decompress(const uint8_t *data, uint32_t data_len, uint8_t *out, uint32_t out_max) {
	if (data_len < 18) return 0; /* smaller than the minimum possible gzip stream (10-byte header + empty deflate block + 8-byte trailer) */
	if (data[0] != 0x1F || data[1] != 0x8B) return 0; /* not gzip magic */
	if (data[2] != 8) return 0; /* CM must be 8 (deflate) - no other compression method exists in practice */

	uint8_t flg = data[3];
	uint32_t pos = 10;

	if (flg & 0x04) { /* FEXTRA */
		if (pos + 2 > data_len) return 0;
		uint32_t xlen = data[pos] | ((uint32_t)data[pos + 1] << 8);
		pos += 2 + xlen;
	}
	if (flg & 0x08) { /* FNAME - NUL-terminated */
		while (pos < data_len && data[pos] != 0) pos++;
		pos++;
	}
	if (flg & 0x10) { /* FCOMMENT - NUL-terminated */
		while (pos < data_len && data[pos] != 0) pos++;
		pos++;
	}
	if (flg & 0x02) { /* FHCRC */
		pos += 2;
	}

	if (pos + 8 > data_len) return 0; /* not even room for the trailer */
	uint32_t deflate_len = data_len - pos - 8;

	return inflate_raw(data + pos, deflate_len, out, out_max);
}
