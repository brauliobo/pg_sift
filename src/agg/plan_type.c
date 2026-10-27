/*
 * agg/plan_type.c - predicates on the JSON type, the wide image and the ctid of a leaf
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#if __has_include("col/col.h")
#include "col/am.h"
#endif
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* the SIFT_SRC_JT_* bits of the type names jsonb_typeof gives (a JSON null is 'null'); another string has none */
static uint32
jtype_bits(List *names)
{
	static const struct
	{
		const char *name;
		uint32		bit;
	}			known[] = {
		{"string", SIFT_SRC_JT_STRING}, {"number", SIFT_SRC_JT_NUMBER}, {"boolean", SIFT_SRC_JT_BOOLEAN},
		{"object", SIFT_SRC_JT_OBJECT}, {"array", SIFT_SRC_JT_ARRAY}, {"null", SIFT_SRC_JT_NULL}
	};
	uint32		bits = 0;

	foreach_ptr(String, s, names)
		for (int t = 0; t < lengthof(known); t++)
			if (strcmp(strVal(s), known[t].name) == 0)
				bits |= known[t].bit;
	return bits;
}

static bool
typeof_type(Oid typid)
{
	return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == FLOAT4OID || typid == FLOAT8OID ||
		typid == NUMERICOID || typid == TEXTOID;
}

/* the leaf of the jsonb value `x->'k'` whose chunks know the JSON type of the path: a scalar of a type a JSON value of one type fills, or the text of any scalar (astext), whose chunks keep the number and boolean kinds */
int
typeof_leaf(AggCtx *c, Node *e)
{
	JsonRef		r = {0};

	if (!json_access(c, e, &r) || r.as_text || r.att == 0 || r.path == NIL)
		return -1;
	for (int i = 0; i < c->lay->nleaves - c->nvleaf; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' &&
			paths_equal(l->path, r.path) && typeof_type(l->typid))
			return i;
	}
	return -1;
}

bool
source_can_types(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_types(get_rel_relam(c->indexoid), true) : sift_colsrc_can_types(get_rel_relam(agg_src_table(c)), false);
}

/* the constant text operand of `jsonb_typeof(..) = 'a'` as a name list, with the call in *call; either side */
static bool
typeof_cmp_operands(OpExpr *op, Node **call, List **names)
{
	Node	   *l = strip_relabel(linitial(op->args)),
			   *r = strip_relabel(lsecond(op->args));

	if (func_of(l) != F_JSONB_TYPEOF)
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (!IsA(r, Const) || ((Const *) r)->constisnull)
		return false;
	*call = l;
	*names = list_make1(makeString(TextDatumGetCString(((Const *) r)->constvalue)));
	return true;
}

/*
 * A test of `jsonb_typeof(x->'k')` as the SIFT_SRC_JT_* types it keeps: `= 'a'` and `IN (..)`, their complements `<>` and `NOT IN` and IS [NOT]
 * NULL. The type is NULL where the path has no value, which no comparison keeps, so a complement is the other types but none. *call is the
 * jsonb_typeof call. False for any other test, and for a type no row has.
 */
static bool
typeof_test(Node *n, Node **call, uint32 *types)
{
	Oid			eq = TextEqualOperator,
				ne = get_negator(TextEqualOperator);
	List	   *names = NIL;
	bool		negate;

	if (IsA(n, NullTest))
	{
		*call = strip_relabel((Node *) ((NullTest *) n)->arg);
		*types = SIFT_SRC_JT_NONE;
		negate = ((NullTest *) n)->nulltesttype == IS_NOT_NULL;
	}
	else if (IsA(n, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;

		if (list_length(sa->args) != 2 || !((sa->opno == eq && sa->useOr) || (sa->opno == ne && !sa->useOr)) || !path_keys(lsecond(sa->args), &names))
			return false;
		*call = strip_relabel(linitial(sa->args));
		*types = jtype_bits(names);
		negate = !sa->useOr;
	}
	else if (IsA(n, OpExpr) && (((OpExpr *) n)->opno == eq || ((OpExpr *) n)->opno == ne) && list_length(((OpExpr *) n)->args) == 2)
	{
		if (!typeof_cmp_operands((OpExpr *) n, call, &names))
			return false;
		*types = jtype_bits(names);
		negate = ((OpExpr *) n)->opno == ne;
	}
	else
		return false;
	if (negate)
		*types = SIFT_SRC_JT_ALL & ~SIFT_SRC_JT_NONE & ~*types;
	return *types != 0;
}

/*
 * Does a test of these types (SIFT_SRC_JT_*) tell what only the kinds of a chunk say: a JSON null from a missing key (the types have one of them
 * and not the other), or the types of the values of an astext leaf apart (some of the string, number and boolean types and not all)? Where the
 * leaf has a segment without them (SiftSrcStats.nokinds) the source could only raise the REINDEX error.
 */
static bool
types_need_kinds(uint32 types, bool astext)
{
	uint32		vals = types & (SIFT_SRC_JT_STRING | SIFT_SRC_JT_NUMBER | SIFT_SRC_JT_BOOLEAN);

	return ((types & SIFT_SRC_JT_NONE) != 0) != ((types & SIFT_SRC_JT_NULL) != 0) ||
		(astext && vals != 0 && vals != (SIFT_SRC_JT_STRING | SIFT_SRC_JT_NUMBER | SIFT_SRC_JT_BOOLEAN));
}

/*
 * A test of the JSON type of a leaf's path as a conjunct of the query (typeof_test): the source keeps the rows whose type is one of the types,
 * a stored value by the type of the leaf and the values it could not hold by the type the chunk kept for them (SAP_TYPE). Not as a branch of OR:
 * the reads of the leaf give the values of the other rows as NULL, which only a conjunct that drops those rows makes safe.
 */
SiftAggPred *
compile_typeof(AggCtx *c, Node *n)
{
	Node	   *call;
	uint32		types;
	int			leaf;
	SiftAggPred *p;

	n = strip_relabel(n);
	if (!typeof_test(n, &call, &types) || func_of(call) != F_JSONB_TYPEOF || (leaf = typeof_leaf(c, linitial(func_args(call)))) < 0 ||
		!source_can_types(c) || (c->stats.nokinds[leaf] && types_need_kinds(types, leaf_of(c, leaf)->astext)))
		return NULL;
	p = new_pred(c, SAP_TYPE);
	p->leaf = leaf;
	p->level = leaf_level(c, leaf);
	c->used = bms_add_member(c->used, leaf);
	c->rec_bad = bms_add_member(c->rec_bad, leaf);
	c->txt_bad = bms_add_member(c->txt_bad, leaf);
	p->op = types;
	p->desc = expr_text(c, n);
	return p;
}

/* the types a wide leaf keeps as lanes (all the comparisons run on the lanes), and those it keeps as an image that is the same bytes exactly when the values are equal (= <> IN NOT IN run on it) */
static bool
lane_type(Oid t)
{
	return t == NUMERICOID || t == CASHOID || t == INTERVALOID;
}

/* is an expression of type `expr` the value of a leaf of type `leaf`, or its operand: inet and cidr are one type to the operators (cidr has none) and one image */
static bool
wide_type_is(Oid expr, Oid leaf)
{
	return expr == leaf || ((leaf == INETOID || leaf == CIDROID) && (expr == INETOID || expr == CIDROID));
}

static bool
bytes_equal_type(Oid t)
{
	return t == UUIDOID || t == BYTEAOID || t == INETOID || t == CIDROID || t == MACADDROID || t == MACADDR8OID || t == BITOID || t == VARBITOID;
}

/*
 * The wide leaf an expression is exactly the value of: an attribute or index expression of numeric, money or
 * interval that the index keeps as lanes, or of an image type compared for equality. The other tests of an image leaf are the executor's.
 */
static int
wide_leaf(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	for (int i = 0; att > 0 && i < c->lay->nleaves - c->nvleaf; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && OidIsValid(l->wtypid) && wide_type_is(exprType(e), l->wtypid) &&
			(lane_type(l->wtypid) || bytes_equal_type(l->wtypid)))
			return i;
	}
	return -1;
}

static bool
source_can_wide(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_wide(get_rel_relam(c->indexoid), true) : sift_colsrc_can_wide(get_rel_relam(agg_src_table(c)), false);
}

/* the operator, leaf and operand of `leaf op operand` (or the commuted form), `leaf IN / NOT IN (operands)` and `leaf IS [NOT] NULL` on a wide leaf; false for any other clause */
static bool
wide_test(AggCtx *c, Node *n, int *leaf, int *op, Node **operand, Oid *argtype)
{
	Oid			lt,
				rt;
	Node	   *l,
			   *r;

	*operand = NULL;
	*argtype = InvalidOid;
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow)
	{
		*op = ((NullTest *) n)->nulltesttype == IS_NOT_NULL ? SWT_NOTNULL : SWT_ISNULL;
		return (*leaf = wide_leaf(c, strip_relabel((Node *) ((NullTest *) n)->arg))) >= 0 && lane_type(leaf_of(c, *leaf)->wtypid);
	}
	if (IsA(n, OpExpr) && list_length(((OpExpr *) n)->args) == 2 && (*op = builtin_cmp(((OpExpr *) n)->opno, &lt, &rt)) >= 0)
	{
		l = strip_relabel(linitial(((OpExpr *) n)->args));
		r = strip_relabel(lsecond(((OpExpr *) n)->args));
		if ((*leaf = wide_leaf(c, l)) >= 0 && pseudo_const(r))
			*operand = r;
		else if ((*leaf = wide_leaf(c, r)) >= 0 && pseudo_const(l))
		{
			*operand = l;
			*op = commute(*op);
		}
		else
			return false;
		*argtype = exprType(*operand);
		return wide_type_is(*argtype, leaf_of(c, *leaf)->wtypid) && (lane_type(*argtype) || *op == SCMP_EQ || *op == SCMP_NE);
	}
	if (IsA(n, ScalarArrayOpExpr) && list_length(((ScalarArrayOpExpr *) n)->args) == 2)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;
		int			cmp = builtin_cmp(sa->opno, &lt, &rt);

		if (!((sa->useOr && cmp == SCMP_EQ) || (!sa->useOr && cmp == SCMP_NE)))
			return false;
		l = strip_relabel(linitial(sa->args));
		r = strip_relabel(lsecond(sa->args));
		*op = sa->useOr ? SWT_IN : SWT_NOTIN;
		*operand = r;
		*argtype = rt;
		return (*leaf = wide_leaf(c, l)) >= 0 && pseudo_const(r) && wide_type_is(lt, leaf_of(c, *leaf)->wtypid) && rt == lt;
	}
	return false;
}

/*
 * A test of a wide leaf as a conjunct of the query: the source keeps the rows that pass (SAP_WIDE) and the executor never reads the values, which
 * is why it is only a conjunct: an OR, a CASE, a FILTER or a key would have to read them.
 */
SiftAggPred *
compile_wide(AggCtx *c, Node *n)
{
	int			leaf,
				op;
	Node	   *operand;
	Oid			argtype;
	SiftAggPred *p;

	n = strip_relabel(n);
	if (c->nwide >= SIFT_AGG_MAX_WIDE || !wide_test(c, n, &leaf, &op, &operand, &argtype) || !source_can_wide(c))
		return NULL;
	c->nwide++;
	p = new_pred(c, SAP_WIDE);
	p->leaf = leaf;
	p->level = 0;
	p->op = op;
	p->expr = operand;
	p->argtype = argtype;
	p->desc = expr_text(c, n);
	p->sel = clause_selectivity(c->root, n, c->relid, JOIN_INNER, NULL);
	return p;
}

/* the TID comparison a function is, SiftCmpOp, or -1 (<> keeps no range) */
static int
tid_cmp(Oid fn)
{
	switch (fn)
	{
		case F_TIDEQ:
			return SCMP_EQ;
		case F_TIDLT:
			return SCMP_LT;
		case F_TIDLE:
			return SCMP_LE;
		case F_TIDGT:
			return SCMP_GT;
		case F_TIDGE:
			return SCMP_GE;
		default:
			return -1;
	}
}

static bool
is_ctid(AggCtx *c, Node *n)
{
	Var		   *v = (Var *) n;

	return IsA(n, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno == SelfItemPointerAttributeNumber;
}

/*
 * `ctid op constant` as a conjunct of the query (op: = < <= > >=, either side): the scan reads the superblocks of the range of all the conjuncts and
 * tests the TIDs of the rows of its edges (SAP_TID, sift_aggspec_tid_range). Not as a branch of OR, an expression or a key; not for a partitioned
 * table, whose rows have a TID in each partition.
 */
SiftAggPred *
compile_tid(AggCtx *c, Node *n)
{
	Node	   *l,
			   *r;
	int			op;
	SiftAggPred *p;

	n = strip_relabel(n);
#if __has_include("col/col.h")
	/* the units of a key-cut index are not heap superblocks: a range of them is not a range of the heap */
	if (OidIsValid(c->indexoid) && sift_col_index_keyed(c->indexoid))
		return NULL;
#endif
	if (!IsA(n, OpExpr) || c->nparts > 0 || list_length(((OpExpr *) n)->args) != 2 || (op = tid_cmp(func_of(n))) < 0 ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(agg_src_table(c)), false)))
		return NULL;
	l = strip_relabel(linitial(((OpExpr *) n)->args));
	r = strip_relabel(lsecond(((OpExpr *) n)->args));
	if (is_ctid(c, r))
	{
		Node	   *t = l;

		l = r;
		r = t;
		op = commute(op);
	}
	if (!is_ctid(c, l) || !IsA(r, Const) || ((Const *) r)->constisnull)
		return NULL;
	p = new_pred(c, SAP_TID);
	p->op = op;
	p->expr = r;
	p->argtype = TIDOID;
	p->sel = 1.0;
	p->desc = expr_text(c, n);
	return p;
}

/*
 * The type of a path tested by an expression (a FILTER, a CASE, an OR, a key) while the query reads the values of the path: the leaf cannot hold
 * the strings among its numbers, so the scan would raise where PostgreSQL's guard skips them. Only a conjunct of the query (SAP_TYPE) guards a read.
 */
bool
types_guard_reads(AggCtx *c)
{
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].types && bms_is_member(c->vleaf[j].obj, c->read))
			return true;
	return false;
}

/*
 * Can a row the SAP_TYPE conjunct p keeps, whose leaf the query reads elsewhere, hold a value the leaf cannot store? The types a leaf stores are its
 * own (a JSON null is a NULL slot); a container, a value of another type and the paths of a document that is no object (jsonb_typeof is NULL) overflow.
 * The source cannot read such a row again for the test: it would have the type of its text. The normal plan keeps the query.
 */
bool
typeof_keeps_unstorable(AggCtx *c, const SiftAggPred *p)
{
	const SiftSrcLeaf *l = leaf_of(c, p->leaf);
	uint32		stored = l->typid == BOOLOID ? SIFT_SRC_JT_BOOLEAN :
		l->typid != TEXTOID ? SIFT_SRC_JT_NUMBER :
		l->astext ? SIFT_SRC_JT_STRING | SIFT_SRC_JT_NUMBER | SIFT_SRC_JT_BOOLEAN : SIFT_SRC_JT_STRING;

	return (p->op & SIFT_SRC_JT_READ) && (p->op & SIFT_SRC_JT_ALL & ~(stored | SIFT_SRC_JT_NULL)) != 0;
}

/* a SAP_TYPE conjunct whose leaf the query reads elsewhere says so: a row it keeps with a value the leaf cannot hold is then an error */
void
typeof_reads(AggCtx *c, SiftAggSpec *s)
{
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE && bms_is_member(s->pred[i]->leaf, c->read))
			s->pred[i]->op |= SIFT_SRC_JT_READ;
}
