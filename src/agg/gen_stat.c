/*
 * agg/gen_stat.c - the statistics aggregates (var_*, stddev_*, corr, covar_*, regr_*) of float8 inputs, a chunk at a time
 * The transition functions float8_accum and float8_regr_accum, called as ExecAgg calls them, cost a call through fmgr, the arguments as Datums and a reset
 * of the expression memory for each row. Here the state array of PostgreSQL (float8[3] or float8[6], which the transition function updates in
 * place) is updated from the values of the chunk with the same Youngs-Cramer steps, so that the partial states, the final functions and the spill
 * stay PostgreSQL's. A row whose values or results are not finite goes through the transition function itself, which raises its errors and makes
 * its NaNs. var_* and stddev_* of an int2 or int4 column have the state of PostgreSQL's int4_accum (a count and two 128-bit sums); the first row of a group
 * makes it through the function, and the layout is checked against what the function made before a row is added to it by the kernel.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/primnodes.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"

#include "agg/agg_int.h"
#include "agg/gen_int.h"

static int stat_variant(Oid fn);

#ifdef HAVE_INT128
/* the state of int2_accum and int4_accum (numeric.c, PolyNumAggState with 128-bit integers) */
typedef struct StatPoly
{
	bool		calcSumX2;
	int64		N;
	int128		sumX;
	int128		sumX2;
} StatPoly;
#endif

/* how argument k is read: a float8 leaf, or an int2 / int4 leaf the aggregate converts (float8(int4) over the leaf's Param) */
static int
stat_arg_mode(const SiftAggGenArg *ar, const SiftSrcLeaf *lf)
{
	if (ar->mapped || ar->nleaves != 1)
		return GEN_STAT_NONE;
	if (ar->expr == NULL)
		return lf->vtype == SCV_F64 && lf->typid == FLOAT8OID ? GEN_STAT_F64 : GEN_STAT_NONE;
	if (lf->vtype == SCV_I32 && (lf->typid == INT4OID || lf->typid == INT2OID) && IsA(ar->expr, FuncExpr))
	{
		const FuncExpr *fe = (const FuncExpr *) ar->expr;

		if (list_length(fe->args) == 1 && IsA(linitial(fe->args), Param) &&
			fe->funcid == (lf->typid == INT4OID ? F_FLOAT8_INT4 : F_FLOAT8_INT2))
			return GEN_STAT_I32;
	}
	return GEN_STAT_NONE;
}

/*
 * Does the kernel run the aggregate whose transition function is fn: no DISTINCT, ORDER BY or WITHIN GROUP, and every argument a float8 leaf, or an int2 or
 * int4 leaf (converted to float8 by the aggregate, or itself for the int accumulators). The planner prices the aggregate by it, the executor selects by it.
 * leaf(arg, id) gives the leaf; smode and sint (NULL: not wanted) get how each argument is read and the int type of the integer accumulators.
 */
bool
gen_stat_eligible(Oid fn, const SiftAggGen *g, const SiftSrcLeaf *(*leaf) (void *arg, int id), void *arg, int *smode, Oid *sint)
{
	if (g->distinct || g->ordered || g->sorted || g->first || g->hyp || g->ntrans != g->nargs)
		return false;
#ifdef HAVE_INT128
	if ((fn == F_INT4_ACCUM || fn == F_INT2_ACCUM) && g->nargs == 1 && !g->arg[0].mapped && g->arg[0].nleaves == 1 && g->arg[0].expr == NULL)
	{
		const SiftSrcLeaf *lf = leaf(arg, g->arg[0].leaf[0]);

		if (lf->vtype != SCV_I32 || lf->typid != (fn == F_INT4_ACCUM ? INT4OID : INT2OID))
			return false;
		if (smode)
			smode[0] = GEN_STAT_I32;
		if (sint)
			*sint = lf->typid;
		return true;
	}
#endif
	if (!(fn == F_FLOAT8_ACCUM && g->nargs == 1) && !(fn == F_FLOAT8_REGR_ACCUM && g->nargs == 2))
		return false;
	for (int k = 0; k < g->nargs; k++)
	{
		int			m = g->arg[k].nleaves == 1 ? stat_arg_mode(&g->arg[k], leaf(arg, g->arg[k].leaf[0])) : GEN_STAT_NONE;

		if (m == GEN_STAT_NONE)
			return false;
		if (smode)
			smode[k] = m;
	}
	if (sint)
		*sint = InvalidOid;
	return true;
}

static const SiftSrcLeaf *
stat_exec_leaf(void *arg, int id)
{
	return &((AggExec *) arg)->lay->leaf[id];
}

void
gen_stat_pick(AggExec *ex, GenSt *st)
{
	Oid			fn = st->transfn.fn_oid;

	st->stat = 0;
	st->sint = InvalidOid;
	if (!gen_stat_eligible(fn, st->g, stat_exec_leaf, ex, st->smode, &st->sint))
		return;
	if (st->sint == InvalidOid && (st->fma = stat_variant(fn)) < 0)
		return;
	st->stat = st->g->nargs;
}

/* the value of argument k of tuple j (false: NULL) */
static inline bool
stat_value(const GenSt *st, const GenIn *in, int k, int j, float8 *x)
{
	const SiftSrcVec *v = in->vec[k][0];
	uint32		i = in->idx[k][0][j];

	if (v->isnull && v->isnull[i])
		return false;
	*x = st->smode[k] == GEN_STAT_F64 ? ((const double *) v->data)[i] : (double) ((const int32 *) v->data)[i];
	return true;
}

/*
 * float8_accum on the finite inputs, false when the new state is not finite. fm: the steps PostgreSQL's function was compiled to, with the product of
 * x * n - sx fused (a build for a CPU with FMA contracts it; it matters when the mean is large and the variance small): stat_variant tells.
 */
static inline bool
accum1(float8 *t, float8 x, bool fm)
{
	float8		n0 = t[0],
				n = n0 + 1.0,
				sx = t[1] + x,
				sxx = t[2];

	if (n0 > 0.0)
	{
		float8		tmp = fm ? __builtin_fma(x, n, -sx) : x * n - sx;

		sxx += tmp * tmp / (n * n0);
	}
	if (!isfinite(sx) || !isfinite(sxx))
		return false;
	t[0] = n;
	t[1] = sx;
	t[2] = sxx;
	return true;
}

/* float8_regr_accum on the finite inputs (x the second argument of the aggregate, y the first, as the transition function takes them) */
static inline bool
accum2(float8 *t, float8 x, float8 y, bool fm)
{
	float8		n0 = t[0],
				n = n0 + 1.0,
				sx = t[1] + x,
				sy = t[3] + y,
				sxx = t[2],
				syy = t[4],
				sxy = t[5];

	if (n0 > 0.0)
	{
		float8		tx = fm ? __builtin_fma(x, n, -sx) : x * n - sx,
					ty = fm ? __builtin_fma(y, n, -sy) : y * n - sy,
					scale = 1.0 / (n * n0);

		if (fm)
		{
			sxx = __builtin_fma(tx * tx, scale, sxx);
			syy = __builtin_fma(ty * ty, scale, syy);
			sxy = __builtin_fma(tx * ty, scale, sxy);
		}
		else
		{
			sxx += tx * tx * scale;
			syy += ty * ty * scale;
			sxy += tx * ty * scale;
		}
	}
	if (!isfinite(sx) || !isfinite(sy) || !isfinite(sxx) || !isfinite(syy) || !isfinite(sxy))
		return false;
	t[0] = n;
	t[1] = sx;
	t[2] = sxx;
	t[3] = sy;
	t[4] = syy;
	t[5] = sxy;
	return true;
}

/* a state and a value where the fused and the plain steps give other bits */
static const float8 PROBE1_T[3] = {4.0, 1854.6511990795068, 0.00046920947556794435};
static const float8 PROBE1_X = 463.5162386317092;
static const float8 PROBE2_T[6] = {5.0, 1809.8828173201484, 0.013738174758122113, 1051.0434822708926, 0.07921008134267216, 0.5683115575669015};
static const float8 PROBE2_X = 359.1805880572742;
static const float8 PROBE2_Y = 219.495981393353;

/*
 * Which steps the transition function of this server does, found by running it on the probe: 1 fused, 0 plain, -1 neither (another formula: the kernel is not
 * used, the function is). A build of PostgreSQL for another CPU or compiler gives the other answer, and the rows are its own all the same.
 */
static int
stat_variant(Oid fn)
{
	static int	known[2] = {-2, -2};
	int			which = fn == F_FLOAT8_ACCUM ? 0 : 1,
				n = which == 0 ? 3 : 6;

#if PG_VERSION_NUM >= 190000
	/* the state of float8_regr_accum has 8 elements from 19 (it also keeps whether the inputs were constant, and then leaves Sxx, Syy and Sxy at zero): not the 6 of the steps here, the function is used */
	if (which == 1)
		return -1;
#endif
	if (known[which] == -2)
	{
		Datum		el[6];
		ArrayType  *in;
		float8	   *res;
		Datum		out;

		for (int i = 0; i < n; i++)
			el[i] = Float8GetDatum(which == 0 ? PROBE1_T[i] : PROBE2_T[i]);
		in = construct_array(el, n, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE);
		out = which == 0 ? DirectFunctionCall2(float8_accum, PointerGetDatum(in), Float8GetDatum(PROBE1_X)) :
			DirectFunctionCall3(float8_regr_accum, PointerGetDatum(in), Float8GetDatum(PROBE2_Y), Float8GetDatum(PROBE2_X));
		res = (float8 *) ARR_DATA_PTR(DatumGetArrayTypeP(out));
		known[which] = -1;
		for (int fm = 0; fm < 2 && known[which] < 0; fm++)
		{
			float8		mine[6];

			memcpy(mine, which == 0 ? PROBE1_T : PROBE2_T, n * sizeof(float8));
			if ((which == 0 ? accum1(mine, PROBE1_X, fm) : accum2(mine, PROBE2_X, PROBE2_Y, fm)) && memcmp(mine, res, n * sizeof(float8)) == 0)
				known[which] = fm;
		}
	}
	return known[which];
}

#ifdef HAVE_INT128
/* the state a group's first row made is the layout the kernel adds to: a count of one and the value and its square in the sums; otherwise the kernel is off */
static void
stat_int_check(GenSt *st, AggAcc *acc, uint32 gid, int64 v)
{
	const StatPoly *s;

	if (st->sint_ok || (acc->nf[gid] & GEN_NULL))
		return;
	s = (const StatPoly *) DatumGetPointer(acc->v[gid]);
	if (s->calcSumX2 && s->N == 1 && s->sumX == (int128) v && s->sumX2 == (int128) v * v)
		st->sint_ok = true;
	else
		st->stat = 0;
}
#endif

/* the tuples of the chunk through the kernel; the caller has switched to the per-tuple memory of the aggregates */
void
gen_stat_chunk(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, const uint64 *ok, int nsel)
{
	const int	nk = st->stat;		/* the check of the int state may turn the kernel off for the chunks that follow */

	for (int j = 0; j < nsel; j++)
	{
		uint32		gid = ex->gid[j];
		float8		x[2] = {0, 0};
		bool		good = true;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		for (int k = 0; k < nk; k++)
			if (!stat_value(st, in, k, j, &x[k]))
			{
				good = false;
				break;
			}
		if (!good)
			continue;				/* the transition functions are strict */
#ifdef HAVE_INT128
		if (st->sint)
		{
			int64		v = (int64) x[0];

			if (st->sint_ok && (acc->nf[gid] & GEN_INIT) && !(acc->nf[gid] & GEN_NULL))
			{
				StatPoly   *s = (StatPoly *) DatumGetPointer(acc->v[gid]);

				s->N++;
				s->sumX += v;
				s->sumX2 += (int128) v * v;
				continue;
			}
			{
				Datum		args[1] = {st->sint == INT4OID ? Int32GetDatum((int32) v) : Int16GetDatum((int16) v)};
				bool		nulls[1] = {false};

				if (!(acc->nf[gid] & GEN_INIT))
					gen_state_init(st, acc, gid);
				gen_advance(ex, st, acc, gid, args, nulls);
				ResetExprContext(ex->lutcx);
				stat_int_check(st, acc, gid, v);
			}
			continue;
		}
#endif
		if (!(acc->nf[gid] & GEN_INIT))
			gen_state_init(st, acc, gid);
		if (isfinite(x[0]) && (nk == 1 || isfinite(x[1])))
		{
			float8	   *t = (float8 *) ARR_DATA_PTR((ArrayType *) DatumGetPointer(acc->v[gid]));

			if (nk == 1 ? accum1(t, x[0], st->fma) : accum2(t, x[1], x[0], st->fma))
				continue;
		}
		{
			Datum		args[2] = {Float8GetDatum(x[0]), Float8GetDatum(x[1])};
			bool		nulls[2] = {false, false};

			gen_advance(ex, st, acc, gid, args, nulls);
			ResetExprContext(ex->lutcx);
		}
	}
}
