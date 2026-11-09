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
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
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
	SiftConnProbe *p = MemoryContextAllocZero(cx, sizeof(SiftConnProbe));

	p->parent = RelationGetRelid(parent);
	p->index = sift_col_row_key_unique(parent, key);
	p->keytype = TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid;
	p->snap = snap;
	p->cx = cx;
	p->seen = seen_new(cx);
	return p;
}

static bool
key_datum(Oid typid, int64 k, Datum *d, RegProcedure *eq)
{
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

/* For each row of the batch that has a key (nulls: its bitmap), whether the parent has a visible row for it: its unique index holds at most one */
void
sift_conn_probe_batch(SiftConnProbe *p, const int64 *keys, const uint64 *nulls, uint32 nrows, bool *one)
{
	Relation	heap = table_open(p->parent, NoLock);
	Relation	idx = index_open(p->index, NoLock);
	IndexScanDesc scan = sift_index_beginscan(heap, idx, p->snap, 1);
	TupleTableSlot *slot = table_slot_create(heap, NULL);

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
			ScanKeyData skey;
			Datum		d;
			RegProcedure eq;

			e->one = false;
			if (key_datum(p->keytype, keys[row], &d, &eq))
			{
				ScanKeyInit(&skey, 1, BTEqualStrategyNumber, eq, d);
				index_rescan(scan, &skey, 1, NULL, 0);
				e->one = index_getnext_slot(scan, ForwardScanDirection, slot);
			}
		}
		one[row] = e->one;
		if (hash_get_num_entries(p->seen) > PROBE_SEEN_MAX)
		{
			hash_destroy(p->seen);
			p->seen = seen_new(p->cx);
		}
		CHECK_FOR_INTERRUPTS();
	}
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(idx, NoLock);
	table_close(heap, NoLock);
}
