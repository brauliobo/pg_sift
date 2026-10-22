/*
 * col/dict.c - L1 of the column engine: the dictionaries of strings (code = first-seen order).
 */
#include "core/host.h"

#include "port/pg_bswap.h"

#include "col/enc.h"
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

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0)
		return -1;
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
static int
str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
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
			return str_cmp(s, len, c->v.s, c->len) < 0;
		case SIFT_COLOP_LE:
			return str_cmp(s, len, c->v.s, c->len) <= 0;
		case SIFT_COLOP_GT:
			return str_cmp(s, len, c->v.s, c->len) > 0;
		case SIFT_COLOP_GE:
			return str_cmp(s, len, c->v.s, c->len) >= 0;
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

	return str_cmp(d->bytes + d->off[x], cdict_len(d, x), d->bytes + d->off[y], cdict_len(d, y));
}

/* d->sorted: a radix sort on the first 8 bytes of every string, then the entries that tie on them by their whole strings */
static void
cdict_sort(SiftCDict *d)
{
	SortEnt    *a = palloc(d->n * sizeof(SortEnt)),
			   *b = palloc(d->n * sizeof(SortEnt));

	for (uint32 c = 0; c < d->n; c++)
	{
		uint64		k = 0;

		memcpy(&k, d->bytes + d->off[c], Min(cdict_len(d, c), sizeof(k)));
		a[c].key = pg_bswap64(k);
		a[c].code = c;
	}
	for (int pass = 0; pass < 8; pass++)
	{
		uint32		count[256] = {0},
					at = 0;
		int			shift = pass * 8;

		for (uint32 i = 0; i < d->n; i++)
			count[(a[i].key >> shift) & 255]++;
		if (count[(a[0].key >> shift) & 255] == d->n)
			continue;
		for (int v = 0; v < 256; v++)
		{
			uint32		c = count[v];

			count[v] = at;
			at += c;
		}
		for (uint32 i = 0; i < d->n; i++)
			b[count[(a[i].key >> shift) & 255]++] = a[i];
		{
			SortEnt    *t = a;

			a = b;
			b = t;
		}
	}
	for (uint32 i = 0, j; i < d->n; i = j)
	{
		for (j = i + 1; j < d->n && a[j].key == a[i].key; j++)
			;
		if (j - i > 1)
			qsort_arg(a + i, j - i, sizeof(SortEnt), sort_ent_cmp, d);
	}
	d->sorted = MemoryContextAlloc(d->mcxt, (Size) d->n * sizeof(uint32));
	for (uint32 i = 0; i < d->n; i++)
		d->sorted[i] = a[i].code;
	pfree(a);
	pfree(b);
}

/* The first position of the sorted order whose string is not below c (after: not at or below it) */
static uint32
sorted_bound(const SiftCDict *d, const SiftColValue *c, bool after)
{
	uint32		lo = 0,
				hi = d->n;

	SIFT_BISECT(uint32, m, lo, hi,
				after ? str_cmp(SIFT_COL_DICT_STR(d, d->sorted[m]), cdict_len(d, d->sorted[m]), c->v.s, c->len) <= 0
				: str_cmp(SIFT_COL_DICT_STR(d, d->sorted[m]), cdict_len(d, d->sorted[m]), c->v.s, c->len) < 0);
	return lo;
}

uint32
sift_cdict_match(SiftCDict *d, SiftColOp op, const SiftColValue *c, uint64 *lut)
{
	uint32		count = 0,
				from,
				to;

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
	switch (op)
	{
		case SIFT_COLOP_LT:
			from = 0;
			to = sorted_bound(d, c, false);
			break;
		case SIFT_COLOP_LE:
			from = 0;
			to = sorted_bound(d, c, true);
			break;
		case SIFT_COLOP_GT:
			from = sorted_bound(d, c, true);
			to = d->n;
			break;
		case SIFT_COLOP_GE:
			from = sorted_bound(d, c, false);
			to = d->n;
			break;
		default:					/* PREFIX: the strings that start with c are the run that begins where c would be inserted */
			{
				uint32		lo = sorted_bound(d, c, false);

				from = lo;
				to = d->n;
				SIFT_BISECT(uint32, m, lo, to,
							sift_cstr_matches(SIFT_COLOP_PREFIX, SIFT_COL_DICT_STR(d, d->sorted[m]), cdict_len(d, d->sorted[m]), c));
			}
			break;
	}
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
#define DICT_PLAIN		0
#define DICT_STRS		1
#define DICT_HEAD		(2 * sizeof(uint32))

void
sift_cdict_serialize(const SiftCDict *d, const SiftCcPolicy *pol, StringInfo out)
{
	Size		plain = sizeof(uint32) * (d->n + 1) + d->nbytes;
	uint32		form = DICT_PLAIN;
	StringInfoData z;

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

SiftCDict *
sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used)
{
	const uint8 *p = data;
	uint32		head[2],
				nbytes;

	if (len < DICT_HEAD)
		return NULL;
	memcpy(head, p, DICT_HEAD);
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

void
sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view)
{
	view->n = d->n;
	view->off = d->off;
	view->bytes = d->bytes;
	view->id = id;
}
