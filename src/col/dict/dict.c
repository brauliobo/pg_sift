/*
 * col/dict/dict.c - the dictionaries of strings: create, intern, find (code = first-seen order)
 * the hash table of the strings and its growth
 * the other files order, serialize and parse a dictionary
 */
#include "core/host.h"
#include "core/limits.h"
#include "col/dict/dict_int.h"

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
		memset(d->slots, 0, d->nslots * sizeof(uint64));
}

/* a slot holds the hash of the string above its code + 1 (0: free): a probe reads the strings only of the slots with its hash, and a growth reinserts without hashing */
static uint64 *
cdict_probe(const SiftCDict *d, const char *s, uint32 len, uint32 hash)
{
	uint32		mask = d->nslots - 1;

	for (uint32 i = hash & mask;; i = (i + 1) & mask)
	{
		uint64		u = d->slots[i];
		uint32		c = (uint32) u;

		if (c == 0 || ((uint32) (u >> 32) == hash && cdict_len(d, c - 1) == len && memcmp(d->bytes + d->off[c - 1], s, len) == 0))
			return &d->slots[i];
	}
}

static void
cdict_put(SiftCDict *d, uint64 slot)
{
	uint32		mask = d->nslots - 1,
				i = (uint32) (slot >> 32) & mask;

	while (d->slots[i])
		i = (i + 1) & mask;
	d->slots[i] = slot;
}

/* The hash table exists and has room for one more code at load <= 1/2 (rebuilt by doubling) */
static void
cdict_ready(SiftCDict *d)
{
	uint64	   *old = d->slots;
	uint32		nold = d->nslots;

	if (old != NULL && (d->n + 1) * 2 <= nold)
		return;
	d->nslots = pg_nextpower2_32(Max(DICT_MINCAP, d->n + 1) * 2);
	d->slots = MemoryContextAllocZero(d->mcxt, d->nslots * sizeof(uint64));
	if (old != NULL)
	{
		for (uint32 i = 0; i < nold; i++)
			if (old[i])
				cdict_put(d, old[i]);
		pfree(old);
	}
	else
		for (uint32 c = 0; c < d->n; c++)
			cdict_put(d, (uint64) hash_bytes((const unsigned char *) d->bytes + d->off[c], (int) cdict_len(d, c)) << 32 | (c + 1));
	if (d->built)
		d->built(d, d->built_arg);
}

uint32
sift_cdict_intern(SiftCDict *d, const char *s, uint32 len)
{
	uint64	   *slot;
	uint32		hash;
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
	hash = hash_bytes((const unsigned char *) s, (int) len);
	slot = cdict_probe(d, s, len, hash);
	if ((uint32) *slot)
		return (uint32) *slot - 1;
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
	/* the byte order made before (by a top-N scan of a batch the write buffer keeps) does not have this string */
	if (d->sorted != NULL)
	{
		pfree(d->sorted);
		d->sorted = NULL;
	}
	memcpy(d->bytes + d->nbytes, s, len);
	d->nbytes += len;
	d->off[d->n + 1] = (uint32) d->nbytes;
	*slot = (uint64) hash << 32 | (d->n + 1);
	return d->n++;
}

/*
 * A dictionary read for a scan is searched entry by entry until it has been asked DICT_HASH_AFTER times: the table (about 16 bytes a code) and the
 * sorted order (4) cost as much as a few dozen such searches, and the first query of a backend, or of a dictionary too large to be kept
 * (col/dictcache.c), would pay them for one use. A dictionary of the write path has its table from the first intern.
 */
#define DICT_HASH_AFTER		8
/* ... or once when the table is shared (d->built: col/dictshm.c writes it to the ring when it is made), so that the cost is that of the server and not of each backend */
#define DICT_HASH_AFTER_SHARED	1

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0)
		return -1;
	if (d->look)
		return d->look->find(d->look, s, len);
	if (d->slots == NULL && d->finds++ < (d->built != NULL ? DICT_HASH_AFTER_SHARED : DICT_HASH_AFTER))
	{
		for (uint32 c = 0; c < d->n; c++)
			if (cdict_len(d, c) == len && memcmp(d->bytes + d->off[c], s, len) == 0)
				return c;
		return -1;
	}
	cdict_ready(d);
	return (int64) (uint32) *cdict_probe(d, s, len, hash_bytes((const unsigned char *) s, (int) len)) - 1;
}
