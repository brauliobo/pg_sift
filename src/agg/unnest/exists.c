/*
 * agg/unnest/exists.c - the planning of the tests of the elements of an array column of the table (EXISTS over jsonb_to_recordset, jsonb_path_exists, @>) as the aggregate scan of an unnest
 * (agg/unnest/exists.h): each test is a boolean of the row that the WHERE keeps or a FILTER reads as a column.
 */
#include "postgres.h"

#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_operator_d.h"
#include "access/stratnum.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/kern/kern.h"
#include "core/compat.h"
#include "agg/plan/int.h"
#include "agg/ext.h"
#include "agg/safety.h"
#include "agg/source_rel.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/path.h"

/* the cost the scan of the table has for each row of it, in operators: the leaves of the elements are read for every granule of the index (the pull prunes none by the quals of the table) */
#define EXISTS_ROW_OPERATORS	50

/* a test of the statement: the sub-plan of an EXISTS or the jsonpath call, as the statement has it, and the WHERE clause it is the whole of (NULL: a conjunct of a FILTER) */
typedef struct TestCand
{
	const Node *node;
	bool		negated;
	const RestrictInfo *where;
	const SubPlan *sp;
	const PlannerInfo *sub;
} TestCand;

/* a candidate made a test of the elements of the array of one index */
typedef struct TestBuilt
{
	const TestCand *cand;
	int			index;
	SiftV6XTest test;
	List	   *quals;
	Expr	   *heap;
	int			map[SIFT_V6_XELEM_COLS];
	RelOptInfo *elem_rel;
} TestBuilt;

/* the sub-plan of an EXISTS whose sub-select is a single function scan, and the root it was planned under; NULL for any other clause */
static const SubPlan *
exists_subplan(const PlannerInfo *root, const Node *n, const PlannerInfo **sub)
{
	const SubPlan *sp = (const SubPlan *) n;
	const Query *q;

	if (!IsA(n, SubPlan) || sp->subLinkType != EXISTS_SUBLINK || sp->useHashTable || sp->testexpr != NULL || sp->args == NIL || list_length(sp->args) != list_length(sp->parParam))
		return NULL;
	*sub = list_nth(root->glob->subroots, sp->plan_id - 1);
	q = (*sub)->parse;
	if (list_length(q->rtable) != 1 || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->groupClause != NIL || q->havingQual != NULL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->distinctClause != NIL || q->rowMarks != NIL || q->setOperations != NULL || q->cteList != NIL)
		return NULL;
	return sp;
}

/* the sub-plan or call c is, under at most one NOT */
static const Node *
test_node(const Node *c, bool *negated)
{
	*negated = IsA(c, BoolExpr) && ((const BoolExpr *) c)->boolop == NOT_EXPR;
	if (*negated)
		c = linitial(((const BoolExpr *) c)->args);
	return IsA(c, SubPlan) || sift_v6_unnest_path_call(c) ? c : NULL;
}

static TestCand *
cand_of(const PlannerInfo *root, const Node *clause, const RestrictInfo *where)
{
	TestCand   *c = palloc0(sizeof(TestCand));

	c->node = test_node(clause, &c->negated);
	c->where = where;
	if (c->node == NULL || (IsA(c->node, SubPlan) && (c->sp = exists_subplan(root, c->node, &c->sub)) == NULL))
		return NULL;
	return c;
}

typedef struct FilterWalk
{
	List	   *filters;
} FilterWalk;

static bool
collect_filters(Node *n, FilterWalk *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		if (((Aggref *) n)->aggfilter != NULL)
			w->filters = lappend(w->filters, ((Aggref *) n)->aggfilter);
		return false;
	}
	return expression_tree_walker(n, collect_filters, w);
}

/* the tests the WHERE and the FILTERs of the statement have (a clause equal to one already found is the same test) */
static List *
collect_cands(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra)
{
	List	   *cands = NIL;
	FilterWalk	w = {0};

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		TestCand   *c = ri->pseudoconstant ? NULL : cand_of(root, (Node *) ri->clause, ri);

		if (c != NULL)
			cands = lappend(cands, c);
	}
	collect_filters((Node *) out->reltarget->exprs, &w);
	collect_filters(extra->havingQual, &w);
	foreach_ptr(Expr, filter, w.filters)
		foreach_ptr(Node, conjunct, make_ands_implicit(filter))
		{
			TestCand   *c = cand_of(root, conjunct, NULL);
			bool		seen = false;

			foreach_ptr(TestCand, o, cands)
				seen |= c != NULL && o->where == NULL && equal(o->node, c->node);
			if (c != NULL && !seen)
				cands = lappend(cands, c);
		}
	return list_length(cands) <= SIFT_V6_XTESTS ? cands : NIL;
}

/* ---- the columns of the elements the tests of the plan read, and one test's comparisons with the columns of its row ---- */

static bool
is_int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

/* the SiftCmpOp of the comparison opno: its btree strategy, or <> as the negator of an equality */
static bool
cmp_op_of(Oid opno, int *op)
{
	static const int ops[] = {SCMP_LT, SCMP_LE, SCMP_EQ, SCMP_GE, SCMP_GT};
	const Oid	neg = get_negator(opno);

	foreach_int(strat, sift_op_strategies(opno))
		if (strat >= BTLessStrategyNumber && strat <= BTGreaterStrategyNumber)
		{
			*op = ops[strat - 1];
			return true;
		}
	if (OidIsValid(neg))
		foreach_int(strat, sift_op_strategies(neg))
			if (strat == BTEqualStrategyNumber)
			{
				*op = SCMP_NE;
				return true;
			}
	return false;
}

/* the comparison `element column <op> $n` (or commuted), $n standing for a column of the row of the table, of the sub-select of test b */
static bool
outer_cmp(V6Plan *p, const TestBuilt *b, const OpExpr *op, SiftV6XCmp *out)
{
	const SubPlan *sp = b->cand->sp;
	Node	   *l = linitial(op->args),
			   *r = lsecond(op->args);
	Oid			opno = op->opno;
	SiftV6Col	col;
	int			k = 0;

	if (IsA(l, Param) && IsA(r, Var))
	{
		Node	   *t = l;

		l = r;
		r = t;
		opno = get_commutator(opno);
	}
	if (!OidIsValid(opno) || !IsA(l, Var) || !IsA(r, Param) || ((const Var *) l)->varno != b->elem_rel->relid || ((const Var *) l)->varlevelsup != 0 || ((const Param *) r)->paramkind != PARAM_EXEC)
		return false;
	foreach_int(id, sp->parParam)
	{
		if (id == ((const Param *) r)->paramid)
			break;
		k++;
	}
	if (k == list_length(sp->parParam) || !sift_v6_plan_column(p, list_nth(sp->args, k), false, &col))
		return false;
	if (!((((const Var *) l)->vartype == FLOAT8OID && col.typid == FLOAT8OID) || (is_int_type(((const Var *) l)->vartype) && is_int_type(col.typid))) || !cmp_op_of(opno, &out->op))
		return false;
	out->elem = b->map[((const Var *) l)->varattno - 1];
	out->outer = col.attno;
	out->outer_type = col.typid;
	return true;
}

/* the quals of the sub-select of an EXISTS over the columns of the elements, and its comparisons with the columns of the row */
static bool
exists_quals(V6Plan *p, TestBuilt *b)
{
	foreach_node(RestrictInfo, ri, b->elem_rel->baserestrictinfo)
	{
		Expr	   *q;

		if (ri->pseudoconstant)
			return false;
		if (agg_node_has_param((Node *) ri->clause))
		{
			SiftV6XCmp	cmp;

			if (!IsA(ri->clause, OpExpr) || list_length(((OpExpr *) ri->clause)->args) != 2 || b->test.ncmps == SIFT_V6_XCMPS || !outer_cmp(p, b, (const OpExpr *) ri->clause, &cmp))
				return false;
			b->test.cmp[b->test.ncmps++] = cmp;
			continue;
		}
		if ((q = sift_v6_unnest_elem_qual(p, (Node *) ri->clause)) == NULL)
			return false;
		b->quals = lappend(b->quals, q);
	}
	return true;
}

/* the declared columns of an EXISTS among the columns of the elements of the plan, which they join */
static bool
merge_declared(SiftV6ElemRte *rte, const SiftV6ElemRte *one, TestBuilt *b)
{
	int			c = 0;

	if (rte->fn == SV6E_PATH || (rte->array != NULL && !equal(rte->array, one->array)))
		return false;
	if (rte->array == NULL)
		*rte = (SiftV6ElemRte) {.fn = SV6E_RECORDSET,.array = one->array,.base = one->base,.key = one->key};
	foreach_node(String, n, one->names)
	{
		const int	at = sift_v6_unnest_rte_col(rte, strVal(n), list_nth_oid(one->types, c), false);

		if (at < 0)
			return false;
		b->map[c++] = at;
	}
	return true;
}

static bool
build_exists(V6Plan *p, SiftV6ElemRte *rte, TestBuilt *b)
{
	const TestCand *c = b->cand;
	SiftV6ElemRte one;

	b->elem_rel = c->sub->simple_rel_array[1];
	return b->elem_rel != NULL && sift_v6_unnest_exists_rte(planner_rt_fetch(1, (PlannerInfo *) c->sub), c->sp, &one) && sift_v6_unnest_array_ok(p->root, p->rel, one.array) && merge_declared(rte, &one, b);
}

static bool
build_path(V6Plan *p, SiftV6ElemRte *rte, TestBuilt *b)
{
	const Node *e = b->cand->node;
	const Expr *array = sift_v6_unnest_path_array(e);
	SiftV6ElemRte base;

	if (array == NULL || !sift_v6_unnest_array_ok(p->root, p->rel, array))
		return false;
	sift_v6_unnest_array_rte(array, &base);
	if (rte->fn == SV6E_RECORDSET || (rte->array != NULL && !equal(rte->array, array)))
		return false;
	if (rte->array == NULL)
		*rte = base;
	b->test.nullable = true;
	b->heap = copyObject(unconstify(Expr *, (const Expr *) e));
	return sift_v6_unnest_path_quals(p, rte, e, &b->quals) && (sift_v6_unnest_doc_leaf(p, rte, &b->test.doc_leaf) || b->test.need == 1);
}

/* the tests of the plan for the candidates: their columns among those of the elements (rte), and their quals */
static bool
build_tests(V6Plan *p, const List *cands, SiftV6ElemRte *rte)
{
	foreach_ptr(TestCand, c, cands)
	{
		TestBuilt  *b = palloc0(sizeof(TestBuilt));

		b->cand = c;
		b->index = list_length(p->tests);
		b->test = (SiftV6XTest) {.need = c->where == NULL ? 0 : c->negated ? 2 : 1,.doc_leaf = -1};
		if (!(c->sp != NULL ? build_exists(p, rte, b) : build_path(p, rte, b)))
			return false;
		p->tests = lappend(p->tests, b);
	}
	return true;
}

/* the quals of the sub-selects, which need the Ext of the plan for the columns of the elements */
static bool
build_exists_quals(V6Plan *p)
{
	foreach_ptr(TestBuilt, b, p->tests)
	{
		p->elem_rel = b->elem_rel;
		p->elem_map = b->map;
		if (b->cand->sp != NULL && !exists_quals(p, b))
			return false;
	}
	p->elem_rel = NULL;
	p->elem_map = NULL;
	return true;
}

static void
store_tests(V6Plan *p)
{
	SiftV6Ext  *x = p->xseed;

	foreach_ptr(TestBuilt, b, p->tests)
	{
		x->test[x->ntests++] = b->test;
		x->test_quals = lappend(x->test_quals, list_make2(b->quals, b->heap != NULL ? list_make1(b->heap) : NIL));
	}
}

Expr *
sift_v6_unnest_test_qual(const V6Plan *p, const Node *conjunct)
{
	bool		negated;
	const Node *node = test_node(conjunct, &negated);
	Var		   *flag;

	if (node == NULL)
		return NULL;
	foreach_ptr(TestBuilt, b, p->tests)
		if (b->cand->where == NULL && equal(b->cand->node, node))
		{
			flag = makeVar(INDEX_VAR, SIFT_V6_XFLAG_VATT + b->index, BOOLOID, -1, InvalidOid, 0);
			return make_opclause(BooleanEqualOperator, BOOLOID, false, (Expr *) flag, (Expr *) makeBoolConst(!negated, false), InvalidOid, InvalidOid);
		}
	return NULL;
}

bool
sift_v6_unnest_test_ri(const V6Plan *p, const RestrictInfo *ri)
{
	if (ri == p->sub_qual)
		return true;
	foreach_ptr(TestBuilt, b, p->tests)
		if (b->cand->where == ri)
			return true;
	return false;
}

static Cost
exists_cost(const RelOptInfo *rel)
{
	return rel->tuples * cpu_operator_cost * EXISTS_ROW_OPERATORS;
}

static void
try_index(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs, IndexOptInfo *ix, const List *cands)
{
	V6Plan		p = {.root = root,.rel = rel,.out = out,.relid = ix->indexoid,.index = ix};
	Relation	irel = index_open(ix->indexoid, AccessShareLock);
	SiftV6ElemRte rte = {0};
	bool		ok;

	sift_v6_plan_index(&p, irel);
	ok = p.ops != NULL && build_tests(&p, cands, &rte) && sift_v6_unnest_seed(&p, NULL, &rte, false) && build_exists_quals(&p);
	if (ok)
	{
		store_tests(&p);
		ok = sift_v6_unnest_compile(&p, extra, group_exprs) && sift_v6_unnest_where(&p);
	}
	if (ok)
		sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, exists_cost(rel), rel->rows);
	index_close(irel, NoLock);
}

void
sift_v6_unnest_exists_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs)
{
	List	   *cands;

	if (!sift_v6_plan_fact_ok(root, rel) || (cands = collect_cands(root, rel, out, extra)) == NIL)
		return;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (sift_v6_index_owned(ix))
			try_index(root, rel, out, extra, group_exprs, ix, cands);
}
