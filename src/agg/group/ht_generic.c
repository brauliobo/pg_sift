/*
 * agg/group/ht_generic.c - the interner of the group keys of a type with no bytewise equality (agg/group/ht_generic.h).
 */
/* check-memory: allow-allocs every array here follows the distinct values of one key column of one statement, which the memory of the group table accounts for (sift_genkey_bytes) */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/group/ht_generic.h"
#include "core/memtrack.h"

#define GEN_FIRST_TABLE	64

struct SiftGenKey
{
	Oid			typid;
	Oid			collid;
	int16		typlen;
	bool		typbyval;
	bool		by_image;		/* the type has no hash function and equal values are equal bytes: the hash is the one of the bytes */
	FmgrInfo	hash_fn;
	FmgrInfo	eq_fn;
	FmgrInfo	cmp_fn;
	MemoryContext cx;
	MemoryContext vcx;			/* the copies of the values: reset with the interner */
	uint32		count;
	uint32		cap;			/* entries vals and hashes have room for */
	Datum	   *vals;			/* the value of code c is vals[c - 1] */
	uint32	   *hashes;
	uint32		slots;			/* a power of 2 */
	uint32	   *tab;			/* the code at each slot, 0 for an empty one */
	uint64		value_bytes;
};

static TypeCacheEntry *
type_entry(Oid typid)
{
	return lookup_type_cache(typid, TYPECACHE_HASH_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO);
}

/* money, bit and bit varying have a btree operator class and no hash one; their equal values are equal bytes (a bit string keeps the unused bits of its last byte zero) */
static bool
image_equality(Oid typid)
{
	return typid == CASHOID || typid == BITOID || typid == VARBITOID;
}

bool
sift_genkey_supported(Oid typid)
{
	const TypeCacheEntry *te = type_entry(typid);

	return OidIsValid(te->eq_opr) && (OidIsValid(te->hash_proc) || image_equality(typid));
}

SiftGenKey *
sift_genkey_new(MemoryContext cx, Oid typid, Oid collid)
{
	TypeCacheEntry *te = type_entry(typid);
	SiftGenKey *k = MemoryContextAllocZero(cx, sizeof(SiftGenKey));

	Assert(OidIsValid(te->eq_opr));
	k->by_image = !OidIsValid(te->hash_proc);
	k->typid = typid;
	k->collid = collid;
	k->typlen = te->typlen;
	k->typbyval = te->typbyval;
	k->cx = cx;
	k->vcx = sift_mem_allocset_create(cx, "sift generic key values");
	fmgr_info_copy(&k->hash_fn, &te->hash_proc_finfo, cx);
	fmgr_info_copy(&k->eq_fn, &te->eq_opr_finfo, cx);
	if (OidIsValid(te->cmp_proc))
		fmgr_info_copy(&k->cmp_fn, &te->cmp_proc_finfo, cx);
	k->slots = GEN_FIRST_TABLE;
	k->tab = MemoryContextAllocZero(cx, k->slots * sizeof(uint32));
	return k;
}

static uint32
value_hash(SiftGenKey *k, Datum value)
{
	if (k->by_image && k->typbyval)
		return DatumGetUInt32(hash_bytes((const unsigned char *) &value, k->typlen));
	if (k->by_image)
		return DatumGetUInt32(hash_bytes((const unsigned char *) VARDATA_ANY(DatumGetPointer(value)), VARSIZE_ANY_EXHDR(DatumGetPointer(value))));
	return DatumGetUInt32(FunctionCall1Coll(&k->hash_fn, k->collid, value));
}

static bool
value_equal(SiftGenKey *k, Datum a, Datum b)
{
	return DatumGetBool(FunctionCall2Coll(&k->eq_fn, k->collid, a, b));
}

static inline uint32
slot_of(uint32 hash, uint32 slots)
{
	hash ^= hash >> 16;
	hash *= 0x85ebca6bU;
	hash ^= hash >> 13;
	return hash & (slots - 1);
}

static void
grow_table(SiftGenKey *k)
{
	const uint32 slots = k->slots * 2;
	uint32	   *tab = MemoryContextAllocZero(k->cx, slots * sizeof(uint32));

	for (uint32 c = 1; c <= k->count; c++)
	{
		uint32		at = slot_of(k->hashes[c - 1], slots);

		while (tab[at] != 0)
			at = (at + 1) & (slots - 1);
		tab[at] = c;
	}
	pfree(k->tab);
	k->tab = tab;
	k->slots = slots;
}

static void
grow_values(SiftGenKey *k)
{
	const uint32 cap = Max(k->cap * 2, GEN_FIRST_TABLE);

	if (k->vals == NULL)
	{
		k->vals = MemoryContextAlloc(k->cx, cap * sizeof(Datum));
		k->hashes = MemoryContextAlloc(k->cx, cap * sizeof(uint32));
	}
	else
	{
		k->vals = repalloc(k->vals, cap * sizeof(Datum));
		k->hashes = repalloc(k->hashes, cap * sizeof(uint32));
	}
	k->cap = cap;
}

uint32
sift_genkey_intern(SiftGenKey *k, Datum value)
{
	const uint32 hash = value_hash(k, value);
	uint32		at = slot_of(hash, k->slots);
	MemoryContext old;

	for (uint32 c = k->tab[at]; c != 0; c = k->tab[at])
	{
		if (k->hashes[c - 1] == hash && value_equal(k, k->vals[c - 1], value))
			return c;
		at = (at + 1) & (k->slots - 1);
	}
	if (k->count == k->cap)
		grow_values(k);
	old = MemoryContextSwitchTo(k->vcx);
	k->vals[k->count] = datumCopy(value, k->typbyval, k->typlen);
	MemoryContextSwitchTo(old);
	k->hashes[k->count] = hash;
	k->value_bytes += k->typbyval ? 0 : datumGetSize(k->vals[k->count], k->typbyval, k->typlen);
	k->tab[at] = ++k->count;
	if ((uint64) k->count * 2 > k->slots)
		grow_table(k);
	return k->count;
}

Datum
sift_genkey_datum(const SiftGenKey *k, uint32 code)
{
	Assert(code >= 1 && code <= k->count);
	return k->vals[code - 1];
}

uint32
sift_genkey_code_hash(const SiftGenKey *k, uint32 code)
{
	Assert(code >= 1 && code <= k->count);
	return k->hashes[code - 1];
}

void
sift_genkey_reset(SiftGenKey *k)
{
	MemoryContextReset(k->vcx);
	if (k->vals != NULL)
	{
		pfree(k->vals);
		pfree(k->hashes);
	}
	pfree(k->tab);
	k->vals = NULL;
	k->hashes = NULL;
	k->count = k->cap = 0;
	k->value_bytes = 0;
	k->slots = GEN_FIRST_TABLE;
	k->tab = MemoryContextAllocZero(k->cx, k->slots * sizeof(uint32));
}

int
sift_genkey_cmp(SiftGenKey *k, Datum a, Datum b)
{
	const int32 r = DatumGetInt32(FunctionCall2Coll(&k->cmp_fn, k->collid, a, b));

	return r < 0 ? -1 : r > 0;
}

uint32
sift_genkey_count(const SiftGenKey *k)
{
	return k->count;
}

uint64
sift_genkey_bytes(const SiftGenKey *k)
{
	return k->value_bytes + (uint64) k->cap * (sizeof(Datum) + sizeof(uint32)) + (uint64) k->slots * sizeof(uint32);
}

Oid
sift_genkey_typid(const SiftGenKey *k)
{
	return k->typid;
}

void
sift_genkey_free(SiftGenKey *k)
{
	MemoryContextDelete(k->vcx);
	if (k->vals != NULL)
	{
		pfree(k->vals);
		pfree(k->hashes);
	}
	pfree(k->tab);
	pfree(k);
}
