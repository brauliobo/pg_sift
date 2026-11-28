/*
 * col/v1_engine/store_ops_preagg_pred.c - the tests of a statement as the pre-aggregates of a Format 1 unit decide them (col/v1_engine/store_ops_preagg_pred.h): on the zone of a leaf, on a group of
 * the histogram of its dimension, on a calendar cell of a date or timestamp leaf. An OR of ANDs of the tests of one leaf is decided as a whole: inside one of its ranges a row passes, outside all of
 * them it does not.
 */
/* check-memory: allow-allocs every array here follows the tests of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#include "agg/expr/pg.h"

#include "col/v1/preagg/preagg.h"
#include "col/v1/store_ops.h"
#include "col/v1_engine/store_ops_cal.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

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

static SiftV1CmpOp
cmp_op(Oid opno)
{
	static const char *const names[] = {"<", "<=", "=", ">=", ">", "<>"};
	const char *name = get_opname(opno);

	for (int i = 0; name != NULL && i < lengthof(names); i++)
		if (strcmp(name, names[i]) == 0)
			return (SiftV1CmpOp) (i + 1);
	return 0;
}

/* The leaf of a column of the heap a clause tests; NULL for a virtual attribute (a leaf of a document, SIFT_V6_JLEAF_VATT and up), which no cube has as a dimension */
static const SiftSrcLeaf *
qual_leaf(Relation host, const SiftSrcLayout *lay, int heap_att)
{
	if (host->rd_index == NULL)
		return heap_att <= RelationGetNumberOfAttributes(host) ? sift_v1_att_leaf(lay, heap_att) : NULL;
	for (int i = 0; i < host->rd_index->indnatts; i++)
		if (host->rd_index->indkey.values[i] == heap_att)
			return sift_v1_att_leaf(lay, i + 1);
	return NULL;
}

static const Var *
heap_var(const Node *n)
{
	const Var  *v = (const Var *) n;

	return n != NULL && IsA(n, Var) && v->varno == 1 && v->varlevelsup == 0 && v->varattno > 0 ? v : NULL;
}

static bool
set_of(const Const *k, Oid vartype, SiftV1Pred *p)
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

/* A string leaf a clause tests by its strings: = and <> by their bytes under a deterministic collation, the order of a comparison by the collation */
static bool
str_test(const SiftSrcLeaf *l, Oid vartype, Oid consttype, Oid collid, SiftV1CmpOp op)
{
	return sift_v1_leaf_plain(l) && l->vtype == SCV_CODE && !l->astext && vartype == TEXTOID && consttype == TEXTOID && OidIsValid(collid) &&
		((op != SV1O_EQ && op != SV1O_NE) || get_collation_isdeterministic(collid));
}

static bool
str_set_of(const Const *k, SiftV1Pred *p)
{
	Datum	   *vals;
	bool	   *nulls;
	int			n;

	deconstruct_array_builtin(DatumGetArrayTypeP(k->constvalue), TEXTOID, &vals, &nulls, &n);
	p->str = palloc(Max(n, 1) * sizeof(text *));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			p->str[p->n++] = DatumGetTextPP(vals[i]);
	return true;
}

/* The test a clause of the statement makes on one leaf, in the terms the zone of a unit or the groups of a histogram decide; false for any other clause */
static bool
leaf_pred_of(Relation host, const SiftSrcLayout *lay, const Node *clause, SiftV1Pred *p)
{
	const Var  *var;
	const SiftSrcLeaf *l;

	*p = (SiftV1Pred) {0};
	if (IsA(clause, NullTest))
	{
		const NullTest *nt = (const NullTest *) clause;

		if (nt->argisrow || (var = heap_var((const Node *) nt->arg)) == NULL || (l = qual_leaf(host, lay, var->varattno)) == NULL || !sift_v1_leaf_plain(l))
			return false;
		*p = (SiftV1Pred) {.kind = nt->nulltesttype == IS_NULL ? SV1P_NULL : SV1P_NOTNULL,.leaf = l->id};
		return true;
	}
	if (IsA(clause, OpExpr) && list_length(((const OpExpr *) clause)->args) == 2)
	{
		const OpExpr *op = (const OpExpr *) clause;
		const Const *k = (const Const *) lsecond(op->args);

		if ((var = heap_var(linitial(op->args))) == NULL || !IsA(k, Const) || k->constisnull || (p->op = cmp_op(op->opno)) == 0 ||
			(l = qual_leaf(host, lay, var->varattno)) == NULL)
			return false;
		p->kind = SV1P_CMP;
		p->leaf = l->id;
		if (str_test(l, var->vartype, k->consttype, op->inputcollid, p->op))
		{
			p->collid = op->inputcollid;
			p->str = palloc(sizeof(text *));
			p->str[p->n++] = DatumGetTextPP(k->constvalue);
			return true;
		}
		if (!comparable(var->vartype, k->consttype) || !sift_v1_leaf_int(l))
			return false;
		p->val = datum_int(k->consttype, k->constvalue);
		return true;
	}
	if (IsA(clause, ScalarArrayOpExpr))
	{
		const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) clause;
		const Const *k = (const Const *) lsecond(sa->args);

		if (!sa->useOr || cmp_op(sa->opno) != SV1O_EQ || list_length(sa->args) != 2 || (var = heap_var(linitial(sa->args))) == NULL || !IsA(k, Const) || k->constisnull ||
			(l = qual_leaf(host, lay, var->varattno)) == NULL)
			return false;
		p->kind = SV1P_ANY;
		p->leaf = l->id;
		if (str_test(l, var->vartype, get_element_type(k->consttype), sa->inputcollid, SV1O_EQ))
			return str_set_of(k, p);
		return sift_v1_leaf_int(l) && set_of(k, var->vartype, p);
	}
	return false;
}

/* A test of one date or timestamp leaf through calendar calls, which its groups and the cells of a grain it keeps its value over decide */
static bool
cal_pred_of(Relation host, const SiftSrcLayout *lay, const Node *clause, SiftV1Pred *p)
{
	List	   *vars;
	const Var  *var;
	const SiftSrcLeaf *l;

	if (!sift_v6_pgexpr_vars((const Expr *) clause, 1, 0, &vars) || list_length(vars) != 1 || (var = heap_var(linitial(vars))) == NULL ||
		(l = qual_leaf(host, lay, var->varattno)) == NULL || !sift_v1_leaf_int(l))
		return false;
	*p = (SiftV1Pred) {.kind = SV1P_CAL,.leaf = l->id,.cal = sift_v1_cal_test_of(clause, vars, l->typid)};
	return p->cal != NULL;
}

/* ---- what the zone of a unit says of a test ---- */

static bool
in_set(const SiftV1Pred *p, int64 v)
{
	for (int i = 0; i < p->n; i++)
		if (p->set[i] == v)
			return true;
	return false;
}

static SiftV1Verdict
cmp_verdict(const SiftV1Pred *p, const SiftSrcUnitAgg *z)
{
	const int64 v = p->val;
	bool		none = false,
				all = false;

	switch (p->op)
	{
		case SV1O_LT:
			none = z->min >= v;
			all = z->max < v;
			break;
		case SV1O_LE:
			none = z->min > v;
			all = z->max <= v;
			break;
		case SV1O_EQ:
			none = v < z->min || v > z->max;
			all = z->min == z->max && z->min == v;
			break;
		case SV1O_GE:
			none = z->max < v;
			all = z->min >= v;
			break;
		case SV1O_GT:
			none = z->max <= v;
			all = z->min > v;
			break;
		case SV1O_NE:
			none = z->min == z->max && z->min == v;
			all = v < z->min || v > z->max;
			break;
	}
	return none ? SV1V_NONE : all && z->nnull == 0 ? SV1V_ALL : SV1V_SOME;
}

static SiftV1Verdict
any_verdict(const SiftV1Pred *p, const SiftSrcUnitAgg *z)
{
	bool		none = true;

	for (int i = 0; i < p->n; i++)
		none &= p->set[i] < z->min || p->set[i] > z->max;
	return none ? SV1V_NONE : z->nnull == 0 && z->min == z->max && in_set(p, z->min) ? SV1V_ALL : SV1V_SOME;
}

static SiftV1Verdict
leaf_verdict(const SiftV1Pred *p, const SiftSrcUnitAgg *z)
{
	if (p->kind == SV1P_CAL)
		return SV1V_SOME;
	if (p->kind == SV1P_NULL)
		return z->nnull == z->nvals ? SV1V_ALL : z->nnull == 0 ? SV1V_NONE : SV1V_SOME;
	if (p->kind == SV1P_NOTNULL)
		return z->nnull == 0 ? SV1V_ALL : z->nnull == z->nvals ? SV1V_NONE : SV1V_SOME;
	if (z->nnull == z->nvals)
		return SV1V_NONE;
	if (!z->has_minmax || p->str != NULL)
		return SV1V_SOME;
	return p->kind == SV1P_CMP ? cmp_verdict(p, z) : any_verdict(p, z);
}

/* Whether the string of a group is one of the strings of a test */
static bool
str_in(const SiftV1Pred *p, const SiftSrcHist *h, uint32 r)
{
	const SiftSrcDict *d = h->dict;
	const uint32 len = d->off[h->key[r] + 1] - d->off[h->key[r]];

	for (int i = 0; i < p->n; i++)
		if (VARSIZE_ANY_EXHDR(p->str[i]) == len && memcmp(VARDATA_ANY(p->str[i]), d->bytes + d->off[h->key[r]], len) == 0)
			return true;
	return false;
}

/* Whether a value that compares c (< 0, 0, > 0) to the value of the test passes its operator */
static bool
cmp_holds(SiftV1CmpOp op, int c)
{
	switch (op)
	{
		case SV1O_LT:
			return c < 0;
		case SV1O_LE:
			return c <= 0;
		case SV1O_EQ:
			return c == 0;
		case SV1O_GE:
			return c >= 0;
		case SV1O_GT:
			return c > 0;
		case SV1O_NE:
			return c != 0;
	}
	return false;
}

/* The order of the string of group r against the string of the test, by its collation */
static int
str_order(const SiftV1Pred *p, const SiftSrcHist *h, uint32 r)
{
	const SiftSrcDict *d = h->dict;

	return varstr_cmp(d->bytes + d->off[h->key[r]], (int) (d->off[h->key[r] + 1] - d->off[h->key[r]]), VARDATA_ANY(p->str[0]), (int) VARSIZE_ANY_EXHDR(p->str[0]), p->collid);
}

static bool
leaf_passes(const SiftV1Pred *p, const SiftSrcHist *h, uint32 r)
{
	if (p->kind == SV1P_CAL)
		return sift_v1_cal_test_passes(p->cal, h, r);
	if (h->has_null && r == h->ngroups - 1)
		return p->kind == SV1P_NULL;
	if (p->kind == SV1P_NULL || p->kind == SV1P_NOTNULL)
		return p->kind == SV1P_NOTNULL;
	if (p->str != NULL && (p->kind == SV1P_ANY || p->op == SV1O_EQ || p->op == SV1O_NE))
		return str_in(p, h, r) == (p->kind == SV1P_ANY || p->op == SV1O_EQ);
	if (p->str != NULL)
		return cmp_holds(p->op, str_order(p, h, r));
	if (p->kind == SV1P_ANY)
		return in_set(p, h->key[r]);
	return cmp_holds(p->op, (h->key[r] > p->val) - (h->key[r] < p->val));
}


/* the tests of a disjunct of an OR: an AND of tests of the leaf, or one */
static bool
and_of(Relation host, const SiftSrcLayout *lay, const Node *clause, SiftV1Pred *p)
{
	const List *args;

	if (!is_andclause(clause))
		return leaf_pred_of(host, lay, clause, p);
	args = ((const BoolExpr *) clause)->args;
	*p = (SiftV1Pred) {.kind = SV1P_AND,.kid = palloc0(list_length(args) * sizeof(SiftV1Pred))};
	foreach_ptr(Node, arg, args)
		if (!leaf_pred_of(host, lay, arg, &p->kid[p->n++]) || p->kid[p->n - 1].leaf != p->kid[0].leaf)
			return false;
	p->leaf = p->kid[0].leaf;
	return true;
}

bool
sift_v1_pred_of(Relation host, const SiftSrcLayout *lay, const Node *clause, SiftV1Pred *p)
{
	const List *args;

	if (IsA(clause, RestrictInfo))
		clause = (const Node *) ((const RestrictInfo *) clause)->clause;
	if (!is_orclause(clause))
		return leaf_pred_of(host, lay, clause, p) || cal_pred_of(host, lay, clause, p);
	args = ((const BoolExpr *) clause)->args;
	*p = (SiftV1Pred) {.kind = SV1P_OR,.kid = palloc0(list_length(args) * sizeof(SiftV1Pred))};
	foreach_ptr(Node, arg, args)
		if (!and_of(host, lay, arg, &p->kid[p->n++]) || p->kid[p->n - 1].leaf != p->kid[0].leaf)
			return false;
	p->leaf = p->kid[0].leaf;
	return true;
}

SiftV1Verdict
sift_v1_pred_verdict(const SiftV1Pred *p, const SiftSrcUnitAgg *z)
{
	SiftV1Verdict v = p->kind == SV1P_AND ? SV1V_ALL : SV1V_NONE;

	if (p->kind != SV1P_AND && p->kind != SV1P_OR)
		return leaf_verdict(p, z);
	for (int i = 0; i < p->n; i++)
	{
		const SiftV1Verdict k = sift_v1_pred_verdict(&p->kid[i], z);

		v = p->kind == SV1P_AND ? Min(v, k) : Max(v, k);
	}
	return v;
}

bool
sift_v1_pred_passes(const SiftV1Pred *p, const SiftSrcHist *h, uint32 r)
{
	if (p->kind != SV1P_AND && p->kind != SV1P_OR)
		return leaf_passes(p, h, r);
	for (int i = 0; i < p->n; i++)
		if (sift_v1_pred_passes(&p->kid[i], h, r) == (p->kind == SV1P_OR))
			return p->kind == SV1P_OR;
	return p->kind == SV1P_AND;
}

/* the first day of the cell after the one that starts on day (days since 2000-01-01) */
static int64
next_cell(int grain, int64 day)
{
	return grain == SIFT_CAL_DAY ? day + 1 : sift_cpagg_cal_cell(grain, day + (grain == SIFT_CAL_MONTH ? 31 : 366));
}

SiftV1Verdict
sift_v1_pred_cell(const SiftV1Pred *p, Oid typid, const SiftSrcHist *h, uint32 r)
{
	const int64 unit = typid == DATEOID ? 1 : USECS_PER_DAY;
	const SiftSrcUnitAgg z = {.nvals = h->count[r],.has_minmax = true,.min = h->key[r] * unit,.max = next_cell(h->grain, h->key[r]) * unit - 1};

	if (p->kind == SV1P_CAL)
		return sift_v1_cal_test_cell(p->cal, h, r);
	return sift_v1_pred_verdict(p, &z);
}
