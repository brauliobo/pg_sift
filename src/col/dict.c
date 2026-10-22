/*
 * col/dict.c - L1 of the column engine: the dictionaries of strings (code = first-seen order).
 */
#include "core/host.h"

#include "col/enc.h"
#include "col/enc_int.h"
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
	d->nslots = pg_nextpower2_32(Max(DICT_MINCAP, d->n + 1) * 4);
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

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0)
		return -1;
	cdict_ready(d);
	return (int64) *cdict_probe(d, s, len) - 1;
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
