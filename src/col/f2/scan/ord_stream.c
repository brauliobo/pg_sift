/*
 * col/f2/scan/ord_stream.c - the ordered scan of an index of format major 2 (col/f2/scan/ord_stream.h): the partitions tile the key space in order; within one the parts overlap, each sorted by key and
 * address (the L0 slab too, once sorted). A cursor reads a part a granule at a time through the sink of f2_scan.c (predicates, zones, deletion vectors and pending deletes decide the rows as for any
 * scan) and a heap merges the cursor heads. A cursor sits between two rows; a mark is those positions, and a turn takes back the last row its cursor advanced over.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "catalog/pg_index.h"

#include "col/f2/f2_cache.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_part_pin_scan.h"
#include "col/f2/key_plan.h"
#include "col/f2/l0_stage.h"
#include "col/f2/scan/ord_stream.h"
#include "col/f2/scan/par_int.h"
#include "col/f2/scan/rescan_cache.h"
#include "core/memtrack.h"
#include "core/errctx.h"

typedef struct OrdRow
{
	uint64		tid;
	int64		key[SIFT_BOUND_COMPS];
	uint32		retnull;
	uint8		knull;
	uint8		maybe;
	Datum		ret[FLEXIBLE_ARRAY_MEMBER];
} OrdRow;

typedef struct OrdCursor
{
	struct SiftOrdStream *os;
	SiftF2PartPin *pin;			/* NULL for the slab */
	PartCx		pcx;
	const SiftPointCands *cands;
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	const SiftPendDelIndex *px;
	bool		cached;			/* its granules are read decoded (sift_rescan_part_ok) */
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	uint32		ngran;
	bool		loaded;			/* the live rows of granule gi are in rows */
	uint32		gi;
	char	   *rows;
	int			nrows;
	int			cap;
	int			pos;			/* the cursor is between rows pos - 1 and pos */
	int			head;			/* the row the cursor gives next in the direction of the scan, -1: none */
	MemoryContext datacx;
} OrdCursor;

typedef struct OrdPos
{
	bool		loaded;
	uint32		gi;
	int			pos;
} OrdPos;

typedef struct OrdMark
{
	bool		valid;
	bool		fwd;
	bool		ended;
	int			part;
	int			last;
	int			ncur;
	OrdPos	   *pos;
} OrdMark;

typedef struct SiftOrdStream
{
	F2Scan	   *s;
	Relation	rel;
	Snapshot	snap;
	const SiftPlanSnapshot *plan;
	const SiftF2Rel *f;
	const SiftColSchema *schema;
	const SiftPendDelIndex *staged_px;
	SiftRescanScan *rc;			/* not NULL: the granules of a part are read decoded (scan/rescan_cache.c) */
	int			nkey;
	int			key_leaf[SIFT_BOUND_COMPS];
	bool		nulls_first[SIFT_BOUND_COMPS];
	bool		desc[SIFT_BOUND_COMPS];
	int			leaves[SIFT_BOUND_COMPS + INDEX_MAX_KEYS];
	int			nleaves;
	Size		stride;
	int			plo;			/* the partitions that can hold a row, in the order of the key */
	int			phi;
	int			part;			/* the partition the cursors read; plo - 1 before the first and phi + 1 after the last */
	bool		fwd;
	bool		ended;
	MemoryContext pcx;			/* the cursors of the partition */
	OrdCursor  *cur;
	int			ncur;
	int		   *heap;
	int			nheap;
	int			last;			/* the cursor that gave the last row, -1: none */
	bool		itup_ok;		/* every returned attribute has a fixed size: the row is an index tuple (what the planner reads the ends of a column from); else a heap tuple */
	IndexTuple	itup;
	OrdMark		mark;
} SiftOrdStream;

static inline OrdRow *
row_at(const SiftOrdStream *os, const OrdCursor *c, int i)
{
	return (OrdRow *) (c->rows + (Size) i * os->stride);
}

static int
row_cmp(const SiftOrdStream *os, const OrdRow *a, const OrdRow *b)
{
	for (int k = 0; k < os->nkey; k++)
	{
		const bool	na = (a->knull >> k) & 1,
					nb = (b->knull >> k) & 1;
		int			c;

		if (na || nb)
		{
			if (na != nb)
				return na == os->nulls_first[k] ? -1 : 1;
			continue;
		}
		c = a->key[k] < b->key[k] ? -1 : a->key[k] > b->key[k];
		if (c != 0)
			return os->desc[k] ? -c : c;
	}
	return a->tid < b->tid ? -1 : a->tid > b->tid;
}

static int
row_qcmp(const void *a, const void *b, void *arg)
{
	return row_cmp(arg, a, b);
}

/* Whether the row satisfies the row comparisons of the scan: the first pair of values that differ decides, and a NULL among those it reaches makes the comparison NULL, which no row satisfies */
static bool
row_accepted(const SiftOrdStream *os, const OrdRow *o)
{
	for (int j = 0; j < os->s->nrowcmp; j++)
	{
		const SiftRowBound *rb = &os->s->rowcmp[j];
		int			c = 0;

		for (int k = 0; k < rb->n && c == 0; k++)
		{
			if ((o->knull >> k) & 1)
				return false;
			c = o->key[k] < rb->vals[k].i ? -1 : o->key[k] > rb->vals[k].i;
		}
		if (!(rb->op == SIFT_KOP_GT ? c > 0 : rb->op == SIFT_KOP_GE ? c >= 0 : rb->op == SIFT_KOP_LT ? c < 0 : c <= 0))
			return false;
	}
	return true;
}

static void
ord_sink(void *arg, const SiftF2Batch *b)
{
	OrdCursor  *c = arg;
	SiftOrdStream *os = c->os;
	const F2Scan *s = os->s;
	const int	nlive = sift_f2_batch_rows(b);
	MemoryContext old;

	if (nlive > c->cap)
	{
		c->rows = c->rows == NULL ? MemoryContextAlloc(os->pcx, (Size) nlive * os->stride) :
			repalloc(c->rows, (Size) nlive * os->stride);
		c->cap = nlive;
	}
	old = MemoryContextSwitchTo(c->datacx);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		for (uint64 bits = b->live[w] | b->maybe[w]; bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);
			OrdRow	   *o = row_at(os, c, c->nrows++);

			o->tid = b->tid[r];
			o->maybe = (((b->maybe[w] >> (r & 63)) & 1) | s->recheck_all) != 0;
			o->knull = 0;
			o->retnull = 0;
			for (int k = 0; k < os->nkey; k++)
			{
				const ColView *cv = &b->views[os->key_leaf[k]];

				o->key[k] = cv->isnull[r] ? 0 : cv->i[r];
				o->knull |= (uint8) cv->isnull[r] << k;
			}
			if (!row_accepted(os, o))
			{
				c->nrows--;
				continue;
			}
			for (int j = 0; j < s->nret; j++)
				if (!sift_f2_view_datum(&os->schema->cols[s->ret_leaf[j]], &b->views[s->ret_leaf[j]], r, &o->ret[j],
									 TupleDescAttr(s->itupdesc, s->ret_att[j] - 1)))
					o->retnull |= 1U << j;
		}
	MemoryContextSwitchTo(old);
}

/* Granule gi (of the granules of the spans in order) of the cursor, its live rows in the order of the key */
static void
cur_load(SiftOrdStream *os, OrdCursor *c, uint32 gi)
{
	F2Scan	   *s = os->s;
	uint32		g = gi;
	int			sp = 0;

	MemoryContext old = MemoryContextSwitchTo(s->cx);

	MemoryContextReset(c->datacx);
	c->nrows = 0;
	c->gi = gi;
	c->loaded = true;
	s->sink_arg = c;
	s->part = c->d;
	if (c->pin == NULL)
	{
		scan_slab(os->rel, s, os->f, os->schema, c->e, c->px, os->snap);
		qsort_arg(c->rows, c->nrows, os->stride, row_qcmp, os);
		MemoryContextSwitchTo(old);
		return;
	}
	for (; g >= c->spans[sp][1] - c->spans[sp][0]; sp++)
		g -= c->spans[sp][1] - c->spans[sp][0];
	if (c->cached)
	{
		sift_rescan_granule(os->rel, s, os->rc, os->schema, c->pin, g + c->spans[sp][0], c->px, os->snap);
		MemoryContextSwitchTo(old);
		return;
	}
	if ((c->e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0)
	{
		int			nexc = c->pcx.pp.nexc;

		sift_cdi_scan_exceptions(s, os->rel, os->schema, c->e, c->pin->part, os->snap, c->pcx.pp.exc, &nexc);
	}
	s->cands = c->cands;
	g += c->spans[sp][0];
	scan_span(os->rel, s, os->schema, c->pin, &c->pcx.pp, g, g + 1, c->px, os->snap);
	s->cands = NULL;
	MemoryContextSwitchTo(old);
}

/* The next row of the cursor in the direction (its index in rows), loading the granule that has it; -1 when there is none */
static int
cur_head(SiftOrdStream *os, OrdCursor *c, bool fwd)
{
	if (!c->loaded)
	{
		if (c->ngran == 0)
			return -1;
		cur_load(os, c, fwd ? 0 : c->ngran - 1);
		c->pos = fwd ? 0 : c->nrows;
	}
	for (;;)
	{
		if (fwd ? c->pos < c->nrows : c->pos > 0)
			return fwd ? c->pos : c->pos - 1;
		if (fwd ? c->gi + 1 >= c->ngran : c->gi == 0)
			return -1;
		cur_load(os, c, fwd ? c->gi + 1 : c->gi - 1);
		c->pos = fwd ? 0 : c->nrows;
	}
}

/* Takes back the row the cursor advanced over in direction fwd: its position is before it again */
static void
cur_unstep(SiftOrdStream *os, OrdCursor *c, bool fwd)
{
	while (fwd ? c->pos == 0 : c->pos == c->nrows)
	{
		cur_load(os, c, fwd ? c->gi - 1 : c->gi + 1);
		c->pos = fwd ? c->nrows : 0;
	}
	c->pos += fwd ? -1 : 1;
}

/* ---- the merge ---- */

static bool
heap_before(const SiftOrdStream *os, int a, int b)
{
	const OrdCursor *x = &os->cur[a],
			   *y = &os->cur[b];
	int			c = row_cmp(os, row_at(os, x, x->head), row_at(os, y, y->head));

	c = os->fwd ? c : -c;
	return c < 0 || (c == 0 && a < b);
}

static void
heap_down(SiftOrdStream *os, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					m = i,
					t;

		if (l < os->nheap && heap_before(os, os->heap[l], os->heap[m]))
			m = l;
		if (l + 1 < os->nheap && heap_before(os, os->heap[l + 1], os->heap[m]))
			m = l + 1;
		if (m == i)
			return;
		t = os->heap[i];
		os->heap[i] = os->heap[m];
		os->heap[m] = t;
		i = m;
	}
}

/* The heads of the cursors in the direction of the scan, and the heap of them */
static void
heap_prime(SiftOrdStream *os)
{
	os->nheap = 0;
	for (int i = 0; i < os->ncur; i++)
	{
		os->cur[i].head = cur_head(os, &os->cur[i], os->fwd);
		if (os->cur[i].head >= 0)
			os->heap[os->nheap++] = i;
	}
	for (int i = os->nheap / 2 - 1; i >= 0; i--)
		heap_down(os, i);
}

static void
ord_release(void *arg)
{
	SiftOrdStream *os = arg;

	for (int i = 0; i < os->ncur; i++)
		if (os->cur[i].pin != NULL)
			sift_f2_pin_release(os->cur[i].pin);
	os->ncur = 0;
}

static void
cur_init(SiftOrdStream *os, OrdCursor *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	memset(c, 0, sizeof(*c));
	c->os = os;
	c->e = e;
	c->d = d;
	c->px = px;
	c->datacx = sift_mem_allocset_create(os->pcx, "sift f2 ordered row");
	c->ngran = 1;
}

/* The cursors of partition idx: its L0 slab and the parts the key range and the time window leave, the granules of each that the key plan reaches */
static void
part_cursors(SiftOrdStream *os, int idx)
{
	F2Scan	   *s = os->s;
	const SiftPartitionEntry *e = &os->plan->partitions[idx];
	const SiftPlanPartList *list = &os->plan->partlists[idx];
	const bool	in_window = sift_f2_entry_meets_time(e, &s->time);
	MemoryContext old;
	MemoryContextCallback *cb;
	const SiftPendDelIndex *px;

	MemoryContextReset(os->pcx);
	os->ncur = os->nheap = 0;
	os->last = -1;
	old = MemoryContextSwitchTo(os->pcx);
	cb = MemoryContextAlloc(os->pcx, sizeof(*cb));
	*cb = (MemoryContextCallback) {ord_release, os};
	MemoryContextRegisterResetCallback(os->pcx, cb);
	os->cur = MemoryContextAllocZero(os->pcx, (list->nparts + 1) * sizeof(OrdCursor));
	os->heap = MemoryContextAlloc(os->pcx, (list->nparts + 1) * sizeof(int));
	px = sift_f2_pending_index(os->rel, e, os->snap, os->staged_px);
	if (e->l0_slab_blk != InvalidBlockNumber)
		cur_init(os, &os->cur[os->ncur++], e, NULL, px);
	for (int p = 0; p < list->nparts; p++)
	{
		const SiftPartDesc *d = &list->parts[p];
		OrdCursor  *c = &os->cur[os->ncur];

		if (!in_window || !sift_f2_part_overlaps(d, &s->keys) || !sift_f2_part_meets_time(d, &s->time))
			continue;
		cur_init(os, c, e, d, px);
		os->ncur++;
		c->pin = sift_f2_scan_pin(os->rel, d, (uint32) e->part_id);
		s->part = d;
		c->ngran = 0;
		c->cached = os->rc != NULL && sift_rescan_part_ok(os->rel, e, c->pin);
		if (c->cached || part_open(os->rel, s, os->schema, e, c->pin, os->snap, &c->pcx))
		{
			c->cands = s->cands;
			s->cands = NULL;
			c->nspans = sift_f2_granule_spans(os->rel, c->pin->part, os->f, &s->keys, &s->keyplan, c->spans);
			for (int i = 0; i < c->nspans; i++)
				c->ngran += c->spans[i][1] - c->spans[i][0];
		}
		if (c->ngran == 0)
		{
			sift_f2_pin_release(c->pin);
			c->pin = NULL;
			MemoryContextDelete(c->datacx);
			os->ncur--;
		}
	}
	MemoryContextSwitchTo(old);
}

/* Moves through the partitions in the direction until one has a row to give; false past the last */
static bool
ord_fill(SiftOrdStream *os)
{
	while (os->nheap == 0)
	{
		os->part += os->fwd ? 1 : -1;
		if (os->part < os->plo || os->part > os->phi)
		{
			os->ended = true;
			return false;
		}
		part_cursors(os, os->part);
		heap_prime(os);
	}
	return true;
}

static void
ord_tuple(IndexScanDesc scan, SiftOrdStream *os, const OrdRow *o)
{
	F2Scan	   *s = os->s;
	Datum		vals[INDEX_MAX_KEYS] = {0};
	bool		nulls[INDEX_MAX_KEYS];
	MemoryContext old = MemoryContextSwitchTo(s->cx);

	memset(nulls, true, scan->xs_hitupdesc->natts);
	for (int j = 0; j < s->nret; j++)
	{
		vals[s->ret_att[j] - 1] = o->ret[j];
		nulls[s->ret_att[j] - 1] = (o->retnull >> j) & 1;
	}
	if (os->itup_ok)
	{
		if (os->itup != NULL)
			pfree(os->itup);
		scan->xs_itup = os->itup = index_form_tuple(scan->xs_itupdesc, vals, nulls);
		os->itup->t_tid = scan->xs_heaptid;
	}
	else
	{
		if (s->htup != NULL)
			heap_freetuple(s->htup);
		scan->xs_hitup = s->htup = heap_form_tuple(scan->xs_hitupdesc, vals, nulls);
	}
	MemoryContextSwitchTo(old);
}

static bool
ord_next(IndexScanDesc scan, SiftOrdStream *os)
{
	OrdCursor  *c;

	if (os->ended || !ord_fill(os))
		return false;
	c = &os->cur[os->heap[0]];
	sift_f2_tid_set(&scan->xs_heaptid, row_at(os, c, c->head)->tid);
	scan->xs_recheck = row_at(os, c, c->head)->maybe;
	if (scan->xs_want_itup)
		ord_tuple(scan, os, row_at(os, c, c->head));
	os->last = os->heap[0];
	c->pos += os->fwd ? 1 : -1;
	c->head = cur_head(os, c, os->fwd);
	if (c->head < 0)
		os->heap[0] = os->heap[--os->nheap];
	heap_down(os, 0);
	return true;
}

/* A scroll that turns round: the row the last cursor gave is not the next one back, the rows between the cursors are */
static void
ord_flip(SiftOrdStream *os, bool fwd)
{
	if (os->ended)
		os->nheap = 0;
	else
	{
		if (os->last >= 0)
			cur_unstep(os, &os->cur[os->last], os->fwd);
		os->fwd = fwd;
		heap_prime(os);
	}
	os->ended = false;
	os->fwd = fwd;
	os->last = -1;
}

static SiftOrdStream *
ord_begin(IndexScanDesc scan, bool fwd)
{
	F2Scan	   *s = scan->opaque;
	Relation	rel = scan->indexRelation;
	SiftOrdStream *os = MemoryContextAllocZero(s->cx, sizeof(SiftOrdStream));
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	int			nstaged;
	uint64	   *staged;

	if (!s->prepared)
		sift_f2_scan_prepare(scan, s);
	os->s = s;
	os->rel = rel;
	os->snap = scan->xs_snapshot != NULL ? scan->xs_snapshot : GetActiveSnapshot();
	os->f = sift_f2_rel(rel);
	os->schema = sift_col_rel(rel)->schema;
	os->fwd = fwd;
	os->last = -1;
	os->nkey = os->f->nkey;
	os->pcx = sift_mem_allocset_create(s->cx, "sift f2 ordered partition");
	for (int k = 0; k < os->nkey; k++)
	{
		os->key_leaf[k] = os->f->key_cols[k];
		os->desc[k] = (os->f->spec.sort_flags[k] & SIFT_KEY_DESC) != 0;
		os->nulls_first[k] = (os->f->spec.sort_flags[k] & SIFT_KEY_NULLS_FIRST) != 0;
		os->leaves[os->nleaves++] = os->key_leaf[k];
	}
	for (int j = 0; j < s->nret; j++)
	{
		bool		have = false;

		for (int i = 0; i < os->nleaves; i++)
			have |= os->leaves[i] == s->ret_leaf[j];
		if (!have)
			os->leaves[os->nleaves++] = s->ret_leaf[j];
	}
	os->itup_ok = true;
	for (int j = 0; j < s->nret; j++)
		os->itup_ok &= TupleDescAttr(s->itupdesc, s->ret_att[j] - 1)->attlen > 0;
	scan->xs_itupdesc = s->itupdesc;
	os->stride = offsetof(OrdRow, ret) + (Size) s->nret * sizeof(Datum);
	s->sink = ord_sink;
	s->sink_leaves = os->leaves;
	s->nsink_leaves = os->nleaves;
	s->sink_maybe = true;
	if (!s->possible)
	{
		os->ended = true;
		MemoryContextSwitchTo(old);
		return os;
	}
	if (!RecoveryInProgress() && sift_stage_has_pending(rel))
		sift_stage_flush(rel);
	staged = sift_stage_dead(rel, os->snap, &nstaged);
	os->staged_px = sift_f2_staged_index(staged, nstaged);
	if (!s->compiled)
		compile_preds(s, os->schema);
	os->rc = sift_rescan_begin(s, os->f, os->schema);
	os->plan = sift_plan_cache_get(rel, s->cx);
	os->plo = sift_f2_first_reaching(os->plan, s->keys.hull.has_lo ? s->keys.hull.lo : NULL);
	os->phi = sift_f2_last_overlapping(os->plan, os->plo, s->keys.hull.has_hi ? s->keys.hull.hi : NULL);
	os->part = fwd ? os->plo - 1 : os->phi + 1;
	MemoryContextSwitchTo(old);
	return os;
}

bool
sift_f2_ord_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	F2Scan	   *s = scan->opaque;
	SiftOrdStream *os;
	SiftErrCtx	ctx;
	bool		found;

	if (s->ord == NULL && !s->ord_off)
		s->ord_off = !sift_f2_native_order_matches(scan->indexRelation);
	if (s->ord_off || ScanDirectionIsNoMovement(dir))
		return sift_f2_amgettuple(scan, dir);
	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	if (s->ord == NULL)
		s->ord = ord_begin(scan, ScanDirectionIsForward(dir));
	os = s->ord;
	if (ScanDirectionIsForward(dir) != os->fwd)
		ord_flip(os, !os->fwd);
	found = ord_next(scan, os);
	sift_errctx_pop(&ctx);
	return found;
}

/* ---- mark and restore ---- */

void
sift_f2_ord_markpos(IndexScanDesc scan)
{
	SiftOrdStream *os = ((F2Scan *) scan->opaque)->ord;

	if (os == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a sift_col index of format major 2 marks the position of an ordered scan only"),
						errhint("Read the rows in the order of the index key, or through a bitmap scan.")));
	os->mark.pos = os->mark.pos == NULL ? MemoryContextAlloc(os->s->cx, Max(os->ncur, 1) * sizeof(OrdPos)) : repalloc(os->mark.pos, Max(os->ncur, 1) * sizeof(OrdPos));
	os->mark = (OrdMark) {true, os->fwd, os->ended, os->part, os->last, os->ncur, os->mark.pos};
	for (int i = 0; i < os->ncur; i++)
		os->mark.pos[i] = (OrdPos) {os->cur[i].loaded, os->cur[i].gi, os->cur[i].pos};
}

void
sift_f2_ord_restrpos(IndexScanDesc scan)
{
	SiftOrdStream *os = ((F2Scan *) scan->opaque)->ord;
	SiftErrCtx	ctx;

	SIFT_INVARIANT(os != NULL && os->mark.valid, "an ordered scan restores a position it marked");
	os->fwd = os->mark.fwd;
	os->ended = os->mark.ended;
	if (os->mark.part < os->plo || os->mark.part > os->phi)
	{
		os->part = os->mark.part;
		os->nheap = 0;
		os->last = -1;
		return;
	}
	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	if (os->part != os->mark.part || os->ncur == 0)
		part_cursors(os, os->mark.part);
	os->part = os->mark.part;
	SIFT_INVARIANT(os->ncur == os->mark.ncur, "the partition of a mark has the cursors it had");
	for (int i = 0; i < os->ncur; i++)
	{
		OrdCursor  *c = &os->cur[i];
		const OrdPos *p = &os->mark.pos[i];

		if (p->loaded && !(c->loaded && c->gi == p->gi))
			cur_load(os, c, p->gi);
		c->loaded = p->loaded;
		c->pos = p->pos;
	}
	os->last = os->mark.last;
	heap_prime(os);
	sift_errctx_pop(&ctx);
}
