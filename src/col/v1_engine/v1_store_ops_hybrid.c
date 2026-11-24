/*
 * col/v1_engine/v1_store_ops_hybrid.c - SiftHybridOps of Format 1: the scan of the store a BM25 text scan joins, the survey of its granules, whether the predicates of a sift_table leave a scan
 * to do, and the fold of the rows of a scan into the groups of an aggregate. The store is the unit reader's source (col/v1/v1_store_ops.c): it decides visibility and the quals it compiles
 * itself, so a batch is exact (SiftHybridBatch.exact_vis) and only the rows of the quals it cannot compile, or of the predicates the planner added (ext_preds), are the heap's to decide (maybe).
 * The columns of a batch are ColViews by leaf of the layout of the relation, as the host reads them; the fold is the aggregate scan of col/source_fold.h given those rows.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "executor/executor.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/v6/v6_cost_model.h"
#include "agg/hybrid.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/vexpr_str.h"
#include "col/source_fold.h"
#include "col/v1/v1_reader.h"
#include "col/v1/v1_store_ops.h"
#include "core/memtrack.h"
#include "core/tidbridge.h"
#include "table/v1/v1_tbl_ops.h"

/* what the host asks of one leaf: the slot of the column in the input of the source, and what the leaf holds */
typedef struct HybCol
{
	int			leaf;
	int			slot;			/* of the input of the source, -1: the format holds no column of the attribute in one leaf of its class, the heap's row gives it */
	SiftV6Class cls;
} HybCol;

typedef struct HybScan
{
	const SiftHybridScan *s;
	SiftV6Input in;
	HybCol		col[SIFT_V6_MAX_COLS];
	int			ncols;
	int			nviews;
	SiftV6Vexpr quals;
	bool		source_quals;
	bool		heap_decides;	/* a predicate the source does not decide: every row it keeps is the heap's to judge */
} HybScan;

static const SiftV1ReaderOps *
reader_of(const SiftHybridScan *s)
{
	return sift_v1_reader_of(s->index == NULL);
}

static Relation
host_of(const SiftHybridScan *s)
{
	return s->index != NULL ? s->index : s->heap;
}

static SiftVecType
vec_type_of(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

static bool
leaf_served(const SiftHybridScan *s, int att, SiftV6Class cls)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(RelationGetRelid(host_of(s)), s->index == NULL);

	return cls != SV6C_NONE && ops->leaf_holds(RelationGetRelid(host_of(s)), s->index == NULL, att, cls);
}

/* the input of the source: the columns of the leaves the host reads, then those the quals add */
static void
scan_columns(HybScan *h, const SiftSrcLayout *lay)
{
	const SiftHybridScan *s = h->s;

	if (s->index == NULL)
		sift_v6_input_init_table(&h->in, s->heap);
	else
		sift_v6_input_init_rel(&h->in, s->index);
	for (int i = 0; i < s->nleaves; i++)
	{
		const int	leaf = s->leaves[i];
		const int	att = lay->leaf[leaf].attno;
		const Oid	typid = sift_v1_att_typid(lay, att);
		const SiftV6Class cls = sift_v6_class(typid);
		HybCol	   *c = &h->col[h->ncols++];

		*c = (HybCol) {.leaf = leaf,.slot = -1,.cls = cls};
		h->nviews = Max(h->nviews, leaf + 1);
		if (!leaf_served(s, att, cls))
			continue;
		for (int k = 0; k < h->in.ncols && c->slot < 0; k++)
			if (h->in.col[k].attno == att)
				c->slot = k;
		if (c->slot < 0)
		{
			c->slot = h->in.ncols;
			h->in.col[h->in.ncols++] = (SiftV6InCol) {.attno = att,.typid = typid,.type = vec_type_of(cls)};
		}
	}
}

static List *
scan_clauses(const SiftHybridScan *s)
{
	return s->index != NULL ? sift_srcfold_scankey_quals(s->index, s->keys, s->nkeys) : s->quals != NULL ? s->quals->clauses : NIL;
}

static void
scan_quals(HybScan *h, MemoryContext cx)
{
	const SiftHybridScan *s = h->s;
	List	   *clauses = scan_clauses(s);
	EState	   *estate = s->quals != NULL ? s->quals->estate : NULL;

	h->heap_decides = s->n_ext_preds > 0;
	if (estate != NULL)
		sift_v6_input_bind(&h->in, estate);
	if (clauses == NIL)
		return;
	h->source_quals = sift_v6_vexpr_quals(cx, clauses, 1, estate != NULL ? estate->es_param_list_info : NULL, &h->in, &h->quals);
	h->heap_decides |= !h->source_quals;
}

/* ---- the columns of a batch, as the ColViews the host reads ---- */

static void
view_of_vec(ColView *cv, const SiftVec *v, int n, SiftV6Class cls)
{
	bool	   *isnull = NULL;

	SIFT_INVARIANT(v->dense || v->sel == NULL, "a batch of Format 1 is dense");
	if (!v->all_valid)
	{
		isnull = palloc(n * sizeof(bool));
		for (int r = 0; r < n; r++)
			isnull[r] = !((v->valid[r >> 6] >> (r & 63)) & 1);
	}
	cv->isnull = isnull;
	if (v->type == SVEC_STRVIEW)
	{
		const char **s = palloc(n * sizeof(char *));
		uint32	   *len = palloc(n * sizeof(uint32));

		for (int r = 0; r < n; r++)
		{
			s[r] = sift_vstr_bytes(&((const SiftStrView *) v->data)[r]);
			len[r] = ((const SiftStrView *) v->data)[r].len;
		}
		cv->kind = SIFT_CELL_STR;
		cv->s = s;
		cv->slen = len;
	}
	else if (v->type == SVEC_F64)
	{
		cv->kind = SIFT_CELL_F8;
		cv->f = v->data;
	}
	else
	{
		cv->kind = SIFT_CELL_INT;
		cv->i = v->data;
	}
}

/* the leaf the format does not hold in one leaf of its class: every row is a row of the heap's */
static void
view_of_heap(ColView *cv, int n)
{
	bool	   *isnull = palloc(n * sizeof(bool));
	uint8	   *tag = palloc(n);

	memset(isnull, true, n);
	memset(tag, 1, n);
	cv->kind = SIFT_CELL_INT;
	cv->isnull = isnull;
	cv->tag = tag;
}

static ColView *
batch_views(const HybScan *h, const SiftV6Batch *b)
{
	ColView    *views = palloc0(Max(h->nviews, 1) * sizeof(ColView));

	for (int i = 0; i < h->ncols; i++)
		if (h->col[i].slot >= 0)
			view_of_vec(&views[h->col[i].leaf], &b->col[h->col[i].slot], b->n, h->col[i].cls);
		else
			view_of_heap(&views[h->col[i].leaf], b->n);
	return views;
}

static void
batch_live(const SiftV6Batch *b, uint64 *live)
{
	const int	words = (b->n + 63) / 64;

	for (int w = 0; w < words; w++)
		live[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
	if (b->n & 63)
		live[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
}

static void
scan_batch(const HybScan *h, const SiftV6Batch *b, void (*sink) (void *arg, const SiftHybridBatch *batch), void *arg)
{
	uint64		live[SIFT_VEC_WORDS],
				kept[SIFT_VEC_WORDS];
	float4		score[SIFT_VEC_LEN];
	SiftHybridBatch hb = {.tid = b->tag,.live = live,.n = b->n,.exact_vis = true};

	batch_live(b, live);
	if (h->s->bridge != NULL)
	{
		hb.score = score;
		if (sift_tidbridge_probe_vec(h->s->bridge, b->tag, live, b->n, kept, score) == 0)
			return;
		hb.live = kept;
	}
	if (h->heap_decides)
		hb.maybe = hb.live;
	hb.cols = batch_views(h, b);
	sink(arg, &hb);
}

static void
hybrid_scan(const SiftHybridScan *s, void (*sink) (void *arg, const SiftHybridBatch *batch), void *arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 hybrid scan");
	MemoryContext batch_cx = sift_mem_allocset_create(cx, "sift v1 hybrid batch");
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftV1Rel r = sift_v1_rel_open(RelationGetRelid(host_of(s)), s->index == NULL);
	const SiftSrcLayout *lay = reader_of(s)->layout(r.rel);
	HybScan		h = {.s = s};
	SiftV6Source *src;
	SiftV6Batch b;

	scan_columns(&h, lay);
	scan_quals(&h, cx);
	src = sift_v1_source_open(cx, s->snap, &h.in, h.source_quals ? &h.quals : NULL, NULL);
	while (src->next(src, &b))
	{
		MemoryContextSwitchTo(batch_cx);
		scan_batch(&h, &b, sink, arg);
		MemoryContextReset(batch_cx);
		MemoryContextSwitchTo(cx);
	}
	src->close(src);
	sift_v1_rel_close(&r);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* the granules the structured quals leave: the quals are decided on the rows of the batches, so the units of the relation */
static uint64
hybrid_survey(const SiftHybridScan *s)
{
	const SiftV1Rel r = sift_v1_rel_open(RelationGetRelid(host_of(s)), s->index == NULL);
	SiftSrcStats st;

	reader_of(s)->stats(r.rel, &st);
	sift_v1_rel_close(&r);
	return Max(1, (uint64) st.nrows / SIFT_GRANULE_ROWS);
}

static bool
hybrid_table_has_preds(const struct SiftStoreQuals *q)
{
	return q != NULL && q->clauses != NIL;
}

/* ---- the fold ---- */

typedef struct HybFold
{
	SiftSrcFold *f;
	MemoryContext cx;
	bool		(*recheck) (void *arg, uint64 key);
	void	   *recheck_arg;
	int			ncols;
	int			leaf[SIFT_V6_MAX_COLS];
	int			scale[SIFT_V6_MAX_COLS];	/* the digits after the point of the scaled integer a numeric leaf holds */
	SiftVec		vec[SIFT_V6_MAX_COLS];
	int			npub;
	SiftV6FoldCol *pub;
} HybFold;

static struct SiftV6Scan *
hybrid_fold_begin(const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, Snapshot snap, struct SiftV6ScanStats *stats,
				  bool (*recheck) (void *arg, uint64 key), void *recheck_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 hybrid fold");
	MemoryContext old = MemoryContextSwitchTo(cx);
	HybFold    *vf = palloc0(sizeof(HybFold));
	const SiftV1Rel r = sift_v1_rel_open(RelationGetRelid(index != NULL ? index : heap), index == NULL);
	const SiftSrcLayout *lay = sift_v1_reader_of(index == NULL)->layout(r.rel);
	const SiftV6Input *in;

	vf->cx = cx;
	vf->recheck = recheck;
	vf->recheck_arg = recheck_arg;
	vf->f = sift_srcfold_new(&(SiftSrcFoldArgs) {.spec = spec,.group = g,.stats = stats,.host = index != NULL ? index : heap,.is_table = index == NULL}, sift_mem_allocset_create(cx, "sift v1 fold"));
	in = sift_srcfold_input(vf->f);
	vf->ncols = in->ncols;
	vf->pub = palloc(Max(in->ncols, 1) * sizeof(SiftV6FoldCol));
	for (int i = 0; i < in->ncols; i++)
	{
		const SiftV6InCol *c = &in->col[i];
		const SiftSrcLeaf *l = c->attno == SIFT_V6_SCORE_ATTNO ? NULL : sift_v1_att_leaf(lay, c->attno);

		vf->leaf[i] = l != NULL ? l->id : -1;
		vf->scale[i] = l != NULL ? l->scale : 0;
		vf->vec[i] = (SiftVec) {.data = MemoryContextAllocAligned(cx, (SIFT_VEC_LEN + 1) * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO),
			.valid = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO),.dense = true,.type = c->type,.typid = c->typid,.scale = c->scale};
		if (c->attno != SIFT_V6_SCORE_ATTNO)
		{
			SIFT_INVARIANT(l != NULL, "the aggregate over the rows of relation %u reads attribute %d, which no leaf of its layout holds", RelationGetRelid(r.rel), c->attno);
			vf->pub[vf->npub++] = (SiftV6FoldCol) {.attno = c->attno,.leaf = l->id,.cls = sift_v6_class(c->typid),.scale = c->scale};
		}
	}
	sift_v1_rel_close(&r);
	MemoryContextSwitchTo(old);
	return (struct SiftV6Scan *) vf;
}

static const struct SiftV6FoldCol *
hybrid_fold_cols(const struct SiftV6Scan *f, int *n)
{
	const HybFold *vf = (const HybFold *) f;

	*n = vf->npub;
	return vf->pub;
}

static Datum
numeric_cell(const ColView *cv, int row, int scale)
{
	const int64 x = cv->i[row];

	return sift_v6_scaled_numeric((SiftI128) {.lo = (uint64) x,.hi = x < 0 ? -1 : 0}, scale);
}

static void
fill_vec(SiftVec *v, const ColView *cv, int scale, const float4 *score, int off, int n, bool is_score)
{
	v->len = n;
	v->all_valid = true;
	v->sel = NULL;
	if (is_score)
	{
		for (int r = 0; r < n; r++)
			((double *) v->data)[r] = score != NULL ? score[off + r] : 0.0;
		return;
	}
	for (int r = 0; r < n; r++)
		switch (v->type)
		{
			case SVEC_F64:
				((double *) v->data)[r] = cv->f[off + r];
				break;
			case SVEC_STRVIEW:
				((SiftStrView *) v->data)[r] = sift_vstr_make(cv->s[off + r], cv->slen[off + r]);
				break;
			case SVEC_DATUM:
				((Datum *) v->data)[r] = cv->isnull != NULL && cv->isnull[off + r] ? (Datum) 0 : numeric_cell(cv, off + r, scale);
				break;
			default:
				((int64 *) v->data)[r] = cv->i[off + r];
		}
	if (cv->isnull == NULL)
		return;
	memset(v->valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int r = 0; r < n; r++)
		if (!cv->isnull[off + r])
			v->valid[r >> 6] |= UINT64CONST(1) << (r & 63);
	v->all_valid = false;
}

static void
fold_slice(HybFold *vf, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int off, int n)
{
	const SiftV6Input *in = sift_srcfold_input(vf->f);
	const ColView *cols = views;
	uint64		mask[SIFT_VEC_WORDS] = {0};
	SiftV6Batch b = {.n = n,.col = vf->vec,.mask = mask,.tag = tid + off};

	memcpy(mask, live + off / 64, ((n + 63) / 64) * sizeof(uint64));
	if (maybe != NULL)
		for (int r = 0; r < n; r++)
			if ((maybe[(off + r) / 64] >> ((off + r) & 63) & 1) && vf->recheck(vf->recheck_arg, tid[off + r]))
				mask[r >> 6] |= UINT64CONST(1) << (r & 63);
	if (n & 63)
		mask[n / 64] &= (UINT64CONST(1) << (n & 63)) - 1;
	for (int i = 0; i < vf->ncols; i++)
		fill_vec(&vf->vec[i], in->col[i].attno == SIFT_V6_SCORE_ATTNO ? NULL : &cols[vf->leaf[i]], vf->scale[i], score, off, n, in->col[i].attno == SIFT_V6_SCORE_ATTNO);
	sift_srcfold_batch(vf->f, &b);
}

static void
hybrid_fold_batch_scored(struct SiftV6Scan *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int total)
{
	HybFold    *vf = (HybFold *) f;

	for (int off = 0; off < total; off += SIFT_VEC_LEN)
		fold_slice(vf, tid, live, maybe, views, score, off, Min(SIFT_VEC_LEN, total - off));
}

static void
hybrid_fold_end(struct SiftV6Scan *f)
{
	MemoryContextDelete(((HybFold *) f)->cx);
}

const SiftHybridOps v1_hybrid_ops = {
	.scan = hybrid_scan,
	.survey = hybrid_survey,
	.table_has_preds = hybrid_table_has_preds,
	.fold_begin = hybrid_fold_begin,
	.fold_cols = hybrid_fold_cols,
	.fold_batch_scored = hybrid_fold_batch_scored,
	.fold_end = hybrid_fold_end
};
