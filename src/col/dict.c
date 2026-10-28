/*
 * col/dict.c - L1 of the column engine: the dictionaries of strings (code = first-seen order).
 */
#include "core/host.h"

#include "col/enc.h"
#include "col/codec.h"
#include "col/compress.h"
#include "col/enc_int.h"
#include "core/cmp.h"
#include "core/limits.h"

/* ---------------------------------------------------------------------------
 * 2. Dictionaries
 * ------------------------------------------------------------------------- */

#define DICT_MINCAP		16

SiftCDict *
sift_cdict_create(MemoryContext mcxt)
{
	SiftCDict  *d = MemoryContextAllocZero(mcxt, sizeof(SiftCDict));

	d->mcxt = mcxt;
	d->cap = DICT_MINCAP;
	d->off = MemoryContextAllocZero(mcxt, (d->cap + 1) * sizeof(uint32));
	d->bytescap = 256;
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	return d;
}

void
cdict_reset(SiftCDict *d)
{
	d->n = 0;
	d->nbytes = 0;
	d->off[0] = 0;
	if (d->slots)
		memset(d->slots, 0, d->nslots * sizeof(uint32));
}

static uint32 *
cdict_probe(const SiftCDict *d, const char *s, uint32 len)
{
	uint32		mask = d->nslots - 1;

	for (uint32 i = hash_bytes((const unsigned char *) s, (int) len) & mask;; i = (i + 1) & mask)
	{
		uint32		c = d->slots[i];

		if (c == 0 || (cdict_len(d, c - 1) == len && memcmp(d->bytes + d->off[c - 1], s, len) == 0))
			return &d->slots[i];
	}
}

/* The hash table exists and has room for one more code at load <= 1/2 (rebuilt by doubling) */
static void
cdict_ready(SiftCDict *d)
{
	if (d->slots != NULL && (d->n + 1) * 2 <= d->nslots)
		return;
	if (d->slots)
		pfree(d->slots);
	d->nslots = pg_nextpower2_32(Max(DICT_MINCAP, d->n + 1) * 2);
	d->slots = MemoryContextAllocZero(d->mcxt, d->nslots * sizeof(uint32));
	for (uint32 c = 0; c < d->n; c++)
		*cdict_probe(d, d->bytes + d->off[c], cdict_len(d, c)) = c + 1;
}

uint32
sift_cdict_intern(SiftCDict *d, const char *s, uint32 len)
{
	uint32	   *slot;
	int64_t		max_value = sift_limit(string_bytes, SIFT_COL_MAX_VALUE);

	if (len > max_value)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a value of %u bytes does not fit a sift_col column (at most %lld bytes)", len, (long long) max_value),
				 errhint("Leave the column out of the index, or index left(column, N) or a hash of it.")));
	if (d->nbytes + len > max_value)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the strings of one unit of a sift_col column exceed %lld bytes", (long long) max_value),
				 errhint("Leave the column out of the index, or index left(column, N) or a hash of it.")));
	cdict_ready(d);
	slot = cdict_probe(d, s, len);
	if (*slot)
		return *slot - 1;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->off = repalloc(d->off, (d->cap + 1) * sizeof(uint32));
	}
	if (d->nbytes + len > d->bytescap)
	{
		d->bytescap = Max(d->bytescap * 2, d->nbytes + len);
		d->bytes = repalloc(d->bytes, d->bytescap);
	}
	memcpy(d->bytes + d->nbytes, s, len);
	d->nbytes += len;
	d->off[d->n + 1] = (uint32) d->nbytes;
	*slot = d->n + 1;
	return d->n++;
}

/*
 * A dictionary read for a scan is searched entry by entry until it has been asked DICT_HASH_AFTER times: the table (about 16 bytes a code) and the
 * sorted order (4) cost as much as a few dozen such searches, and the first query of a backend, or of a dictionary too large to be kept
 * (col/dictcache.c), would pay them for one use. A dictionary of the write path has its table from the first intern.
 */
#define DICT_HASH_AFTER		8
#define DICT_SORT_AFTER		16
#define DICT_SORT_MIN		1024
/* ... and sorted for the rows of a top-N once the scans have compared this many times its entries as bytes (a sort costs about that many compares) */
#define DICT_ORDER_ROWS		4

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0)
		return -1;
	if (d->look)
		return d->look->find(d->look, s, len);
	if (d->slots == NULL && d->finds++ < DICT_HASH_AFTER)
	{
		for (uint32 c = 0; c < d->n; c++)
			if (cdict_len(d, c) == len && memcmp(d->bytes + d->off[c], s, len) == 0)
				return c;
		return -1;
	}
	cdict_ready(d);
	return (int64) *cdict_probe(d, s, len) - 1;
}

/* Bytewise order of dictionary values (the C collation) */
int
sift_cstr_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			r = memcmp(a, b, Min(alen, blen));

	return r != 0 ? r : (alen > blen) - (alen < blen);
}

bool
sift_cstr_matches(SiftColOp op, const char *s, uint32 len, const SiftColValue *c)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			return sift_cstr_cmp(s, len, c->v.s, c->len) < 0;
		case SIFT_COLOP_LE:
			return sift_cstr_cmp(s, len, c->v.s, c->len) <= 0;
		case SIFT_COLOP_GT:
			return sift_cstr_cmp(s, len, c->v.s, c->len) > 0;
		case SIFT_COLOP_GE:
			return sift_cstr_cmp(s, len, c->v.s, c->len) >= 0;
		case SIFT_COLOP_CONTAINS:
			return sift_colk_contains1((const uint8 *) s, len, (const uint8 *) c->v.s, c->len);
		default:					/* PREFIX */
			return len >= c->len && memcmp(s, c->v.s, c->len) == 0;
	}
}

typedef struct SortEnt
{
	uint64		key;			/* the first 8 bytes, zero padded, most significant first: byte order of the keys is byte order of the strings up to a tie */
	uint32		code;
} SortEnt;

static int
sort_ent_cmp(const void *a, const void *b, void *arg)
{
	const SiftCDict *d = arg;
	uint32		x = ((const SortEnt *) a)->code,
				y = ((const SortEnt *) b)->code;

	return sift_cstr_cmp(d->bytes + d->off[x], cdict_len(d, x), d->bytes + d->off[y], cdict_len(d, y));
}

#define SORT_SMALL	32		/* a group of this many strings or fewer is sorted by comparing whole strings */

/* the key of the 8 bytes of string c from at, zero padded: byte order of the keys is byte order of the strings from at, up to a tie */
static uint64
sort_key(const SiftCDict *d, uint32 c, uint32 at)
{
	const uint8 *s = (const uint8 *) d->bytes + d->off[c];
	uint32		len = cdict_len(d, c);
	uint64		k = 0;

	for (uint32 i = 0; i < sizeof(uint64); i++)
		k = k << 8 | (at + i < len ? s[at + i] : 0);
	return k;
}

/*
 * The strings of a[0..n) all begin with the same depth bytes: sorted by a radix sort on the 8 bytes after the prefix they still share (the strings of a column
 * of urls, paths or prefixed ids share long ones), then each run that ties on them by the 8 bytes after, until it is small or no string goes on; b is scratch.
 */
static void
sort_group(const SiftCDict *d, SortEnt *a, SortEnt *b, uint32 n, uint32 depth)
{
	const uint8 *s0;
	uint32		len0,
				share,
				maxlen = 0,
				at;
	SortEnt    *src = a,
			   *dst = b;

	if (n <= SORT_SMALL)
	{
		qsort_arg(a, n, sizeof(SortEnt), sort_ent_cmp, (void *) d);
		return;
	}
	s0 = (const uint8 *) d->bytes + d->off[a[0].code];
	len0 = cdict_len(d, a[0].code);
	share = len0 > depth ? len0 - depth : 0;
	for (uint32 i = 0; i < n; i++)
	{
		const uint8 *s = (const uint8 *) d->bytes + d->off[a[i].code];
		uint32		len = cdict_len(d, a[i].code),
					k = 0;

		maxlen = Max(maxlen, len);
		while (k < share && depth + k < len && s[depth + k] == s0[depth + k])
			k++;
		share = k;
	}
	at = depth + share;
	if (at >= maxlen)
	{
		qsort_arg(a, n, sizeof(SortEnt), sort_ent_cmp, (void *) d);
		return;
	}
	for (uint32 i = 0; i < n; i++)
		a[i].key = sort_key(d, a[i].code, at);
	for (int pass = 0; pass < 8; pass++)
	{
		uint32		count[256] = {0},
					pos = 0;
		int			shift = pass * 8;

		for (uint32 i = 0; i < n; i++)
			count[(src[i].key >> shift) & 255]++;
		if (count[(src[0].key >> shift) & 255] == n)
			continue;
		for (int v = 0; v < 256; v++)
		{
			uint32		c = count[v];

			count[v] = pos;
			pos += c;
		}
		for (uint32 i = 0; i < n; i++)
			dst[count[(src[i].key >> shift) & 255]++] = src[i];
		{
			SortEnt    *t = src;

			src = dst;
			dst = t;
		}
	}
	if (src != a)
		memcpy(a, src, n * sizeof(SortEnt));
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && a[j].key == a[i].key; j++)
			;
		if (j - i > 1)
			sort_group(d, a + i, b + i, j - i, at + sizeof(uint64));
	}
}

/* d->sorted: the codes of the strings in byte order */
static void
cdict_sort(SiftCDict *d)
{
	SortEnt    *a = palloc(d->n * sizeof(SortEnt)),
			   *b = palloc(d->n * sizeof(SortEnt));

	for (uint32 c = 0; c < d->n; c++)
		a[c].code = c;
	sort_group(d, a, b, d->n, 0);
	d->sorted = MemoryContextAlloc(d->mcxt, (Size) d->n * sizeof(uint32));
	for (uint32 i = 0; i < d->n; i++)
		d->sorted[i] = a[i].code;
	pfree(a);
	pfree(b);
}

/* The first position of the sorted order whose string is not below c (after: not at or below it) */
uint32
sift_csorted_bound(const SiftCSorted *s, const SiftColValue *c, bool after)
{
	uint32		lo = 0,
				hi = s->n;

	SIFT_BISECT(uint32, m, lo, hi,
				after ? sift_csorted_cmp(s, m, c) <= 0 : sift_csorted_cmp(s, m, c) < 0);
	return lo;
}

/* The positions [from, to) of the sorted order whose strings pass op (LT, LE, GT, GE in byte order, PREFIX) against c */
void
sift_csorted_range(const SiftCSorted *s, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			*from = 0;
			*to = sift_csorted_bound(s, c, false);
			break;
		case SIFT_COLOP_LE:
			*from = 0;
			*to = sift_csorted_bound(s, c, true);
			break;
		case SIFT_COLOP_GT:
			*from = sift_csorted_bound(s, c, true);
			*to = s->n;
			break;
		case SIFT_COLOP_GE:
			*from = sift_csorted_bound(s, c, false);
			*to = s->n;
			break;
		default:					/* PREFIX: the strings that start with c are the run that begins where c would be inserted */
			{
				uint32		lo = sift_csorted_bound(s, c, false),
							hi = s->n;

				*from = lo;
				SIFT_BISECT(uint32, m, lo, hi, sift_csorted_prefix(s, m, c));
				*to = lo;
			}
	}
}

static const char *
mem_str(const void *ctx, uint32 pos, uint32 *len)
{
	const SiftCDict *d = ctx;

	return sift_cdict_str(d, d->sorted[pos], len);
}

uint32
sift_cdict_match(SiftCDict *d, SiftColOp op, const SiftColValue *c, uint64 *lut)
{
	uint32		count = 0,
				from,
				to;

	if (d->look)
		return d->look->match(d->look, op, c, lut);
	if (op == SIFT_COLOP_CONTAINS)
	{
		sift_colk->str_contains(lut, d->bytes, d->off, d->n, c->v.s, c->len);
		return sift_colk->bits_count(lut, (d->n + 63) / 64);
	}
	if (d->sorted == NULL && d->n >= DICT_SORT_MIN && d->tests++ >= DICT_SORT_AFTER)
		cdict_sort(d);
	if (d->sorted == NULL)
	{
		for (uint32 k = 0; k < d->n; k++)
			if (op == SIFT_COLOP_PREFIX ? cdict_len(d, k) >= c->len && memcmp(SIFT_COL_DICT_STR(d, k), c->v.s, c->len) == 0
				: sift_cstr_matches(op, SIFT_COL_DICT_STR(d, k), cdict_len(d, k), c))
			{
				lut[k >> 6] |= UINT64CONST(1) << (k & 63);
				count++;
			}
		return count;
	}
	sift_csorted_range(&(SiftCSorted) {d->n, mem_str, d}, op, c, &from, &to);
	for (uint32 i = from; i < to; i++)
		lut[d->sorted[i] >> 6] |= UINT64CONST(1) << (d->sorted[i] & 63);
	return to - from;
}

const char *
sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len)
{
	*len = cdict_len(d, code);
	return d->bytes + d->off[code];
}

/*
 * A serialized dictionary starts with u32 form, u32 n. The plain form (0) goes on with u32 off[n + 1] and the bytes; the string list form
 * (1) with the codec's list (the lengths as an integer cascade, the bytes under the cold wrapper rule), which starts 8-aligned.
 */

/* The rows of the codes in byte order, by block: the layout of DICT_LOOKUP_COUNT (col/enc.h) */
static void
put_counts(const SiftCDict *d, StringInfo out)
{
	uint32		nb = DICT_COUNT_NB(d->n),
				cum = 0;
	uint32	   *rec = palloc0(((Size) nb + 1) * 2 * sizeof(uint32));
	uint8	   *cmin = palloc0(((Size) nb + 3) / 4 * 4);
	StringInfoData data;

	initStringInfo(&data);
	for (uint32 b = 0; b < nb; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					least = UINT32_MAX,
					most = 0,
					w = 0,
					at;
		uint8	   *bits;

		for (uint32 i = 0; i < cnt; i++)
			least = Min(least, d->rows[d->sorted[lo + i]]);
		cmin[b] = (uint8) Min(least, 255);
		for (uint32 i = 0; i < cnt; i++)
			most = Max(most, d->rows[d->sorted[lo + i]] - cmin[b]);
		while (most >> w)
			w++;
		rec[2 * b] = cum;
		rec[2 * b + 1] = data.len;
		at = data.len;
		for (uint32 i = 0; i < DICT_BLOCK * w / 8; i++)
			appendStringInfoChar(&data, '\0');
		bits = (uint8 *) data.data + at;
		for (uint32 i = 0; i < cnt; i++)
		{
			uint32		x = d->rows[d->sorted[lo + i]] - cmin[b];

			cum += d->rows[d->sorted[lo + i]];
			for (uint32 k = 0; k < w; k++)
				if (x >> k & 1)
					bits[(i * w + k) / 8] |= 1 << ((i * w + k) % 8);
		}
	}
	rec[2 * nb] = cum;
	rec[2 * nb + 1] = data.len;
	put_bytes(out, rec, ((Size) nb + 1) * 2 * sizeof(uint32));
	put_bytes(out, cmin, ((Size) nb + 3) / 4 * 4);
	put_bytes(out, data.data, data.len);
	pfree(rec);
	pfree(cmin);
	pfree(data.data);
}

/*
 * The forms read in place: the strings in blocks of DICT_BLOCK codes, each compressed on its own when the policy allows and it is smaller (a string
 * is one read of its block), and for DICT_LOOKUP the codes in byte order of the strings (the work of a bisection is a few block reads)
 */
uint64
sift_cdict_hash(const char *s, uint32 len)
{
	return hash_bytes_extended((const unsigned char *) s, (int) len, 0);
}

/* The hash of the strings to their codes, DICT_HASHED */
static void
put_hash(const SiftCDict *d, StringInfo out)
{
	uint32		ns = SIFT_CDICT_HASH_SLOTS(d->n),
				cb = sift_cdict_hash_cb(d->n);
	uint32	   *slot = palloc0((Size) ns * sizeof(uint32));

	for (uint32 c = 0; c < d->n; c++)
	{
		uint64		h = sift_cdict_hash(d->bytes + d->off[c], cdict_len(d, c));

		for (uint32 i = sift_cdict_hash_pos(h, ns);; i = i + 1 == ns ? 0 : i + 1)
			if (slot[i] == 0)
			{
				slot[i] = sift_cdict_hash_slot(h, cb, c);
				break;
			}
	}
	put_bytes(out, slot, (Size) ns * sizeof(uint32));
	pfree(slot);
}

static void
serialize_blocks(SiftCDict *d, const SiftCcPolicy *pol, uint32 form, StringInfo out)
{
	uint32		fixed = d->n > 0 ? cdict_len(d, 0) : 0,
				nbytes = (uint32) d->nbytes,
				nblk = (d->n + DICT_BLOCK - 1) / DICT_BLOCK,
				zero = 0;
	int			start = out->len,
				tab;
	uint32	   *boff = palloc((nblk + 1) * sizeof(uint32));
	uint8	   *raw = palloc(DICT_BLOCK_RAW_MAX),
			   *z = NULL;
	Size		zcap = sift_colz_bound(SIFT_CC_ALG_ZSTD, DICT_BLOCK_RAW_MAX);
	bool		zip = pol != NULL && pol->mode != SIFT_CCM_NONE && sift_colz_available(SIFT_CC_ALG_ZSTD);

	for (uint32 c = 1; c < d->n && fixed != 0; c++)
		if (cdict_len(d, c) != fixed)
			fixed = 0;
	if (form == DICT_LOOKUP && d->sorted == NULL)
		cdict_sort(d);
	if (zip)
		z = palloc(zcap);
	if (form == DICT_LOOKUP && d->rows != NULL)
		form = DICT_LOOKUP_COUNT;
	if (d->hashed && (form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT))
		form |= DICT_HASHED;
	put_val(out, form);
	put_val(out, d->n);
	put_val(out, fixed);
	put_val(out, nbytes);
	tab = out->len;
	for (uint32 b = 0; b <= nblk; b++)
		put_val(out, zero);
	boff[0] = 0;
	for (uint32 b = 0; b < nblk; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					rawlen = 0,
					clen = 0;
		uint8		hdr[DICT_BLOCK_HEAD] = {0};

		if (fixed == 0)
			for (uint32 i = 0; i < cnt; i++)
				raw[rawlen++] = (uint8) cdict_len(d, lo + i);
		memcpy(raw + rawlen, d->bytes + d->off[lo], d->off[lo + cnt] - d->off[lo]);
		rawlen += d->off[lo + cnt] - d->off[lo];
		if (zip)
			clen = (uint32) sift_colz_compress(SIFT_CC_ALG_ZSTD, 1, raw, rawlen, z, zcap);
		hdr[2] = (uint8) rawlen;
		hdr[3] = (uint8) (rawlen >> 8);
		if (clen > 0 && clen < rawlen)
		{
			hdr[0] = SIFT_CC_ALG_ZSTD;
			put_bytes(out, hdr, sizeof(hdr));
			put_bytes(out, z, clen);
		}
		else
		{
			put_bytes(out, hdr, sizeof(hdr));
			put_bytes(out, raw, rawlen);
		}
		boff[b + 1] = out->len - tab - (nblk + 1) * sizeof(uint32);
	}
	memcpy(out->data + tab, boff, (nblk + 1) * sizeof(uint32));
	if ((form & ~DICT_HASHED) == DICT_LOOKUP || (form & ~DICT_HASHED) == DICT_LOOKUP_COUNT)
	{
		while ((out->len - start) % sizeof(uint32))
			appendStringInfoChar(out, '\0');
		put_bytes(out, d->sorted, (Size) d->n * sizeof(uint32));
		if ((form & ~DICT_HASHED) == DICT_LOOKUP_COUNT)
			put_counts(d, out);
		if (form & DICT_HASHED)
			put_hash(d, out);
	}
	pfree(boff);
	pfree(raw);
	if (z)
		pfree(z);
}

#define BLOCKS_SAMPLE	16				/* blocks spread over the dictionary that sift_cdict_blocks_extra compresses */
#define COMPACT_SAMPLE	(1 << 20)		/* bytes from the start of the dictionary that it encodes in the compact form */

Size
sift_cdict_blocks_extra(SiftCDict *d, const SiftCcPolicy *pol, Size *compact_bytes)
{
	uint32		nblk = (d->n + DICT_BLOCK - 1) / DICT_BLOCK,
				ns = Min(nblk, BLOCKS_SAMPLE),
				fixed = d->n > 0 ? cdict_len(d, 0) : 0,
				pn = 0;
	uint8	   *raw = palloc(DICT_BLOCK_RAW_MAX),
			   *z = NULL;
	Size		zcap = sift_colz_bound(SIFT_CC_ALG_ZSTD, DICT_BLOCK_RAW_MAX),
				blocks = 0,
				plain,
				compact;
	bool		zip = pol != NULL && pol->mode != SIFT_CCM_NONE && sift_colz_available(SIFT_CC_ALG_ZSTD);

	for (uint32 c = 1; c < d->n && fixed != 0; c++)
		if (cdict_len(d, c) != fixed)
			fixed = 0;
	if (zip)
		z = palloc(zcap);
	for (uint32 k = 0; k < ns; k++)
	{
		uint32		lo = (uint32) ((uint64) k * nblk / ns) * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, d->n - lo),
					rawlen = 0,
					clen = 0;

		if (fixed == 0)
			for (uint32 i = 0; i < cnt; i++)
				raw[rawlen++] = (uint8) cdict_len(d, lo + i);
		memcpy(raw + rawlen, d->bytes + d->off[lo], d->off[lo + cnt] - d->off[lo]);
		rawlen += d->off[lo + cnt] - d->off[lo];
		if (zip)
			clen = (uint32) sift_colz_compress(SIFT_CC_ALG_ZSTD, 1, raw, rawlen, z, zcap);
		blocks += sizeof(uint32) + DICT_BLOCK_HEAD + (clen > 0 && clen < rawlen ? clen : rawlen);
	}
	/* a block is the same size wherever it lies; the compact form is not (it compresses better with its length), so it is encoded from the start for as many bytes as it is cheap to */
	while (pn < d->n && d->off[pn] < COMPACT_SAMPLE)
		pn++;
	plain = sizeof(uint32) * (pn + 1) + d->off[pn];
	compact = plain;
	if (zip && plain >= SIFT_CC_MIN_WRAP)
	{
		StringInfoData e;

		initStringInfo(&e);
		sift_cc_encode_strs(&e, d->off, d->bytes, pn, pol);
		compact = Min(compact, (Size) e.len);
		pfree(e.data);
	}
	compact = (Size) ((double) compact * d->nbytes / Max(d->off[pn], 1));
	blocks = (Size) ((double) blocks * nblk / ns);
	*compact_bytes = compact;
	pfree(raw);
	if (z)
		pfree(z);
	return blocks > compact ? blocks - compact : 0;
}

void
sift_cdict_serialize(SiftCDict *d, const SiftCcPolicy *pol, uint32 blocks, StringInfo out)
{
	Size		plain = sizeof(uint32) * (d->n + 1) + d->nbytes;
	uint32		form = DICT_PLAIN;
	StringInfoData z;

	if (blocks != 0 && d->n > 0)
	{
		serialize_blocks(d, pol, blocks, out);
		return;
	}
	z.data = NULL;
	if (pol != NULL && pol->mode != SIFT_CCM_NONE && d->n <= SIFT_CC_MAXROWS && plain >= SIFT_CC_MIN_WRAP)
	{
		initStringInfo(&z);
		sift_cc_encode_strs(&z, d->off, d->bytes, d->n, pol);
		if ((Size) z.len < plain)
			form = DICT_STRS;
	}
	put_val(out, form);
	put_val(out, d->n);
	if (form == DICT_STRS)
		put_bytes(out, z.data, z.len);
	else
	{
		put_bytes(out, d->off, (d->n + 1) * sizeof(uint32));
		put_bytes(out, d->bytes, d->nbytes);
	}
	if (z.data)
		pfree(z.data);
}

/* The plain form read straight into the arrays of the dictionary (sift_cdict_plain_*): the offsets first, then the bytes they end at, then the checks cdict_from makes */
SiftCDict *
sift_cdict_plain_begin(MemoryContext mcxt, uint32 n)
{
	SiftCDict  *d = sift_cdict_create(mcxt);

	pfree(d->off);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	return d;
}

bool
sift_cdict_plain_bytes(SiftCDict *d, MemoryContext mcxt, uint64 room)
{
	if (d->off[0] != 0 || d->off[d->n] > room)
		return false;
	d->nbytes = d->off[d->n];
	d->bytescap = Max(d->nbytes, 256);
	pfree(d->bytes);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	return true;
}

bool
sift_cdict_plain_valid(const SiftCDict *d)
{
	for (uint32 c = 0; c < d->n; c++)
		if (d->off[c + 1] < d->off[c] || d->off[c + 1] - d->off[c] > SIFT_COL_DICT_MAX_ENTRY)
			return false;
	return true;
}

/* A dictionary of n strings that takes the arrays off[n + 1] (room for Max(n, DICT_MINCAP) + 1) and bytes (room for Max(nbytes, 256)) allocated in mcxt */
static SiftCDict *
cdict_adopt(MemoryContext mcxt, uint32 n, uint32 *off, char *bytes, Size nbytes)
{
	SiftCDict  *d;

	if (off[0] != 0)
		return NULL;
	d = sift_cdict_create(mcxt);
	pfree(d->off);
	pfree(d->bytes);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = off;
	d->nbytes = off[n];
	d->bytescap = Max(nbytes, 256);
	d->bytes = bytes;
	if (d->nbytes != nbytes || !sift_cdict_plain_valid(d))
	{
		d->off = NULL;
		d->bytes = NULL;
		pfree(d);
		return NULL;
	}
	return d;
}

/* A dictionary of n strings: off[n + 1] and bytes[off[n]] are copied */
static SiftCDict *
cdict_from(MemoryContext mcxt, uint32 n, const uint32 *off, const char *bytes)
{
	SiftCDict  *d;

	if (off[0] != 0)
		return NULL;
	d = sift_cdict_create(mcxt);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	memcpy(d->off, off, ((Size) n + 1) * sizeof(uint32));
	d->nbytes = d->off[n];
	for (uint32 c = 0; c < n; c++)
		if (d->off[c + 1] < d->off[c] || cdict_len(d, c) > SIFT_COL_DICT_MAX_ENTRY)
			return NULL;
	d->bytescap = Max(d->nbytes, 256);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	memcpy(d->bytes, bytes, d->nbytes);
	return d;
}

/* The dictionary of a blocks form in memory, with its order for DICT_LOOKUP (the codes are checked: the order is trusted to find and match, sift.col_verify checks that it is one) */
static SiftCDict *
parse_blocks(MemoryContext mcxt, const uint8 *p, Size len, uint32 form, Size *used)
{
	uint32		head[4],
				n,
				fixed,
				nbytes,
				nblk,
				pos = 0;
	uint64		blk0,
				total,
				perm_end;
	uint32	   *boff,
			   *off;
	uint8	   *raw;
	char	   *bytes;
	SiftCDict  *d = NULL;
	bool		ok = true,
				hashed = (form & DICT_HASHED) != 0;

	form &= ~DICT_HASHED;
	if (len < DICT_HEAD_LOOKUP)
		return NULL;
	memcpy(head, p, DICT_HEAD_LOOKUP);
	n = head[1];
	fixed = head[2];
	nbytes = head[3];
	if (n == 0 || fixed > SIFT_COL_DICT_MAX_ENTRY || (fixed != 0 && (uint64) n * fixed != nbytes))
		return NULL;
	nblk = (n + DICT_BLOCK - 1) / DICT_BLOCK;
	blk0 = DICT_HEAD_LOOKUP + ((uint64) nblk + 1) * sizeof(uint32);
	if (blk0 > len)
		return NULL;
	boff = palloc(((Size) nblk + 1) * sizeof(uint32));
	memcpy(boff, p + DICT_HEAD_LOOKUP, ((Size) nblk + 1) * sizeof(uint32));
	total = blk0 + boff[nblk];
	if (form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT)
		total = (total + sizeof(uint32) - 1) / sizeof(uint32) * sizeof(uint32) + (uint64) n * sizeof(uint32);
	perm_end = total;
	if (form == DICT_LOOKUP_COUNT)
	{
		uint32		last;

		if (total + DICT_COUNT_HEAD(n) > len)
		{
			pfree(boff);
			return NULL;
		}
		memcpy(&last, p + total + (uint64) 8 * DICT_COUNT_NB(n) + sizeof(uint32), sizeof(last));
		total += DICT_COUNT_HEAD(n) + last;
	}
	if (hashed)
		total += (uint64) SIFT_CDICT_HASH_SLOTS(n) * sizeof(uint32);
	if (boff[0] != 0 || blk0 + boff[nblk] > len || total > len)
	{
		pfree(boff);
		return NULL;
	}
	/* the arrays are the dictionary's own (cdict_adopt), at the sizes a dictionary of its codes has, so that nothing is copied again */
	off = MemoryContextAlloc(mcxt, ((Size) Max(n, DICT_MINCAP) + 1) * sizeof(uint32));
	bytes = MemoryContextAlloc(mcxt, Max(nbytes, 256));
	raw = palloc(DICT_BLOCK_RAW_MAX);
	for (uint32 b = 0; b < nblk && ok; b++)
	{
		uint32		lo = b * DICT_BLOCK,
					cnt = Min(DICT_BLOCK, n - lo),
					rawlen,
					clen,
					sum = 0;
		const uint8 *h = p + blk0 + boff[b],
				   *src;

		if (boff[b + 1] < boff[b] + DICT_BLOCK_HEAD)
		{
			ok = false;
			break;
		}
		clen = boff[b + 1] - boff[b] - DICT_BLOCK_HEAD;
		rawlen = h[2] | (uint32) h[3] << 8;
		src = h + DICT_BLOCK_HEAD;
		if (rawlen > DICT_BLOCK_RAW_MAX)
			ok = false;
		else if (h[0] == 0)
			ok = clen == rawlen;
		else
		{
			ok = h[0] == SIFT_CC_ALG_ZSTD && sift_colz_available(h[0]) && sift_colz_decompress(h[0], src, clen, raw, rawlen);
			src = raw;
		}
		if (ok && fixed != 0)
		{
			ok = rawlen == cnt * fixed;
			for (uint32 i = 0; ok && i < cnt; i++)
				off[lo + i] = pos + i * fixed;
			sum = rawlen;
		}
		else if (ok)
		{
			ok = rawlen >= cnt;
			for (uint32 i = 0; ok && i < cnt; i++)
			{
				off[lo + i] = pos + sum;
				sum += src[i];
				ok = src[i] <= SIFT_COL_DICT_MAX_ENTRY;
			}
			ok = ok && sum == rawlen - cnt;
			src += cnt;
		}
		if (ok)
			ok = (uint64) pos + sum <= nbytes;
		if (ok)
		{
			memcpy(bytes + pos, src, sum);
			pos += sum;
		}
	}
	if (ok && pos == nbytes)
	{
		off[n] = pos;
		d = cdict_adopt(mcxt, n, off, bytes, nbytes);
	}
	pfree(boff);
	pfree(raw);
	if (d == NULL)
	{
		pfree(off);
		pfree(bytes);
		return NULL;
	}
	d->blocked = true;
	if (form == DICT_BLOCKS)
	{
		*used = total;
		return d;
	}
	d->stored_order = true;
	d->sorted = MemoryContextAlloc(mcxt, (Size) n * sizeof(uint32));
	memcpy(d->sorted, p + (perm_end - (uint64) n * sizeof(uint32)), (Size) n * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
		if (d->sorted[i] >= n)
			return NULL;
	*used = total;
	return d;
}

SiftCDict *
sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used)
{
	const uint8 *p = data;
	uint32		head[2],
				nbytes;

	if (len < DICT_HEAD)
		return NULL;
	memcpy(head, p, DICT_HEAD);
	if ((head[0] & ~DICT_HASHED) == DICT_LOOKUP || (head[0] & ~DICT_HASHED) == DICT_LOOKUP_COUNT || head[0] == DICT_BLOCKS)
		return parse_blocks(mcxt, p, len, head[0], used);
	p += DICT_HEAD;
	len -= DICT_HEAD;
	if (head[0] == DICT_STRS)
	{
		uint32	   *off;
		char	   *bytes;
		SiftCDict  *d;

		sift_cc_decode_strs(mcxt, p, len, head[1], &off, &bytes);
		d = cdict_from(mcxt, head[1], off, bytes);
		pfree(off);
		pfree(bytes);
		*used = DICT_HEAD + len;
		return d;
	}
	if (head[0] != DICT_PLAIN || len / sizeof(uint32) < (Size) head[1] + 1)
		return NULL;
	memcpy(&nbytes, p + sizeof(uint32) * head[1], sizeof(nbytes));
	if (len - sizeof(uint32) * ((Size) head[1] + 1) < nbytes)
		return NULL;
	*used = DICT_HEAD + sizeof(uint32) * ((Size) head[1] + 1) + nbytes;
	return cdict_from(mcxt, head[1], (const uint32 *) p, (const char *) (p + sizeof(uint32) * ((Size) head[1] + 1)));
}

static const uint32 *
cdict_order(const void *octx, uint64 rows)
{
	SiftCDict  *d = (SiftCDict *) octx;

	if (d->sorted == NULL && (d->orows += rows) >= (uint64) DICT_ORDER_ROWS * d->n)
		cdict_sort(d);
	return d->sorted;
}

void
sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view)
{
	view->n = d->n;
	view->off = d->off;
	view->bytes = d->bytes;
	view->id = id;
	view->order = cdict_order;
	view->octx = d;
	view->get = NULL;
	view->gctx = NULL;
}
