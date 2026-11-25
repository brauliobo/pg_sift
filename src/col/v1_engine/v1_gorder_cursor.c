/*
 * col/v1_engine/v1_gorder_cursor.c - the global order of a leaf of a Format 1 index as the rows a snapshot sees (v1_gorder_cursor.h).
 * The sources are the stored segments' blocks and the write buffers' rows, sorted by (key, address): the NULL rows come first or last as the scan says, each source's value entries are read forward or
 * backward and the sources merged by (class, key, address), the addresses backward with the keys of a descending scan. An entry another source repeats (a row in a segment and in a write buffer) is
 * given once. A source that is the ends of a segment's order stops the merge where its entries stop being the segment's.
 */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "utils/rel.h"

#include "col/v1/gorder.h"
#include "col/v1/v1_reader.h"
#include "col/v1_engine/v1_gorder_cursor.h"
#include "core/compat.h"
#include "core/sift.h"

#define GORD_HEAD_MIN	32

/* sift_col_gorder_exact: 1 the row is live and visible to every snapshot; any other answer (the unit holds it live, its segment lost it, it cannot say) leaves the heap to decide */
#define GORD_EXACT		1

typedef struct GCur
{
	int			src;
	uint32		vs;				/* the value entries [vs, ve) and the NULL ones [ns, ne) the scan reads */
	uint32		ve;
	uint32		ns;
	uint32		ne;
	bool		ends;			/* the source is the ends of a segment's order (cuts says where they stop being its own) */
	SiftCGOrdCuts cuts;
	int			region;			/* 0, 1: the regions in the order of the scan; 2: done */
	uint32		taken;			/* the entries of the region read */
	bool		have;			/* cls, k and tid are the cursor's entry */
	int			cls;			/* 0: NULL rows first, 1: values, 2: NULL rows last */
	int64		k;
	uint64		tid;
	SiftCGOrdBuf *buf;
} GCur;

typedef struct GEnt
{
	int			src;
	int			cls;
	int64		key;
	uint64		tid;
} GEnt;

struct SiftV1GOrd
{
	SiftV1GOrdSpec spec;
	void	   *reader;
	SiftColSource *cs;
	int			ncur;
	GCur	   *cur;
	uint32		head;			/* the entries of a chunk decoded when the scan enters it */
	bool		started;
	bool		short_;
	bool		over;
	uint64		examined;
	bool		last_set;
	GEnt		last;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	BlockNumber nblocks;
};

static int64
key_of(Oid typid, Datum d)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

/* ---- the cursor over one source ---- */

/* the chunk of the entry p is decoded as far as p, doubling when the scan goes on */
static pg_noinline void
cur_fill(SiftV1GOrd *g, GCur *c, uint32 p)
{
	const SiftCGOrdBuf *b = c->buf;
	const uint32 w = p - b->at < b->rows ? Max(b->an * 2, g->head) : g->head;

	sift_col_gorder_head(g->cs, c->src, g->spec.leaf, p, w, c->buf);
}

static void
cur_at(SiftV1GOrd *g, GCur *c, uint32 p, int64 *key, uint64 *addr)
{
	SiftCGOrdBuf *b = c->buf;

	if (p - b->at - b->kfrom >= b->kn || p - b->at - b->afrom >= b->an)
		cur_fill(g, c, p);
	*key = b->key[p - b->at];
	*addr = b->addr[p - b->at];
}

/* the region of the cursor that ends (nulls: the NULL rows) is cut short: the segment has entries after its last, in the order of the scan, that the block does not */
static bool
cur_cut(const SiftV1GOrd *g, const GCur *c, bool nulls)
{
	if (!c->ends)
		return false;
	if (nulls)
		return c->cuts.null_cut;
	return g->spec.desc ? c->cuts.lo_cut && c->vs <= c->cuts.lo : c->cuts.hi_cut && c->ve >= c->cuts.hi;
}

/* the NULL rows are read by address, forward in either direction: the rows of a block of the ends that it left out come after the last it has */
static void
cur_next(SiftV1GOrd *g, GCur *c)
{
	for (; c->region < 2; c->region++, c->taken = 0)
	{
		const bool	nulls = (c->region == 0) == g->spec.nulls_first;
		const uint32 lo = nulls ? c->ns : c->vs;
		const uint32 hi = nulls ? c->ne : c->ve;

		if (c->taken < hi - lo)
		{
			const uint32 p = nulls || !g->spec.desc ? lo + c->taken : hi - 1 - c->taken;

			cur_at(g, c, p, &c->k, &c->tid);
			c->taken++;
			c->have = true;
			c->cls = nulls ? (g->spec.nulls_first ? 0 : 2) : 1;
			c->k = nulls ? 0 : c->k;
			return;
		}
		if (cur_cut(g, c, nulls))
		{
			g->short_ = true;
			c->region = 2;
			break;
		}
	}
	c->have = false;
}

/* < 0: a comes before b in the scan */
static int
cand_cmp(const SiftV1GOrd *g, const GCur *a, const GCur *b)
{
	if (a->cls != b->cls)
		return a->cls < b->cls ? -1 : 1;
	if (a->k != b->k)
		return (a->k < b->k) != g->spec.desc ? -1 : 1;
	if (a->tid != b->tid)
		return (a->tid < b->tid) != (g->spec.desc && a->cls == 1) ? -1 : 1;
	return 0;
}

static bool
merge_step(SiftV1GOrd *g, GEnt *ent)
{
	if (!g->started)
	{
		for (int i = 0; i < g->ncur; i++)
			cur_next(g, &g->cur[i]);
		g->started = true;
	}
	while (!g->short_)
	{
		GCur	   *best = NULL;

		for (int i = 0; i < g->ncur; i++)
			if (g->cur[i].have && (best == NULL || cand_cmp(g, &g->cur[i], best) < 0))
				best = &g->cur[i];
		if (best == NULL)
			break;
		*ent = (GEnt) {.src = best->src,.cls = best->cls,.key = best->k,.tid = best->tid};
		cur_next(g, best);
		if (g->last_set && g->last.cls == ent->cls && g->last.key == ent->key && g->last.tid == ent->tid)
			continue;
		g->last_set = true;
		g->last = *ent;
		return true;
	}
	return false;
}

/* ---- the confirmation of an entry ---- */

static bool
fetch_row(SiftV1GOrd *g, const ItemPointerData *entry)
{
	ItemPointerData tid = *entry;
	bool		call_again = false,
				all_dead;

	return ItemPointerGetBlockNumber(&tid) < g->nblocks &&
		table_index_fetch_tuple(g->fetch, &tid, g->spec.snapshot, g->slot, &call_again, &all_dead);
}

/* the heap has a version the snapshot sees whose key is the entry's: a row an UPDATE gave another key, or a vacuumed address another row took, holds another */
static bool
heap_confirms(SiftV1GOrd *g, const SiftV1GRow *row)
{
	bool		hnull;
	Datum		d;

	if (!fetch_row(g, &row->tid))
		return false;
	d = slot_getattr(g->slot, g->spec.heap_att, &hnull);
	return hnull == row->isnull && (hnull || key_of(g->spec.typid, d) == row->key);
}

bool
sift_v1_gord_next(SiftV1GOrd *g, SiftV1GRow *row)
{
	for (;;)
	{
		GEnt		ent;

		if (!merge_step(g, &ent))
			return false;
		if (++g->examined > g->spec.budget)
		{
			g->over = true;
			return false;
		}
		sift_key_to_tid(ent.tid, &row->tid);
		row->key = ent.key;
		row->isnull = ent.cls != 1;
		row->held = false;
		if (sift_col_gorder_exact(g->cs, ent.src, ent.tid) == GORD_EXACT)
			return true;
		if (heap_confirms(g, row))
		{
			row->held = true;
			return true;
		}
	}
}

TupleTableSlot *
sift_v1_gord_row(SiftV1GOrd *g, SiftV1GRow *row)
{
	if (!row->held)
	{
		const bool	found = fetch_row(g, &row->tid);

		SIFT_INVARIANT(found, "the heap holds no row at the address (%u,%u) of an exact entry of index %u", ItemPointerGetBlockNumber(&row->tid),
					   ItemPointerGetOffsetNumber(&row->tid), RelationGetRelid(g->spec.index));
		row->held = true;
	}
	return g->slot;
}

bool
sift_v1_gord_short(const SiftV1GOrd *g)
{
	return g->short_;
}

uint64
sift_v1_gord_examined(const SiftV1GOrd *g)
{
	return g->examined;
}

bool
sift_v1_gord_over(const SiftV1GOrd *g)
{
	return g->over;
}

/* ---- open and close ---- */

static SiftCGOrdBuf *
buf_new(MemoryContext cx, bool desc)
{
	SiftCGOrdBuf *b = MemoryContextAllocZero(cx, sizeof(SiftCGOrdBuf));

	b->at = UINT32_MAX;
	b->desc = desc;
	return b;
}

/* false: the source has no entry the scan reads; its end may still be cut, which it says to the merge */
static bool
cur_open(SiftV1GOrd *g, GCur *c, int src)
{
	uint32		entries,
				values;

	*c = (GCur) {.src = src};
	if (!sift_col_gorder_info(g->cs, src, g->spec.leaf, &entries, &values))
		return false;
	c->ends = sift_col_gorder_cuts(g->cs, src, g->spec.leaf, &c->cuts);
	if (entries == 0)
		return true;
	sift_col_gorder_range(g->cs, src, g->spec.leaf, &c->vs, &c->ve, &c->ns, &c->ne);
	if (c->vs >= c->ve && c->ns >= c->ne)
		g->short_ |= cur_cut(g, c, false) || cur_cut(g, c, true);
	else
		c->buf = buf_new(CurrentMemoryContext, g->spec.desc);
	return true;
}

SiftV1GOrd *
sift_v1_gord_open(MemoryContext cx, const SiftV1GOrdSpec *spec)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV1GOrd *g = palloc0(sizeof(SiftV1GOrd));
	const SiftCGOrdWant want = {.rows = spec->rows,.desc = spec->desc,.nulls_first = spec->nulls_first};
	int			grouped,
				n;

	g->spec = *spec;
	g->reader = sift_v1_idx_reader.open(spec->heap, spec->index, spec->snapshot, cx, NULL);
	g->cs = sift_v1_idx_colsource(g->reader);
	n = sift_col_gorder_open(g->cs, spec->leaf, spec->rows, 0, NULL, NULL, &grouped, 0, &want);
	SIFT_INVARIANT(n < 0 || grouped < 0, "the global order of leaf %d of index %u is grouped though the scan pins no group", spec->leaf, RelationGetRelid(spec->index));
	g->cur = palloc0(Max(n, 1) * sizeof(GCur));
	for (int i = 0; i < n; i++)
	{
		GCur		c;

		if (!cur_open(g, &c, i))
		{
			n = -1;
			break;
		}
		if (c.buf != NULL)
			g->cur[g->ncur++] = c;
	}
	MemoryContextSwitchTo(old);
	if (n < 0)
	{
		sift_v1_idx_reader.close(g->reader);
		return NULL;
	}
	old = MemoryContextSwitchTo(cx);
	g->head = Max(GORD_HEAD_MIN, Min((uint32) SIFT_CGORD_ROWS, g->ncur > 0 ? 5 * spec->rows / (4 * g->ncur) : 0));
	g->fetch = sift_table_index_fetch_begin(spec->heap);
	g->slot = table_slot_create(spec->heap, NULL);
	g->nblocks = RelationGetNumberOfBlocks(spec->heap);
	MemoryContextSwitchTo(old);
	return g;
}

void
sift_v1_gord_close(SiftV1GOrd *g)
{
	table_index_fetch_end(g->fetch);
	ExecDropSingleTupleTableSlot(g->slot);
	sift_v1_idx_reader.close(g->reader);
}
