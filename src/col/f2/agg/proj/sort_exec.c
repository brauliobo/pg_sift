/*
 * col/f2/agg/proj/sort_exec.c - the rows of a statement from a SIFT_PROJ_SORT projection (col/f2/agg/proj/sort_exec.h).
 */
#include "postgres.h"

#include "col/f2/agg/proj/ctx.h"
#include "col/f2/agg/proj/merge.h"
#include "col/f2/agg/proj/seek.h"
#include "col/f2/agg/proj/sort_exec.h"
#include "col/f2/agg/proj/in_seek.h"
#include "col/am.h"
#include "col/f2/proj/overlay.h"

/* What has died in a part since its projection part was written: the addresses the deletion vector has dead and the pending deletes that may name a row of it, and the heap blocks whose rows the heap may have changed */
typedef struct PartDead
{
	SiftPendDelIndex *dv_dead;
	const SiftPendDelIndex *pend;
	SiftProjDirty dirty;
} PartDead;

/* A projection part read in its own order, the rows the predicates accept, as the cells of the columns of the relation */
typedef struct PartStream
{
	const PartDead *dead;
	bool		vet_all;
	SiftProjCursor *cur;
	const SiftProjPartSpec *pps;
	const SiftProjKey *first;
	const SiftProjPred *preds;
	int			npreds;
	int			ncols;
	SiftCell   *row;
	SiftV6SortVetFn vet;
	void	   *vet_arg;
	uint32		skipped;
	uint32		read;
} PartStream;

/* Every row is vetted when the statement has quals no cell can judge; else only the rows whose cells cannot judge the predicates */
static bool
needs_vet(bool vet_all, const SiftProjPred *preds, int npreds, const SiftCell *row)
{
	return vet_all || !sift_proj_preds_decidable(preds, npreds, row);
}

static void
stream_close(PartStream *p)
{
	if (p->cur == NULL)
		return;
	sift_proj_cursor_stats(p->cur, &p->skipped, &p->read);
	sift_proj_cursor_close(p->cur);
	p->cur = NULL;
}

static bool
part_next(void *arg, uint64 *tid, const SiftCell **row)
{
	PartStream *p = arg;
	const SiftRowBuf *rows;
	SiftProjRowVerdict v;
	int			r;

	while (p->cur != NULL && sift_proj_cursor_next(p->cur, &rows, &r))
	{
		for (int c = 0; c < p->ncols; c++)
			p->row[c] = (SiftCell) {.isnull = true};
		for (int i = 0; i < p->pps->nlay; i++)
			p->row[p->pps->lay[i].leaf] = sift_rowbuf_cell(rows, i, r);
		v = sift_proj_row_verdict(p->first, p->preds, p->npreds, p->row);
		if (v == SRV_END)
			break;
		if (sift_proj_sort_row_dead(p->dead->dv_dead, p->dead->pend, rows->tid[r]))
			continue;
		if (v == SRV_TAKE && (!(needs_vet(p->vet_all, p->preds, p->npreds, p->row) || sift_proj_dirty_has(&p->dead->dirty, (BlockNumber) (rows->tid[r] >> 16))) ||
							  p->vet(p->vet_arg, rows->tid[r])))
		{
			*tid = rows->tid[r];
			*row = p->row;
			return true;
		}
	}
	stream_close(p);
	return false;
}

struct SiftV6SortScan
{
	SiftV6ProjCtx ctx;
	const SiftV6ProjSource *src;
	const SiftProjPred *preds;
	int			npreds;
	SiftProjPartSpec *pps;
	PartStream *streams;
	int			nstreams;
	SiftProjTopK *rest;
	SiftProjMerge *merge;
	SiftV6SortVetFn vet;
	void	   *vet_arg;
	bool		vet_all;
	int			nparts;
	int			nbase;
	int64		nrest_rows;
	int			nseeked;
	bool	   *want;			/* of each column of a projection part: the statement, its predicates or the merge read it */
};

static void
rest_row(void *arg, uint64 tid, const SiftCell *row)
{
	SiftV6SortScan *s = arg;

	if (needs_vet(s->vet_all, s->preds, s->npreds, row) && !s->vet(s->vet_arg, tid))
		return;
	sift_proj_topk_add(s->rest, tid, row);
	s->nrest_rows++;
}

/* The leaves the rest of the relation is read for: what the statement reads, what its predicates test and the sort columns the merge orders by */
static int
rest_leaves(const SiftV6ProjQuery *q, const SiftProjSpec *spec, int *leaves)
{
	int			n = 0;
	const int	nkeys = q->norder > 0 ? spec->nkeys : Min(spec->nkeys, 1);

	for (int i = 0; i < q->nread + nkeys; i++)
	{
		int			leaf = i < q->nread ? q->read[i] : spec->key[i - q->nread].leaf;
		bool		seen = false;

		for (int j = 0; j < n; j++)
			seen |= leaves[j] == leaf;
		if (!seen)
			leaves[n++] = leaf;
	}
	return n;
}

/* The statement's own query with its predicates, and the columns they test among the columns it reads */
static SiftV6ProjQuery
query_with_preds(const SiftV6ProjHost *h, const SiftV6ProjQuery *rq)
{
	SiftV6ProjQuery q = *rq;

	q.npred = 0;
	for (int k = 0; k < h->npreds && q.npred < SV6P_MAX; k++)
	{
		bool		read = false;

		q.pred[q.npred++] = sift_v6_proj_query_pred(&h->preds[k]);
		for (int i = 0; i < q.nread; i++)
			read |= q.read[i] == h->preds[k].leaf;
		if (!read && q.nread < lengthof(q.read))
			q.read[q.nread++] = h->preds[k].leaf;
	}
	return q;
}

static PartDead *
part_dead(const SiftV6SortScan *s, int i, const SiftProjLink *link)
{
	const SiftV6BaseParts *b = &s->ctx.base;
	const SiftPendDelIndex *px = b->seg_pdel[b->part_seg[i]];
	PartDead   *d = palloc0(sizeof(PartDead));

	if (s->src->serve[i].serve != SPS_OVERLAY)
		return d;
	d->dirty = (SiftProjDirty) {.blk = b->dirty_blk + b->dirty_off[i],.n = (int) (b->dirty_off[i + 1] - b->dirty_off[i])};
	d->pend = sift_proj_part_meets_pend(&b->part[i], px) ? px : NULL;
	d->dv_dead = sift_proj_dv_unchanged(&b->part[i], link) ? NULL : sift_v6_host_dv_dead(s->ctx.h, b, i);
	return d;
}

/* The integers the predicates on the first sort column leave (a superset: an open bound is the end of the integers); false when that column is not an integer or no predicate bounds it */
static bool
first_key_bounds(const SiftProjPartSpec *pps, const SiftProjSpec *spec, const SiftProjPred *preds, int n, int64 *lo, int64 *hi)
{
	bool		bound = false;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	if (pps->kinds[0] != SIFT_CELL_INT || spec->key[0].fn != SPF_COLUMN)
		return false;
	for (int i = 0; i < n; i++)
	{
		const SiftProjPred *p = &preds[i];

		if (p->leaf != spec->key[0].leaf || p->bytes || p->nvals < 1)
			continue;
		switch ((SiftProjOp) p->op)
		{
			case SPO_EQ:
			case SPO_IN:
				for (int v = 0; v < p->nvals; v++)
				{
					*lo = bound ? Min(*lo, p->vals[v].i) : p->vals[v].i;
					*hi = bound ? Max(*hi, p->vals[v].i) : p->vals[v].i;
					bound = true;
				}
				break;
			case SPO_GT:
			case SPO_GE:
				*lo = Max(*lo, p->vals[0].i);
				bound = true;
				break;
			case SPO_LT:
			case SPO_LE:
				*hi = Min(*hi, p->vals[0].i);
				bound = true;
				break;
			default:
				break;
		}
	}
	return bound;
}

/* The stream of part i of the projection for the predicates given: sought by its marks when they bound the sort columns */
static void
open_stream(SiftV6SortScan *s, int i, const PartDead *dead, const SiftProjPred *preds)
{
	const SiftV6ProjHost *h = s->ctx.h;
	const SiftProjSpec *spec = &s->src->def->spec;
	SiftV6ProjSeek seek;
	PartStream *p = &s->streams[s->nstreams++];
	int64		lo,
				hi;

	*p = (PartStream) {.cur = sift_proj_cursor_open(h->rel, sift_v6_proj_part_of(s->src, s->src->serve[i].proj_gen), SIFT_PROJ_PART_ID, &s->pps->spec, false),
		.pps = s->pps,.first = &spec->key[0],.preds = preds,.npreds = h->npreds,.ncols = h->store->ncols,.row = palloc(h->store->ncols * sizeof(SiftCell)),
		.vet = s->vet,.vet_arg = s->vet_arg,.vet_all = s->vet_all,.dead = dead};
	s->nseeked += sift_v6_proj_seek_plan(s->pps, spec, preds, h->npreds, &seek) && sift_proj_cursor_seek(p->cur, &seek.ks, &seek.plan);
	sift_proj_cursor_columns(p->cur, s->want);
	sift_proj_cursor_progressive(p->cur);
	if (first_key_bounds(s->pps, spec, preds, h->npreds, &lo, &hi))
		sift_proj_cursor_window(p->cur, lo, hi);
	sift_proj_merge_add(s->merge, (SiftProjStream) {part_next, p});
	s->nparts++;
}

/* One stream of each served part for each value of the statement's list, or one stream of each part when it has none */
static void
open_part_streams(SiftV6SortScan *s, int i, const SiftV6ProjQuery *q, const SiftProjSpec *ospec)
{
	const SiftV6ProjHost *h = s->ctx.h;
	const SiftProjSpec *spec = &s->src->def->spec;
	SiftV6SortUse use = sift_v6_sort_use(spec, q);
	int			in_idx = sift_v6_in_seek_pred_index(spec, &use, h->preds, h->npreds);
	PartDead   *dead = part_dead(s, i, sift_proj_link_of(&s->src->parts, s->ctx.base.part[i].part_gen));
	SiftProjConst vals[SIFT_PROJ_IN_SEEK_MAX];
	int			nvals = in_idx >= 0 ? sift_v6_in_seek_values(&h->preds[in_idx], vals) : 0;

	if (in_idx < 0)
		return open_stream(s, i, dead, h->preds);
	for (int v = 0; v < nvals; v++)
	{
		SiftProjPred *pv = palloc(h->npreds * sizeof(SiftProjPred));
		SiftProjConst *one = palloc(sizeof(SiftProjConst));

		sift_v6_in_seek_preds(h->preds, h->npreds, in_idx, &vals[v], pv, one);
		open_stream(s, i, dead, pv);
	}
}

/* false: the rest of the relation could not be read as cells */
static bool
open_streams(SiftV6SortScan *s, const SiftV6ProjQuery *q)
{
	const SiftV6ProjHost *h = s->ctx.h;
	const SiftProjSpec *spec = &s->src->def->spec;
	SiftV6SortUse use = sift_v6_sort_use(spec, q);
	SiftProjSpec ospec = use.in_merge ? sift_v6_in_seek_order(q) : *spec;
	int			leaves[2 * SIFT_PROJ_MAX_COLS + SV6P_MAX];
	int			nleaves = rest_leaves(q, spec, leaves),
				nskip;
	uint64	   *skip = sift_v6_proj_served_gens(&s->ctx, s->src, &nskip);

	s->pps = palloc(sizeof(SiftProjPartSpec));
	sift_proj_part_spec(h->store, spec, &s->ctx.cols, s->pps);
	s->streams = palloc(Max(nskip, 1) * (use.in_merge ? SIFT_PROJ_IN_SEEK_MAX : 1) * sizeof(PartStream));
	s->want = palloc0(Max(s->pps->nlay, 1) * sizeof(bool));
	for (int i = 0; i < s->pps->nlay; i++)
		for (int j = 0; j < nleaves; j++)
			s->want[i] |= s->pps->lay[i].leaf == leaves[j];
	s->rest = sift_proj_topk_new(&ospec, &s->ctx.cols, h->store->ncols, q->limit);
	s->merge = sift_proj_merge_new(&ospec, &s->ctx.cols, q->limit);
	for (int i = 0; i < s->ctx.base.n; i++)
		if (s->src->serve[i].serve != SPS_BASE)
			open_part_streams(s, i, q, &ospec);
	s->nbase = s->ctx.base.n - nskip;
	if (!sift_v6_host_scan_rest(h, s->ctx.plan, skip, nskip, leaves, nleaves, rest_row, s))
		return false;
	sift_proj_merge_add(s->merge, sift_proj_topk_stream(s->rest));
	return true;
}

SiftV6SortScan *
sift_v6_proj_sort_begin(const SiftV6ProjHost *h, const SiftV6ProjQuery *rq, SiftV6SortVetFn vet, void *vet_arg, bool vet_all, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftV6SortScan *s = palloc0(sizeof(SiftV6SortScan));
	SiftV6ProjQuery q = query_with_preds(h, rq);
	SiftV6ProjChoice ch;

	if (!q.wants_rows || !sift_v6_proj_ctx_open(&s->ctx, h, SIFT_PROJ_SORT))
		goto none;
	ch = sift_v6_proj_ctx_choose(&s->ctx, &q);
	if (ch.cand < 0)
		goto none;
	s->src = &s->ctx.src[ch.cand];
	if (q.norder > 0 && !sift_v6_sort_use(&s->src->def->spec, &q).order)
		goto none_open;
	s->preds = h->preds;
	s->npreds = h->npreds;
	s->vet = vet;
	s->vet_arg = vet_arg;
	s->vet_all = vet_all;
	if (open_streams(s, &q))
	{
		MemoryContextSwitchTo(old);
		return s;
	}
none_open:
	sift_v6_proj_sort_end(s);
none:
	MemoryContextSwitchTo(old);
	return NULL;
}

bool
sift_v6_proj_sort_next(SiftV6SortScan *s, uint64 *tid, const SiftCell **row)
{
	return sift_proj_merge_next(s->merge, tid, row);
}

const char *
sift_v6_proj_sort_name(const SiftV6SortScan *s)
{
	return s->src->ref->name;
}

SiftV6SortStats
sift_v6_proj_sort_stats(const SiftV6SortScan *s)
{
	SiftV6SortStats st = {.nparts = s->nparts,.nbase = s->nbase,.nrest_rows = s->nrest_rows,.nseeked = s->nseeked};

	for (int i = 0; i < s->nparts; i++)
	{
		const PartStream *p = &s->streams[i];
		uint32		skipped = p->skipped,
					read = p->read;

		if (p->cur != NULL)
			sift_proj_cursor_stats(p->cur, &skipped, &read);
		st.granules_skipped += skipped;
		st.granules_read += read;
	}
	return st;
}

void
sift_v6_proj_sort_end(SiftV6SortScan *s)
{
	for (int i = 0; i < s->nparts; i++)
		stream_close(&s->streams[i]);
	if (s->merge != NULL)
		sift_proj_merge_free(s->merge);
	if (s->rest != NULL)
		sift_proj_topk_free(s->rest);
	pfree(s);
}
