/*
 * agg/v6/v6_proj_seek.c - the key plan of the predicates of a statement over the sort columns of a projection (agg/v6/v6_proj_seek.h).
 */
#include "core/host.h"

#include "agg/v6/v6_proj_seek.h"

static int
key_kind(SiftCellKind kind, uint8 hint)
{
	if (kind == SIFT_CELL_INT)
		return SIFT_KK_INT8;
	if (kind == SIFT_CELL_F8)
		return SIFT_KK_FLOAT8;
	return (hint & ~SIFT_CHINT_ASTEXT) == SIFT_CHINT_UUID ? SIFT_KK_UUID : SIFT_KK_TEXT;
}

static bool
key_op(SiftProjOp op, int nvals, SiftKeyOp *out)
{
	switch (op)
	{
		case SPO_LT:
			*out = SIFT_KOP_LT;
			return true;
		case SPO_LE:
			*out = SIFT_KOP_LE;
			return true;
		case SPO_EQ:
			*out = SIFT_KOP_EQ;
			return true;
		case SPO_GE:
			*out = SIFT_KOP_GE;
			return true;
		case SPO_GT:
			*out = SIFT_KOP_GT;
			return true;
		case SPO_IN:
			*out = SIFT_KOP_IN;
			return nvals > 0;
		default:
			return false;
	}
}

/* The sort column of the projection the predicate is on, as a column of the key: -1 when it is on none that a cell of the key can be compared with */
static int
key_column(const SiftProjSpec *spec, const SiftV6ProjSeek *s, const SiftProjPred *p)
{
	for (int k = 0; k < s->ks.ncols; k++)
	{
		int			kind = s->ks.cols[k].kind;

		if (spec->key[k].leaf == p->leaf && spec->key[k].fn == SPF_COLUMN && p->bytes == (kind == SIFT_KK_TEXT || kind == SIFT_KK_UUID) && (p->bytes || kind == SIFT_KK_INT8))
			return k;
	}
	return -1;
}

static SiftKeyVal *
key_vals(const SiftProjPred *p)
{
	/* check-memory: allow the values are the constants of one qual */
	SiftKeyVal *v = palloc(Max(p->nvals, 1) * sizeof(SiftKeyVal));

	for (int i = 0; i < p->nvals; i++)
		v[i] = p->bytes ? (SiftKeyVal) {.p = (const uint8 *) p->vals[i].s,.len = p->vals[i].len} : (SiftKeyVal) {.i = p->vals[i].i};
	return v;
}

bool
sift_v6_proj_seek_plan(const SiftProjPartSpec *pps, const SiftProjSpec *spec, const SiftProjPred *p, int n, SiftV6ProjSeek *out)
{
	SiftKeyPred *kp = palloc(Max(n, 1) * sizeof(SiftKeyPred));
	int			nkp = 0;

	out->ks = (SiftKeySpec) {.key_bytes = SIFT_KEY_BYTES_MAX};
	for (int k = 0; k < spec->nkeys && k < SIFT_KEY_COLS_MAX; k++)
		out->ks.cols[out->ks.ncols++] = (SiftKeyCol) {.kind = key_kind(pps->kinds[k], pps->hints[k]),.flags = pps->spec.sort_flags[k]};
	for (int i = 0; i < n && out->ks.ncols > 0; i++)
	{
		SiftKeyOp	op;
		int			col = key_column(spec, out, &p[i]);

		if (col >= 0 && key_op((SiftProjOp) p[i].op, p[i].nvals, &op))
			kp[nkp++] = (SiftKeyPred) {.col = col,.op = op,.nvals = p[i].nvals,.vals = key_vals(&p[i])};
	}
	if (nkp == 0)
		return false;
	sift_key_plan_build(&out->ks, kp, nkp, &out->plan);
	return out->plan.state != SIFT_KEY_PLAN_FULL;
}
