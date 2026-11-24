/*
 * col/f2/f2_v6_unnest_test.c - the quals of an element and the tests of the elements of a row on the element columns of a granule (col/f2/f2_v6_unnest_test.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, and the elements of one granule */
#include "postgres.h"

#include "agg/kern6.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_unnest.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "col/f2/f2_v6_unnest_test.h"

struct SiftF2ElemEval
{
	SiftClause	clause;
	int			ecol[SIFT_V6_MAX_FCOLS];	/* the column of the element each vector of the clause is */
	int			nvec;
	void	   *buf[SIFT_V6_MAX_FCOLS];
	uint64	   *valid[SIFT_V6_MAX_FCOLS];
	SiftVec		in[SIFT_V6_MAX_FCOLS];
	SiftVec    *inp[SIFT_V6_MAX_FCOLS];
};

SiftF2ElemEval *
sift_f2_elem_eval_open(MemoryContext cx, const List *quals)
{
	SiftF2ElemEval *ee = MemoryContextAllocZero(cx, sizeof(SiftF2ElemEval));
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
sift_f2_elem_eval(SiftF2ElemEval *ee, const SiftVec *ev, uint32 lo, uint32 hi, uint64 *keep)
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

struct SiftF2RowTests
{
	MemoryContext cx;
	const SiftV6Ext *x;
	SiftF2ElemEval *ee[SIFT_V6_XTESTS];
	uint64	   *keep;
	uint32	   *par;			/* the row of the batch each element of the granule belongs to */
	uint32		cap;			/* the elements keep and par have room for */
	uint32		zero[SIFT_VEC_LEN];
	void	   *gdata;
	uint64		gvalid[SIFT_VEC_WORDS];
};

SiftF2RowTests *
sift_f2_row_tests_open(MemoryContext cx, const SiftV6Ext *x)
{
	SiftF2RowTests *t = MemoryContextAllocZero(cx, sizeof(SiftF2RowTests));
	int			k = 0;

	t->cx = cx;
	t->x = x;
	t->gdata = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	foreach_ptr(List, tq, x->test_quals)
	{
		if (linitial(tq) != NIL)
			t->ee[k] = sift_f2_elem_eval_open(cx, linitial(tq));
		k++;
	}
	return t;
}

static void
reserve(SiftF2RowTests *t, uint32 n)
{
	if (n <= t->cap)
		return;
	t->cap = Max(n, 2 * t->cap);
	t->keep = t->keep == NULL ? MemoryContextAllocZero(t->cx, ((Size) t->cap / 64 + 2) * sizeof(uint64)) : repalloc(t->keep, ((Size) t->cap / 64 + 2) * sizeof(uint64));
	t->par = t->par == NULL ? MemoryContextAllocZero(t->cx, (Size) t->cap * sizeof(uint32)) : repalloc(t->par, (Size) t->cap * sizeof(uint32));
}

/* the elements [lo, hi) that pass the quals of test k (all of them for a test with none) */
static void
keep_quals(SiftF2RowTests *t, int k, const SiftVec *ev, uint32 lo, uint32 hi)
{
	if (t->ee[k] != NULL)
		sift_f2_elem_eval(t->ee[k], ev, lo, hi, t->keep);
	else
		memset(t->keep + lo / 64, 0xff, ((hi + 63) / 64 - lo / 64) * sizeof(uint64));
}

/* keep &= the elements of [lo, hi) whose value compares as cmp says with outer, the value of its row (idx: the row of each element; NULL: the one value) */
static void
keep_cmp(SiftF2RowTests *t, const SiftV6XCmp *cmp, const SiftVec *e, const SiftVec *outer, const uint32 *idx, uint32 lo, uint32 hi)
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
			t->keep[at / 64 + w] &= bits[w] & t->gvalid[w] & e->valid[at / 64 + w];
	}
}

static void
parents_of(SiftF2RowTests *t, const SiftArrayLayout *lay)
{
	for (uint32 e = lay->offsets[0] - lay->offsets[0] % 64; e < lay->offsets[0]; e++)
		t->par[e] = 0;
	for (uint32 p = 0; p < lay->n_parents; p++)
		for (uint32 e = lay->offsets[p]; e < lay->offsets[p + 1]; e++)
			t->par[e] = p;
}

void
sift_f2_row_tests_eval(SiftF2RowTests *t, const SiftF2ElemStream *es, const SiftVec *ev, const SiftVec *col, const int *outer, const uint64 *rows, uint64 yes[][SIFT_VEC_WORDS],
					   uint64 known[][SIFT_VEC_WORDS])
{
	const SiftArrayLayout *lay = &es->layout;
	const uint32 lo = lay->offsets[0],
				hi = lay->offsets[lay->n_parents];
	bool		have_par = false;

	reserve(t, es->n_elems);
	for (int k = 0; k < t->x->ntests; k++)
	{
		const SiftV6XTest *xt = &t->x->test[k];

		keep_quals(t, k, ev, lo, hi);
		for (int c = 0; c < xt->ncmps; c++)
		{
			if (!have_par)
				parents_of(t, lay);
			have_par = true;
			keep_cmp(t, &xt->cmp[c], &ev[xt->cmp[c].elem], &col[outer[k * SIFT_V6_XCMPS + c]], t->par, lo, hi);
		}
		sift_unnest_any(lay, t->keep, rows, es->doc_valid, yes[k], known[k]);
	}
}

bool
sift_f2_row_test_one(SiftF2RowTests *t, int k, const SiftVec *ev, uint32 nel, const SiftVec *outer)
{
	const SiftV6XTest *xt = &t->x->test[k];

	reserve(t, nel);
	keep_quals(t, k, ev, 0, nel);
	for (int c = 0; c < xt->ncmps; c++)
		keep_cmp(t, &xt->cmp[c], &ev[xt->cmp[c].elem], &outer[c], NULL, 0, nel);
	for (uint32 e = 0; e < nel; e++)
		if ((t->keep[e / 64] >> (e % 64)) & 1)
			return true;
	return false;
}
