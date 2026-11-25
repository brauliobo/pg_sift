/*
 * col/granule/tid.c - the _tid chunk of a granule (SIFT_CENC_TID_BLOCK): a header that names how the heap blocks of the rows are held, the blocks, then the offsets of the rows. The writer
 * holds the blocks five ways and keeps the smallest: SEQ_DELTA (the step from the block of the row before, a run while rows share a page), BLK_DICT (the distinct blocks, at most 2048, and an
 * index of each row), BLK_BITMAP (a bit per block from the lowest and the rank of each row's block, when the blocks are dense), PFOR_MINI (frames of 128 rows of exactly the bits their spread
 * needs, the few rows far from their frame patched; one zstd stream when that saves a twelfth) and, in the cold tiers, LINEAR (block * stride + offset as one cascade, with no offsets after it).
 * The offsets are held raw or as the miss of a guess (the row after the one before on the same page), whichever is smaller.
 */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/granule/int.h"
#include "core/damage.h"
#include "core/popcount.h"
#include "core/varint.h"
#include "core/par/sbuf.h"

#define SIFT_TID_MINI			128
#define SIFT_TID_PATCH_BYTES	6
#define SIFT_TID_PATCH_MAX		8
#define SIFT_TID_DICT_MAX		2048
#define SIFT_TID_BITMAP_BITS	(1U << 24)
#define SIFT_TID_ZSTD_MIN		512

static int64
offset_guess(const uint32 *blk, const int64 *offs, int i)
{
	return i > 0 && blk[i] == blk[i - 1] ? offs[i - 1] + 1 : 1;
}

static void
pack_frame(StringInfo s, const uint32 *v, int cnt, int width)
{
	uint8	   *out = sift_alloc0(16 * (Size) width + 8);
	uint64		acc = 0;
	int			nbits = 0,
				k = 0;

	for (int i = 0; i < SIFT_TID_MINI; i++)
	{
		acc |= (uint64) (i < cnt ? v[i] : 0) << nbits;
		for (nbits += width; nbits >= 8; nbits -= 8, acc >>= 8)
			out[k++] = (uint8) acc;
	}
	sift_sb_append(s, (char *) out, 16 * width);
	sift_free(out);
}

static void
unpack_frame(const uint8 *in, int width, uint32 *v)
{
	uint64		acc = 0;
	int			nbits = 0,
				k = 0;
	uint64		mask = ((uint64) 1 << width) - 1;

	for (int i = 0; i < SIFT_TID_MINI; i++)
	{
		while (nbits < width)
		{
			acc |= (uint64) in[k++] << nbits;
			nbits += 8;
		}
		v[i] = (uint32) (acc & mask);
		acc >>= width;
		nbits -= width;
	}
}

static void
put_seq_delta(StringInfo s, const uint32 *blk, int n, const SiftChunkOpts *opt)
{
	int64	   *v = sift_alloc(n * sizeof(int64));

	v[0] = blk[0];
	for (int i = 1; i < n; i++)
		v[i] = (int64) sift_zigzag((int64) blk[i] - (int64) blk[i - 1]);
	sift_put_cc_ints(s, v, n, opt);
	sift_free(v);
}

static int
cmp_u32(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/* false when the rows touch more distinct blocks than a dictionary holds */
static bool
put_blk_dict(StringInfo s, const uint32 *blk, int n, const SiftChunkOpts *opt, uint16 *ndistinct)
{
	uint32	   *sorted = sift_alloc(n * sizeof(uint32));
	int64	   *dict = sift_alloc(SIFT_TID_DICT_MAX * sizeof(int64));
	int64	   *idx = sift_alloc(n * sizeof(int64));
	int			nd = 0;

	memcpy(sorted, blk, n * sizeof(uint32));
	qsort(sorted, n, sizeof(uint32), cmp_u32);
	for (int i = 0; i < n; i++)
		if (i == 0 || sorted[i] != sorted[i - 1])
		{
			if (nd == SIFT_TID_DICT_MAX)
			{
				sift_free(sorted);
				sift_free(dict);
				sift_free(idx);
				return false;
			}
			dict[nd++] = sorted[i];
		}
	for (int i = 0; i < n; i++)
	{
		int			lo = 0,
					hi = nd - 1;

		while (lo < hi)
		{
			int			mid = (lo + hi) / 2;

			if (dict[mid] < blk[i])
				lo = mid + 1;
			else
				hi = mid;
		}
		idx[i] = lo;
	}
	sift_put_cc_ints(s, dict, nd, opt);
	sift_put_cc_ints(s, idx, n, opt);
	*ndistinct = nd;
	sift_free(sorted);
	sift_free(dict);
	sift_free(idx);
	return true;
}

/* The width of a frame that costs the fewest bytes with the rows past it patched */
static int
frame_width(const uint32 *v, int cnt, int *npatch)
{
	int			hist[34] = {0};
	int			best = 32,
				best_cost = 16 * 32,
				over = cnt;

	for (int i = 0; i < cnt; i++)
		hist[bits_for(v[i])]++;
	*npatch = 0;
	for (int w = 0; w <= 32; w++)
	{
		int			cost;

		over -= hist[w];
		cost = 16 * w + SIFT_TID_PATCH_BYTES * over;
		if (over <= SIFT_TID_PATCH_MAX && cost < best_cost)
		{
			best = w;
			best_cost = cost;
			*npatch = over;
		}
	}
	return best;
}

static void
put_pfor_mini(StringInfo s, const uint32 *blk, int n, uint32 *npatches)
{
	int			nmb = (n + SIFT_TID_MINI - 1) / SIFT_TID_MINI;
	uint32	   *bases = sift_alloc0(nmb * sizeof(uint32));
	uint8	   *widths = sift_alloc0(MAXALIGN(nmb));
	StringInfoData data,
				patches;

	sift_sb_init(&data);
	sift_sb_init(&patches);
	*npatches = 0;
	for (int m = 0; m < nmb; m++)
	{
		int			from = m * SIFT_TID_MINI,
					cnt = Min(SIFT_TID_MINI, n - from),
					np;
		uint32		lo = blk[from],
					v[SIFT_TID_MINI];

		for (int i = 1; i < cnt; i++)
			lo = Min(lo, blk[from + i]);
		for (int i = 0; i < cnt; i++)
			v[i] = blk[from + i] - lo;
		bases[m] = lo;
		widths[m] = (uint8) frame_width(v, cnt, &np);
		for (int i = 0; i < cnt; i++)
			if (bits_for(v[i]) > widths[m])
			{
				uint16		row = from + i;

				sift_sb_append(&patches, (char *) &row, 2);
				sift_sb_append(&patches, (char *) &blk[from + i], 4);
				v[i] = 0;
				(*npatches)++;
			}
		pack_frame(&data, v, cnt, widths[m]);
	}
	sift_sb_append(s, (char *) bases, nmb * sizeof(uint32));
	sift_sb_append(s, (char *) widths, nmb);
	pad8(s);
	sift_sb_append(s, data.data, data.len);
	sift_sb_append(s, patches.data, patches.len);
	pad8(s);
	sift_free(bases);
	sift_free(widths);
	sift_free(data.data);
	sift_free(patches.data);
}

/* The frames as one zstd stream in place of the frames, when that saves a twelfth: true and *s holds it */
static bool
zstd_frames(StringInfo s)
{
	Size		cap = sift_colz_bound(SIFT_CC_ALG_ZSTD, s->len),
				clen;
	char	   *buf;
	uint32		hdr[2];

	if (s->len < SIFT_TID_ZSTD_MIN || !sift_colz_available(SIFT_CC_ALG_ZSTD))
		return false;
	buf = sift_alloc(cap);
	clen = sift_colz_compress(SIFT_CC_ALG_ZSTD, 0, s->data, s->len, buf, cap);
	if (clen == 0 || (uint64) (8 + MAXALIGN(clen)) * 12 > (uint64) s->len * 11)
	{
		sift_free(buf);
		return false;
	}
	hdr[0] = (uint32) s->len;
	hdr[1] = (uint32) clen;
	sift_sb_reset(s);
	sift_sb_append(s, (char *) hdr, 8);
	sift_sb_append(s, buf, (int) clen);
	pad8(s);
	sift_free(buf);
	return true;
}

/* block * (largest offset + 1) + offset of each row, as one cascade: the rows of a build run in the order of another column keep it, and so does this */
static void
put_linear(StringInfo s, const uint32 *blk, const int64 *offs, int n, const SiftChunkOpts *opt, uint16 *max_off)
{
	int64	   *slot = sift_alloc(n * sizeof(int64));
	int64		mx = 0;

	for (int i = 0; i < n; i++)
		mx = Max(mx, offs[i]);
	for (int i = 0; i < n; i++)
		slot[i] = (int64) blk[i] * (mx + 1) + offs[i];
	sift_put_cc_ints(s, slot, n, opt);
	*max_off = (uint16) mx;
	sift_free(slot);
}

static void
put_bits_blob(StringInfo s, const uint64 *words, uint32 nbits, const SiftChunkOpts *opt)
{
	SiftCcPolicy pol;
	StringInfoData blob;

	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	sift_sb_init(&blob);
	sift_cc_encode_bits(&blob, words, nbits, &pol);
	sift_cc_blob_put(s, &blob);
	sift_free(blob.data);
}

/* false when the blocks are not dense enough for a bit per block to cost less than two bytes for each distinct one */
static bool
put_blk_bitmap(StringInfo s, const uint32 *blk, int n, const SiftChunkOpts *opt, uint16 *ndistinct)
{
	uint32		lo = blk[0],
				hi = blk[0];
	uint64		span;
	uint64	   *words;
	uint32	   *before;
	int64	   *rank;
	uint32		nd = 0;

	for (int i = 1; i < n; i++)
	{
		lo = Min(lo, blk[i]);
		hi = Max(hi, blk[i]);
	}
	span = (uint64) hi - lo + 1;
	if (span > 16 * (uint64) n + 64)
		return false;
	words = sift_alloc0((Size) 8 * ((span + 63) / 64 + 1));
	for (int i = 0; i < n; i++)
		words[(blk[i] - lo) / 64] |= UINT64CONST(1) << ((blk[i] - lo) % 64);
	before = sift_alloc(sizeof(uint32) * ((span + 63) / 64 + 1));
	for (uint64 w = 0; w < (span + 63) / 64; w++)
	{
		before[w] = nd;
		nd += (uint32) sift_pc64(words[w]);
	}
	if (span / 8 >= (uint64) nd * 2 || nd > UINT16_MAX)
	{
		sift_free(words);
		sift_free(before);
		return false;
	}
	rank = sift_alloc(n * sizeof(int64));
	for (int i = 0; i < n; i++)
	{
		uint32		b = blk[i] - lo;

		rank[i] = before[b / 64] + sift_pc64(words[b / 64] & ((UINT64CONST(1) << (b % 64)) - 1));
	}
	sift_sb_append(s, (char *) &lo, 4);
	{
		uint32		nbits = (uint32) span;

		sift_sb_append(s, (char *) &nbits, 4);
	}
	put_bits_blob(s, words, (uint32) span, opt);
	sift_put_cc_ints(s, rank, n, opt);
	*ndistinct = (uint16) nd;
	sift_free(words);
	sift_free(before);
	sift_free(rank);
	return true;
}

static void
put_offsets(StringInfo s, const uint32 *blk, const int64 *offs, int n, const SiftChunkOpts *opt, uint8 *flags)
{
	StringInfoData raw,
				guessed;
	int64	   *miss = sift_alloc(n * sizeof(int64));

	for (int i = 0; i < n; i++)
		miss[i] = (int64) sift_zigzag(offs[i] - offset_guess(blk, offs, i));
	sift_sb_init(&raw);
	sift_sb_init(&guessed);
	sift_put_cc_ints(&raw, offs, n, opt);
	sift_put_cc_ints(&guessed, miss, n, opt);
	if (guessed.len < raw.len)
	{
		sift_sb_append(s, guessed.data, guessed.len);
		*flags |= SIFT_TID_OFFS_GUESSED;
	}
	else
		sift_sb_append(s, raw.data, raw.len);
	sift_free(raw.data);
	sift_free(guessed.data);
	sift_free(miss);
}

typedef struct TidMode
{
	StringInfoData body;
	SiftTidBlockHeader hdr;
	bool		ok;
	bool		offsets;		/* the offsets stream follows the body */
} TidMode;

#define TID_NMODES	5

static int
adjacent_blocks(const uint32 *blk, int n)
{
	int			near = 0;

	for (int i = 1; i < n; i++)
		near += blk[i] == blk[i - 1] || blk[i] == blk[i - 1] + 1;
	return near;
}

void
sift_chunk_put_tids(StringInfo s, const uint64 *tid, int n, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	int64	   *offs = sift_alloc(n * sizeof(int64));
	uint32	   *blk = sift_alloc(n * sizeof(uint32));
	TidMode		m[TID_NMODES] = {0};
	StringInfoData tail;
	uint8		oflags = 0;
	int			pick = -1;
	bool		cold = SIFT_TIER_IS_COLD(opt->tier);
	Size		best = 0;

	for (int i = 0; i < n; i++)
	{
		if (tid[i] >> 48 != 0)
			SIFT_FAIL_LIMIT("An address of a heap block has at most 32 bits and an offset 16.", "sift: the heap address " UINT64_FORMAT " is past 48 bits", tid[i]);
		offs[i] = (int64) (tid[i] & 0xFFFF);
		blk[i] = (uint32) (tid[i] >> 16);
	}
	sift_sb_init(&tail);
	put_offsets(&tail, blk, offs, n, opt, &oflags);
	for (int k = 0; k < TID_NMODES; k++)
	{
		sift_sb_init(&m[k].body);
		m[k].hdr.mode = k + 1;
		m[k].hdr.flags = oflags;
		m[k].ok = true;
		m[k].offsets = k != 3;
	}
	m[3].ok = cold;
	m[4].ok = cold;
	if (opt->tier == SIFT_TIER_HOT_L0)
	{
		bool		clustered = adjacent_blocks(blk, n) * 10 >= (n - 1) * 7;

		m[0].ok = clustered;
		m[1].ok = false;
		m[2].ok = !clustered;
	}
	if (m[0].ok)
		put_seq_delta(&m[0].body, blk, n, opt);
	if (m[1].ok)
		m[1].ok = put_blk_dict(&m[1].body, blk, n, opt, &m[1].hdr.param0);
	if (m[2].ok)
	{
		m[2].hdr.param0 = SIFT_TID_MINI;
		put_pfor_mini(&m[2].body, blk, n, &m[2].hdr.n_patches);
		if (cold && zstd_frames(&m[2].body))
			m[2].hdr.flags |= SIFT_TID_PFOR_ZSTD;
	}
	if (m[3].ok)
		put_linear(&m[3].body, blk, offs, n, opt, &m[3].hdr.param0);
	if (m[4].ok)
		m[4].ok = put_blk_bitmap(&m[4].body, blk, n, opt, &m[4].hdr.param0);
	for (int k = 0; k < TID_NMODES; k++)
	{
		Size		sz = m[k].body.len + (m[k].offsets ? tail.len : 0);

		if (m[k].ok && (pick < 0 || sz < best))
		{
			pick = k;
			best = sz;
		}
	}
	m[3].hdr.flags &= (uint8) ~SIFT_TID_OFFS_GUESSED;
	sift_sb_append(s, (char *) &m[pick].hdr, sizeof(SiftTidBlockHeader));
	sift_sb_append(s, m[pick].body.data, m[pick].body.len);
	if (m[pick].offsets)
		sift_sb_append(s, tail.data, tail.len);
	pad8(s);
	ref->enc = SIFT_CENC_TID_BLOCK;
	ref->flags = SIFT_CHUNK_ALL_VALID;
	ref->nnonnull = n;
	for (int k = 0; k < TID_NMODES; k++)
		sift_free(m[k].body.data);
	sift_free(tail.data);
	sift_free(offs);
	sift_free(blk);
}

/* The stream of nvals integers at p, its first w decoded; the stream after it */
static const char *
get_ints_head(const SiftChunk *c, const char *p, uint32 nvals, uint32 w, int64 *out)
{
	uint32		whole;
	SiftCc	   *cc;

	if (p > c->body + c->body_len)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk ends before its streams"));
	cc = sift_cc_blob_open(p, c->body_len - (p - c->body), nvals, &whole);
	if (w < nvals)
		sift_cc_decode_head(cc, w, out);
	else
		sift_cc_decode(cc, out);
	sift_cc_close(cc);
	return p + whole;
}

static void
check_block(int64 v)
{
	if (v < 0 || v > (int64) UINT32_MAX)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a heap block of a _tid chunk is past 32 bits"));
}

static const char *
get_seq_delta(const SiftChunk *c, const char *p, int n, int w, uint32 *blk, int64 *scratch)
{
	p = get_ints_head(c, p, n, w, scratch);
	check_block(scratch[0]);
	blk[0] = (uint32) scratch[0];
	for (int i = 1; i < w; i++)
	{
		int64		b = (int64) blk[i - 1] + sift_unzigzag((uint64) scratch[i]);

		check_block(b);
		blk[i] = (uint32) b;
	}
	return p;
}

static const char *
get_blk_dict(const SiftChunk *c, const char *p, int n, int w, uint32 ndistinct, uint32 *blk, int64 *scratch)
{
	int64		dict[SIFT_TID_DICT_MAX + SIFT_COL_PAD / 8 + 8];

	if (ndistinct == 0 || ndistinct > SIFT_TID_DICT_MAX)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk names %u distinct heap blocks", ndistinct));
	p = get_ints_head(c, p, ndistinct, ndistinct, dict);
	p = get_ints_head(c, p, n, w, scratch);
	for (int i = 0; i < w; i++)
	{
		if ((uint64) scratch[i] >= ndistinct)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk indexes a heap block past its dictionary"));
		check_block(dict[scratch[i]]);
		blk[i] = (uint32) dict[scratch[i]];
	}
	return p;
}

static const char *
get_pfor_frames(const SiftChunk *c, const char *p, int n, int w, uint32 npatches, uint32 *blk)
{
	int			nmb = (n + SIFT_TID_MINI - 1) / SIFT_TID_MINI;
	const char *end = c->body + c->body_len;
	const uint32 *bases = (const uint32 *) p;
	const uint8 *widths = (const uint8 *) (p + (uint64) nmb * 4);
	uint64		bytes = 0;

	if (p + MAXALIGN((uint64) nmb * 5) > end)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the frames of a _tid chunk do not fit it"));
	for (int m = 0; m < nmb; m++)
	{
		if (widths[m] > 32)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a frame of a _tid chunk is %u bits wide", widths[m]));
		bytes += 16 * (uint64) widths[m];
	}
	p += MAXALIGN((uint64) nmb * 5);
	if (p + bytes + (uint64) npatches * SIFT_TID_PATCH_BYTES > end)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the frames of a _tid chunk do not fit it"));
	for (int m = 0; m < nmb; m++)
	{
		uint32		v[SIFT_TID_MINI];
		int			from = m * SIFT_TID_MINI,
					cnt = Min(SIFT_TID_MINI, n - from);

		if (from < w)
		{
			unpack_frame((const uint8 *) p, widths[m], v);
			for (int i = 0; i < cnt; i++)
				blk[from + i] = bases[m] + v[i];
		}
		p += 16 * widths[m];
	}
	for (uint32 k = 0; k < npatches; k++)
	{
		uint16		row;
		uint32		b;

		memcpy(&row, p + (uint64) k * SIFT_TID_PATCH_BYTES, 2);
		memcpy(&b, p + (uint64) k * SIFT_TID_PATCH_BYTES + 2, 4);
		if (row >= n)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a patch of a _tid chunk names row %u of %d", row, n));
		blk[row] = b;
	}
	return p + MAXALIGN((uint64) npatches * SIFT_TID_PATCH_BYTES);
}

/* The frames, in the stream of zstd that SIFT_TID_PFOR_ZSTD says they are in, or as they are */
static const char *
get_pfor_mini(const SiftChunk *c, const char *p, int n, int w, uint32 npatches, uint32 *blk, uint8 flags)
{
	SiftChunk	raw = *c;
	uint32		hdr[2];
	char	   *buf;

	if (!(flags & SIFT_TID_PFOR_ZSTD))
		return get_pfor_frames(c, p, n, w, npatches, blk);
	if (p + 8 > c->body + c->body_len)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk ends before its frames"));
	memcpy(hdr, p, 8);
	if (hdr[0] > (uint64) n * 9 + 4096 || hdr[1] > c->body + c->body_len - (p + 8))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the compressed frames of a _tid chunk do not fit it"));
	buf = sift_alloc(hdr[0] + 8);
	if (!sift_colz_available(SIFT_CC_ALG_ZSTD) || !sift_colz_decompress(SIFT_CC_ALG_ZSTD, p + 8, hdr[1], buf, hdr[0]))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the compressed frames of a _tid chunk are damaged"));
	raw.body = buf;
	raw.body_len = hdr[0];
	(void) get_pfor_frames(&raw, buf, n, w, npatches, blk);
	sift_free(buf);
	return p + 8 + MAXALIGN((uint64) hdr[1]);
}

static const char *
get_blk_bitmap(const SiftChunk *c, const char *p, int n, int w, uint32 ndistinct, uint32 *blk, int64 *scratch)
{
	const char *end = c->body + c->body_len;
	uint32		lo,
				nbits,
				len,
				nd = 0;
	uint64	   *words;
	uint32	   *dict;

	if (p + 16 > end)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk ends before its bitmap"));
	memcpy(&lo, p, 4);
	memcpy(&nbits, p + 4, 4);
	memcpy(&len, p + 8, 4);
	if (nbits == 0 || nbits > SIFT_TID_BITMAP_BITS || (uint64) lo + nbits > (uint64) UINT32_MAX + 1 || len > (uint64) (end - (p + 16)))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the bitmap of the heap blocks of a _tid chunk is out of range"));
	words = sift_alloc0((Size) 8 * ((nbits + 63) / 64 + SIFT_COL_PAD / 8 + 1));
	sift_cc_decode_bits((const uint8 *) p + 16, len, nbits, words);
	dict = sift_alloc(sizeof(uint32) * (ndistinct + 1));
	for (uint32 b = 0; b < nbits; b++)
		if ((words[b / 64] >> (b % 64)) & 1)
		{
			if (nd == ndistinct)
				SIFT_DAMAGE_DATA(NULL, errmsg("sift: the bitmap of a _tid chunk has more blocks than the %u it names", ndistinct));
			dict[nd++] = lo + b;
		}
	if (nd != ndistinct)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the bitmap of a _tid chunk has fewer blocks than the %u it names", ndistinct));
	p = get_ints_head(c, p + 16 + MAXALIGN((uint64) len), n, w, scratch);
	for (int i = 0; i < w; i++)
	{
		if ((uint64) scratch[i] >= nd)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk ranks a heap block past its bitmap"));
		blk[i] = dict[scratch[i]];
	}
	sift_free(words);
	sift_free(dict);
	return p;
}

/* Every row of a LINEAR chunk: the slot is block * (largest offset + 1) + offset */
static void
get_linear(const SiftChunk *c, int n, int w, uint32 max_off, int64 *scratch, uint64 *out)
{
	uint64		stride = (uint64) max_off + 1;

	(void) get_ints_head(c, c->body + sizeof(SiftTidBlockHeader), n, w, scratch);
	for (int i = 0; i < w; i++)
	{
		uint64		slot = (uint64) scratch[i];

		if (scratch[i] < 0 || slot / stride > UINT32_MAX)
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a heap address of a _tid chunk is past 48 bits"));
		out[i] = ((slot / stride) << 16) | (slot % stride);
	}
}

void
sift_chunk_tids(const SiftChunk *c, int n, uint64 *out)
{
	sift_chunk_tids_head(c, n, n, out);
}

void
sift_chunk_tids_head(const SiftChunk *c, int n, int w, uint64 *out)
{
	SiftTidBlockHeader h;
	const char *p = c->body;
	int64	   *scratch = sift_alloc((n + SIFT_COL_PAD / 8 + 8) * sizeof(int64));
	uint32	   *blk = sift_alloc(n * sizeof(uint32));

	if (c->enc != SIFT_CENC_TID_BLOCK)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift: chunk encoding %u is not supported for _tid", c->enc),
						errhint("The part was written by another pg_sift release: REINDEX the index.")));
	if (c->body_len < sizeof(h))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk is shorter than its header"));
	memcpy(&h, p, sizeof(h));
	p += sizeof(h);
	switch (h.mode)
	{
		case SIFT_TID_MODE_SEQ_DELTA:
			p = get_seq_delta(c, p, n, w, blk, scratch);
			break;
		case SIFT_TID_MODE_BLK_DICT:
			p = get_blk_dict(c, p, n, w, h.param0, blk, scratch);
			break;
		case SIFT_TID_MODE_PFOR_MINI:
			if (h.param0 != SIFT_TID_MINI)
				SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk has frames of %u rows", h.param0));
			p = get_pfor_mini(c, p, n, w, h.n_patches, blk, h.flags);
			break;
		case SIFT_TID_MODE_BLK_BITMAP:
			p = get_blk_bitmap(c, p, n, w, h.param0, blk, scratch);
			break;
		case SIFT_TID_MODE_LINEAR:
			get_linear(c, n, w, h.param0, scratch, out);
			sift_free(scratch);
			sift_free(blk);
			return;
		default:
			SIFT_DAMAGE_DATA(NULL, errmsg("sift: a _tid chunk has mode %u", h.mode));
	}
	get_ints_head(c, p, n, w, scratch);
	for (int i = 0; i < w; i++)
	{
		int64		off = scratch[i];

		if (h.flags & SIFT_TID_OFFS_GUESSED)
		{
			int64		prev = i > 0 && blk[i] == blk[i - 1] ? (int64) (out[i - 1] & 0xFFFF) + 1 : 1;

			off = prev + sift_unzigzag((uint64) off);
		}
		out[i] = ((uint64) blk[i] << 16) | (uint64) (off & 0xFFFF);
	}
	sift_free(scratch);
	sift_free(blk);
}
