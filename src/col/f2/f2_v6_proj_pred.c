/*
 * col/f2/f2_v6_proj_pred.c - the tests of the cells of a projection for the predicates of a statement (col/f2/f2_v6_proj_pred.h).
 */
#include "core/host.h"

#include "col/f2/f2_v6_proj_pred.h"

static int
compare(const SiftProjPred *p, const SiftCell *c, const SiftProjConst *k)
{
	uint32		n = Min(c->slen, k->len);
	int			r;

	if (!p->bytes)
		return c->i < k->i ? -1 : c->i > k->i;
	r = n > 0 ? memcmp(c->s, k->s, n) : 0;
	return r != 0 ? r : (c->slen < k->len ? -1 : c->slen > k->len);
}

static bool
holds(SiftProjOp op, int cmp)
{
	switch (op)
	{
		case SPO_LT:
			return cmp < 0;
		case SPO_LE:
			return cmp <= 0;
		case SPO_EQ:
		case SPO_IN:
			return cmp == 0;
		case SPO_IS_NULL:
		case SPO_IS_NOT_NULL:
			return false;
		case SPO_GE:
			return cmp >= 0;
		case SPO_GT:
			return cmp > 0;
		default:
			return cmp != 0;
	}
}

bool
sift_proj_pred_pass(const SiftProjPred *p, const SiftCell *row)
{
	const SiftCell *c = &row[p->leaf];

	if (p->op == SPO_IS_NULL || p->op == SPO_IS_NOT_NULL)
		return c->isnull == (p->op == SPO_IS_NULL);
	if (c->isnull)
		return false;
	for (int k = 0; k < p->nvals; k++)
		if (holds((SiftProjOp) p->op, compare(p, c, &p->vals[k])))
			return true;
	return false;
}

bool
sift_proj_preds_pass(const SiftProjPred *p, int n, const SiftCell *row)
{
	for (int i = 0; i < n; i++)
		if (!sift_proj_pred_pass(&p[i], row))
			return false;
	return true;
}

bool
sift_proj_preds_decidable(const SiftProjPred *p, int n, const SiftCell *row)
{
	for (int i = 0; i < n; i++)
		if (row[p[i].leaf].tag != 0)
			return false;
	return true;
}

bool
sift_proj_pred_past_end(const SiftProjKey *first, const SiftProjPred *p, int n, const SiftCell *row)
{
	if (first->desc || first->nulls_first || first->fn != SPF_COLUMN)
		return false;
	for (int i = 0; i < n; i++)
	{
		if (p[i].leaf != first->leaf || p[i].nvals != 1 || (p[i].op != SPO_LT && p[i].op != SPO_LE && p[i].op != SPO_EQ))
			continue;
		if (row[p[i].leaf].tag != 0)
			continue;
		if (row[p[i].leaf].isnull || (p[i].op == SPO_EQ ? compare(&p[i], &row[p[i].leaf], &p[i].vals[0]) > 0 : !sift_proj_pred_pass(&p[i], row)))
			return true;
	}
	return false;
}

SiftProjRowVerdict
sift_proj_row_verdict(const SiftProjKey *first, const SiftProjPred *p, int n, const SiftCell *row)
{
	if (sift_proj_pred_past_end(first, p, n, row))
		return SRV_END;
	return sift_proj_preds_pass(p, n, row) || !sift_proj_preds_decidable(p, n, row) ? SRV_TAKE : SRV_SKIP;
}

void
sift_proj_pred_of_spec(const SiftProjSpecPred *sp, SiftProjConst *buf, SiftProjPred *out)
{
	for (int i = 0; i < sp->nvals; i++)
		buf[i] = sp->bytes ? (SiftProjConst) {.s = sp->sval[i],.len = sp->slen[i]} : (SiftProjConst) {.i = sp->ival[i]};
	*out = (SiftProjPred) {.leaf = sp->leaf,.op = sp->op,.bytes = sp->bytes,.nvals = sp->nvals,.vals = buf};
}

bool
sift_proj_spec_pred_of_pred(const SiftProjPred *p, SiftProjSpecPred *out)
{
	if (p->nvals > SIFT_PROJ_PRED_VALS)
		return false;
	*out = (SiftProjSpecPred) {.leaf = p->leaf,.op = p->op,.bytes = p->bytes,.nvals = (uint8) p->nvals};
	for (int i = 0; i < p->nvals; i++)
	{
		if (!p->bytes)
			out->ival[i] = p->vals[i].i;
		else if (p->vals[i].len > SIFT_PROJ_PRED_BYTES)
			return false;
		else
		{
			out->slen[i] = (uint8) p->vals[i].len;
			memcpy(out->sval[i], p->vals[i].s, p->vals[i].len);
		}
	}
	return true;
}

/* The integers a predicate accepts as an interval, false when it is not one (a list, a difference, a test for NULL, bytes) */
static bool
interval_of(SiftProjOp op, bool bytes, int nvals, int64 c, int64 *lo, int64 *hi)
{
	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	if (bytes || nvals != 1)
		return false;
	switch (op)
	{
		case SPO_LT:
			*hi = c == PG_INT64_MIN ? c : c - 1;
			return true;
		case SPO_LE:
			*hi = c;
			return true;
		case SPO_EQ:
			*lo = *hi = c;
			return true;
		case SPO_GE:
			*lo = c;
			return true;
		case SPO_GT:
			*lo = c == PG_INT64_MAX ? c : c + 1;
			return true;
		default:
			return false;
	}
}

static bool
same_const(bool bytes, const SiftProjConst *a, const SiftProjSpecPred *w, int i)
{
	return bytes ? a->len == w->slen[i] && (a->len == 0 || memcmp(a->s, w->sval[i], a->len) == 0) : a->i == w->ival[i];
}

static bool
in_spec_vals(const SiftProjPred *q, const SiftProjConst *v, const SiftProjSpecPred *w)
{
	for (int i = 0; i < w->nvals; i++)
		if (same_const(q->bytes, v, w, i))
			return true;
	return false;
}

/* The query's EQ or IN against a spec's EQ, IN or NE: every value of the query is, or is not, among the spec's */
static bool
values_imply(const SiftProjPred *q, const SiftProjSpecPred *w)
{
	for (int k = 0; k < q->nvals; k++)
	{
		bool		among = in_spec_vals(q, &q->vals[k], w);

		if (w->op == SPO_NE ? among : !among)
			return false;
	}
	return true;
}

bool
sift_proj_pred_implies(const SiftProjPred *q, const SiftProjSpecPred *w)
{
	int64		qlo, qhi, wlo, whi;

	if (q->leaf != w->leaf)
		return false;
	if (w->op == SPO_IS_NULL || q->op == SPO_IS_NULL)
		return w->op == q->op;
	if (w->op == SPO_IS_NOT_NULL)
		return true;
	if (q->op == SPO_IS_NOT_NULL || q->bytes != w->bytes)
		return false;
	if ((q->op == SPO_EQ || q->op == SPO_IN) && (w->op == SPO_EQ || w->op == SPO_IN || w->op == SPO_NE))
		return values_imply(q, w);
	if (q->op == SPO_NE && w->op == SPO_NE)
		return q->nvals == 1 && w->nvals == 1 && same_const(q->bytes, &q->vals[0], w, 0);
	if (!interval_of((SiftProjOp) w->op, w->bytes, w->nvals, w->ival[0], &wlo, &whi))
		return false;
	if (q->op == SPO_EQ || q->op == SPO_IN)
	{
		for (int k = 0; k < q->nvals; k++)
			if (q->vals[k].i < wlo || q->vals[k].i > whi)
				return false;
		return !q->bytes;
	}
	return interval_of((SiftProjOp) q->op, q->bytes, q->nvals, q->nvals > 0 ? q->vals[0].i : 0, &qlo, &qhi) && qlo >= wlo && qhi <= whi;
}

bool
sift_proj_const_aligned(SiftProjTime time, SiftProjOp op, int64 val, SiftProjUnit unit, SiftProjOp *mapped_op, int64 *mapped_val)
{
	bool		edge = sift_proj_trunc_cell(time, val, unit) == val,
				next_edge = val < PG_INT64_MAX && sift_proj_trunc_cell(time, val + 1, unit) == val + 1;

	switch (op)
	{
		case SPO_GE:
		case SPO_LT:
			*mapped_op = op;
			*mapped_val = val;
			return edge;
		case SPO_LE:
			*mapped_op = SPO_LT;
			*mapped_val = val + 1;
			return next_edge;
		case SPO_GT:
			*mapped_op = SPO_GE;
			*mapped_val = val + 1;
			return next_edge;
		case SPO_EQ:
			*mapped_op = SPO_EQ;
			*mapped_val = val;
			return time == SPT_DATE && unit == SPU_DAY;
		default:
			return false;
	}
}

bool
sift_proj_pred_is_spec(const SiftProjPred *q, const SiftProjSpecPred *w)
{
	SiftProjSpecPred as_spec;

	return sift_proj_spec_pred_of_pred(q, &as_spec) && sift_proj_spec_pred_equal(&as_spec, w);
}

int
sift_proj_stored_preds(const SiftProjRollup *map, const SiftProjPred *preds, int n, SiftProjPred *out, SiftProjConst *buf)
{
	int			m = 0;

	for (int i = 0; i < n; i++)
	{
		if (map->pred_use[i] == SPPU_SKIP)
			continue;
		out[m] = preds[i];
		if (map->pred_use[i] == SPPU_MAPPED)
		{
			buf[i] = (SiftProjConst) {.i = map->pred_val[i]};
			out[m].op = map->pred_op[i];
			out[m].nvals = 1;
			out[m].vals = &buf[i];
		}
		m++;
	}
	return m;
}
