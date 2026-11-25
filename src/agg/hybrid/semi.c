/*
 * agg/hybrid/semi.c - the key set of a semi join (agg/hybrid/semi.h).
 *
 * check-memory: allow-allocs the set is bounded by its key cap and its byte cap
 */
#include "core/host.h"

#include <stdlib.h>

#include "agg/hybrid/semi.h"
#include "core/memtrack.h"

#define ACL_INIT_KEYS	64

SiftHybridAclSet *
sift_acl_begin(MemoryContext parent, bool is_str, uint32 max_keys, Size mem_bytes)
{
	MemoryContext cx = sift_mem_allocset_create(parent, "sift hybrid acl set");
	SiftHybridAclSet *set = MemoryContextAllocZero(cx, sizeof(SiftHybridAclSet));

	set->mcxt = cx;
	set->is_str = is_str;
	set->cap_keys = Min(max_keys, SIFT_HYB_ACL_MAX_KEYS);
	set->cap_bytes = mem_bytes;
	return set;
}

void
sift_acl_end(SiftHybridAclSet *set)
{
	MemoryContextDelete(set->mcxt);
}

static uint32
bloom_bit(uint64 h)
{
	return (uint32) ((h * UINT64CONST(0x9E3779B97F4A7C15)) >> 52) & (SIFT_HYB_ACL_BLOOM_BITS - 1);
}

static uint64
str_hash(const uint8 *p, uint32 len)
{
	uint64		h = UINT64CONST(0xcbf29ce484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ p[i]) * UINT64CONST(0x100000001b3);
	return h;
}

static bool
room_for(SiftHybridAclSet *set, Size add)
{
	if (set->overflowed || set->nvals >= set->cap_keys || set->bytes + add > set->cap_bytes)
		set->overflowed = true;
	return !set->overflowed;
}

/* The arrays of keys double when full */
static void
ensure_room(SiftHybridAclSet *set)
{
	uint32		cap;

	if (set->nvals < set->arr_cap)
		return;
	cap = set->arr_cap == 0 ? ACL_INIT_KEYS : 2 * set->arr_cap;
	if (set->is_str)
	{
		set->sptr = set->sptr ? repalloc((void *) set->sptr, cap * sizeof(uint8 *)) : MemoryContextAlloc(set->mcxt, cap * sizeof(uint8 *));
		set->slen = set->slen ? repalloc(set->slen, cap * sizeof(uint32)) : MemoryContextAlloc(set->mcxt, cap * sizeof(uint32));
	}
	else
		set->ival = set->ival ? repalloc(set->ival, cap * sizeof(int64)) : MemoryContextAlloc(set->mcxt, cap * sizeof(int64));
	set->arr_cap = cap;
}

bool
sift_acl_add_int(SiftHybridAclSet *set, int64 v)
{
	SIFT_INVARIANT(!set->is_str && !set->finished, "sift: an integer key goes to an open integer set");
	if (!room_for(set, sizeof(int64)))
		return false;
	ensure_room(set);
	set->ival[set->nvals++] = v;
	set->bytes += sizeof(int64);
	return true;
}

bool
sift_acl_add_str(SiftHybridAclSet *set, const uint8 *p, uint32 len)
{
	uint8	   *copy;

	SIFT_INVARIANT(set->is_str && !set->finished, "sift: a string key goes to an open string set");
	if (!room_for(set, sizeof(uint8 *) + sizeof(uint32) + len))
		return false;
	ensure_room(set);
	copy = MemoryContextAlloc(set->mcxt, Max(len, 1));
	memcpy(copy, p, len);
	set->sptr[set->nvals] = copy;
	set->slen[set->nvals++] = len;
	set->bytes += sizeof(uint8 *) + sizeof(uint32) + len;
	return true;
}

static int
int_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

typedef struct StrKey
{
	const uint8 *p;
	uint32		len;
} StrKey;

static int
str_cmp(const void *a, const void *b)
{
	const StrKey *x = a,
			   *y = b;
	int			c = memcmp(x->p, y->p, Min(x->len, y->len));

	return c != 0 ? c : (x->len < y->len ? -1 : x->len > y->len);
}

static inline uint32
acl_slot(const SiftHybridAclSet *set, uint64 h)
{
	return (uint32) ((h * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & set->mask;
}

static inline uint64
acl_key_hash(const SiftHybridAclSet *set, uint32 i)
{
	return set->is_str ? str_hash(set->sptr[i], set->slen[i]) : (uint64) set->ival[i];
}

/* the keys, sorted and distinct, in a table of at least twice their number of slots */
static void
acl_build_table(SiftHybridAclSet *set)
{
	uint32		slots = 1;

	while (slots < 2 * set->nvals)
		slots <<= 1;
	set->mask = slots - 1;
	set->tab = MemoryContextAllocZero(set->mcxt, (Size) slots * sizeof(uint32));
	for (uint32 i = 0; i < set->nvals; i++)
	{
		uint32		h = acl_slot(set, acl_key_hash(set, i));

		while (set->tab[h])
			h = (h + 1) & set->mask;
		set->tab[h] = i + 1;
	}
}

void
sift_acl_finish(SiftHybridAclSet *set)
{
	uint32		m = 0;

	SIFT_INVARIANT(!set->finished, "sift: a set is finished once");
	set->finished = true;
	if (set->overflowed)
		return;
	if (set->is_str && set->nvals > 0)
	{
		StrKey	   *k = palloc(set->nvals * sizeof(StrKey));

		for (uint32 i = 0; i < set->nvals; i++)
			k[i] = (StrKey) {set->sptr[i], set->slen[i]};
		qsort(k, set->nvals, sizeof(StrKey), str_cmp);
		for (uint32 i = 0; i < set->nvals; i++)
			if (m == 0 || str_cmp(&k[m - 1], &k[i]) != 0)
				k[m++] = k[i];
		for (uint32 i = 0; i < m; i++)
		{
			set->sptr[i] = k[i].p;
			set->slen[i] = k[i].len;
			set->bloom[bloom_bit(str_hash(k[i].p, k[i].len)) >> 6] |= UINT64CONST(1) << (bloom_bit(str_hash(k[i].p, k[i].len)) & 63);
		}
		pfree(k);
	}
	else if (set->nvals > 0)
	{
		qsort(set->ival, set->nvals, sizeof(int64), int_cmp);
		for (uint32 i = 0; i < set->nvals; i++)
			if (m == 0 || set->ival[m - 1] != set->ival[i])
				set->ival[m++] = set->ival[i];
		for (uint32 i = 0; i < m; i++)
			set->bloom[bloom_bit((uint64) set->ival[i]) >> 6] |= UINT64CONST(1) << (bloom_bit((uint64) set->ival[i]) & 63);
	}
	set->nvals = m;
	set->empty = m == 0;
	if (m >= SIFT_HYB_ACL_HASH_MIN)
		acl_build_table(set);
}

bool
sift_acl_has_int(const SiftHybridAclSet *set, int64 v)
{
	uint32		lo = 0,
				hi = set->nvals;

	if (!((set->bloom[bloom_bit((uint64) v) >> 6] >> (bloom_bit((uint64) v) & 63)) & 1))
		return false;
	if (set->tab != NULL)
	{
		for (uint32 h = acl_slot(set, (uint64) v); set->tab[h]; h = (h + 1) & set->mask)
			if (set->ival[set->tab[h] - 1] == v)
				return true;
		return false;
	}
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (set->ival[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < set->nvals && set->ival[lo] == v;
}

bool
sift_acl_has_str(const SiftHybridAclSet *set, const uint8 *p, uint32 len)
{
	uint32		lo = 0,
				hi = set->nvals,
				b = bloom_bit(str_hash(p, len));
	StrKey		probe = {p, len};

	if (!((set->bloom[b >> 6] >> (b & 63)) & 1))
		return false;
	if (set->tab != NULL)
	{
		for (uint32 h = acl_slot(set, str_hash(p, len)); set->tab[h]; h = (h + 1) & set->mask)
			if (set->slen[set->tab[h] - 1] == len && memcmp(set->sptr[set->tab[h] - 1], p, len) == 0)
				return true;
		return false;
	}
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		StrKey		cur = {set->sptr[mid], set->slen[mid]};

		if (str_cmp(&cur, &probe) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < set->nvals && set->slen[lo] == len && memcmp(set->sptr[lo], p, len) == 0;
}
