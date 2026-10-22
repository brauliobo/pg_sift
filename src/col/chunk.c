/*
 * col/chunk.c - L1 of the column engine: reading a column chunk, its validation, tags and decoding.
 */
#include "core/host.h"

#include "col/enc.h"
#include "col/enc_int.h"
#include "core/popcount.h"

/* ---------------------------------------------------------------------------
 * 5. Codec seam: the plain FOR and PACK encodings of docs/COL.md 2.2. col-compress replaces this section (what a chunk of a given span
 * looks like, its size, its validity, and how it is written and read); the rest of the file only goes through these functions.
 * ------------------------------------------------------------------------- */

/* The codec's reader of a CC chunk (the blob is validated; verify also checks its check word) */
SiftCc *
cc_open(const SiftCChunk *c, bool verify)
{
	return sift_cc_open_rows(CurrentMemoryContext, c->payload, c->paylen, verify, c->n);
}

/*
 * The payload of an inline STR chunk: u32 length of the prefix blob, u32 0, the prefix blob (a codec blob of the n keys sift_col_str_prefix of
 * the strings, NULL slots filler: filters answer "which rows can hold this string" on the encoded form), then the string list.
 */
bool
istr_split(const SiftCChunk *c, const uint8 **pre, uint32 *prelen, const uint8 **strs, uint32 *strslen)
{
	uint32		plen,
				zero;

	if (c->paylen < 8)
		return false;
	memcpy(&plen, c->payload, sizeof(plen));
	memcpy(&zero, c->payload + 4, sizeof(zero));
	if (zero != 0 || plen == 0 || plen % 8 != 0 || (uint64) plen + 8 + 8 > c->paylen)
		return false;
	*pre = c->payload + 8;
	*prelen = plen;
	*strs = c->payload + 8 + plen;
	*strslen = c->paylen - 8 - plen;
	return true;
}

void
sift_cchunk_strs(const SiftCChunk *c, MemoryContext mcxt, uint32 **off, char **bytes)
{
	const uint8 *pl = c->payload;
	uint32		len = c->paylen;

	if (c->enc != SIFT_COLENC_STRS && c->enc != SIFT_COLENC_ISTR)
		elog(ERROR, "sift_col: chunk encoding %d holds no strings", c->enc);
	if (c->enc == SIFT_COLENC_ISTR)
	{
		const uint8 *pre;
		uint32		prelen;

		if (!istr_split(c, &pre, &prelen, &pl, &len))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the inline text payload is malformed")));
	}
	sift_cc_decode_strs(mcxt, pl, len, c->n, off, bytes);
}

/* The lengths of the strings of a STRS or ISTR chunk, the keys of its VAR zone and of its row tests */
static void
strs_lengths(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *off;
	char	   *bytes;

	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	for (uint32 k = 0; k < m; k++)
		dst[k] = off[(idx ? idx[k] : k) + 1] - off[idx ? idx[k] : k];
	pfree(off);
	pfree(bytes);
}

/* dst[0..n) = base + payload (the NULL slots hold base) */
static void
codec_decode(const SiftCChunk *c, int64 *dst)
{
	uint32		tmp[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			strs_lengths(c, NULL, c->n, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode(cc, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 i = 0; i < c->n; i++)
				dst[i] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64[codec_slot(c->bits)](c->payload, c->n, c->base, dst);
			break;
		default:
			for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
			{
				uint32		m = Min(CODEC_BLOCK, c->n - s);

				sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, m, c->base, dst + s);
			}
	}
}

static void
codec_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *tmp;

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
		case SIFT_COLENC_ISTR:
			strs_lengths(c, idx, m, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode_sel(cc, idx, m, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 k = 0; k < m; k++)
				dst[k] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64_sel[codec_slot(c->bits)](c->payload, idx, m, c->base, dst);
			break;
		default:
			tmp = palloc(c->n * sizeof(uint32) + SIFT_COL_PAD);
			sift_colk->unpack(c->payload, c->bits, c->n, tmp);
			sift_colk->for_i64_sel[SIFT_COLW_32](tmp, idx, m, c->base, dst);
			pfree(tmp);
	}
}

/* ---------------------------------------------------------------------------
 * 6. Chunks
 * ------------------------------------------------------------------------- */

#define CHUNK_HEAD		16
#define CHUNK_MAX_N		(1U << 28)

/* Set bits of the bitmap of n bits */
uint32
bitmap_count(const uint64 *bits, uint32 n)
{
	uint32		cnt = 0;

	for (uint32 w = 0; w < words_of(n); w++)
		cnt += sift_pc64(w == n / 64 ? bits[w] & ((UINT64CONST(1) << (n % 64)) - 1) : bits[w]);
	return cnt;
}

bool
sift_cchunk_istr_codes(const SiftCChunk *c, MemoryContext mcxt, SiftCDict **dict, SiftCChunk *out)
{
	uint32	   *off,
			   *codes;
	char	   *bytes;

	*out = *c;
	*dict = NULL;
	if (c->enc != SIFT_COLENC_ISTR)
		return false;
	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	*dict = sift_cdict_create(mcxt);
	codes = MemoryContextAllocZero(mcxt, (Size) c->n * sizeof(uint32) + SIFT_COL_PAD);
	for (uint32 i = 0; i < c->n; i++)
		if (!sift_col_bit_test(c->nulls, i))
			codes[i] = sift_cdict_intern(*dict, bytes + off[i], off[i + 1] - off[i]);
	out->enc = SIFT_COLENC_FOR;
	out->bits = 32;
	out->base = 0;
	out->payload = (const uint8 *) codes;
	out->paylen = c->n * sizeof(uint32);
	pfree(off);
	pfree(bytes);
	return true;
}

static bool tags_valid(const SiftCChunk *c);
static bool kinds_parse(const uint8 *p, uint32 len, uint64 *pos, SiftCChunk *c);
static bool kinds_valid(const SiftCChunk *c);

bool
sift_cchunk_parse(const uint8 *p, uint32 len, SiftCChunk *c)
{
	uint64		pos = CHUNK_HEAD,
				words;
	bool		whole;

	if (len < CHUNK_HEAD || ((uintptr_t) p & 7) != 0)
		return false;
	memset(c, 0, sizeof(*c));
	c->enc = p[0];
	c->bits = p[1];
	c->flags = p[2];
	memcpy(&c->n, p + 4, sizeof(c->n));
	memcpy(&c->base, p + 8, sizeof(c->base));
	if ((c->flags & ~(SIFT_COLC_NULLS | SIFT_COLC_OVF | SIFT_COLC_TAGS | SIFT_COLC_KINDS)) != 0 || ((c->flags & SIFT_COLC_OVF) && !(c->flags & SIFT_COLC_NULLS)) ||
		((c->flags & SIFT_COLC_TAGS) && !(c->flags & SIFT_COLC_OVF)) || !codec_valid(c->enc, c->bits) || c->n > CHUNK_MAX_N)
		return false;
	words = words_of(c->n);
	if (c->flags & SIFT_COLC_NULLS)
	{
		c->nulls = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	if (c->flags & SIFT_COLC_OVF)
	{
		c->ovf = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	if (c->flags & SIFT_COLC_TAGS)
	{
		if (pos > len)
			return false;
		c->tags = p + pos;
		pos += tags_bytes(bitmap_count(c->ovf, c->n));
	}
	if ((c->flags & SIFT_COLC_KINDS) && !kinds_parse(p, len, &pos, c))
		return false;
	whole = c->enc == SIFT_COLENC_CC || c->enc == SIFT_COLENC_STRS || c->enc == SIFT_COLENC_ISTR;
	c->paylen = whole ? (pos <= len ? (uint32) (len - pos) : 0) : (uint32) codec_paylen(c->enc, c->bits, c->n);
	if (pos + c->paylen > len || (whole && (c->paylen == 0 || c->paylen % 8 != 0 || c->n == 0)))
		return false;
	c->payload = p + pos;
	if (c->enc == SIFT_COLENC_ISTR)
	{
		const uint8 *pre,
				   *strs;
		uint32		prelen,
					strslen;

		if (!istr_split(c, &pre, &prelen, &strs, &strslen))
			return false;
	}
	if (c->n % 64 != 0)
	{
		uint64		tail = ~UINT64CONST(0) << (c->n % 64);

		if ((c->nulls && (c->nulls[words - 1] & tail)) || (c->ovf && (c->ovf[words - 1] & tail)))
			return false;
	}
	for (uint64 w = 0; c->ovf != NULL && w < words; w++)
		if (c->ovf[w] & ~c->nulls[w])
			return false;
	return (c->tags == NULL || tags_valid(c)) && kinds_valid(c);
}

static inline uint8
tag_nibble(const uint8 *tags, uint32 k)
{
	return (tags[k / 2] >> (k % 2 * 4)) & 0xF;
}

/* Every nibble is a tag, and the unused ones (the high nibble of an odd count, the padding) are zero */
static bool
tags_valid(const SiftCChunk *c)
{
	uint32		nov = bitmap_count(c->ovf, c->n);

	for (uint32 k = 0; k < nov; k++)
		if (tag_nibble(c->tags, k) > SIFT_COLTAG_OVF_MAX)
			return false;
	for (uint32 k = nov; k < tags_bytes(nov) * 2; k++)
		if (tag_nibble(c->tags, k) != 0)
			return false;
	return true;
}

/* the tags a kind can have: the type of an astext leaf's number, boolean (or string) and JSON null */
#define KINDS_ALLOWED	(1U << SIFT_COLTAG_STRING | 1U << SIFT_COLTAG_NUMBER | 1U << SIFT_COLTAG_BOOLEAN | 1U << SIFT_COLTAG_JNULL)

/* The kinds word and bitmaps at *pos: the word has only the bits of tags 1..MAX and every bitmap is there */
static bool
kinds_parse(const uint8 *p, uint32 len, uint64 *pos, SiftCChunk *c)
{
	uint64		head;
	Size		bytes = words_of(c->n) * 8;

	if (*pos + 8 > len)
		return false;
	memcpy(&head, p + *pos, sizeof(head));
	*pos += 8;
	c->kmask = (uint8) head;
	if (head != c->kmask || head == 0 || (head & ~KINDS_ALLOWED))
		return false;
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
		if (c->kmask >> tag & 1)
		{
			if (*pos + bytes > len)
				return false;
			c->kind[tag] = (const uint64 *) (p + *pos);
			*pos += bytes;
		}
	return true;
}

/* A kind is a slot that does not overflow: JSON null is a NULL slot, the others hold a value, no slot has two, and none is empty */
static bool
kinds_valid(const SiftCChunk *c)
{
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
	{
		uint64		any = 0;

		if (c->kind[tag] == NULL)
			continue;
		for (uint32 w = 0; w < words_of(c->n); w++)
		{
			uint64		m = c->kind[tag][w],
						nul = c->nulls ? c->nulls[w] : 0;

			if ((w == c->n / 64 && c->n % 64 != 0 && (m >> (c->n % 64)) != 0) || (c->ovf && (m & c->ovf[w])) ||
				(tag == SIFT_COLTAG_JNULL ? (m & ~nul) : (m & nul)))
				return false;
			for (uint8 o = 1; o < tag; o++)
				if (c->kind[o] && (m & c->kind[o][w]))
					return false;
			any |= m;
		}
		if (any == 0)
			return false;
	}
	return true;
}

uint8
sift_cchunk_ovf_tag(const SiftCChunk *c, uint32 slot)
{
	uint32		k = 0;

	if (c->tags == NULL || !sift_col_bit_test(c->ovf, slot))
		return SIFT_COLTAG_UNKNOWN;
	for (uint32 w = 0; w < slot / 64; w++)
		k += sift_pc64(c->ovf[w]);
	k += sift_pc64(c->ovf[slot / 64] & ((UINT64CONST(1) << (slot % 64)) - 1));
	return tag_nibble(c->tags, k);
}

bool
sift_cchunk_tags(const SiftCChunk *c, uint8 *dst)
{
	uint32		k = 0;

	if (c->tags == NULL && c->kmask == 0)
		return false;
	memset(dst, 0, c->n);
	for (uint32 w = 0; c->tags != NULL && w < words_of(c->n); w++)
		for (uint64 m = c->ovf[w]; m; m &= m - 1)
			dst[w * 64 + pg_rightmost_one_pos64(m)] = tag_nibble(c->tags, k++);
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
		for (uint32 w = 0; c->kind[tag] != NULL && w < words_of(c->n); w++)
			for (uint64 m = c->kind[tag][w]; m; m &= m - 1)
				dst[w * 64 + pg_rightmost_one_pos64(m)] = tag;
	return true;
}

void
sift_cchunk_tag_bits(const SiftCChunk *c, uint8 tag, uint64 *bits)
{
	uint32		k = 0;

	memset(bits, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 w = 0; c->ovf != NULL && w < words_of(c->n); w++)
		for (uint64 m = c->ovf[w]; m; m &= m - 1, k++)
			if ((c->tags ? tag_nibble(c->tags, k) : SIFT_COLTAG_UNKNOWN) == tag)
				bits[w] |= m & -m;
	for (uint32 w = 0; tag != SIFT_COLTAG_UNKNOWN && c->kind[tag] != NULL && w < words_of(c->n); w++)
		bits[w] |= c->kind[tag][w];
}

/* The NULL slots of a decoded lane (any element type) are 0 */
#define ZERO_NULLS(nulls, n, dst) \
	do { \
		for (uint32 w_ = 0; (nulls) != NULL && w_ < words_of(n); w_++) \
			for (uint64 m_ = (nulls)[w_]; m_; m_ &= m_ - 1) \
				(dst)[w_ * 64 + pg_rightmost_one_pos64(m_)] = 0; \
	} while (0)

void
sift_cchunk_decode(const SiftCChunk *c, int64 *dst)
{
	codec_decode(c, dst);
	ZERO_NULLS(c->nulls, c->n, dst);
}

void
sift_cchunk_decode_u32(const SiftCChunk *c, uint32 *dst)
{
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_decode_u32(cc, dst);
		sift_cc_close(cc);
	}
	else
	{
		int64	   *tmp = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

		codec_decode(c, tmp);
		for (uint32 i = 0; i < c->n; i++)
			dst[i] = (uint32) tmp[i];
		pfree(tmp);
	}
	ZERO_NULLS(c->nulls, c->n, dst);
}

/* the codes of a dictionary of ndict values, whose NULL code is ndict: the tables the executor indexes by them have ndict + 1 entries */
void
sift_cchunk_decode_codes(const SiftCChunk *c, uint32 ndict, uint32 *dst)
{
	sift_cchunk_decode_u32(c, dst);
	sift_ccodes_below(dst, c->n, ndict + 1);
}

void
sift_cchunk_decode_f8(const SiftCChunk *c, double *dst)
{
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_decode_f8(cc, dst);
		sift_cc_close(cc);
		ZERO_NULLS(c->nulls, c->n, dst);
	}
	else
	{
		sift_cchunk_decode(c, (int64 *) dst);
		sift_colk->f8_from_keys((int64 *) dst, c->n);
	}
}

void
sift_cchunk_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	codec_decode_sel(c, idx, m, dst);
	for (uint32 k = 0; c->nulls != NULL && k < m; k++)
		if (sift_col_bit_test(c->nulls, idx[k]))
			dst[k] = 0;
}
