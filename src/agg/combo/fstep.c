/*
 * agg/combo/fstep.c - combos: the steps of a float column
 * floor, ceil, trunc, round and the integer casts of x, x / c or x * c change only where x crosses a whole number of c, so
 * the expressions of a combo that read the column only through one of them take the value of a bucket from the first row of it, not from every
 * distinct value (a measurement column has a value a row). The bucket number of a row is computed as PostgreSQL's functions do, and
 * a row they could not give a number (NULL, NaN, an infinity, a result that raises or leaves int32, a negative zero) is a distinct value as before.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"

#include "agg/combo/int.h"

#define FSTEP_SPAN_MAX	(1 << 20)	/* bucket numbers one table covers (4 MB); a wider histogram keeps the ids of its values */

/* the argument of a step is the leaf: Param 0, a float4 one through its conversion */
static bool
fstep_leaf(Node *n)
{
	if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcid == F_FLOAT8_FLOAT4)
		n = linitial(((FuncExpr *) n)->args);
	return IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid == 0 &&
		(((Param *) n)->paramtype == FLOAT8OID || ((Param *) n)->paramtype == FLOAT4OID);
}

/* the finite constant c other than 0 of a float8 Const */
static bool
fstep_const(Node *n, double *c)
{
	Const	   *k = (Const *) n;

	if (!IsA(n, Const) || k->constisnull || k->consttype != FLOAT8OID)
		return false;
	*c = DatumGetFloat8(k->constvalue);
	return isfinite(*c) && *c != 0;
}

/* n is floor(a), ceil(a), trunc(a), round(a) or an integer cast of a, a being the leaf, leaf / c, leaf * c or c * leaf */
static bool
fstep_match(Node *n, FStep *fs)
{
	FuncExpr   *f;
	Node	   *a;

	if (!IsA(n, FuncExpr))
		return false;
	f = (FuncExpr *) n;
	switch (f->funcid)
	{
		case F_FLOOR_FLOAT8:
			fs->op = SIFT_FSTEP_FLOOR;
			break;
		case F_CEIL_FLOAT8:
		case F_CEILING_FLOAT8:
			fs->op = SIFT_FSTEP_CEIL;
			break;
		case F_TRUNC_FLOAT8:
		case F_DTRUNC:
			fs->op = SIFT_FSTEP_TRUNC;
			break;
		case F_ROUND_FLOAT8:
		case F_DROUND:
			fs->op = SIFT_FSTEP_ROUND;
			break;
		case F_INT2_FLOAT8:
		case F_INT4_FLOAT8:
		case F_INT8_FLOAT8:
			fs->op = SIFT_FSTEP_TOINT;
			break;
		default:
			return false;
	}
	a = linitial(f->args);
	fs->form = SIFT_FSTEP_PLAIN;
	fs->c = 1;
	if (fstep_leaf(a))
		return true;
	if (IsA(a, OpExpr))
	{
		OpExpr	   *o = (OpExpr *) a;

		set_opfuncid(o);
		if (o->opfuncid == F_FLOAT8DIV && fstep_leaf(linitial(o->args)) && fstep_const(lsecond(o->args), &fs->c))
		{
			fs->form = SIFT_FSTEP_DIV;
			return true;
		}
		if (o->opfuncid == F_FLOAT8MUL &&
			((fstep_leaf(linitial(o->args)) && fstep_const(lsecond(o->args), &fs->c)) || (fstep_leaf(lsecond(o->args)) && fstep_const(linitial(o->args), &fs->c))))
		{
			fs->form = SIFT_FSTEP_MUL;
			return true;
		}
	}
	return false;
}

typedef struct FsCtx
{
	FStep		fs;
	bool		seen;
	bool		ok;
} FsCtx;

/* every read of the leaf is inside one and the same step */
static bool
fstep_walker(Node *n, FsCtx *x)
{
	FStep		s;

	if (n == NULL || !x->ok)
		return false;
	if (fstep_match(n, &s))
	{
		if (x->seen && (x->fs.op != s.op || x->fs.form != s.form || x->fs.c != s.c))
			x->ok = false;
		x->fs = s;
		x->seen = true;
		return false;
	}
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid == 0)
	{
		x->ok = false;
		return false;
	}
	return expression_tree_walker(n, fstep_walker, x);
}

/* the step all the slots of a combo of one float leaf read it through; false: there is none */
bool
combo_fstep(const SiftAggCombo *cb, FStep *fs)
{
	FsCtx		x = {{0}, false, true};

	for (int sl = 0; sl < cb->nslots; sl++)
		if (contain_volatile_functions((Node *) cb->slot[sl].expr))
			return false;
	for (int sl = 0; sl < cb->nslots; sl++)
		(void) fstep_walker((Node *) cb->slot[sl].expr, &x);
	*fs = x.fs;
	return x.ok && x.seen;
}

/* the table of bucket numbers covers [mn, mx]; false: it would pass FSTEP_SPAN_MAX */
static bool
fstep_cover(ComboSt *cs, int32 mn, int32 mx)
{
	int64		lo = mn,
				hi = mx,
				span,
				cap;
	uint32	   *tab;

	if (cs->qcap)
	{
		if (mn >= cs->qlo && mx < (int64) cs->qlo + cs->qcap)
			return true;
		lo = Min(lo, cs->qlo);
		hi = Max(hi, (int64) cs->qlo + cs->qcap - 1);
	}
	span = hi - lo + 1;
	if (span > FSTEP_SPAN_MAX)
		return false;
	cap = Min(FSTEP_SPAN_MAX, Max(span, 2 * (int64) cs->qcap));
	cap = Max(cap, 256);
	lo = Max(lo - (cap - span) / 2, (int64) PG_INT32_MIN);
	tab = MemoryContextAllocZero(cs->cxt, cap * sizeof(uint32));
	if (cs->qcap)
	{
		memcpy(tab + (cs->qlo - lo), cs->qtab, (size_t) cs->qcap * sizeof(uint32));
		pfree(cs->qtab);
	}
	cs->qtab = tab;
	cs->qlo = (int32) lo;
	cs->qcap = (uint32) cap;
	return true;
}

/*
 * eid[0..n) of the tuples of a combo by_id on a float leaf that every expression reads through one step: the entry of a tuple is the one of the first value its bucket
 * had, so the expressions run once per bucket. false: the buckets of the data are too many for a table, and the combo keeps the ids of the values from now on.
 */
bool
combo_fstep_resolve(AggExec *ex, int ci, int n, const uint32 *idx, uint32 *eid, const uint64 *alive, const SiftSrcLeaf *lf, const SiftSrcVec *v)
{
	ComboSt    *cs = &ex->cs[ci];
	const double *data = v->data;
	double	   *x = (double *) ex->cvid[0];
	int32	   *q;
	uint32		last = UINT32_MAX,
				e = 0;
	int32		mn,
				mx;
	bool		any;

	if (cs->fq == NULL)
		cs->fq = MemoryContextAlloc(cs->cxt, (VEC + PADW) * sizeof(int32));
	q = cs->fq;
	/* a NULL is NaN, which has no number: wide_vid gives it its id 0 */
	if (v->isnull)
		for (int i = 0; i < n; i++)
			x[i] = v->isnull[idx[i]] ? NAN : data[idx[i]];
	else
		for (int i = 0; i < n; i++)
			x[i] = data[idx[i]];
	any = sift_aggk->fstep(q, &mn, &mx, x, n, cs->fs.op, cs->fs.form, cs->fs.c);
	if (any && !fstep_cover(cs, mn, mx))
	{
		cs->fstep = false;
		return false;
	}
	for (int i = 0; i < n; i++)
	{
		if (alive && !sift_colsrc_bit(alive, i))
		{
			eid[i] = COMBO_NONE;
			continue;
		}
		if (idx[i] != last)
		{
			last = idx[i];
			e = q[i] == SIFT_FSTEP_NONE ? 0 : cs->qtab[q[i] - cs->qlo];
			if (e == 0)
			{
				uint64		id = wide_vid(cs, 0, lf, v, last);

				/* the entries of a by_id combo are made up to an id */
				if (id >= cs->nent)
					(void) combo_ent(ex, ci, &id);
				e = (uint32) id;
				if (q[i] != SIFT_FSTEP_NONE)
					cs->qtab[q[i] - cs->qlo] = e;
			}
		}
		eid[i] = e;
	}
	return true;
}
