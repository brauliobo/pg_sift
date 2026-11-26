/*
 * col/v1_engine/store_ops_preagg.c - SiftAggV6 answering the stored units of a Format 1 index or sift_table from their pre-aggregates (col/v1/store_ops.h SiftV1UnitSink).
 *
 * The source offers each unit it is about to read. A unit is answered when the statement is one the cubes hold (no key, or one key that is a dimension of the unit's histogram, and aggregates that are
 * counts, sums (of a leaf or of a line of one), least and greatest of the leaves the histogram has measures of, or of the zone of the unit when there is no key, and the distinct values of
 * a histogram dimension), the unit stands for exactly the rows the snapshot sees (stored, no
 * dead row, every heap page all-visible: SiftV1ReaderOps.unit_exact) and its zones decide every test of the statement for all its rows (or for none: it adds nothing). Any other unit is read as it
 * was, rows and visibility checks included, so an answer is the scan's.
 */
/* check-memory: allow-allocs every array here follows the keys, aggregates and tests of one statement or the groups of one unit, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/vexpr/str.h"
#include "col/source_fold.h"
#include "col/v1/store_ops.h"
#include "col/v1_engine/store_ops_preagg_ext.h"
#include "core/memtrack.h"

typedef enum Verdict
{
	V_NONE,						/* no row of the unit passes the test */
	V_SOME,						/* the zones do not tell */
	V_ALL						/* every row passes */
} Verdict;

typedef enum PredKind
{
	PK_NULL,
	PK_NOTNULL,
	PK_CMP,						/* leaf op val */
	PK_ANY						/* leaf = ANY (set) */
} PredKind;

typedef enum CmpOp
{
	OP_LT = 1,
	OP_LE,
	OP_EQ,
	OP_GE,
	OP_GT,
	OP_NE
} CmpOp;

typedef struct Pred
{
	PredKind	kind;
	int			leaf;
	CmpOp		op;
	int64		val;
	int			n;
	int64	   *set;
} Pred;

typedef struct CubeAgg
{
	const SiftV6Agg *a;
	int			leaf;			/* -1: count(*) */
	bool		is_float;
	int			slot;			/* the measure of the histogram that holds the leaf */
	bool		linear;			/* the sum or average of a line of the leaf */
	SiftV6Linear lin;
} CubeAgg;

typedef struct SiftV1Cube
{
	SiftV1UnitSink sink;
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	int			nkeys;
	int			key_leaf;
	bool		key_text;
	int			dist_leaf;		/* no key: the leaf COUNT(DISTINCT) reads (its histogram gives the values), -1: none */
	bool		dist_text;
	CubeAgg    *agg;
	int			npred;
	Pred	   *pred;
	SiftSrcRollupNeed need;
	V6Acc	   *acc;
	void	   *kdata;
	uint64		kvalid[SIFT_VEC_WORDS];
	SiftVec		kvec;
	SiftVec    *kvp[1];
} SiftV1Cube;

static bool
int_like(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

/* a constant of the type of the column, or of another integer width, is the integer the unit stores for it */
static bool
comparable(Oid vartype, Oid consttype)
{
	return (int_like(vartype) && int_like(consttype)) || (vartype == consttype && (vartype == DATEOID || vartype == TIMESTAMPOID || vartype == TIMESTAMPTZOID || vartype == BOOLOID));
}

static int64
datum_int(Oid type, Datum d)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		case BOOLOID:
			return DatumGetBool(d);
		default:
			return DatumGetInt64(d);
	}
}

static CmpOp
cmp_op(Oid opno)
{
	static const char *const names[] = {"<", "<=", "=", ">=", ">", "<>"};
	const char *name = get_opname(opno);

	for (int i = 0; name != NULL && i < lengthof(names); i++)
		if (strcmp(name, names[i]) == 0)
			return (CmpOp) (i + 1);
	return 0;
}

static const SiftSrcLeaf *
qual_leaf(Relation host, const SiftSrcLayout *lay, int heap_att)
{
	if (host->rd_index == NULL)
		return sift_v1_att_leaf(lay, heap_att);
	for (int i = 0; i < host->rd_index->indnatts; i++)
		if (host->rd_index->indkey.values[i] == heap_att)
			return sift_v1_att_leaf(lay, i + 1);
	return NULL;
}

static bool
leaf_plain(const SiftSrcLeaf *l)
{
	return l != NULL && !l->wide && !l->lossy && l->typid != NUMERICOID;
}

static bool
leaf_int(const SiftSrcLeaf *l)
{
	return leaf_plain(l) && (l->vtype == SCV_I32 || l->vtype == SCV_I64);
}

static const Var *
heap_var(const Node *n)
{
	const Var  *v = (const Var *) n;

	return n != NULL && IsA(n, Var) && v->varno == 1 && v->varlevelsup == 0 && v->varattno > 0 ? v : NULL;
}

static bool
set_of(const Const *k, Oid vartype, Pred *p)
{
	ArrayType  *arr = DatumGetArrayTypeP(k->constvalue);
	const Oid	elem = ARR_ELEMTYPE(arr);
	int16		len;
	bool		byval;
	char		align;
	Datum	   *vals;
	bool	   *nulls;
	int			n;

	if (k->constisnull || !comparable(vartype, elem))
		return false;
	get_typlenbyvalalign(elem, &len, &byval, &align);
	deconstruct_array(arr, elem, len, byval, align, &vals, &nulls, &n);
	p->set = palloc(Max(n, 1) * sizeof(int64));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			p->set[p->n++] = datum_int(elem, vals[i]);
	return true;
}

/* The test a clause of the statement makes on one leaf, in the terms the zone of a unit decides; false for any other clause */
static bool
pred_of(Relation host, const SiftSrcLayout *lay, const Node *clause, Pred *p)
{
	const Var  *var;
	const SiftSrcLeaf *l;

	*p = (Pred) {0};
	if (IsA(clause, NullTest))
	{
		const NullTest *nt = (const NullTest *) clause;

		if (nt->argisrow || (var = heap_var((const Node *) nt->arg)) == NULL || (l = qual_leaf(host, lay, var->varattno)) == NULL || !leaf_plain(l))
			return false;
		*p = (Pred) {.kind = nt->nulltesttype == IS_NULL ? PK_NULL : PK_NOTNULL,.leaf = l->id};
		return true;
	}
	if (IsA(clause, OpExpr) && list_length(((const OpExpr *) clause)->args) == 2)
	{
		const OpExpr *op = (const OpExpr *) clause;
		const Const *k = (const Const *) lsecond(op->args);

		if ((var = heap_var(linitial(op->args))) == NULL || !IsA(k, Const) || k->constisnull || !comparable(var->vartype, k->consttype) || (p->op = cmp_op(op->opno)) == 0 ||
			(l = qual_leaf(host, lay, var->varattno)) == NULL || !leaf_int(l))
			return false;
		p->kind = PK_CMP;
		p->leaf = l->id;
		p->val = datum_int(k->consttype, k->constvalue);
		return true;
	}
	if (IsA(clause, ScalarArrayOpExpr))
	{
		const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) clause;
		const Const *k = (const Const *) lsecond(sa->args);

		if (!sa->useOr || cmp_op(sa->opno) != OP_EQ || list_length(sa->args) != 2 || (var = heap_var(linitial(sa->args))) == NULL || !IsA(k, Const) ||
			(l = qual_leaf(host, lay, var->varattno)) == NULL || !leaf_int(l) || !set_of(k, var->vartype, p))
			return false;
		p->kind = PK_ANY;
		p->leaf = l->id;
		return true;
	}
	return false;
}

/* ---- what the zone of a unit says of a test ---- */

static bool
in_set(const Pred *p, int64 v)
{
	for (int i = 0; i < p->n; i++)
		if (p->set[i] == v)
			return true;
	return false;
}

static Verdict
cmp_verdict(const Pred *p, const SiftSrcUnitAgg *z)
{
	const int64 v = p->val;
	bool		none = false,
				all = false;

	switch (p->op)
	{
		case OP_LT:
			none = z->min >= v;
			all = z->max < v;
			break;
		case OP_LE:
			none = z->min > v;
			all = z->max <= v;
			break;
		case OP_EQ:
			none = v < z->min || v > z->max;
			all = z->min == z->max && z->min == v;
			break;
		case OP_GE:
			none = z->max < v;
			all = z->min >= v;
			break;
		case OP_GT:
			none = z->max <= v;
			all = z->min > v;
			break;
		case OP_NE:
			none = z->min == z->max && z->min == v;
			all = v < z->min || v > z->max;
			break;
	}
	return none ? V_NONE : all && z->nnull == 0 ? V_ALL : V_SOME;
}

static Verdict
any_verdict(const Pred *p, const SiftSrcUnitAgg *z)
{
	bool		none = true;

	for (int i = 0; i < p->n; i++)
		none &= p->set[i] < z->min || p->set[i] > z->max;
	return none ? V_NONE : z->nnull == 0 && z->min == z->max && in_set(p, z->min) ? V_ALL : V_SOME;
}

static Verdict
pred_verdict(const Pred *p, const SiftSrcUnitAgg *z)
{
	if (p->kind == PK_NULL)
		return z->nnull == z->nvals ? V_ALL : z->nnull == 0 ? V_NONE : V_SOME;
	if (p->kind == PK_NOTNULL)
		return z->nnull == 0 ? V_ALL : z->nnull == z->nvals ? V_NONE : V_SOME;
	if (z->nnull == z->nvals)
		return V_NONE;
	if (!z->has_minmax)
		return V_SOME;
	return p->kind == PK_CMP ? cmp_verdict(p, z) : any_verdict(p, z);
}

/* What the tests of the statement say of every row of the unit: V_NONE when one of them passes none */
static Verdict
unit_verdict(const SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	Verdict		all = V_ALL;

	for (int i = 0; i < c->npred; i++)
	{
		SiftSrcUnitAgg z;
		Verdict		v;

		if (!rd->unit_agg(src, unit, c->pred[i].leaf, &z) || z.nvals != unit->nrow)
			return V_SOME;
		if ((v = pred_verdict(&c->pred[i], &z)) == V_NONE)
			return V_NONE;
		all = Min(all, v);
	}
	return all;
}

/* ---- cells to states ---- */

static void
acc_sum(const CubeAgg *g, int64 n, SiftSrcI128 isum, double fsum, V6Acc *out)
{
	out->n = n;
	if (n == 0)
		return;
	if (g->is_float)
		*out = v6g_acc_of_f64_sum(fsum, 0, n);
	else
		v6acc_set_i128(out, (SiftI128) {(uint64) isum, (int64) (isum >> 64)});
}

static void
acc_extreme(const CubeAgg *g, int64 n, int64 imin, int64 imax, double fmin, double fmax, V6Acc *out)
{
	const bool	min = g->a->kind == SV6_MIN;

	out->n = n;
	if (n == 0)
		return;
	if (g->is_float)
		v6acc_set_f64(out, min ? fmin : fmax);
	else
		out->v0 = (uint64) (min ? imin : imax);
}

/* The state of the unit's whole rows for aggregate g from the zone of its leaf; false when the unit has no such zone */
static bool
global_acc(const SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, const CubeAgg *g, V6Acc *out)
{
	SiftSrcUnitAgg z;
	int64		n;

	*out = (V6Acc) {0};
	if (g->a->kind == SV6_COUNT_STAR)
	{
		out->n = unit->nrow;
		return true;
	}
	if (g->a->kind == SV6_COUNT_DISTINCT)
		return true;
	if (!rd->unit_agg(src, unit, g->leaf, &z) || z.nvals != unit->nrow || z.loose)
		return false;
	n = z.nvals - z.nnull;
	if (g->linear)
		return (n == 0 || (z.has_sums && z.has_minmax)) && sift_v1_line_acc(&g->lin, n, z.sum, z.min, z.max, out);
	switch (g->a->kind)
	{
		case SV6_COUNT:
			out->n = n;
			return true;
		case SV6_SUM:
		case SV6_AVG:
			if (n > 0 && !g->is_float && !z.has_sums)
				return false;
			acc_sum(g, n, z.sum, z.sum_f, out);
			return true;
		default:
			if (n > 0 && !z.has_minmax)
				return false;
			acc_extreme(g, n, z.min, z.max, z.min_f, z.max_f, out);
			return true;
	}
}

static void
merge(SiftV1Cube *c, int n)
{
	c->kvec.len = (uint16) n;
	sift_v6_group_merge_cells(c->group, c->kvp, c->acc, (uint16) n);
}

static bool
fold_global(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftSrcHist h;

	if (c->dist_leaf >= 0 && (!rd->unit_hist(src, unit, c->dist_leaf, &(SiftSrcRollupNeed) {0}, &h) || (c->dist_text && h.dict == NULL)))
		return false;
	for (int j = 0; j < c->spec->naggs; j++)
		if (!global_acc(c, rd, src, unit, &c->agg[j], &c->acc[j]))
			return false;
	merge(c, 1);
	for (int j = 0; j < c->spec->naggs; j++)
		if (c->agg[j].a->kind == SV6_COUNT_DISTINCT)
			sift_v1_hist_merge_dist(c->group, j, &h, c->dist_text, c->kdata);
	return true;
}

/* ---- the groups of a histogram ---- */

/* Where each aggregate is in the histogram; false when it lacks what the aggregate reads */
static bool
bind_hist(SiftV1Cube *c, const SiftSrcHist *h)
{
	if (c->key_text && h->dict == NULL)
		return false;
	for (int j = 0; j < c->spec->naggs; j++)
	{
		CubeAgg    *g = &c->agg[j];
		int			m = 0;

		g->slot = -1;
		if (g->leaf < 0 || g->leaf == c->key_leaf)
			continue;
		while (m < h->nmeas && h->meas[m] != g->leaf)
			m++;
		if (m == h->nmeas)
			return false;
		g->slot = m;
		if (g->linear && (h->sum[m] == NULL || h->min[m] == NULL || h->max[m] == NULL))
			return false;
		if (g->a->kind == SV6_SUM || g->a->kind == SV6_AVG)
		{
			if (g->is_float ? h->sumf[m] == NULL : h->sum[m] == NULL)
				return false;
		}
		else if ((g->a->kind == SV6_MIN || g->a->kind == SV6_MAX) && (g->is_float || h->min[m] == NULL || h->max[m] == NULL))
			return false;
	}
	return true;
}

/* The states of the aggregates of group r; false when one is not the group's (a line that may leave its type) */
static bool
group_acc(const SiftV1Cube *c, const SiftSrcHist *h, uint32 r, V6Acc *out)
{
	const bool	null_group = h->has_null && r == h->ngroups - 1;

	for (int j = 0; j < c->spec->naggs; j++)
	{
		const CubeAgg *g = &c->agg[j];
		const int	m = g->slot;

		out[j] = (V6Acc) {0};
		if (g->a->kind == SV6_COUNT_STAR)
			out[j].n = h->count[r];
		else if (g->a->kind == SV6_COUNT_DISTINCT)
			continue;
		else if (g->linear && !sift_v1_line_acc(&g->lin, h->nn[m][r], h->sum[m][r], h->min[m][r], h->max[m][r], &out[j]))
			return false;
		else if (g->linear)
			continue;
		else if (m < 0)
			out[j].n = null_group ? 0 : h->count[r];
		else if (g->a->kind == SV6_COUNT)
			out[j].n = h->nn[m][r];
		else if (g->a->kind == SV6_MIN || g->a->kind == SV6_MAX)
			acc_extreme(g, h->nn[m][r], h->min[m][r], h->max[m][r], 0, 0, &out[j]);
		else
			acc_sum(g, h->nn[m][r], g->is_float ? 0 : h->sum[m][r], g->is_float ? h->sumf[m][r] : 0, &out[j]);
	}
	return true;
}

static void
put_key(SiftV1Cube *c, const SiftSrcHist *h, uint32 r, int row)
{
	const bool	null = h->has_null && r == h->ngroups - 1;

	if (!null)
		c->kvalid[row >> 6] |= UINT64CONST(1) << (row & 63);
	if (c->key_text)
	{
		const SiftSrcDict *d = h->dict;

		((SiftStrView *) c->kdata)[row] = null ? (SiftStrView) {0} : sift_vstr_make(d->bytes + d->off[h->key[r]], d->off[h->key[r] + 1] - d->off[h->key[r]]);
	}
	else
		((int64 *) c->kdata)[row] = null ? 0 : h->key[r];
}

static void
batch_start(SiftV1Cube *c, bool any_null)
{
	memset(c->kvalid, 0, sizeof(c->kvalid));
	c->kvec = (SiftVec) {.data = c->kdata,.dense = true,.type = c->key_text ? SVEC_STRVIEW : SVEC_I64,.all_valid = !any_null,.valid = any_null ? c->kvalid : NULL};
}

static bool
fold_hist(SiftV1Cube *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftSrcHist h;
	int			n = 0;

	if (!rd->unit_hist(src, unit, c->key_leaf, &c->need, &h) || !bind_hist(c, &h))
		return false;
	for (uint32 r = 0; r < h.ngroups; r++)
		if (h.count[r] > 0 && !group_acc(c, &h, r, c->acc))
			return false;
	batch_start(c, h.has_null);
	for (uint32 r = 0; r < h.ngroups; r++)
	{
		if (h.count[r] == 0)
			continue;
		put_key(c, &h, r, n);
		group_acc(c, &h, r, &c->acc[n * c->spec->naggs]);
		if (++n == SIFT_VEC_LEN)
		{
			merge(c, n);
			batch_start(c, h.has_null);
			n = 0;
		}
	}
	if (n > 0)
		merge(c, n);
	for (int j = 0; j < c->spec->naggs; j++)
		if (c->agg[j].a->kind == SV6_COUNT_DISTINCT)
			sift_v1_hist_merge_dist(c->group, j, &h, c->key_text, c->kdata);
	return true;
}

/* ---- the unit ---- */

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	SiftV1Cube *c = arg;
	Verdict		v;
	bool		done;

	if (unit->nsu != 0 || !rd->unit_exact(src, unit) || !sift_v6_group_takes_cells(c->group))
		return false;
	if ((v = unit_verdict(c, rd, src, unit)) == V_SOME)
		return false;
	done = v == V_NONE || (c->nkeys == 0 ? fold_global(c, rd, src, unit) : fold_hist(c, rd, src, unit));
	c->stats->grans_preagg += done;
	return done;
}

/* ---- the statement ---- */

/* COUNT(DISTINCT leaf) is the dimension of a histogram: of the key, or of one leaf under no key */
static bool
bind_dist(SiftV1Cube *c, const SiftSrcLeaf *l)
{
	if (l->vtype != SCV_CODE && l->vtype != SCV_I32 && l->vtype != SCV_I64)
		return false;
	if (c->nkeys > 0)
		return l->id == c->key_leaf;
	if (c->dist_leaf >= 0 && c->dist_leaf != l->id)
		return false;
	c->dist_leaf = l->id;
	c->dist_text = l->vtype == SCV_CODE;
	return true;
}

static bool
bind_aggs(SiftV1Cube *c, const SiftSrcLayout *lay)
{
	c->dist_leaf = -1;
	for (int j = 0; j < c->spec->naggs; j++)
	{
		const SiftV6Agg *a = &c->spec->agg[j];
		const SiftSrcLeaf *l = a->kind == SV6_COUNT_STAR ? NULL : sift_v1_att_leaf(lay, a->arg.attno);
		const SiftV6Class cls = sift_v6_class(a->arg.typid);
		CubeAgg    *g = &c->agg[j];

		*g = (CubeAgg) {.a = a,.leaf = l == NULL ? -1 : l->id,.is_float = cls == SV6C_F64,.slot = -1};
		g->linear = sift_v6_agg_linear(c->spec, a, &g->lin);
		if (a->kind != SV6_COUNT_STAR && (!leaf_plain(l) || (cls == SV6C_F64) != (l->vtype == SCV_F64) || (cls == SV6C_TEXT && l->vtype != SCV_CODE)))
			return false;
		if (a->kind == SV6_COUNT_DISTINCT)
		{
			if (!bind_dist(c, l))
				return false;
			continue;
		}
		if ((g->linear && !leaf_int(l)) || ((a->kind == SV6_SUM || a->kind == SV6_AVG) && cls == SV6C_F64))
			return false;
		if (c->nkeys > 0 && l != NULL && l->id == c->key_leaf && a->kind != SV6_COUNT)
			return false;
		if ((a->kind == SV6_MIN || a->kind == SV6_MAX || a->kind == SV6_SUM || a->kind == SV6_AVG) && cls == SV6C_TEXT)
			return false;
		if (c->nkeys > 0 && l != NULL && l->id != c->key_leaf)
		{
			int			m = 0;

			while (m < c->need.nmeas && c->need.meas[m].leaf != l->id)
				m++;
			if (m == SIFT_SRC_MAX_MEAS)
				return false;
			c->need.meas[m].leaf = l->id;
			c->need.meas[m].sums |= a->kind == SV6_SUM || a->kind == SV6_AVG;
			c->need.meas[m].minmax |= a->kind == SV6_MIN || a->kind == SV6_MAX || g->linear;
			c->need.nmeas = Max(c->need.nmeas, m + 1);
		}
	}
	return true;
}

static bool
bind_key(SiftV1Cube *c, const SiftSrcLayout *lay)
{
	const SiftV6Col *k = &c->spec->key[0];
	const SiftSrcLeaf *l = sift_v1_att_leaf(lay, k->attno);

	c->key_text = sift_v6_class(k->typid) == SV6C_TEXT;
	if (!leaf_plain(l) || sift_v6_class(k->typid) == SV6C_BOOL || (c->key_text ? l->vtype != SCV_CODE : (l->vtype != SCV_I32 && l->vtype != SCV_I64)))
		return false;
	c->key_leaf = l->id;
	return true;
}

static bool
bind_preds(SiftV1Cube *c, Relation host, const SiftSrcLayout *lay, const List *quals)
{
	c->pred = palloc0(Max(list_length(quals), 1) * sizeof(Pred));
	foreach_ptr(Node, q, (List *) quals)
		if (!pred_of(host, lay, q, &c->pred[c->npred++]))
			return false;
	return true;
}

SiftV1UnitSink *
sift_v1_cube_begin(const SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx)
{
	const SiftV1ReaderOps *rd;
	const SiftSrcLayout *lay;
	SiftV1Cube *c;

	if (a->group == NULL || a->emit != NULL || a->star != NULL || a->gate != NULL || a->stats == NULL || a->spec->nkeys > 1 || !sift_v6_spec_cubeable(a->spec) ||
		(scan != NULL && (scan->keep != NULL || scan->bridge != NULL || scan->ntypes > 0 || scan->tid_range)))
		return NULL;
	rd = sift_v1_reader_of(a->host->rd_index == NULL);
	lay = rd->layout(a->host);
	c = MemoryContextAllocZero(cx, sizeof(SiftV1Cube));
	c->spec = a->spec;
	c->group = a->group;
	c->stats = a->stats;
	c->nkeys = a->spec->nkeys;
	c->agg = MemoryContextAlloc(cx, a->spec->naggs * sizeof(CubeAgg));
	c->acc = MemoryContextAlloc(cx, (Size) Max(SIFT_VEC_LEN, 1) * a->spec->naggs * sizeof(V6Acc));
	c->kdata = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	c->kvp[0] = &c->kvec;
	c->sink = (SiftV1UnitSink) {.offer = offer,.arg = c};
	return (c->nkeys == 0 || bind_key(c, lay)) && bind_aggs(c, lay) && bind_preds(c, a->host, lay, a->quals) ? &c->sink : NULL;
}
