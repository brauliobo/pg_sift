/*
 * col/f2/scan/verdict.c - what the predicates of a scan say of one row of the stored values (yes, no, or undecided for the heap), and how a row is handed to a sink.
 */
#include "postgres.h"

#include "col/f2/scan/scan_int.h"
#include "core/popcount.h"

/* a numeric NaN is above every number and equals itself, whatever constant (a number) a comparison holds */
static Verdict
numeric_nan_verdict(SiftColOp op)
{
	switch (op)
	{
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
		case SIFT_COLOP_NOTNULL:
			return V_YES;
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_ISNULL:
			return V_NO;
		default:
			return V_MAYBE;
	}
}

static Verdict
pred_row(const SiftColDef *d, const SiftColPred *p, SiftPredSet *ps, const ColView *cv, int r)
{
	bool		isnull;

	if (p->op == SIFT_COLOP_KEY_EXISTS)
		return cv->keyp[r] ? V_YES : V_NO;
	isnull = cv->isnull[r];
	if (p->op == SIFT_COLOP_JSON_TYPE)
	{
		uint8		type = sift_f2_json_type(d, isnull, cv->tag != NULL ? cv->tag[r] : 0);

		return type == SIFT_COLTAG_UNSURE ? V_MAYBE : (p->vals[0].v.i >> type) & 1 ? V_YES : V_NO;
	}
	if (isnull && cv->tag != NULL && cv->tag[r] == SIFT_COLTAG_UNSURE && d->type == SIFT_COLT_NUMERIC)
		return numeric_nan_verdict(p->op);
	if (isnull && cv->tag != NULL && cv->tag[r] != 0)
		return V_MAYBE;
	if (p->op == SIFT_COLOP_ISNULL)
		return isnull ? V_YES : V_NO;
	if (p->op == SIFT_COLOP_NOTNULL)
		return isnull ? V_NO : V_YES;
	if (isnull)
		return V_NO;
	if (sift_f2_undecided(d, p))
		return V_MAYBE;
	if (cv->kind == SIFT_CELL_STR)
		return (Verdict) (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN ? (sift_f2_str_pred_in(ps, p, cv->s[r], cv->slen[r]) ? SIFT_LIKE_YES : SIFT_LIKE_NO) : sift_f2_str_verdict(p, cv->s[r], cv->slen[r]));
	if (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN)
		return (cv->kind == SIFT_CELL_INT ? sift_f2_in_i64(ps, p, cv->i[r]) : sift_f2_in_f8(ps, p, cv->f[r])) == (p->op == SIFT_COLOP_IN) ? V_YES : V_NO;
	return sift_f2_cmp_op(p->op, cv->kind == SIFT_CELL_INT ? (cv->i[r] < p->vals[0].v.i ? -1 : cv->i[r] > p->vals[0].v.i) : sift_f2_f8_cmp(cv->f[r], p->vals[0].v.f)) ? V_YES : V_NO;
}

Verdict
sift_f2_row_verdict(const SiftColSchema *schema, const F2Scan *s, const ColView *views, const bool *decided, int r)
{
	Verdict		all = V_YES;

	for (int k = 0; k < s->npreds; k++)
	{
		Verdict		v;

		if (decided[k])
			continue;
		v = pred_row(&schema->cols[s->preds[k].col], &s->preds[k], &s->psets[k], &views[s->preds[k].col], r);

		if (v == V_NO)
			return V_NO;
		if (v == V_MAYBE)
			all = V_MAYBE;
	}
	return all;
}

/* A row handed to a sink: decided yes, or undecided for a sink that takes those; any other sink counts only what the stored values decided, so it is an error for it */
void
sift_f2_sink_verdict(const F2Scan *s, Verdict v, uint64 *live, uint64 *maybe, int r)
{
	if (v == V_MAYBE && !s->sink_maybe)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
						errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
	live[r >> 6] |= (uint64) (v == V_YES) << (r & 63);
	maybe[r >> 6] |= (uint64) (v == V_MAYBE) << (r & 63);
}

/* A row of a granule that is not decoded: undecided, or yes when the chunks decided every predicate and the sink wants no column (the heap gives the columns of an undecided row) */
Verdict
sift_f2_heap_verdict(const F2Scan *s, const bool *decided)
{
	for (int k = 0; k < s->npreds; k++)
		if (!decided[k])
			return V_MAYBE;
	return s->nsink_leaves == 0 ? V_YES : V_MAYBE;
}

/* The rows a bridge leaves in a granule that the heap decides faster than the columns of the granule decode (a hot heap fetch against a chunk of SIFT_GRANULE_ROWS values) */
bool
sift_f2_scan_few_rows(const uint64 *active, int n)
{
	int			rows = 0;

	for (int w = 0; w < (n + 63) / 64; w++)
		rows += sift_pc64(active[w]);
	return rows <= SIFT_F2_HEAP_DECIDE_ROWS;
}
