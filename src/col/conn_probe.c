/*
 * col/conn_probe.c - the btree probe of the keys of a batch (col/conn_fill.c, docs/COL-CONNECT.md)
 *
 * The other way to ask whether a parent has one visible row for a key: look the key up in the unique btree index of the parent (the journal vouches only for a parent that has one), under the snapshot of the stamp. It reads the keys the batch asks for and nothing else, so its price follows the batch (about 1.8 us a distinct key) where the key set (col/conn_keys.c)
 * follows the parent (about 0.13 us a parent row), and it holds no memory beyond a small table of the keys it already asked.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/kern.h"
#include "col/conn.h"

/* The keys asked are remembered up to this many, then forgotten: the batches of one segment repeat the keys of a parent that is small next to them */
#define PROBE_SEEN_MAX	(1 << 18)

typedef struct ProbeSeen
{
	int64		key;
	bool		one;			/* the parent has a visible row with the key */
} ProbeSeen;

struct SiftConnProbe
{
	Oid			parent;
	Oid			index;
	Oid			keytype;
	SiftKeyMap *km;				/* the probed column is not an integer: a key is its code (sift_conn_probe_codec) */
	AttrNumber	pos;			/* the probed column in the btree */
	Snapshot	snap;
	MemoryContext cx;
	HTAB	   *seen;
};

static HTAB *
seen_new(MemoryContext cx)
{
	HASHCTL		ctl = {.keysize = sizeof(int64),.entrysize = sizeof(ProbeSeen),.hcxt = cx};

	return hash_create("sift connection probe", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

SiftConnProbe *
sift_conn_probe_begin(MemoryContext cx, Relation parent, AttrNumber key, Snapshot snap)
{
	return sift_conn_probe_begin_on(cx, parent, sift_col_row_key_unique(parent, key), key, snap);
}

SiftConnProbe *
sift_conn_probe_begin_on(MemoryContext cx, Relation parent, Oid btree, AttrNumber key, Snapshot snap)
{
	SiftConnProbe *p = MemoryContextAllocZero(cx, sizeof(SiftConnProbe));
	Relation	idx;

	/* the lock on the parent table is the one fill_begin took; the btree is locked here, and both are held to the end of the transaction (a batch opens them with NoLock) */
	Assert(CheckRelationLockedByMe(parent, AccessShareLock, true));
	p->parent = RelationGetRelid(parent);
	p->index = btree;
	idx = index_open(p->index, AccessShareLock);
	for (int j = 0; j < idx->rd_index->indnkeyatts; j++)
		if (idx->rd_index->indkey.values[j] == key)
			p->pos = j + 1;
	Assert(p->pos > 0);
	index_close(idx, NoLock);
	p->keytype = TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid;
	p->snap = snap;
	p->cx = cx;
	p->seen = seen_new(cx);
	return p;
}

void
sift_conn_probe_codec(SiftConnProbe *p, SiftKeyMap *km)
{
	p->km = km;
	p->keytype = sift_keymap_typid(km) == UUIDOID ? UUIDOID : TEXTOID;
}

static bool
key_datum(const SiftConnProbe *p, int64 k, Datum *d, RegProcedure *eq)
{
	Oid			typid = p->keytype;

	if (p->km != NULL)
	{
		*d = sift_keymap_datum(p->km, k);
		*eq = typid == UUIDOID ? F_UUID_EQ : F_TEXTEQ;
		return true;
	}
	switch (typid)
	{
		case INT2OID:
			*d = Int16GetDatum((int16) k);
			*eq = F_INT2EQ;
			return k >= PG_INT16_MIN && k <= PG_INT16_MAX;
		case INT4OID:
			*d = Int32GetDatum((int32) k);
			*eq = F_INT4EQ;
			return k >= PG_INT32_MIN && k <= PG_INT32_MAX;
		default:
			*d = Int64GetDatum(k);
			*eq = F_INT8EQ;
			return true;
	}
}

void
sift_conn_probe_open(SiftConnProbe *p, SiftConnProbeScan *s)
{
	s->heap = table_open(p->parent, NoLock);
	s->idx = index_open(p->index, NoLock);
	Assert(CheckRelationLockedByMe(s->heap, AccessShareLock, true) && CheckRelationLockedByMe(s->idx, AccessShareLock, true));
	s->scan = sift_index_beginscan(s->heap, s->idx, p->snap, 1);
	s->slot = table_slot_create(s->heap, NULL);
}

void
sift_conn_probe_key(SiftConnProbe *p, SiftConnProbeScan *s, int64 key, SiftConnProbeFn each, void *arg)
{
	ScanKeyData skey;
	Datum		d;
	RegProcedure eq;

	if (!key_datum(p, key, &d, &eq))
		return;
	ScanKeyEntryInitialize(&skey, 0, p->pos, BTEqualStrategyNumber, InvalidOid, p->km != NULL ? sift_keymap_collation(p->km) : InvalidOid, eq, d);
	index_rescan(s->scan, &skey, 1, NULL, 0);
	while (index_getnext_slot(s->scan, ForwardScanDirection, s->slot) && each(arg, s->slot))
		;
	CHECK_FOR_INTERRUPTS();
}

void
sift_conn_probe_keys(SiftConnProbe *p, SiftConnProbeScan *s, const int64 *keys, uint32 n, SiftConnProbeFn each, void *arg)
{
	Datum	   *vals = palloc(Max(n, 1) * sizeof(Datum));
	RegProcedure eq = F_INT8EQ;
	uint32		nv = 0;
	int16		len;
	bool		byval;
	char		align;
	ScanKeyData skey;

	for (uint32 i = 0; i < n; i++)
		nv += key_datum(p, keys[i], &vals[nv], &eq);
	if (nv > 0)
	{
		get_typlenbyvalalign(p->keytype, &len, &byval, &align);
		ScanKeyEntryInitialize(&skey, SK_SEARCHARRAY, p->pos, BTEqualStrategyNumber, p->keytype, p->km != NULL ? sift_keymap_collation(p->km) : InvalidOid, eq,
							   PointerGetDatum(construct_array(vals, nv, p->keytype, len, byval, align)));
		index_rescan(s->scan, &skey, 1, NULL, 0);
		while (index_getnext_slot(s->scan, ForwardScanDirection, s->slot) && each(arg, s->slot))
			CHECK_FOR_INTERRUPTS();
	}
	pfree(vals);
}

void
sift_conn_probe_close(SiftConnProbeScan *s)
{
	ExecDropSingleTupleTableSlot(s->slot);
	index_endscan(s->scan);
	index_close(s->idx, NoLock);
	table_close(s->heap, NoLock);
}

static bool
seen_one(void *arg, TupleTableSlot *slot)
{
	*(bool *) arg = true;
	return false;
}

/* For each row of the batch that has a key (nulls: its bitmap), whether the parent has a visible row for it: its unique index holds at most one */
void
sift_conn_probe_batch(SiftConnProbe *p, const int64 *keys, const uint64 *nulls, uint32 nrows, bool *one)
{
	SiftConnProbeScan s;

	sift_conn_probe_open(p, &s);
	for (uint32 row = 0; row < nrows; row++)
	{
		ProbeSeen  *e;
		bool		hit;

		one[row] = false;
		if (sift_col_bit_test(nulls, row))
			continue;
		e = hash_search(p->seen, &keys[row], HASH_ENTER, &hit);
		if (!hit)
		{
			e->one = false;
			sift_conn_probe_key(p, &s, keys[row], seen_one, &e->one);
		}
		one[row] = e->one;
		if (hash_get_num_entries(p->seen) > PROBE_SEEN_MAX)
		{
			hash_destroy(p->seen);
			p->seen = seen_new(p->cx);
		}
	}
	sift_conn_probe_close(&s);
}
