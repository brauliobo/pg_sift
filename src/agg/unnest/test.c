/*
 * agg/unnest/test.c - the quals of an element and the tests of the elements of a row on the element columns of a granule (agg/unnest/test.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, and the elements of one granule */
#include "postgres.h"

#include "nodes/makefuncs.h"

#include "agg/kern/kern.h"
#include "agg/filter/pg.h"
#include "agg/gather.h"
#include "agg/unnest/unnest.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/test.h"

struct SiftV6ElemEval
{
	SiftClause	clause;
	int			ecol[SIFT_V6_MAX_FCOLS];	/* the column of the element each vector of the clause is */
	int			nvec;
	void	   *buf[SIFT_V6_MAX_FCOLS];
	uint64	   *valid[SIFT_V6_MAX_FCOLS];
	SiftVec		in[SIFT_V6_MAX_FCOLS];
	SiftVec    *inp[SIFT_V6_MAX_FCOLS];
};

SiftV6ElemEval *
sift_v6_elem_eval_open(MemoryContext cx, const List *quals)
{
	SiftV6ElemEval *ee = MemoryContextAllocZero(cx, sizeof(SiftV6ElemEval));
	bool		bound = sift_v6_unnest_elem_clause(cx, quals, &ee->clause, ee->ecol, &ee->nvec);

	SIFT_INVARIANT(bound, "the clause of the element of the aggregate scan of an unnest compiled when it was planned");
	for (int v = 0; v < ee->nvec; v++)
	{
		ee->buf[v] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		ee->valid[v] = MemoryContextAllocZero(cx, SIFT_VEC_WORDS * sizeof(uint64));
		ee->inp[v] = &ee->in[v];
	}
	return ee;
}

void
sift_v6_elem_eval(SiftV6ElemEval *ee, const SiftVec *ev, uint32 lo, uint32 hi, uint64 *keep)
{
	for (uint32 at = lo - lo % 64; at < hi; at += SIFT_VEC_LEN)
	{
		const uint32 cnt = Min(SIFT_VEC_LEN, hi - at);
		const uint32 words = (cnt + 63) / 64;
		uint64		chunk[SIFT_VEC_WORDS];

		for (int v = 0; v < ee->nvec; v++)
		{
			const SiftVec *e = &ev[ee->ecol[v]];
			const size_t w = sift_gather_width(e->type);

			memcpy(ee->buf[v], (const char *) e->data + (size_t) at * w, (size_t) cnt * w);
			memset(ee->valid[v], 0, SIFT_VEC_WORDS * sizeof(uint64));
			memcpy(ee->valid[v], e->valid + at / 64, words * sizeof(uint64));
			if (cnt & 63)
				ee->valid[v][words - 1] &= (UINT64CONST(1) << (cnt & 63)) - 1;
			ee->in[v] = (SiftVec) {.data = ee->buf[v],.valid = ee->valid[v],.len = (uint16) cnt,.all_valid = false,.dense = true,.type = e->type};
		}
		sift_clause_eval(&ee->clause, ee->inp, cnt, chunk);
		memcpy(keep + at / 64, chunk, words * sizeof(uint64));
	}
}

Expr *
sift_v6_unnest_or_arms(List *arms)
{
	return list_length(arms) == 1 ? linitial(arms) : makeBoolExpr(OR_EXPR, arms, -1);
}

struct SiftV6RowTests
{
	MemoryContext cx;
	const SiftV6Ext *x;
	SiftV6ElemEval *ee[SIFT_V6_XTESTS];
	List	   *ors[SIFT_V6_XTESTS];	/* of each disjunction of test k: the evaluator of its arms over the element columns (NULL: it has none) */
	uint64	   *keep;
	uint64	   *any;			/* the elements a disjunction keeps */
	uint64	   *arm;			/* those one arm of it keeps */
	uint32	   *par;			/* the row of the batch each element of the granule belongs to */
	uint32		cap;			/* the elements keep and par have room for */
	uint32		zero[SIFT_VEC_LEN];
	void	   *gdata;
	uint64		gvalid[SIFT_VEC_WORDS];
};

SiftV6RowTests *
sift_v6_row_tests_open(MemoryContext cx, const SiftV6Ext *x)
{
	SiftV6RowTests *t = MemoryContextAllocZero(cx, sizeof(SiftV6RowTests));
	int			k = 0;

	t->cx = cx;
	t->x = x;
	t->gdata = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	foreach_ptr(List, tq, x->test_quals)
	{
		if (linitial(tq) != NIL)
			t->ee[k] = sift_v6_elem_eval_open(cx, linitial(tq));
		foreach_ptr(List, arms, lthird(tq))
			t->ors[k] = lappend(t->ors[k], arms != NIL ? sift_v6_elem_eval_open(cx, list_make1(sift_v6_unnest_or_arms(arms))) : NULL);
		k++;
	}
	return t;
}

static void
reserve(SiftV6RowTests *t, uint32 n)
{
	if (n <= t->cap)
		return;
	t->cap = Max(n, 2 * t->cap);
	t->keep = t->keep == NULL ? MemoryContextAllocZero(t->cx, ((Size) t->cap / 64 + 2) * sizeof(uint64)) : repalloc(t->keep, ((Size) t->cap / 64 + 2) * sizeof(uint64));
	t->any = t->any == NULL ? MemoryContextAllocZero(t->cx, ((Size) t->cap / 64 + 2) * sizeof(uint64)) : repalloc(t->any, ((Size) t->cap / 64 + 2) * sizeof(uint64));
	t->arm = t->arm == NULL ? MemoryContextAllocZero(t->cx, ((Size) t->cap / 64 + 2) * sizeof(uint64)) : repalloc(t->arm, ((Size) t->cap / 64 + 2) * sizeof(uint64));
	t->par = t->par == NULL ? MemoryContextAllocZero(t->cx, (Size) t->cap * sizeof(uint32)) : repalloc(t->par, (Size) t->cap * sizeof(uint32));
}

/* the elements [lo, hi) that pass the quals of test k (all of them for a test with none) */
static void
keep_quals(SiftV6RowTests *t, int k, const SiftVec *ev, uint32 lo, uint32 hi)
{
	if (t->ee[k] != NULL)
		sift_v6_elem_eval(t->ee[k], ev, lo, hi, t->keep);
	else
		memset(t->keep + lo / 64, 0xff, ((hi + 63) / 64 - lo / 64) * sizeof(uint64));
}

/* out = the elements of [lo, hi) whose value compares as cmp says with outer, the value of its row (idx: the row of each element; NULL: the one value) */
static void
cmp_rows(SiftV6RowTests *t, const SiftV6XCmp *cmp, const SiftVec *e, const SiftVec *outer, const uint32 *idx, uint32 lo, uint32 hi, uint64 *out)
{
	for (uint32 at = lo - lo % 64; at < hi; at += SIFT_VEC_LEN)
	{
		const uint32 cnt = Min(SIFT_VEC_LEN, hi - at);
		const void *a = (const char *) e->data + (size_t) at * sizeof(int64);
		SiftVec		g;
		uint64		bits[SIFT_VEC_WORDS];

		sift_vec_gather(&g, t->gvalid, t->gdata, outer, idx != NULL ? idx + at : t->zero, cnt);
		if (e->type == SVEC_F64)
			sift_aggk6->cmp2_f64(bits, a, g.data, (uint16) cnt, (SiftCmpOp) cmp->op);
		else
			sift_aggk6->cmp2_i64(bits, a, g.data, (uint16) cnt, (SiftCmpOp) cmp->op);
		for (uint32 w = 0; w < (cnt + 63) / 64; w++)
			out[at / 64 + w] = bits[w] & t->gvalid[w] & e->valid[at / 64 + w];
	}
}

static void
and_rows(uint64 *dst, const uint64 *src, uint32 lo, uint32 hi)
{
	for (uint32 w = lo / 64; w < (hi + 63) / 64; w++)
		dst[w] &= src[w];
}

static void
or_rows(uint64 *dst, const uint64 *src, uint32 lo, uint32 hi)
{
	for (uint32 w = lo / 64; w < (hi + 63) / 64; w++)
		dst[w] |= src[w];
}

/*
 * keep &= the elements of [lo, hi) that pass the comparisons of test k with the columns of their row, outer[c] the column of comparison c (idx: the row of each element; NULL: one value): each
 * conjunct, and of each disjunction one arm, a comparison of its group or a qual of its element
 */
static void
keep_cmps(SiftV6RowTests *t, int k, const SiftVec *ev, const SiftVec *const *outer, const uint32 *idx, uint32 lo, uint32 hi)
{
	const SiftV6XTest *xt = &t->x->test[k];
	int			g = 0;

	for (int c = 0; c < xt->ncmps; c++)
		if (xt->cmp[c].group == 0)
		{
			cmp_rows(t, &xt->cmp[c], &ev[xt->cmp[c].elem], outer[c], idx, lo, hi, t->arm);
			and_rows(t->keep, t->arm, lo, hi);
		}
	foreach_ptr(SiftV6ElemEval, arms, t->ors[k])
	{
		g++;
		memset(t->any + lo / 64, 0, ((hi + 63) / 64 - lo / 64) * sizeof(uint64));
		if (arms != NULL)
			sift_v6_elem_eval(arms, ev, lo, hi, t->any);
		for (int c = 0; c < xt->ncmps; c++)
			if (xt->cmp[c].group == g)
			{
				cmp_rows(t, &xt->cmp[c], &ev[xt->cmp[c].elem], outer[c], idx, lo, hi, t->arm);
				or_rows(t->any, t->arm, lo, hi);
			}
		and_rows(t->keep, t->any, lo, hi);
	}
}

static void
parents_of(SiftV6RowTests *t, const SiftArrayLayout *lay)
{
	for (uint32 e = lay->offsets[0] - lay->offsets[0] % 64; e < lay->offsets[0]; e++)
		t->par[e] = 0;
	for (uint32 p = 0; p < lay->n_parents; p++)
		for (uint32 e = lay->offsets[p]; e < lay->offsets[p + 1]; e++)
			t->par[e] = p;
}

void
sift_v6_row_tests_eval(SiftV6RowTests *t, const SiftArrayLayout *lay, const uint64 *doc_valid, const SiftVec *ev, const SiftVec *col, const int *outer, const uint64 *rows,
					   uint64 yes[][SIFT_VEC_WORDS], uint64 known[][SIFT_VEC_WORDS])
{
	const uint32 lo = lay->offsets[0],
				hi = lay->offsets[lay->n_parents];
	bool		have_par = false;

	reserve(t, lay->n_elems);
	for (int k = 0; k < t->x->ntests; k++)
	{
		const SiftV6XTest *xt = &t->x->test[k];

		const SiftVec *cols[SIFT_V6_XCMPS];

		keep_quals(t, k, ev, lo, hi);
		if (xt->ncmps > 0 && !have_par)
			parents_of(t, lay);
		have_par |= xt->ncmps > 0;
		for (int c = 0; c < xt->ncmps; c++)
			cols[c] = &col[outer[k * SIFT_V6_XCMPS + c]];
		keep_cmps(t, k, ev, cols, t->par, lo, hi);
		sift_unnest_any(lay, t->keep, rows, doc_valid, yes[k], known[k]);
	}
}

bool
sift_v6_row_test_one(SiftV6RowTests *t, int k, const SiftVec *ev, uint32 nel, const SiftVec *outer)
{
	const SiftV6XTest *xt = &t->x->test[k];

	const SiftVec *cols[SIFT_V6_XCMPS];

	reserve(t, nel);
	keep_quals(t, k, ev, 0, nel);
	for (int c = 0; c < xt->ncmps; c++)
		cols[c] = &outer[c];
	keep_cmps(t, k, ev, cols, NULL, 0, nel);
	for (uint32 e = 0; e < nel; e++)
		if ((t->keep[e / 64] >> (e % 64)) & 1)
			return true;
	return false;
}
