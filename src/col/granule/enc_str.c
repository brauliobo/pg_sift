/*
 * col/granule/enc_str.c - the encoders of the string columns of a granule (col/granule.h): dictionaries of the granule and of the part, strings in a block, in views, in halves and in the table of symbols of FSST.
 */
#include "core/host.h"

#include <math.h>

#include "common/hashfn.h"
#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/int.h"
#include "core/par/sbuf.h"

#define SIFT_FSST_MIN_BYTES		4096	/* a column of fewer bytes keeps the codec's list: the table (up to 2.3 kB) would not pay */
#define SIFT_FSST_AVG_MAX		64		/* strings this long on average are left to LZ4 and zstd, which find the repeats within them */
#define SIFT_FSST_MAX_PERMILLE	800		/* the table codes its sample in at most this much of its size */
#define SIFT_DICT_MAX		65536
#define SIFT_DICT_PART_MAX	(1 << 20)
#define SIFT_DICT_ENTRY_MAX	1024
#define SIFT_DICT_HOT_MAX	256

/* The strings by first appearance: the dictionary entry of every value, or -1 when there are more than maxnd distinct ones. slot (cap entries, a power of two, at least twice the values) is the caller's scratch */
static int
dict_codes(const char *const *p, const uint32 *len, const bool *isnull, int n, uint32 *code, int *first, int *slot, int cap, int maxnd)
{
	int			nd = 0;

	memset(slot, -1, cap * sizeof(int));
	for (int i = 0, k = 0; i < n; i++)
	{
		uint32		h;
		int			at;

		if (isnull != NULL && isnull[i])
			continue;
		h = hash_bytes((const unsigned char *) p[i], len[i]);
		for (at = h & (cap - 1); slot[at] >= 0; at = (at + 1) & (cap - 1))
			if (len[first[slot[at]]] == len[i] && memcmp(p[first[slot[at]]], p[i], len[i]) == 0)
				break;
		if (slot[at] < 0)
		{
			if (nd >= maxnd)
				return -1;
			first[nd] = i;
			slot[at] = nd++;
		}
		code[k++] = slot[at];
	}
	return nd;
}

static int
first_cmp_len(const char *const *p, const uint32 *len, int a, int b)
{
	int			c = memcmp(p[a], p[b], Min(len[a], len[b]));

	return c != 0 ? c : (len[a] > len[b]) - (len[a] < len[b]);
}

typedef struct DictSort
{
	const char *const *p;
	const uint32 *len;
	const int  *first;
} DictSort;

static int
dict_sort_cmp(const void *a, const void *b, void *arg)
{
	const DictSort *ds = arg;

	return first_cmp_len(ds->p, ds->len, ds->first[*(const int *) a], ds->first[*(const int *) b]);
}

/* The dictionary of the column of a granule (strings in byte order, which the cascade of their lengths and the wrapper take best) and the codes as a cascade */
static void
put_dict(StringInfo s, const char *const *p, const uint32 *len, uint32 *code, int *first, int nd, int nn, const SiftChunkOpts *opt)
{
	int		   *order = sift_alloc(nd * sizeof(int)),
			   *rank = sift_alloc(nd * sizeof(int));
	DictSort	ctx = {p, len, first};
	uint32	   *off = sift_alloc((nd + 1) * sizeof(uint32));
	int64	   *c = sift_alloc(nn * sizeof(int64));
	StringInfoData bytes,
				blob;
	SiftCcPolicy pol;
	uint32		hdr[2] = {nd, 0};

	for (int d = 0; d < nd; d++)
		order[d] = d;
	qsort_arg(order, nd, sizeof(int), dict_sort_cmp, &ctx);
	for (int d = 0; d < nd; d++)
		rank[order[d]] = d;
	sift_sb_init(&bytes);
	off[0] = 0;
	for (int d = 0; d < nd; d++)
	{
		sift_sb_append(&bytes, p[first[order[d]]], len[first[order[d]]]);
		off[d + 1] = bytes.len;
	}
	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	sift_sb_init(&blob);
	sift_cc_encode_strs(&blob, off, bytes.data, nd, &pol, NULL);
	sift_sb_append(s, (char *) hdr, 8);
	sift_cc_blob_put(s, &blob);
	for (int i = 0; i < nn; i++)
		c[i] = rank[code[i]];
	sift_put_cc_ints(s, c, nn, opt);
	sift_free(order);
	sift_free(rank);
	sift_free(off);
	sift_free(c);
	sift_free(bytes.data);
	sift_free(blob.data);
}

static void
put_inline(StringInfo s, const char *const *p, const uint32 *len, const bool *isnull, int n)
{
	uint32		off = 0;

	sift_sb_append(s, (char *) &off, 4);
	for (int i = 0; i < n; i++)
		if (isnull == NULL || !isnull[i])
		{
			off += len[i];
			sift_sb_append(s, (char *) &off, 4);
		}
	for (int i = 0; i < n; i++)
		if (isnull == NULL || !isnull[i])
			sift_sb_append(s, p[i], len[i]);
	pad8(s);
}

typedef struct BlockFrames
{
	const uint32 *off;
	const char *bytes;
	const SiftCcPolicy *pol;
} BlockFrames;

static void
put_block_frame(StringInfo frame, int first, int cnt, void *arg)
{
	const BlockFrames *bf = arg;
	uint32	   *off = sift_alloc((cnt + 1) * sizeof(uint32));
	StringInfoData blob;

	for (int i = 0; i <= cnt; i++)
		off[i] = bf->off[first + i] - bf->off[first];
	sift_sb_init(&blob);
	sift_cc_encode_strs(&blob, off, bf->bytes + bf->off[first], cnt, bf->pol, NULL);
	sift_cc_blob_put(frame, &blob);
	sift_free(off);
	sift_free(blob.data);
}

/* The strings as the codec's list: the offsets as a cascade and the bytes wrapped when the cold rule says so, in frames of a part of a sift_table; the modifiers of the encoding it needs */
static uint16
put_block(StringInfo s, const char *const *p, const uint32 *len, const bool *isnull, int n, int nn, const SiftChunkOpts *opt)
{
	uint32	   *off = sift_alloc((nn + 1) * sizeof(uint32));
	StringInfoData bytes,
				blob;
	SiftCcPolicy pol;
	uint16		mods = 0;

	sift_sb_init(&bytes);
	off[0] = 0;
	for (int i = 0, k = 0; i < n; i++)
		if (isnull == NULL || !isnull[i])
		{
			sift_sb_append(&bytes, p[i], len[i]);
			off[++k] = bytes.len;
		}
	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	if (opt->framed)
	{
		sift_frames_put(s, nn, put_block_frame, &(BlockFrames) {off, bytes.data, &pol});
		sift_free(off);
		sift_free(bytes.data);
		return SIFT_ENC_FRAMED;
	}
	sift_sb_init(&blob);
	sift_cc_encode_strs(&blob, off, bytes.data, nn, &pol, NULL);
	if (opt->cdict != NULL)
	{
		StringInfoData zblob;

		sift_sb_init(&zblob);
		if (sift_cc_encode_strs_dict(&zblob, off, bytes.data, nn, &pol, opt->cdict) && zblob.len < blob.len)
		{
			sift_free(blob.data);
			blob = zblob;
			mods = SIFT_ENC_PART_DICT;
		}
		else
			sift_free(zblob.data);
	}
	sift_cc_blob_put(s, &blob);
	sift_free(off);
	sift_free(bytes.data);
	sift_free(blob.data);
	return mods;
}

/*
 * The strings as the symbol table of col/fsst.h: the table, the raw and coded sizes, the coded lengths as a cascade and the codes with one byte past them (the decoder reads one past a
 * string's end). false, nothing written, when the strings are too few bytes or too long for the table to pay, or it saves less than a fifth of the sample.
 */
static bool
put_fsst(StringInfo s, const SiftColIn *col, int n, int nn, const SiftChunkOpts *opt)
{
	const char **p = sift_alloc(nn * sizeof(char *));
	uint32	   *len = sift_alloc(nn * sizeof(uint32));
	uint64		raw = 0;
	SiftFsst   *t = sift_alloc(sizeof(SiftFsst));
	uint8	   *enc;
	int64	   *coded = sift_alloc(nn * sizeof(int64));
	uint32		total = 0,
				hdr[2];
	bool		pays;

	for (int i = 0, k = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			p[k] = col->s[i];
			raw += len[k++] = col->slen[i];
		}
	pays = raw >= SIFT_FSST_MIN_BYTES && raw < (uint64) SIFT_FSST_AVG_MAX * nn && sift_fsst_train(t, p, len, nn) <= SIFT_FSST_MAX_PERMILLE;
	if (pays)
	{
		enc = sift_alloc(2 * (Size) raw + 1);
		for (int k = 0; k < nn; k++)
		{
			coded[k] = sift_fsst_encode(t, p[k], len[k], enc + total);
			total += coded[k];
		}
		sift_fsst_put_table(s, t);
		hdr[0] = (uint32) raw;
		hdr[1] = total;
		sift_sb_append(s, (char *) hdr, 8);
		sift_put_cc_ints(s, coded, nn, opt);
		sift_sb_append(s, (char *) enc, total);
		sift_sb_char(s, '\0');
		pad8(s);
		sift_free(enc);
	}
	sift_free(p);
	sift_free(len);
	sift_free(t);
	sift_free(coded);
	return pays;
}

/* Whether the symbol table's strings are kept over the codec's list of them: the deepest cold level keeps them when no larger, the others when no more than a third larger */
static bool
fsst_beats_block(Size fsst, Size block, SiftCompressTier tier)
{
	/* STOR[STOR-CODEC-04]: L2 takes FSST only when it is no larger than the zstd block (1.00x, was 1.05x; B/str) */
	return tier == SIFT_TIER_COLD_L2 ? fsst <= block : block * 4 > fsst * 3;
}

/* The strings of a cold column that no dictionary holds: the symbol table, or the codec's list when that is smaller by more than the margin of fsst_beats_block or the table does not pay (or the owner keeps compression off) */
static uint16
put_cold_strs(StringInfo s, const SiftColIn *col, int n, int nn, const SiftChunkOpts *opt)
{
	StringInfoData block,
				fsst;
	bool		use_fsst;
	uint16		mods;

	sift_sb_init(&block);
	sift_sb_init(&fsst);
	mods = put_block(&block, col->s, col->slen, col->isnull, n, nn, opt);
	use_fsst = opt->alg >= 0 && put_fsst(&fsst, col, n, nn, opt) && fsst_beats_block(fsst.len, block.len, opt->tier);
	sift_sb_append(s, use_fsst ? fsst.data : block.data, use_fsst ? fsst.len : block.len);
	sift_free(block.data);
	sift_free(fsst.data);
	return use_fsst ? SIFT_CENC_STR_FSST : SIFT_CENC_STR_BLOCK | mods;
}

/*
 * The strings coded by the table of symbols of the whole part: the raw and coded sizes, the coded lengths as a cascade and the codes with one byte past them, and no table. false, nothing
 * written, when this granule's strings do not follow the part's (they code in more than 92% of their size), which keep the rest of the paths.
 */
static bool
put_part_fsst(StringInfo s, const SiftColIn *col, int n, int nn, const SiftChunkOpts *opt)
{
	uint64		raw = 0;
	uint32		total = 0,
				hdr[2];
	uint8	   *enc;
	int64	   *coded = sift_alloc(nn * sizeof(int64));
	bool		pays;

	for (int i = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
			raw += col->slen[i];
	enc = sift_alloc(2 * (Size) raw + 1);
	for (int i = 0, k = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			coded[k] = sift_fsst_encode(opt->fsst, col->s[i], col->slen[i], enc + total);
			total += coded[k++];
		}
	pays = (uint64) total * 100 <= raw * 92;
	if (pays)
	{
		hdr[0] = (uint32) raw;
		hdr[1] = total;
		sift_sb_append(s, (char *) hdr, 8);
		sift_put_cc_ints(s, coded, nn, opt);
		sift_sb_append(s, (char *) enc, total);
		sift_sb_char(s, '\0');
		pad8(s);
	}
	sift_free(enc);
	sift_free(coded);
	return pays;
}

static void
put_view(StringInfo s, const char *const *p, const uint32 *len, const bool *isnull, int n, int nn)
{
	SiftStrView *views = memset(sift_alloc_aligned(sizeof(SiftStrView) * nn, _Alignof(SiftStrView)), 0, sizeof(SiftStrView) * nn);
	StringInfoData heap;
	int			k = 0;

	sift_sb_init(&heap);
	for (int i = 0; i < n; i++)
	{
		SiftStrView *w;

		if (isnull != NULL && isnull[i])
			continue;
		w = &views[k++];
		w->len = len[i];
		memcpy(w->prefix, p[i], Min(len[i], 4));
		if (len[i] <= 12)
		{
			if (len[i] > 4)
				memcpy(w->u.tail, p[i] + 4, len[i] - 4);
		}
		else
		{
			w->u.offset = heap.len;
			sift_sb_append(&heap, p[i], len[i]);
		}
	}
	sift_sb_append(s, (char *) views, nn * sizeof(SiftStrView));
	sift_sb_append(s, heap.data, heap.len);
	pad8(s);
	sift_free(views);
	sift_free(heap.data);
}

static int
fixed16_enc(int hint)
{
	return hint == SIFT_CHINT_UUID ? SIFT_CENC_UUID16 : hint == SIFT_CHINT_NUM128 ? SIFT_CENC_NUM128 : SIFT_CENC_INTERVAL16;
}

#define DICT_TRIAL_DIV		8
#define UUID_PROBE_ROWS		64
#define UUID_PREFIX_MAX		8

/* 16-byte values as the cascades of their two halves: a half that is constant (the upper half of a numeric that fits 64 bits) costs 8 bytes */
static void
put_halves(StringInfo s, const char *const *p, const bool *isnull, int n, int nn, const SiftChunkOpts *opt)
{
	int64	   *lane = sift_alloc(nn * sizeof(int64));

	for (int h = 0; h < 2; h++)
	{
		for (int i = 0, k = 0; i < n; i++)
			if (isnull == NULL || !isnull[i])
				memcpy(&lane[k++], p[i] + 8 * h, 8);
		sift_put_cc_ints(s, lane, nn, opt);
	}
	sift_free(lane);
}

/* STOR[STOR-CODEC-05]: ids that share their top 32 bits (UUIDv7, UUIDv1, ULID) split into FIXED16 halves (-4..6 B/id); random UUIDv4 stay raw 16 B/id */
static bool
uuid_clustered(const SiftColIn *col, int n)
{
	uint32		seen[UUID_PREFIX_MAX];
	int			ns = 0,
				probed = 0,
				step = Max(n / UUID_PROBE_ROWS, 1);

	for (int i = 0; i < n && probed < UUID_PROBE_ROWS; i += step)
	{
		uint32		top;
		int			k = 0;

		if (col->isnull != NULL && col->isnull[i])
			continue;
		probed++;
		memcpy(&top, col->s[i], 4);
		while (k < ns && seen[k] != top)
			k++;
		if (k == ns && ns++ == UUID_PREFIX_MAX)
			return false;
		seen[k] = top;
	}
	return probed > 0;
}

void
sift_chunk_put_str(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	bool		cold = SIFT_TIER_IS_COLD(opt->tier);
	uint32	   *code;
	int		   *first,
			   *slot;
	int			nd = -1,
				cap = 16,
				maxnd;
	bool		fixed = col->hint == SIFT_CHINT_UUID || col->hint == SIFT_CHINT_NUM128 || col->hint == SIFT_CHINT_INTERVAL;
	int			slot_hot[2 * SIFT_DICT_HOT_MAX];

	for (int i = 0; i < n && fixed; i++)
		fixed = (col->isnull != NULL && col->isnull[i]) || col->slen[i] == 16;
	if (!fixed && col->hint == SIFT_CHINT_VIEW)
	{
		put_view(s, col->s, col->slen, col->isnull, n, nn);
		ref->enc |= SIFT_CENC_STR_VIEW;
		return;
	}
	if (opt->dict != NULL && !fixed)
	{
		int64	   *c = sift_alloc(nn * sizeof(int64));

		for (int i = 0, k = 0; i < n; i++)
			if (col->isnull == NULL || !col->isnull[i])
				c[k++] = col->dcode != NULL ? col->dcode[i] : sift_dict_code(opt->dict, col->s[i], col->slen[i]);
		sift_put_cc_ints(s, c, nn, opt);
		ref->enc |= SIFT_CENC_DICT_CODE | SIFT_ENC_PART_DICT;
		sift_free(c);
		return;
	}
	if (nn >= 8)
	{
		code = sift_alloc(nn * sizeof(uint32));
		if (cold)
		{
			maxnd = fixed ? Min(nn / 4, SIFT_DICT_MAX) : Min(nn * 35 / 100, SIFT_DICT_MAX);
			while (cap < nn * 2)
				cap *= 2;
			first = sift_alloc((maxnd + 1) * sizeof(int));
			slot = sift_alloc(cap * sizeof(int));
			nd = maxnd > 0 ? dict_codes(col->s, col->slen, col->isnull, n, code, first, slot, cap, maxnd) : -1;
			sift_free(slot);
		}
		else
		{
			maxnd = Min(nn / 2, SIFT_DICT_HOT_MAX);
			first = sift_alloc((maxnd + 1) * sizeof(int));
			nd = fixed || maxnd == 0 ? -1 : dict_codes(col->s, col->slen, col->isnull, n, code, first, slot_hot, 2 * SIFT_DICT_HOT_MAX, maxnd);
		}
		if (nd > 0)
		{
			/* STOR[STOR-CODEC-04]: a granule dictionary over an eighth of the values is kept only if the zstd block is not 5% smaller (B/str) */
			bool		block_wins = false;
			StringInfoData dict,
						block;
			uint16		mods = 0;

			if (cold && (uint64) nd * DICT_TRIAL_DIV > (uint64) nn)
			{
				sift_sb_init(&dict);
				sift_sb_init(&block);
				put_dict(&dict, col->s, col->slen, code, first, nd, nn, opt);
				mods = put_block(&block, col->s, col->slen, col->isnull, n, nn, opt);
				block_wins = (uint64) block.len * 100 < (uint64) dict.len * 95;
				sift_sb_append(s, block_wins ? block.data : dict.data, block_wins ? block.len : dict.len);
				sift_free(dict.data);
				sift_free(block.data);
			}
			else
				put_dict(s, col->s, col->slen, code, first, nd, nn, opt);
			ref->enc |= block_wins ? SIFT_CENC_STR_BLOCK | mods : SIFT_CENC_DICT_CODE;
			sift_free(code);
			sift_free(first);
			return;
		}
		sift_free(code);
		sift_free(first);
	}
	if (opt->fsst != NULL && !fixed && nn > 0 && opt->tier == SIFT_TIER_COLD_L2)
	{
		StringInfoData part,
					block;
		uint16		mods;

		sift_sb_init(&part);
		sift_sb_init(&block);
		if (put_part_fsst(&part, col, n, nn, opt))
		{
			mods = put_block(&block, col->s, col->slen, col->isnull, n, nn, opt);
			if (fsst_beats_block(part.len, block.len, opt->tier))
			{
				sift_sb_append(s, part.data, part.len);
				ref->enc |= SIFT_CENC_STR_FSST | SIFT_ENC_PART_DICT;
			}
			else
			{
				sift_sb_append(s, block.data, block.len);
				ref->enc |= SIFT_CENC_STR_BLOCK | mods;
			}
			sift_free(part.data);
			sift_free(block.data);
			return;
		}
		sift_free(part.data);
		sift_free(block.data);
	}
	else if (opt->fsst != NULL && !fixed && nn > 0 && put_part_fsst(s, col, n, nn, opt))
	{
		ref->enc |= SIFT_CENC_STR_FSST | SIFT_ENC_PART_DICT;
		return;
	}
	if (fixed)
	{
		if (cold && (col->hint != SIFT_CHINT_UUID || uuid_clustered(col, n)))
		{
			put_halves(s, col->s, col->isnull, n, nn, opt);
			ref->enc |= SIFT_CENC_FIXED16;
		}
		else
		{
			for (int i = 0; i < n; i++)
				if (col->isnull == NULL || !col->isnull[i])
					sift_sb_append(s, col->s[i], 16);
			ref->enc |= fixed16_enc(col->hint);
		}
	}
	else if (cold)
		ref->enc |= put_cold_strs(s, col, n, nn, opt);
	else
	{
		put_inline(s, col->s, col->slen, col->isnull, n);
		ref->enc |= SIFT_CENC_STR_INLINE;
	}
}



/* The dictionary of a column over the whole part */

static int
enc_str_cmp(const void *a, const void *b, void *arg)
{
	const DictSort *ds = arg;

	return first_cmp_len(ds->p, ds->len, *(const int *) a, *(const int *) b);
}

uint32
sift_part_dict_max(uint64 nn)
{
	return nn < 64 ? 0 : (uint32) Min(nn * 35 / 100, SIFT_DICT_PART_MAX);
}

SiftDictEnc *
sift_dict_enc_build(const SiftColIn *col, int n)
{
	int			nn = 0,
				cap = 16,
				nd,
				maxnd;
	uint32	   *code,
			   *rank;
	int		   *first,
			   *slot,
			   *order;
	SiftDictEnc *d;
	DictSort	ctx;

	if (col->kind != SIFT_CELL_STR || col->hint != SIFT_CHINT_AUTO)
		return NULL;
	for (int i = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			if (col->slen[i] > SIFT_DICT_ENTRY_MAX)
				return NULL;
			nn++;
		}
	maxnd = sift_part_dict_max(nn);
	if (maxnd == 0)
		return NULL;
	while (cap < Min(nn, 2 * SIFT_DICT_PART_MAX) * 2)
		cap *= 2;
	code = sift_alloc(nn * sizeof(uint32));
	first = sift_alloc((maxnd + 1) * sizeof(int));
	slot = sift_alloc(cap * sizeof(int));
	nd = dict_codes(col->s, col->slen, col->isnull, n, code, first, slot, cap, maxnd);
	sift_free(slot);
	if (nd <= 0)
	{
		sift_free(code);
		sift_free(first);
		return NULL;
	}
	order = sift_alloc(nd * sizeof(int));
	memcpy(order, first, nd * sizeof(int));
	ctx = (DictSort) {col->s, col->slen};
	qsort_arg(order, nd, sizeof(int), enc_str_cmp, &ctx);
	d = sift_alloc0(sizeof(SiftDictEnc));
	d->n = nd;
	d->p = sift_alloc(nd * sizeof(char *));
	d->len = sift_alloc(nd * sizeof(uint32));
	for (int i = 0; i < nd; i++)
	{
		d->p[i] = col->s[order[i]];
		d->len[i] = col->slen[order[i]];
	}
	/* the code of every row in the sorted dictionary, from the codes the rows got as they came: one search for each distinct string, none for a row */
	rank = sift_alloc(nd * sizeof(uint32));
	for (int id = 0; id < nd; id++)
		rank[id] = sift_dict_code(d, col->s[first[id]], col->slen[first[id]]);
	/* check-memory: allow the rows of one part build, as the codes and the strings of the rows are */
	d->row_codes = sift_alloc(Max(n, 1) * sizeof(uint32));
	d->nrows = n;
	for (int i = 0, k = 0; i < n; i++)
		d->row_codes[i] = (col->isnull != NULL && col->isnull[i]) ? 0 : rank[code[k++]];
	sift_free(rank);
	sift_free(order);
	sift_free(code);
	sift_free(first);
	return d;
}

uint32
sift_dict_code(const SiftDictEnc *d, const char *s, uint32 len)
{
	uint32		lo = 0,
				hi = d->n;

	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;
		int			c = memcmp(d->p[mid], s, Min(d->len[mid], len));

		if (c == 0)
			c = (d->len[mid] > len) - (d->len[mid] < len);
		if (c == 0)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	SIFT_INVARIANT(false, "a string of a column is in the dictionary of its part");
	return 0;
}

