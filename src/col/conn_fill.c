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
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/conn.h"

/*
 * What the two ways to know the keys of a parent cost (docs/COL-CONNECT.md): the key set reads every row of the parent twice, 130 ns a parent row; the probe asks a btree for each key
 * the writer fills, 2 us a key when the batch is small and the pages are warm and up to 4 us for a merge's partition over a parent of millions of keys (520 and 9,200 instructions).
 * The fill takes the cheaper for the rows it is to fill: the probe below about 4% of the parent's rows.
 */
#define FILL_SCAN_NS_PER_PARENT_ROW	130
#define FILL_PROBE_NS_PER_KEY		3500
/* a sift_col journal index asked for a run of keys in one search: its segments are opened once, then each key is a point lookup and the heap row it names */
#define FILL_JOURNAL_NS				60000
#define FILL_JOURNAL_NS_PER_KEY		2000


#ifdef SIFT_TESTING
int			sift_test_conn_fill = 0;
#endif

typedef struct FillConn
{
	int			found;			/* the leaf of the flag */
	int			fk_leaf;		/* the leaf of the child that holds the key */
	SiftConnStamp stamp;
	SiftConnKeySet *keys;		/* the keys of the parent (NULL: no stamp, or the probe) */
	SiftConnProbe *probe;		/* the btree probe of the keys of each batch (NULL: no stamp, or the key set) */
	bool	   *one;			/* the probe's answer for each row of the batch in hand */
	uint32		one_cap;
	bool		orphans;		/* a row had a key the parent has no (single) row for: the segment is not clean */
} FillConn;

typedef struct SiftConnFill
{
	SiftColFill hook;			/* first: the writer holds this */
	MemoryContext cx;
	int			nconn;
	Snapshot	snap;			/* kept for the probes until the end call (NULL: none to release) */
	FillConn	c[SIFT_CONN_MAX_CONNECTIONS];
} SiftConnFill;

static void fill_batch(SiftColFill *hook, SiftColBatch *view);
static SiftConnStamp fill_stamp(const SiftColFill *hook, int i);
static void fill_end(SiftColFill *hook);

/* The estimated rows of a table */
double
sift_conn_rows_of(Relation rel)
{
	BlockNumber pages;
	double		tuples,
				allvisfrac;

	table_relation_estimate_size(rel, NULL, &pages, &tuples, &allvisfrac);
	return tuples;
}

/* Is asking the btree for `rows` keys cheaper than another way to the same keys that costs `read_ns`? */
bool
sift_conn_probe_cheaper_than(uint64 rows, double read_ns)
{
#ifdef SIFT_TESTING
	if (sift_test_conn_fill != 0)
		return sift_test_conn_fill == 2;
#endif
	return (double) rows * FILL_PROBE_NS_PER_KEY < read_ns;
}

/* ... and asking a journal index in one search for them */
bool
sift_conn_journal_cheaper_than(uint64 rows, double read_ns)
{
#ifdef SIFT_TESTING
	if (sift_test_conn_fill != 0)
		return sift_test_conn_fill == 2;
#endif
	return FILL_JOURNAL_NS + (double) rows * FILL_JOURNAL_NS_PER_KEY < read_ns;
}

/* Is asking the btree for `rows` keys cheaper than reading a parent of `parent_rows` rows? */
bool
sift_conn_probe_cheaper(uint64 rows, double parent_rows)
{
	return sift_conn_probe_cheaper_than(rows, parent_rows * FILL_SCAN_NS_PER_PARENT_ROW);
}

SiftColFill *
sift_conn_fill_begin(Relation index, MemoryContext mcxt, uint64 rows)
{
	const SiftColRel *r;
	SiftConnFill *f;
	SiftConnSpecs *specs;
	Relation	heap;
	Snapshot	snap;
	bool		probing = false;

	if (!sift_col_is_index(index) || sift_col_rel(index)->nconn == 0)
		return NULL;
	/* a lock taken below (the table, a parent) can reload the relcache entry of the index and free the state it caches: the fill keeps a copy of its own */
	r = sift_col_rel_private(index, mcxt);
	specs = sift_conn_specs(index);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	f = MemoryContextAllocZero(mcxt, sizeof(SiftConnFill));
	f->hook = (SiftColFill) {.batch = fill_batch,.stamp = fill_stamp,.end = fill_end};
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
			AttrNumber	key = get_attnum(RelationGetRelid(parent), s->parent_key);

			if (!sift_conn_probe_cheaper(rows, sift_conn_rows_of(parent)))
				fc->keys = sift_conn_keyset_load(mcxt, parent, key, snap, (Size) maintenance_work_mem * 1024 / 2);
			if (fc->keys == NULL)
			{
				fc->probe = sift_conn_probe_begin(mcxt, parent, key, snap);
				probing = true;
			}
		}
		index_close(pindex, NoLock);
		table_close(parent, NoLock);
	}
	if (IsInParallelMode())
		f->snap = NULL;
	else if (probing)
		f->snap = snap;
	else
		UnregisterSnapshot(snap);
	table_close(heap, NoLock);
	return &f->hook;
}

/* The writer is done: a probing fill gives back the snapshot it kept (in parallel mode it is the statement's own) */
static void
fill_end(SiftColFill *hook)
{
	SiftConnFill *f = (SiftConnFill *) hook;

	if (f->snap != NULL)
		UnregisterSnapshot(f->snap);
	f->snap = NULL;
}

void
sift_conn_fill_end(SiftColFill *fill)
{
	if (fill != NULL && fill->end != NULL)
		fill->end(fill);
}

/* Each row gets the flag that the one row of its parent was found, or NULL */
static void
fill_batch(SiftColFill *hook, SiftColBatch *view)
{
	SiftConnFill *f = (SiftConnFill *) hook;

	for (int i = 0; i < f->nconn; i++)
	{
		FillConn   *fc = &f->c[i];
		const SiftCVals *kv = &view->col[fc->fk_leaf];

		if (fc->probe == NULL)
			continue;
		if (view->nrows > fc->one_cap)
		{
			fc->one = fc->one == NULL ? MemoryContextAlloc(f->cx, view->nrows) : repalloc(fc->one, view->nrows);
			fc->one_cap = view->nrows;
		}
		sift_conn_probe_batch(fc->probe, kv->key, kv->nulls, view->nrows, fc->one);
	}
	for (uint32 row = 0; row < view->nrows; row++)
		for (int i = 0; i < f->nconn; i++)
		{
			FillConn   *fc = &f->c[i];
			const SiftCVals *kv = &view->col[fc->fk_leaf];
			bool		one = false;
			SiftColValue v;

			if (fc->keys != NULL || fc->probe != NULL)
			{
				if (!sift_col_bit_test(kv->nulls, row))
				{
					one = fc->keys != NULL ? sift_conn_keyset_has_one(fc->keys, kv->key[row]) : fc->one[row];
					fc->orphans |= !one;
				}
			}
			v = (SiftColValue) {.isnull = !one,.tag = SIFT_COLTAG_UNKNOWN,.v.i = 1};
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
		uint64		inserted,
					changed;

		if (!s->c[i].vouches)
			continue;
		if (st.lineage != s->c[i].now.lineage)
			return true;
		if (sift_conn_changed(s->c[i].heap, s->c[i].index, &st, s->snap, (uint32) Max(limit, 1), &keys) != SIFT_CONN_OK)
			return true;
		/* no journal records the inserted rows: a segment with orphans (no flag is read once a parent row was inserted, found.c) counts the rows inserted since with the keys changed */
		inserted = st.reserved & SIFT_CONN_STAMP_CLEAN ? 0 : sift_conn_inserted_rows(s->c[i].heap, s->c[i].index, &st);
		changed = keys.n + inserted;
		sift_conn_keys_free(&keys);
		if (changed >= limit)
			return true;
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
