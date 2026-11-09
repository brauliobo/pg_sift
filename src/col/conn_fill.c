/*
 * col/conn_fill.c - the found flags of a connected index (col/conn.h, docs/COL-CONNECT.md): the lookup of the parent tables under a stamp, and the filling of the flags of a batch
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/conn.h"

typedef struct FillEnt
{
	int64		key;
	bool		repeated;		/* the key is in two visible rows of the table: neither is used */
} FillEnt;

typedef struct FillConn
{
	int			found;			/* the leaf of the flag */
	int			fk_leaf;		/* the leaf of the child that holds the key */
	SiftConnStamp stamp;
	HTAB	   *tab;			/* NULL: no stamp, the leaves are cleared */
	bool		orphans;		/* a row had a key the parent has no (single) row for: the segment is not clean */
} FillConn;

typedef struct SiftConnFill
{
	SiftColFill hook;			/* first: the writer holds this */
	MemoryContext cx;
	int			nconn;
	FillConn	c[SIFT_CONN_MAX_CONNECTIONS];
} SiftConnFill;

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

/* The table of the parent keys under snap, or NULL when it holds more than the memory of a maintenance operation allows */
static void fill_batch(SiftColFill *hook, SiftColBatch *view);
static SiftConnStamp fill_stamp(const SiftColFill *hook, int i);

static HTAB *
table_of(MemoryContext cx, Relation parent, AttrNumber key, Snapshot snap)
{
	HASHCTL		ctl = {.keysize = sizeof(int64),.entrysize = sizeof(FillEnt),.hcxt = cx};
	HTAB	   *tab = hash_create("sift connection fill", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	TableScanDesc scan = sift_table_beginscan(parent, snap, 0, NULL);
	TupleTableSlot *slot = table_slot_create(parent, NULL);
	Oid			keytype = TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid;
	Size		used = 0,
				limit = (Size) maintenance_work_mem * 1024 / 2;
	bool		fits = true;

	while (fits && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull,
					found;
		Datum		d = slot_getattr(slot, key, &isnull);
		int64		k;
		FillEnt    *e;

		if (isnull || !key_of(keytype, d, &k))
			continue;
		e = hash_search(tab, &k, HASH_ENTER, &found);
		e->repeated = found;
		if (!found)
		{
			used += sizeof(FillEnt) + 32;
			fits = used <= limit;
		}
		CHECK_FOR_INTERRUPTS();
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	if (!fits)
	{
		hash_destroy(tab);
		return NULL;
	}
	return tab;
}

SiftColFill *
sift_conn_fill_begin(Relation index, MemoryContext mcxt)
{
	const SiftColRel *r;
	SiftConnFill *f;
	SiftConnSpecs *specs;
	Relation	heap;
	Snapshot	snap;

	if (!sift_col_is_index(index) || sift_col_rel(index)->nconn == 0)
		return NULL;
	/* a lock taken below (the table, a parent) can reload the relcache entry of the index and free the state it caches: the fill keeps a copy of its own */
	r = sift_col_rel_private(index, mcxt);
	specs = sift_conn_specs(index);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	f = MemoryContextAllocZero(mcxt, sizeof(SiftConnFill));
	f->hook = (SiftColFill) {.batch = fill_batch,.stamp = fill_stamp};
	f->cx = mcxt;
	f->nconn = r->nconn;
	/* in parallel mode no snapshot can be taken: the statement's own (it may be older than the journal, which then does not vouch) */
	snap = IsInParallelMode() ? GetActiveSnapshot() : RegisterSnapshot(GetTransactionSnapshot());
	for (int i = 0; i < r->nconn; i++)
	{
		FillConn   *fc = &f->c[i];
		const SiftConnSpec *s = specs->c[i];
		Relation	parent,
					pindex;
		int			a = 0;

		fc->found = r->conn_found[i];
		for (int j = 0; j < index->rd_index->indnkeyatts; j++)
			if (index->rd_index->indkey.values[j] == get_attnum(RelationGetRelid(heap), s->fk))
				a = j + 1;
		fc->fk_leaf = r->first[a];
		pindex = index_open(sift_conn_parent_index(heap, index, s, &parent), AccessShareLock);
#ifdef SIFT_TESTING
		sift_col_test_inval(index);
#endif
		if (sift_conn_take(parent, pindex, snap, &fc->stamp, NULL) == SIFT_CONN_OK)
		{
			fc->tab = table_of(mcxt, parent, get_attnum(RelationGetRelid(parent), s->parent_key), snap);
			if (fc->tab == NULL)
				fc->stamp = (SiftConnStamp) {.reserved = SIFT_CONN_STAMP_REFUSED};
		}
		index_close(pindex, NoLock);
		table_close(parent, NoLock);
	}
	if (!IsInParallelMode())
		UnregisterSnapshot(snap);
	table_close(heap, NoLock);
	return &f->hook;
}

/* Each row gets the flag that the one row of its parent was found, or NULL */
static void
fill_batch(SiftColFill *hook, SiftColBatch *view)
{
	SiftConnFill *f = (SiftConnFill *) hook;

	for (uint32 row = 0; row < view->nrows; row++)
		for (int i = 0; i < f->nconn; i++)
		{
			FillConn   *fc = &f->c[i];
			const SiftCVals *kv = &view->col[fc->fk_leaf];
			FillEnt    *e = NULL;
			int64		k = kv->key[row];

			SiftColValue v;

			if (fc->tab != NULL && !sift_col_bit_test(kv->nulls, row))
			{
				e = hash_search(fc->tab, &k, HASH_FIND, NULL);
				fc->orphans |= e == NULL || e->repeated;
			}
			v = (SiftColValue) {.isnull = e == NULL || e->repeated,.tag = SIFT_COLTAG_UNKNOWN,.v.i = 1};
			sift_cfound_put(view, fc->found, &v);
		}
}

static SiftConnStamp
fill_stamp(const SiftColFill *hook, int i)
{
	const FillConn *fc = &((const SiftConnFill *) hook)->c[i];
	SiftConnStamp st = fc->stamp;

	if (st.lineage != 0 && !fc->orphans)
		st.reserved |= SIFT_CONN_STAMP_CLEAN;
	return st;
}

int			sift_col_connect_refresh_pct = 10;

struct SiftConnStale
{
	int			n;
	Snapshot	snap;
	struct
	{
		Relation	heap,
					index;
		SiftConnStamp now;
		bool		vouches;
		double		rows;
	}			c[SIFT_CONN_MAX_CONNECTIONS];
};

SiftConnStale *
sift_conn_stale_begin(Relation index, MemoryContext mcxt)
{
	const SiftColRel *r;
	SiftConnStale *s;
	SiftConnSpecs *specs;
	Relation	heap;

	if (!sift_col_is_index(index) || sift_col_rel(index)->nconn == 0)
		return NULL;
	r = sift_col_rel_private(index, mcxt);
	specs = sift_conn_specs(index);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	s = MemoryContextAllocZero(mcxt, sizeof(SiftConnStale));
	s->n = r->nconn;
	s->snap = IsInParallelMode() ? GetActiveSnapshot() : GetTransactionSnapshot();
	for (int i = 0; i < s->n; i++)
	{
		s->c[i].index = index_open(sift_conn_parent_index(heap, index, specs->c[i], &s->c[i].heap), AccessShareLock);
#ifdef SIFT_TESTING
		sift_col_test_inval(index);
#endif
		s->c[i].vouches = sift_conn_take(s->c[i].heap, s->c[i].index, s->snap, &s->c[i].now, NULL) == SIFT_CONN_OK;
		s->c[i].rows = Max(s->c[i].index->rd_rel->reltuples, 1);
	}
	table_close(heap, NoLock);
	return s;
}

bool
sift_conn_stale(SiftConnStale *s, const SiftColSegment *seg)
{
	for (int i = 0; i < s->n; i++)
	{
		SiftConnStamp st = i < seg->nconn ? seg->conn[i] : (SiftConnStamp) {0};
		double		limit = s->c[i].rows * sift_col_connect_refresh_pct / 100;
		SiftConnKeys keys;

		if (!s->c[i].vouches)
			continue;
		if (st.lineage == 0 ? st.reserved != SIFT_CONN_STAMP_REFUSED : st.lineage != s->c[i].now.lineage)
			return true;
		if (st.lineage == 0)
			continue;
		if (sift_conn_changed(s->c[i].heap, s->c[i].index, &st, s->snap, (uint32) Max(limit, 1), &keys) != SIFT_CONN_OK)
			return true;
		if (keys.n >= limit)
		{
			sift_conn_keys_free(&keys);
			return true;
		}
		sift_conn_keys_free(&keys);
	}
	return false;
}

void
sift_conn_stale_end(SiftConnStale *s)
{
	for (int i = 0; i < s->n; i++)
	{
		index_close(s->c[i].index, NoLock);
		table_close(s->c[i].heap, NoLock);
	}
}
