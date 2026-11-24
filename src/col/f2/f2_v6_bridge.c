/*
 * col/f2/f2_v6_bridge.c - what the index of format major 2 gives the operators of SiftAggV6 (agg/v6/v6_vexpr.h): which indexes and leaves it holds, how its parts are sorted, and the source of batches it opens.
 *
 * A source reads the partitions in key order. Of a partition it reads the rows its slab holds and the granules of each of its parts the quals' bounds leave (the zones of the leaf tell; a part
 * is passed over whole by the time bounds of its descriptor), one granule at a time. Unsorted, the runs of a partition are read one after the other, in slices of SIFT_VEC_LEN rows; sorted, a partition of
 * more than one run is merged by the sort key of the index (and the address) one row at a time, so that the batches come in the order of the keys. A batch holds the rows no deletion
 * vector or pending delete removed, that the quals pass (they are tried before the heap is looked at) and that the heap shows to the snapshot; the tag of a row is its address.
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/f2/f2.h"
#include "col/f2/f2_marks.h"
#include "col/f2/f2_v6_bridge.h"
#include "col/f2/f2_v6_read.h"
#include "col/f2/f2_pred.h"
#include "col/f2/part_format.h"
#include "core/memtrack.h"
#include "core/popcount.h"
#include "core/sift.h"

/* ---- the format ---- */

static bool
plain_leaf(const SiftColDef *d)
{
	return d->lane == 0 && d->kind == SIFT_COLK_SCALAR && !(d->flags & (SIFT_COLD_LOSSY | SIFT_COLD_DERIVED)) && d->path[0] == '\0';
}

static bool
leaf_serves(const SiftColDef *d, SiftV6Class cls)
{
	switch (cls)
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			return d->store == SIFT_COLS_INT && d->lanes <= 1;
		case SV6C_F64:
			return d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8;
		case SV6C_TEXT:
			return d->store == SIFT_COLS_STR && d->type == SIFT_COLT_TEXT;
		default:
			return false;
	}
}

SiftF2OrderMatch
sift_f2_v6_order_match(Oid indexoid, int att, bool want_desc, bool want_nulls_first)
{
	Relation	ix = index_open(indexoid, AccessShareLock);
	const SiftColRel *r = sift_col_rel_private(ix, CurrentMemoryContext);
	const SiftF2Rel *f = sift_f2_rel(ix);
	SiftF2OrderMatch m = SF2_ORDER_NONE;

	if (!r->stale && f->nkey >= 1 && r->schema->cols[f->key_cols[0]].attno == att && plain_leaf(&r->schema->cols[f->key_cols[0]]))
		m = sift_f2_order_match(f->spec.sort_flags[0] & SIFT_KEY_DESC, f->spec.sort_flags[0] & SIFT_KEY_NULLS_FIRST, want_desc, want_nulls_first);
	index_close(ix, AccessShareLock);
	return m;
}

bool
sift_f2_v6_resolve_path(Oid indexoid, int att, const char *path, SiftV6Class cls, int *leaf_out)
{
	Relation	ix = index_open(indexoid, AccessShareLock);
	const SiftColRel *r = sift_col_rel_private(ix, CurrentMemoryContext);
	bool		found = false;

	for (int c = 0; !r->stale && !found && c < r->schema->ncols; c++)
	{
		const SiftColDef *d = &r->schema->cols[c];

		found = d->attno == att && d->lane == 0 && d->kind == SIFT_COLK_SCALAR && !(d->flags & (SIFT_COLD_LOSSY | SIFT_COLD_DERIVED | SIFT_COLD_ASTEXT)) &&
			strcmp(d->path, path) == 0 && leaf_serves(d, cls);
		if (found)
			*leaf_out = c;
	}
	index_close(ix, AccessShareLock);
	return found;
}

/* ---- the runs of a partition ---- */

typedef struct F2Run
{
	F2V6Part   *part;			/* NULL: the slab */
	const SiftPartitionEntry *e;
	MemoryContext cx;			/* the granule in cur, reset when the next is read */
	uint32		g;				/* the next granule to read */
	uint32		spans[SIFT_F2_MAX_RANGES][2];	/* the granules the key ranges of the quals can reach: [spans[i][0], spans[i][1]) */
	int			nspans;
	int			span;			/* the span of the next granule */
	F2V6Gran	cur;
	int			pos;
	bool		live;
	SiftZone  **zones;			/* per bound, the zones of its leaf over the granules; NULL: the part keeps none */
} F2Run;

struct F2Src
{
	SiftV6Source base;
	MemoryContext cx;
	MemoryContext batch_cx;		/* the strings a merged batch holds */
	F2V6Ctx    *c;
	SiftV6Input in;
	SiftV6Vexpr quals;
	bool		has_quals;
	bool		sorted;
	SiftVexprCtx *vctx;
	int			nleaves;
	int			leaves[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS + SIFT_KEY_ORDER_MAX];
	int			leaf[SIFT_V6_MAX_COLS];
	int			mleaf[SIFT_V6_MAX_MASKS];
	int			bound_leaf[SIFT_V6_MAX_BOUNDS];
	SiftF2TimeWin win;
	SiftF2KeySet keys;			/* the integers of the lead key the bounds of the quals leave */
	SiftKeyPlan keyplan;		/* and the ranges of the whole key they leave */
	bool		possible;		/* false: none */
	SiftV6SkipStats skips;
	int			pi;				/* the next partition */
	MemoryContext part_cx;		/* the runs of the current partition */
	F2Run	   *runs;			/* the slab and every part of the partition, in part_cx */
	int			nruns;
	int		   *heap;			/* the runs that have a row, the one whose row comes first at the top (merged sources) */
	int			nheap;
	bool		heap_ready;
	int			cur_run;		/* the run an unsorted source reads */
	/* the batch */
	SiftVec		vec[SIFT_V6_MAX_COLS];
	void	   *data[SIFT_V6_MAX_COLS];
	uint64	   *valid[SIFT_V6_MAX_COLS];
	uint64	   *nan[SIFT_V6_MAX_COLS];	/* of a numeric column: the rows that hold a NaN, which valid says are NULL */
	bool		has_nan[SIFT_V6_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_V6_MAX_MASKS];
	uint64	   *mbits[SIFT_V6_MAX_MASKS];
	uint64	   *mvalid[SIFT_V6_MAX_MASKS];
	uint64		live_mask[SIFT_VEC_WORDS];
	uint64		mask[SIFT_VEC_WORDS];
	uint64		tag[SIFT_VEC_LEN];
};

typedef struct F2Src F2Src;

/* The units the bounds of the quals on the lead key column leave: a partition or a part whose keys lie outside them holds no row that passes */
static void
key_set_init(F2Src *s)
{
	const SiftF2Rel *f = s->c->f;
	SiftColPred preds[2 * SIFT_V6_MAX_BOUNDS];
	SiftColValue vals[2 * SIFT_V6_MAX_BOUNDS] = {0};
	int			n = 0;

	for (int b = 0; f->nkey > 0 && b < s->quals.nbounds; b++)
	{
		const SiftV6Bound *bd = &s->quals.bound[b];

		if (s->bound_leaf[b] != f->key_cols[0])
			continue;
		if (bd->has_lo)
		{
			vals[n].v.i = bd->lo;
			preds[n] = (SiftColPred) {.col = s->bound_leaf[b],.op = SIFT_COLOP_GE,.nvals = 1,.vals = &vals[n]};
			n++;
		}
		if (bd->has_hi)
		{
			vals[n].v.i = bd->hi;
			preds[n] = (SiftColPred) {.col = s->bound_leaf[b],.op = SIFT_COLOP_LE,.nvals = 1,.vals = &vals[n]};
			n++;
		}
	}
	sift_f2_key_plan(f, s->c->schema->cols, preds, n, &s->keyplan);
	s->possible = sift_f2_key_set(f, preds, n, &s->keys) && s->keyplan.state != SIFT_KEY_PLAN_EMPTY;
}

bool
sift_f2_v6_eval_entry_meets_keys(const F2Eval *s, const SiftPartitionEntry *e)
{
	return s->possible && sift_manifest_overlaps(e->key_lo, e->key_hi, s->keys.hull.has_lo ? s->keys.hull.lo : NULL, s->keys.hull.has_hi ? s->keys.hull.hi : NULL);
}

bool
sift_f2_v6_eval_part_meets_keys(const F2Eval *s, const SiftPartDesc *d)
{
	return s->possible && sift_f2_part_overlaps(d, &s->keys);
}

SiftV6SkipStats *
sift_f2_v6_eval_skips(F2Eval *s)
{
	return &s->skips;
}

static size_t
elem_size(SiftVecType t)
{
	return t == SVEC_I32 ? sizeof(int32) : t == SVEC_BOOL ? sizeof(uint8) : t == SVEC_STRVIEW ? sizeof(SiftStrView) : sizeof(int64);
}

static void
leaves_of(F2Src *s, const SiftColRel *r, const SiftF2Rel *f)
{
	for (int i = 0; i < s->in.ncols; i++)
		s->leaves[s->nleaves++] = s->leaf[i] = r->first[s->in.col[i].attno];
	for (int m = 0; m < s->in.nmasks; m++)
	{
		const SiftV6InMask *im = &s->in.mask[m];

		s->mleaf[m] = im->key == NULL ? r->first[im->attno] : sift_f2_key_leaf(f, im->attno, im->key, strlen(im->key));
		s->leaves[s->nleaves++] = s->mleaf[m];
	}
	for (int k = 0; s->sorted && k < f->nkey; k++)
		s->leaves[s->nleaves++] = f->key_cols[k];
	for (int b = 0; b < s->quals.nbounds; b++)
		s->bound_leaf[b] = s->leaf[s->quals.bound[b].slot];
}

/* The window of the time columns the quals' bounds leave */
static void
time_window(F2Src *s)
{
	const SiftF2Rel *f = s->c->f;

	for (int b = 0; b < s->quals.nbounds; b++)
		for (int t = 0; t < SIFT_MANIFEST_TIME_COLS; t++)
		{
			const SiftV6Bound *bd = &s->quals.bound[b];

			if (f->spec.time_cols[t] != s->bound_leaf[b] + 1)
				continue;
			s->win.use[t] = true;
			s->win.lo[t] = bd->has_lo ? Max(s->win.lo[t], bd->lo) : s->win.lo[t];
			s->win.hi[t] = bd->has_hi ? Min(s->win.hi[t], bd->hi) : s->win.hi[t];
		}
}

/* ---- granules ---- */

/* The zones of the leaves of the quals' bounds (zones[b] for bound b, NULL: none kept) say no row of a granule of nrows rows can pass them */
bool
sift_f2_v6_ruled_out(const F2Eval *s, SiftZone *const *zones, uint32 g, uint32 nrows)
{
	for (int b = 0; b < s->quals.nbounds; b++)
	{
		const SiftV6Bound *bd = &s->quals.bound[b];
		const SiftZone *z = zones[b] != NULL ? &zones[b][g] : NULL;

		if (z == NULL || (z->flags & SIFT_ZONE_NONE))
			continue;
		if ((bd->has_lo && z->max < bd->lo) || (bd->has_hi && z->min > bd->hi) || z->nnull >= nrows)
			return true;
	}
	return false;
}

static bool
run_next_granule(F2Src *s, F2Run *r)
{
	MemoryContextReset(r->cx);
	r->pos = 0;
	if (r->part == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(r->cx);

		if (r->g++ > 0)
		{
			MemoryContextSwitchTo(old);
			return r->live = false;
		}
		f2v6_slab(s->c, r->e, NULL, s->sorted, s->leaves, s->nleaves, &r->cur);
		MemoryContextSwitchTo(old);
		return r->live = r->cur.n > 0;
	}
	while (r->span < r->nspans)
	{
		uint32		g;
		MemoryContext old;

		r->g = Max(r->g, r->spans[r->span][0]);
		if (r->g >= r->spans[r->span][1])
		{
			r->span++;
			continue;
		}
		g = r->g++;
		if (sift_f2_v6_ruled_out(s, r->zones, g, r->part->dir[g].nrows))
		{
			s->skips.granules++;
			continue;
		}
		old = MemoryContextSwitchTo(r->cx);
		f2v6_granule(s->c, r->part, g, s->leaves, s->nleaves, &r->cur);
		MemoryContextSwitchTo(old);
		return r->live = true;
	}
	return r->live = false;
}

static F2Run *
run_add(F2Src *s, const SiftPartitionEntry *e, F2V6Part *part, const SiftPendDelIndex *px)
{
	F2Run	   *r = &s->runs[s->nruns++];

	*r = (F2Run) {.part = part,.e = e,.cx = sift_mem_allocset_create(s->part_cx, "sift f2 run")};
	if (part != NULL)
	{
		r->zones = MemoryContextAllocZero(s->part_cx, Max(s->quals.nbounds, 1) * sizeof(SiftZone *));
		for (int b = 0; b < s->quals.nbounds; b++)
			r->zones[b] = f2v6_zones(s->c, part, s->bound_leaf[b]);
		r->nspans = sift_f2_granule_spans(s->c->index, part->part, s->c->f, &s->keys, &s->keyplan, r->spans);
	}
	return r;
}

/* The runs of the next partition the bounds leave; false when there is none */
static bool
partition_open(F2Src *s)
{
	const SiftPlanSnapshot *plan = s->c->plan;

	while (s->pi < plan->npartitions)
	{
		const SiftPartitionEntry *e = &plan->partitions[s->pi];
		const SiftPlanPartList *list = &plan->partlists[s->pi++];
		const SiftPendDelIndex *px;
		MemoryContext old;

		if (!sift_f2_v6_eval_entry_meets_keys(s, e) || (e->l0_slab_blk == InvalidBlockNumber && !f2v6_entry_meets_time(s->c, e, &s->win)))
		{
			s->skips.partitions++;
			continue;
		}
		MemoryContextReset(s->part_cx);
		s->nruns = 0;
		s->heap_ready = false;
		old = MemoryContextSwitchTo(s->part_cx);
		s->runs = palloc((list->nparts + 1) * sizeof(F2Run));
		s->heap = palloc((list->nparts + 1) * sizeof(int));
		px = f2v6_pending(s->c, e);
		if (e->l0_slab_blk != InvalidBlockNumber)
			run_add(s, e, NULL, px);
		for (int j = 0; j < list->nparts; j++)
			if (f2v6_part_meets_time(s->c, e, &list->parts[j], &s->win) && sift_f2_v6_eval_part_meets_keys(s, &list->parts[j]))
				run_add(s, e, f2v6_part_open(s->c, e, &list->parts[j], px), px);
			else
				s->skips.parts++;
		MemoryContextSwitchTo(old);
		s->cur_run = 0;
		for (int i = 0; i < s->nruns; i++)
			run_next_granule(s, &s->runs[i]);
		if (s->nruns > 0)
			return true;
	}
	return false;
}

/* ---- the batch ---- */

static SiftStrView
strview_of(const char *p, uint32 len)
{
	SiftStrView v = {.len = len};

	if (len == 0)
		return v;
	memcpy(v.prefix, p, Min(len, 4));
	if (len <= 12)
		memcpy(v.u.tail, p + 4, len > 4 ? len - 4 : 0);
	else
		v.u.ptr = p;
	return v;
}

static inline void
set_bit(uint64 *w, int k, bool v)
{
	w[k >> 6] |= (uint64) v << (k & 63);
}

/* Rows r0 .. r0 + n of granule g into batch rows k0 ..; own_strs: the strings go to memory of the batch (the granule is not there when the batch is given) */
static void
fill_rows(F2Src *s, const F2V6Gran *g, int r0, int n, int k0, bool own_strs)
{
	for (int i = 0; i < s->in.ncols; i++)
	{
		const ColView *v = &g->views[s->leaf[i]];
		const SiftV6InCol *ic = &s->in.col[i];

		for (int j = 0; j < n; j++)
		{
			const int	r = r0 + j,
						k = k0 + j;
			const bool	ok = !v->isnull[r];

			set_bit(s->valid[i], k, ok);
			if (!ok && ic->typid == NUMERICOID && v->tag != NULL && v->tag[r] == SIFT_COLTAG_UNSURE)
			{
				set_bit(s->nan[i], k, true);
				s->has_nan[i] = true;
			}
			switch (ic->type)
			{
				case SVEC_I32:
					((int32 *) s->data[i])[k] = ok ? (int32) v->i[r] : 0;
					break;
				case SVEC_BOOL:
					((uint8 *) s->data[i])[k] = ok && v->i[r] != 0;
					break;
				case SVEC_F64:
					((double *) s->data[i])[k] = ok ? v->f[r] : 0;
					break;
				case SVEC_STRVIEW:
					{
						const char *p = ok ? v->s[r] : NULL;

						if (ok && own_strs && v->slen[r] > 12)
							p = memcpy(MemoryContextAlloc(s->batch_cx, v->slen[r]), p, v->slen[r]);
						((SiftStrView *) s->data[i])[k] = strview_of(p, ok ? v->slen[r] : 0);
					}
					break;
				default:
					((int64 *) s->data[i])[k] = ok ? v->i[r] : 0;
			}
		}
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		const ColView *v = &g->views[s->mleaf[m]];

		for (int j = 0; j < n; j++)
		{
			const bool	key = s->in.mask[m].key != NULL;

			set_bit(s->mvalid[m], k0 + j, key || !v->isnull[r0 + j]);
			set_bit(s->mbits[m], k0 + j, key ? v->keyp[r0 + j] : v->i[r0 + j] != 0);
		}
	}
	for (int j = 0; j < n; j++)
		s->tag[k0 + j] = g->tid[r0 + j];
}

static void
batch_reset(F2Src *s)
{
	MemoryContextReset(s->batch_cx);
	for (int i = 0; i < s->in.ncols; i++)
	{
		memset(s->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->nan[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		s->has_nan[i] = false;
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		memset(s->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->mvalid[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	}
}

static void
cover_zone(F2Src *s, int n)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	for (int i = 0; s->quals.zone != NULL && i < s->in.ncols; i++)
		if (s->in.col[i].typid == TIMESTAMPTZOID)
			for (int k = 0; k < n; k++)
				if ((s->valid[i][k >> 6] >> (k & 63)) & 1)
				{
					lo = Min(lo, ((int64 *) s->data[i])[k]);
					hi = Max(hi, ((int64 *) s->data[i])[k]);
				}
	if (lo <= hi)
		zone_cover(s->quals.zone, lo, hi);
}

/* The batch of n rows with the rows of live_mask: the quals first, then the heap; false when no row is left */
static bool
batch_finish(F2Src *s, int n, SiftV6Batch *out)
{
	uint16		left;

	for (int i = 0; i < s->in.ncols; i++)
	{
		const SiftV6InCol *ic = &s->in.col[i];

		s->vec[i] = (SiftVec) {.data = s->data[i],.valid = s->valid[i],.nan = s->has_nan[i] ? s->nan[i] : NULL,.len = (uint16) n,.all_valid = false,.dense = true,.type = ic->type,.scale = ic->scale};
	}
	for (int m = 0; m < s->in.nmasks; m++)
		s->masks[m] = (SiftVexprMaskIn) {.bits = s->mbits[m],.valid = s->mvalid[m]};
	*out = (SiftV6Batch) {.n = (uint16) n,.col = s->vec,.masks = s->masks,.mask = s->live_mask,.tag = s->tag};
	cover_zone(s, n);
	left = sift_v6_vexpr_filter(s->vctx, s->has_quals ? &s->quals : NULL, out, s->mask);
	for (int w = 0; left > 0 && w < (n + 63) / 64; w++)
		for (uint64 bits = s->mask[w]; bits != 0; bits &= bits - 1)
		{
			const int	k = w * 64 + __builtin_ctzll(bits);

			if (!f2v6_row_visible(s->c, s->tag[k]))
			{
				s->mask[w] &= ~(UINT64CONST(1) << (k & 63));
				left--;
			}
		}
	out->mask = s->mask;
	return left > 0;
}

/* ---- unsorted: the runs one after the other, a granule in slices ---- */

static int
slice_next(F2Src *s)
{
	for (;;)
	{
		F2Run	   *r;
		int			n;

		if (s->cur_run >= s->nruns)
		{
			s->nruns = 0;
			return 0;
		}
		r = &s->runs[s->cur_run];
		if (!r->live || r->pos >= r->cur.n)
		{
			if (!r->live || !run_next_granule(s, r))
				s->cur_run++;
			continue;
		}
		n = Min(SIFT_VEC_LEN, r->cur.n - r->pos);
		batch_reset(s);
		fill_rows(s, &r->cur, r->pos, n, 0, false);
		memset(s->live_mask, 0, sizeof(s->live_mask));
		memcpy(s->live_mask, r->cur.live + r->pos / 64, (n + 63) / 64 * sizeof(uint64));
		if (n & 63)
			s->live_mask[n / 64] &= (UINT64CONST(1) << (n & 63)) - 1;
		r->pos += n;
		return n;
	}
}

/* ---- sorted: the runs of a partition merged by the sort key of the index and the address ---- */

static int
cmp_cell(const ColView *a, int ra, const ColView *b, int rb)
{
	const bool	na = a->isnull[ra],
				nb = b->isnull[rb];

	if (na || nb)
		return na == nb ? 0 : na ? 1 : -1;
	if (a->kind == SIFT_CELL_INT)
		return a->i[ra] < b->i[rb] ? -1 : a->i[ra] > b->i[rb];
	if (a->kind == SIFT_CELL_F8)
		return sift_f2_f8_cmp(a->f[ra], b->f[rb]);
	{
		const int	c = memcmp(a->s[ra], b->s[rb], Min(a->slen[ra], b->slen[rb]));

		return c != 0 ? c : a->slen[ra] < b->slen[rb] ? -1 : a->slen[ra] > b->slen[rb];
	}
}

static int
cmp_rows(const F2Src *s, const F2Run *a, const F2Run *b)
{
	const SiftF2Rel *f = s->c->f;

	for (int k = 0; k < f->nkey; k++)
	{
		const int	c = cmp_cell(&a->cur.views[f->key_cols[k]], a->pos, &b->cur.views[f->key_cols[k]], b->pos);

		if (c != 0)
			return c;
	}
	return a->cur.tid[a->pos] < b->cur.tid[b->pos] ? -1 : a->cur.tid[a->pos] > b->cur.tid[b->pos];
}

static bool
run_live_row(F2Src *s, F2Run *r)
{
	while (r->live && r->pos < r->cur.n)
	{
		if ((r->cur.live[r->pos >> 6] >> (r->pos & 63)) & 1)
			return true;
		r->pos++;
	}
	return r->live && run_next_granule(s, r) && run_live_row(s, r);
}

/* The run whose row comes first; runs of equal rows (an address is one row) by their place */
static bool
heap_before(const F2Src *s, int a, int b)
{
	const int	c = cmp_rows(s, &s->runs[a], &s->runs[b]);

	return c < 0 || (c == 0 && a < b);
}

static void
heap_down(F2Src *s, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1,
					m = i,
					t;

		if (l < s->nheap && heap_before(s, s->heap[l], s->heap[m]))
			m = l;
		if (l + 1 < s->nheap && heap_before(s, s->heap[l + 1], s->heap[m]))
			m = l + 1;
		if (m == i)
			return;
		t = s->heap[i];
		s->heap[i] = s->heap[m];
		s->heap[m] = t;
		i = m;
	}
}

static void
heap_build(F2Src *s)
{
	s->nheap = 0;
	for (int i = 0; i < s->nruns; i++)
		if (run_live_row(s, &s->runs[i]))
			s->heap[s->nheap++] = i;
	for (int i = s->nheap / 2 - 1; i >= 0; i--)
		heap_down(s, i);
	s->heap_ready = true;
}

/* The rows of the runs of the partition in order, a batch of at most SIFT_VEC_LEN; fewer when the partition ends */
static int
merge_next(F2Src *s)
{
	int			n = 0;

	batch_reset(s);
	memset(s->live_mask, 0, sizeof(s->live_mask));
	if (!s->heap_ready)
		heap_build(s);
	while (n < SIFT_VEC_LEN && s->nheap > 0)
	{
		F2Run	   *best = &s->runs[s->heap[0]];

		fill_rows(s, &best->cur, best->pos++, 1, n, true);
		set_bit(s->live_mask, n++, true);
		if (!run_live_row(s, best))
			s->heap[0] = s->heap[--s->nheap];
		heap_down(s, 0);
	}
	if (s->nheap == 0)
	{
		s->nruns = 0;
		s->heap_ready = false;
	}
	return n;
}

static bool
src_next(SiftV6Source *base, SiftV6Batch *out)
{
	F2Src	   *s = (F2Src *) base;

	for (;;)
	{
		int			n;

		if (s->nruns == 0 && !partition_open(s))
			return false;
		n = s->sorted && s->nruns > 1 ? merge_next(s) : slice_next(s);
		if (n > 0 && batch_finish(s, n, out))
			return true;
	}
}

static bool
src_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	return f2v6_fetch(((F2Src *) base)->c, tag, attnos, n, values, isnull);
}

static void
src_close(SiftV6Source *base)
{
	F2Src	   *s = (F2Src *) base;

	f2v6_end(s->c);
	MemoryContextDelete(s->cx);
}

F2Eval *
sift_f2_v6_eval_new(MemoryContext cx, F2V6Ctx *c, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	F2Src	   *s = palloc0(sizeof(F2Src));

	s->cx = cx;
	s->batch_cx = sift_mem_allocset_create(cx, "sift f2 v6 batch");
	s->part_cx = sift_mem_allocset_create(cx, "sift f2 v6 partition");
	s->in = *in;
	s->sorted = sorted;
	if (quals != NULL)
	{
		s->quals = *quals;
		s->has_quals = true;
	}
	s->c = c;
	s->vctx = sift_vexpr_ctx_new(cx);
	leaves_of(s, c->rel, c->f);
	for (int t = 0; t < SIFT_MANIFEST_TIME_COLS; t++)
	{
		s->win.lo[t] = INT64_MIN;
		s->win.hi[t] = INT64_MAX;
	}
	time_window(s);
	key_set_init(s);
	for (int i = 0; i < in->ncols; i++)
	{
		s->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * elem_size(in->col[i].type), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->nan[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	}
	for (int m = 0; m < in->nmasks; m++)
	{
		s->mbits[m] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->mvalid[m] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	}
	MemoryContextSwitchTo(old);
	return s;
}

const int *
sift_f2_v6_eval_leaves(const F2Eval *s, int *n)
{
	*n = s->nleaves;
	return s->leaves;
}

const SiftF2TimeWin *
sift_f2_v6_eval_window(const F2Eval *s)
{
	return &s->win;
}

int
sift_f2_v6_eval_bound_leaf(const F2Eval *s, int b)
{
	return s->bound_leaf[b];
}

int
sift_f2_v6_eval_nbounds(const F2Eval *s)
{
	return s->quals.nbounds;
}

/* The rows of the granule g that cand has (n rows, a bit each) which the quals pass and the heap shows, into pass (the words of cand's size) */
uint32
sift_f2_v6_eval_rows(F2Eval *s, const F2V6Gran *g, const uint64 *cand, uint64 *pass)
{
	uint32		total = 0;
	SiftV6Batch b;

	memset(pass, 0, (g->n + 63) / 64 * sizeof(uint64));
	for (int r0 = 0; r0 < g->n; r0 += SIFT_VEC_LEN)
	{
		const int	n = Min(SIFT_VEC_LEN, g->n - r0),
					words = (n + 63) / 64;

		batch_reset(s);
		fill_rows(s, g, r0, n, 0, false);
		memset(s->live_mask, 0, sizeof(s->live_mask));
		memcpy(s->live_mask, cand + r0 / 64, words * sizeof(uint64));
		if (n & 63)
			s->live_mask[n / 64] &= (UINT64CONST(1) << (n & 63)) - 1;
		if (batch_finish(s, n, &b))
		{
			memcpy(pass + r0 / 64, s->mask, words * sizeof(uint64));
			for (int w = 0; w < words; w++)
				total += pg_popcount64(s->mask[w]);
		}
	}
	return total;
}

SiftV6Source *
sift_f2_v6_index_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift f2 v6 source");
	F2Src	   *s = sift_f2_v6_eval_new(cx, f2v6_begin(cx, estate->es_snapshot, in->indexoid), in, quals, sorted);

	s->base = (SiftV6Source) {.next = src_next,.fetch = src_fetch,.close = src_close};
	return &s->base;
}
