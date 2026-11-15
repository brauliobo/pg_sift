/*
 * col/conn_keys.c - the set of the keys of a parent table that the fill of the found flags asks (col/conn_fill.c, docs/COL-CONNECT.md)
 *
 * Exact membership of an integer key under a snapshot, in the least memory the keys allow. Two passes over the parent, both under the same snapshot: the first counts the
 * keys and finds the smallest and the largest; when the range is under 64 slots a key (one bit a slot is then less than the 8 bytes of a sorted key) the second sets the bits of a bitmap,
 * otherwise it collects the keys, sorts them and looks them up by bisection. A key that two visible rows hold is in neither: the fill reads it like a key the parent lacks.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/conn.h"

/* A bitmap needs a bit a slot of the range; a sorted array 8 bytes a key: dense below this many slots a key */
#define KEYS_DENSE_SLOTS	64

struct SiftConnKeySet
{
	int64		lo;				/* the smallest key: the bitmap starts here */
	uint64		slots;			/* bits of the bitmap, 0 for a sorted set */
	uint64	   *bits;			/* a key is held (once or more) */
	uint64	   *twice;			/* ... by two visible rows (allocated with the first such key) */
	int64	   *keys;			/* sorted, a repeated key once */
	uint64		nkeys;
	bool	   *again;			/* sorted set: the key is held by two visible rows */
};

static bool
key_of(Oid typid, Datum d, int64 *out)
{
	switch (typid)
	{
		case INT2OID:
			*out = DatumGetInt16(d);
			return true;
		case INT4OID:
			*out = DatumGetInt32(d);
			return true;
		case INT8OID:
			*out = DatumGetInt64(d);
			return true;
		default:
			return false;
	}
}

/* Calls f for every row of the parent that holds a key, under snap */
static void
scan_keys(Relation parent, AttrNumber key, Snapshot snap, void (*f) (void *arg, int64 k), void *arg)
{
	TableScanDesc scan = sift_table_beginscan(parent, snap, 0, NULL);
	TupleTableSlot *slot = table_slot_create(parent, NULL);
	Oid			keytype = TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid;

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, key, &isnull);
		int64		k;

		if (!isnull && key_of(keytype, d, &k))
			f(arg, k);
		CHECK_FOR_INTERRUPTS();
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
}

typedef struct KeyStats
{
	uint64		rows;
	int64		lo,
				hi;
} KeyStats;

static void
count_key(void *arg, int64 k)
{
	KeyStats   *st = arg;

	st->lo = st->rows == 0 ? k : Min(st->lo, k);
	st->hi = st->rows == 0 ? k : Max(st->hi, k);
	st->rows++;
}

typedef struct KeyFill
{
	SiftConnKeySet *set;
	uint64		n;
} KeyFill;

static void
set_bit(void *arg, int64 k)
{
	SiftConnKeySet *set = arg;
	uint64		slot = (uint64) k - (uint64) set->lo;

	if (set->bits[slot / 64] & ((uint64) 1 << (slot % 64)))
	{
		if (set->twice == NULL)
			set->twice = MemoryContextAllocExtended(GetMemoryChunkContext(set->bits), (set->slots + 63) / 64 * sizeof(uint64), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		set->twice[slot / 64] |= (uint64) 1 << (slot % 64);
	}
	set->bits[slot / 64] |= (uint64) 1 << (slot % 64);
}

static void
add_key(void *arg, int64 k)
{
	KeyFill    *kf = arg;

	kf->set->keys[kf->n++] = k;
}

static int
cmp_key(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* The keys of the parent under snap, or NULL when they need more than `limit` bytes (a bit a slot of the range, or 9 bytes a key) */
SiftConnKeySet *
sift_conn_keyset_load(MemoryContext cx, Relation parent, AttrNumber key, Snapshot snap, Size limit)
{
	KeyStats	st = {0};
	uint64		bytes;
	SiftConnKeySet *set;
	uint64		range;
	MemoryContext old = MemoryContextSwitchTo(cx);

	scan_keys(parent, key, snap, count_key, &st);
	range = st.rows == 0 ? 0 : (uint64) st.hi - (uint64) st.lo;
	set = palloc0(sizeof(SiftConnKeySet));
	set->lo = st.lo;
	if (st.rows > 0 && range < (uint64) KEYS_DENSE_SLOTS * st.rows)
	{
		set->slots = range + 1;
		bytes = (set->slots + 63) / 64 * sizeof(uint64);
	}
	else
		bytes = st.rows * (sizeof(int64) + sizeof(bool));
	if (bytes > limit)
	{
		MemoryContextSwitchTo(old);
		pfree(set);
		return NULL;
	}
	if (set->slots > 0)
	{
		set->bits = palloc_extended((set->slots + 63) / 64 * sizeof(uint64), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		scan_keys(parent, key, snap, set_bit, set);
	}
	else
	{
		KeyFill		kf = {.set = set};
		uint64		n = 0;

		/* check-memory: allow the bytes were checked against the limit of the caller (maintenance_work_mem / 2) above */
		set->keys = palloc_extended(Max(st.rows, 1) * sizeof(int64), MCXT_ALLOC_HUGE);
		scan_keys(parent, key, snap, add_key, &kf);
		set->nkeys = kf.n;
		qsort(set->keys, set->nkeys, sizeof(int64), cmp_key);
		set->again = palloc_extended(Max(set->nkeys, 1), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		for (uint64 i = 0; i < set->nkeys; i++)
			if (n > 0 && set->keys[n - 1] == set->keys[i])
				set->again[n - 1] = true;
			else
				set->keys[n++] = set->keys[i];
		set->nkeys = n;
	}
	MemoryContextSwitchTo(old);
	return set;
}

bool
sift_conn_keyset_has_one(const SiftConnKeySet *set, int64 k)
{
	if (set->slots > 0)
	{
		uint64		slot = (uint64) k - (uint64) set->lo;

		if (k < set->lo || slot >= set->slots || !(set->bits[slot / 64] & ((uint64) 1 << (slot % 64))))
			return false;
		return set->twice == NULL || !(set->twice[slot / 64] & ((uint64) 1 << (slot % 64)));
	}
	/* interpolation on every other step (the keys of a table are mostly evenly spread: two or three reads), bisection on the others (log2 steps whatever the spread) */
	{
		int64		lo = 0,
					hi = (int64) set->nkeys - 1;

		for (int step = 0; lo <= hi && k >= set->keys[lo] && k <= set->keys[hi]; step++)
		{
			int64		mid = lo + (hi - lo) / 2;

			if (step % 2 == 0 && set->keys[hi] > set->keys[lo])
				mid = lo + (int64) ((double) ((uint64) k - (uint64) set->keys[lo]) / (double) ((uint64) set->keys[hi] - (uint64) set->keys[lo]) * (double) (hi - lo));
			if (set->keys[mid] == k)
				return !set->again[mid];
			if (set->keys[mid] < k)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
		return false;
	}
}
