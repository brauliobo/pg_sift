/*
 * col/dict.c - L1 of the column engine: the dictionaries of strings (code = first-seen order).
 */
#include "core/host.h"

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

/* d->sorted: a radix sort on the first 8 bytes of every string, then the entries that tie on them by their whole strings */
static void
cdict_sort(SiftCDict *d)
{
	SortEnt    *a = palloc(d->n * sizeof(SortEnt)),
			   *b = palloc(d->n * sizeof(SortEnt));

	for (uint32 c = 0; c < d->n; c++)
	{
		const uint8 *s = (const uint8 *) d->bytes + d->off[c];
		uint32		len = Min(cdict_len(d, c), sizeof(uint64));
		uint64		k = 0;

		for (uint32 i = 0; i < sizeof(uint64); i++)
			k = k << 8 | (i < len ? s[i] : 0);
		a[c].key = k;
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

/* The form searched in place: the strings as they are (no offsets when they all have one length) and the codes in byte order of the strings */
static void
serialize_lookup(SiftCDict *d, StringInfo out)
{
	uint32		form = DICT_LOOKUP,
				fixed = d->n > 0 ? cdict_len(d, 0) : 0,
				nbytes = (uint32) d->nbytes;
	int			start = out->len;

	for (uint32 c = 1; c < d->n && fixed != 0; c++)
		if (cdict_len(d, c) != fixed)
			fixed = 0;
	if (d->sorted == NULL)
		cdict_sort(d);
	put_val(out, form);
	put_val(out, d->n);
	put_val(out, fixed);
	put_val(out, nbytes);
	if (fixed == 0)
		put_bytes(out, d->off, ((Size) d->n + 1) * sizeof(uint32));
	put_bytes(out, d->bytes, d->nbytes);
	while ((out->len - start) % sizeof(uint32))
		appendStringInfoChar(out, '\0');
	put_bytes(out, d->sorted, (Size) d->n * sizeof(uint32));
}

void
sift_cdict_serialize(SiftCDict *d, const SiftCcPolicy *pol, bool lookup, StringInfo out)
{
	Size		plain = sizeof(uint32) * (d->n + 1) + d->nbytes;
	uint32		form = DICT_PLAIN;
	StringInfoData z;

	if (lookup && d->n > 0)
	{
		serialize_lookup(d, out);
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

/* The dictionary of the searched form in memory, with its order (the codes are checked: the order is trusted to find and match, sift.col_verify checks that it is one) */
static SiftCDict *
parse_lookup(MemoryContext mcxt, const uint8 *p, Size len, Size *used)
{
	uint32		head[4];
	uint64		offs,
				end,
				total;
	uint32	   *off;
	SiftCDict  *d;

	if (len < DICT_HEAD_LOOKUP)
		return NULL;
	memcpy(head, p, DICT_HEAD_LOOKUP);
	offs = head[2] == 0 ? ((uint64) head[1] + 1) * sizeof(uint32) : 0;
	end = DICT_HEAD_LOOKUP + offs + head[3];
	total = (end + sizeof(uint32) - 1) / sizeof(uint32) * sizeof(uint32) + (uint64) head[1] * sizeof(uint32);
	if (head[1] == 0 || total > len || head[2] > SIFT_COL_DICT_MAX_ENTRY || (head[2] != 0 && (uint64) head[1] * head[2] != head[3]))
		return NULL;
	off = palloc(((Size) head[1] + 1) * sizeof(uint32));
	if (head[2] == 0)
		memcpy(off, p + DICT_HEAD_LOOKUP, offs);
	else
		for (uint32 c = 0; c <= head[1]; c++)
			off[c] = c * head[2];
	if (off[head[1]] != head[3])
	{
		pfree(off);
		return NULL;
	}
	d = cdict_from(mcxt, head[1], off, (const char *) p + DICT_HEAD_LOOKUP + offs);
	pfree(off);
	if (d == NULL)
		return NULL;
	d->stored_order = true;
	d->sorted = MemoryContextAlloc(mcxt, (Size) head[1] * sizeof(uint32));
	memcpy(d->sorted, p + (total - (uint64) head[1] * sizeof(uint32)), (Size) head[1] * sizeof(uint32));
	for (uint32 i = 0; i < head[1]; i++)
		if (d->sorted[i] >= head[1])
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
	if (head[0] == DICT_LOOKUP)
		return parse_lookup(mcxt, p, len, used);
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
