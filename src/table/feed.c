/*
 * table/feed.c - what the column groups give the aggregate scan (table/src.c): the schemas, the write buffer rows as an engine batch and the statistics of the declared paths
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

/* ---------- the column source of the aggregate scan (table/src.c) ---------- */

const SiftColSchema *
sift_tbl_layout_schema(Relation rel, SiftTblRel *st)
{
	return layout_of(rel, st)->schema;
}

const SiftColSchema *
sift_tbl_seg_schema(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	return schema_of(rel, st, e);
}

/* The page idx of seg into copy; false for a page of overflow bytes */
bool
seg_page(Relation rel, SiftTblSeg *seg, uint32 idx, Page copy)
{
	Buffer		buf = sift_lane_read_locked(rel, sift_tbl_wbuf_page(rel, seg, idx), BUFFER_LOCK_SHARE);

	sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return !(TBL_PAGE(copy)->flags & SIFT_TBL_PAGE_OVFDATA);
}

typedef void (*RowFn) (const SiftColRow *row, void *arg);

/* The live rows of page p of the write buffer seg (copy: the page) as column rows, in rowcx, which the caller resets */
static void
page_rows(Relation rel, const SiftTblLayout *l, const SiftTblSeg *seg, uint32 p, Page copy, MemoryContext rowcx, RowFn fn, void *arg)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));
	BlockNumber vblock = seg->vbase + p;

	for (uint32 off = 1; off <= TBL_PAGE(copy)->nrows; off++)
	{
		const SiftTblRec *rec = sift_tbl_page_rec(copy, seg->gen, off);
		MemoryContext old = MemoryContextSwitchTo(rowcx);
		ItemPointerData tid;
		SiftColRow	row;
		HeapTupleData tup;

		if (rec != NULL && !(rec->flags & SIFT_TBL_REC_DEAD))
		{
			ItemPointerSet(&tid, vblock, off);
			tup = *sift_tbl_rec_tuple(rel, rec, seg->gen, sift_key_from_tid(&tid));
			heap_deform_tuple(&tup, desc, values, isnull);
			row_in(l, desc, &tid, values, isnull, &row);
			fn(&row, arg);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(rowcx);
	}
	pfree(values);
	pfree(isnull);
}

static void
batch_add(const SiftColRow *row, void *arg)
{
	sift_cbatch_add(arg, row);
}

SiftColBatch *
sift_tbl_wbuf_batch(Relation rel, SiftTblRel *st, SiftTblSeg *seg, int nseg, uint32 sb, MemoryContext mcxt)
{
	const SiftTblLayout *l = layout_of(rel, st);
	MemoryContext rowcx = AllocSetContextCreate(CurrentMemoryContext, "sift_table batch row", ALLOCSET_SMALL_SIZES);
	SiftColBatch *b = sift_cbatch_create(mcxt, l->schema);
	PGAlignedBlock copy;

	sift_cbatch_reset(b, sb);
	for (int i = 0; i < nseg; i++)
		for (uint32 p = 0; p < seg[i].npages; p++)
			if ((seg[i].vbase + p) / SIFT_SB_PAGES == sb && seg_page(rel, &seg[i], p, copy.data))
				page_rows(rel, l, &seg[i], p, copy.data, rowcx, batch_add, b);
	MemoryContextDelete(rowcx);
	return b;
}

/* ---------- the statistics of the declared paths ---------- */

/* The values a path leaf cannot hold, which make a query that reads it keep the normal plan (SiftSrcStats.ovf) */
typedef struct PathWbuf
{
	uint64		gen;
	uint16	   *seen;			/* [npages] the rows each page held when it was last read, 0xFFFF before: a page fills while it is the target of a backend */
	bool		ovf[SIFT_COL_MAX_COLS];
} PathWbuf;

typedef struct PathOvf
{
	const SiftColSchema *schema;
	bool	   *ovf;
} PathOvf;

static void
row_path_ovf(const SiftColRow *row, void *arg)
{
	const PathOvf *po = arg;

	for (int c = 0; c < po->schema->ncols; c++)
	{
		const SiftColDef *def = &po->schema->cols[c];

		if (def->attno == 0 || !def->path[0])
			continue;
		if (def->parent < 0)
			po->ovf[c] |= row->val[c].overflow;
		else
			for (uint64 j = 0, n = row->val[def->parent].isnull ? 0 : row->val[def->parent].v.i; j < n; j++)
				po->ovf[c] |= row->elem[c][j].overflow;
	}
}

static PathWbuf *
path_wbuf(SiftTblRel *st, const SiftTblSeg *seg)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	MemoryContext old;

	for (int i = 0; i < c->npwb; i++)
		if (c->pwb[i].gen == seg->gen)
			return &c->pwb[i];
	old = MemoryContextSwitchTo(st->cxt);
	c->pwb = c->pwb ? repalloc(c->pwb, (c->npwb + 1) * sizeof(PathWbuf)) : palloc((c->npwb + 1) * sizeof(PathWbuf));
	c->pwb[c->npwb] = (PathWbuf) {.gen = seg->gen,.seen = palloc(SIFT_TBL_MAX_WBUF_PAGES * sizeof(uint16))};
	memset(c->pwb[c->npwb].seen, 0xFF, SIFT_TBL_MAX_WBUF_PAGES * sizeof(uint16));
	MemoryContextSwitchTo(old);
	return &c->pwb[c->npwb++];
}

/* The leaf of seg that holds the leaf d of the table's layout (same attribute, path and type), -1 when it has none */
static int
seg_leaf(const SiftColSchema *seg, const SiftColDef *d)
{
	for (int j = 0; j < seg->ncols; j++)
	{
		const SiftColDef *e = &seg->cols[j];

		if (e->attno == d->attno && e->kind == d->kind && e->store == d->store && e->type == d->type && e->path && strcmp(e->path, d->path) == 0)
			return j;
	}
	return -1;
}

/* What the zones of the segments say about the leaves of the declared paths, kept until the set of segments changes */
static const SiftColLeafStats *
path_zones(Relation rel, SiftTblRel *st, const SiftTblLayout *l)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	uint64		key = st->dir.nsegs;

	for (int i = 0; i < st->dir.nsegs; i++)
		key = key * 1000003 + st->dir.segs[i].gen * (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE);
	if (c->pls && c->pkey == key)
		return c->pls;
	if (c->pls)
		pfree(c->pls);
	c->pls = MemoryContextAlloc(st->cxt, l->schema->ncols * sizeof(SiftColLeafStats));
	for (int i = 0; i < l->schema->ncols; i++)
		sift_col_leaf_stats_init(&c->pls[i]);
	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift_table path stats", ALLOCSET_SMALL_SIZES);
			SiftColSegment *seg = sift_cseg_open(rel, &st->dir.segs[i], tmp);

			for (int k = 0; k < l->schema->ncols; k++)
			{
				const SiftColDef *d = &l->schema->cols[k];
				int			j = d->attno > 0 && d->path[0] ? seg_leaf(seg->schema, d) : -1;

				if (j >= 0)
					sift_cseg_leaf_stats(seg, j, &c->pls[k]);
			}
			sift_cseg_close(seg);
			MemoryContextDelete(tmp);
		}
	c->pkey = key;
	return c->pls;
}

/* The leaves of the write buffers that hold a value the leaf cannot: only the pages that grew since the last call are decoded */
static void
path_wbuf_ovf(Relation rel, SiftTblRel *st, const SiftTblLayout *l, bool *ovf)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	MemoryContext rowcx = AllocSetContextCreate(CurrentMemoryContext, "sift_table path ovf", ALLOCSET_SMALL_SIZES);
	PGAlignedBlock copy;
	int			n = 0;

	for (int i = 0; i < st->nwbuf; i++)
	{
		SiftTblSeg	seg = st->wbuf[i];
		PathWbuf   *w = path_wbuf(st, &seg);
		PathOvf		po = {l->schema, w->ovf};

		sift_tbl_seg_load(rel, &seg);
		for (uint32 p = 0; p < seg.npages; p++)
		{
			if (!seg_page(rel, &seg, p, copy.data) || w->seen[p] == TBL_PAGE(copy.data)->nrows)
				continue;
			page_rows(rel, l, &seg, p, copy.data, rowcx, row_path_ovf, &po);
			w->seen[p] = TBL_PAGE(copy.data)->nrows;
		}
		for (int k = 0; k < l->schema->ncols; k++)
			ovf[k] |= w->ovf[k];
	}
	MemoryContextDelete(rowcx);
	for (int i = 0; i < c->npwb; i++)
	{
		bool		live = false;

		for (int k = 0; k < st->nwbuf && !live; k++)
			live = st->wbuf[k].gen == c->pwb[i].gen;
		if (live)
			c->pwb[n++] = c->pwb[i];
		else
			pfree(c->pwb[i].seen);
	}
	c->npwb = n;
}

void
sift_tbl_path_stats(Relation rel, SiftTblRel *st, SiftColLeafStats *ls, bool *wovf)
{
	const SiftTblLayout *l = layout_of(rel, st);

	memset(wovf, 0, l->schema->ncols * sizeof(bool));
	if (!l->shred)
	{
		for (int i = 0; i < l->schema->ncols; i++)
			sift_col_leaf_stats_init(&ls[i]);
		return;
	}
	sift_tbl_refresh(rel, st);
	memcpy(ls, path_zones(rel, st, l), l->schema->ncols * sizeof(SiftColLeafStats));
	path_wbuf_ovf(rel, st, l, wovf);
}

/* Does the segment schema hold the leaf of every declared path of the table's layout (a segment written before the declaration does not)? */
static bool
schema_has_paths(const SiftColSchema *seg, const SiftColSchema *cur)
{
	for (int i = 0; i < cur->ncols; i++)
	{
		const SiftColDef *d = &cur->cols[i];
		bool		found = false;

		if (d->attno == 0 || !d->path[0])
			continue;
		for (int j = 0; j < seg->ncols && !found; j++)
		{
			const SiftColDef *e = &seg->cols[j];

			found = e->attno == d->attno && e->kind == d->kind && e->store == d->store && e->type == d->type && e->path && strcmp(e->path, d->path) == 0;
		}
		if (!found)
			return false;
	}
	return true;
}

bool
sift_tbl_paths_complete(Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		/* the types of the values of a path are in a segment of column format minor 4 and later */
		if (e->kind == SIFT_SEG_IMMUTABLE && (e->format < SIFT_COL_FORMAT_TAGS || !schema_has_paths(schema_of(rel, st, e), l->schema)))
			return false;
	}
	return true;
}
