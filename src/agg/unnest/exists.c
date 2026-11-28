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
#include "agg/exists_plan.h"
#include "agg/safety.h"
#include "agg/source_rel.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/path.h"
#include "agg/unnest/test.h"

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
	const CaseExpr *bind;		/* of a split (SiftV6ExistsSplit): the CASE of the sub-select its constant stands for (NULL: none) */
	const Const *value;
} TestCand;

/* a candidate made a test of the elements of the array of one index */
typedef struct TestBuilt
{
	const TestCand *cand;
	int			index;
	SiftV6XTest test;
	List	   *quals;			/* of an EXISTS: the one part, built after its stream is seeded */
	List	   *ors;			/* of each disjunction of the comparisons with the row: its other arms */
	List	   *parts;			/* of a jsonpath or a containment */
	Expr	   *heap;
	int			map[SIFT_V6_XELEM_COLS];
	RelOptInfo *elem_rel;
} TestBuilt;

/* the one relation the sub-select scans, NULL when it scans several: a sub-select PostgreSQL pulled up into it leaves its entry in the range table, but no relation */
static RelOptInfo *
sub_rel(const PlannerInfo *sub)
{
	RelOptInfo *one = NULL;

	for (int r = 1; r < sub->simple_rel_array_size; r++)
	{
		RelOptInfo *rel = sub->simple_rel_array[r];

		if (rel == NULL)
			continue;
		if (one != NULL || rel->reloptkind != RELOPT_BASEREL)
			return NULL;
		one = rel;
	}
	return one;
}

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
	if (sub_rel(*sub) == NULL || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->groupClause != NIL || q->havingQual != NULL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->distinctClause != NIL || q->rowMarks != NIL || q->setOperations != NULL || q->cteList != NIL)
		return NULL;
	return sp;
}

/* the sub-plan or call c is, under at most one NOT, the whole under an IS TRUE, which keeps the rows a WHERE or a FILTER keeps (table: of a sift_table, agg/unnest/path.h) */
static const Node *
test_node(const Node *c, bool table, bool *negated)
{
	if (IsA(c, BooleanTest) && ((const BooleanTest *) c)->booltesttype == IS_TRUE)
		c = (const Node *) ((const BooleanTest *) c)->arg;
	*negated = IsA(c, BoolExpr) && ((const BoolExpr *) c)->boolop == NOT_EXPR;
	if (*negated)
		c = linitial(((const BoolExpr *) c)->args);
	return IsA(c, SubPlan) || sift_v6_unnest_path_call(c, table) ? c : NULL;
}

static TestCand *
cand_of(const PlannerInfo *root, const Node *clause, bool table, const RestrictInfo *where)
{
	TestCand   *c = palloc0(sizeof(TestCand));

	c->node = test_node(clause, table, &c->negated);
	c->where = where;
	if (c->node == NULL || (IsA(c->node, SubPlan) && (c->sp = exists_subplan(root, c->node, &c->sub)) == NULL))
		return NULL;
	return c;
}

typedef struct FilterWalk
{
	List	   *filters;
} FilterWalk;

/* the conditions of the CASE of an argument of an aggregate, which the plan makes FILTERs of (an aggregate of CASE WHEN c THEN x ELSE y END is a FILTER on c and one on c IS NOT TRUE) */
static bool
case_conds(Node *n, FilterWalk *w)
{
	if (n != NULL && IsA(n, CaseExpr))
		foreach_node(CaseWhen, when, ((CaseExpr *) n)->args)
			w->filters = lappend(w->filters, when->expr);
	return n != NULL && expression_tree_walker(n, case_conds, w);
}

static bool
collect_filters(Node *n, FilterWalk *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
	{
		if (((Aggref *) n)->aggfilter != NULL)
			w->filters = lappend(w->filters, ((Aggref *) n)->aggfilter);
		(void) case_conds((Node *) ((Aggref *) n)->args, w);
		return false;
	}
	return expression_tree_walker(n, collect_filters, w);
}

/* the tests among the conjuncts of a FILTER and the arms of its ORs, added to cands (a clause equal to one already found is the same test) */
static List *
filter_cands(PlannerInfo *root, Node *clause, bool table, List *cands)
{
	foreach_ptr(Node, conjunct, make_ands_implicit((Expr *) clause))
	{
		TestCand   *c = is_orclause(conjunct) ? NULL : cand_of(root, conjunct, table, NULL);
		bool		seen = false;

		if (is_orclause(conjunct))
			foreach_ptr(Node, arm, ((const BoolExpr *) conjunct)->args)
				cands = filter_cands(root, arm, table, cands);
		foreach_ptr(TestCand, o, cands)
			seen |= c != NULL && o->where == NULL && o->bind == NULL && equal(o->node, c->node);
		if (c != NULL && !seen)
			cands = lappend(cands, c);
	}
	return cands;
}

/* the tests of the arms of an OR of the WHERE: the calls alone, which a row read from the heap evaluates with the whole clause (a sub-plan has no parent plan there) */
static List *
where_or_cands(PlannerInfo *root, Node *clause, bool table, List *cands)
{
	foreach_ptr(TestCand, c, filter_cands(root, clause, table, NIL))
	{
		bool		seen = c->sp != NULL;

		foreach_ptr(TestCand, o, cands)
			seen |= o->where == NULL && o->bind == NULL && equal(o->node, c->node);
		if (!seen)
			cands = lappend(cands, c);
	}
	return cands;
}

typedef struct MemberWalk
{
	PlannerInfo *root;
	bool		table;
	List	   *cands;
} MemberWalk;

/* the tests of the lists of strings among the members of the pattern of a containment anywhere in a clause, whose flags the clause reads (sift_v6_tests_split) */
static bool
member_walker(Node *n, MemberWalk *w)
{
	Expr	   *rest;
	List	   *members = n != NULL ? sift_v6_unnest_path_members(n, &rest) : NIL;

	if (members != NIL)
		w->cands = filter_cands(w->root, (Node *) make_ands_explicit(members), w->table, w->cands);
	return n != NULL && members == NIL && expression_tree_walker(n, member_walker, w);
}

static List *
member_cands(PlannerInfo *root, Node *clause, bool table, List *cands)
{
	MemberWalk	w = {root, table, cands};

	(void) member_walker(clause, &w);
	return w.cands;
}

/* the tests of a split: its EXISTS with each constant of its CASE */
static List *
split_cands(PlannerInfo *root, const List *splits, bool table, List *cands)
{
	foreach_ptr(SiftV6ExistsSplit, sx, splits)
		foreach_node(Const, value, sift_v6_exists_split_values(sx))
		{
			TestCand   *c = cand_of(root, (const Node *) sx->sp, table, NULL);

			if (c == NULL)
				return NIL;
			c->bind = sx->inner;
			c->value = value;
			cands = lappend(cands, c);
		}
	return cands;
}

/* the tests the WHERE and the FILTERs of the statement have (a SELECT DISTINCT has no extra, and no FILTER): a WHERE that is a test, the tests of the arms of an OR of the WHERE where the scan reads their flags
 * (agg/plan/filter.c), and those of the splits of its join */
static List *
collect_cands(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, bool or_arms, bool table, const List *splits)
{
	List	   *cands = NIL;
	FilterWalk	w = {0};

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		TestCand   *c = ri->pseudoconstant ? NULL : cand_of(root, (Node *) ri->clause, table, ri);

		if (c != NULL)
			cands = lappend(cands, c);
		else if (or_arms && !ri->pseudoconstant && is_orclause(ri->clause))
			cands = member_cands(root, (Node *) ri->clause, table, where_or_cands(root, (Node *) ri->clause, table, cands));
		else if (or_arms && !ri->pseudoconstant)
			cands = member_cands(root, (Node *) ri->clause, table, cands);
	}
	collect_filters((Node *) out->reltarget->exprs, &w);
	if (extra != NULL)
		collect_filters(extra->havingQual, &w);
	foreach_ptr(Expr, filter, w.filters)
		cands = member_cands(root, (Node *) filter, table, filter_cands(root, (Node *) filter, table, cands));
	if (splits != NIL && (cands = split_cands(root, splits, table, cands)) == NIL)
		return NIL;
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

/* the quals over the columns of the element are one clause the scan evaluates (agg/filter/pg.c) */
static bool
elem_clause_ok(Expr *q)
{
	SiftClause	clause;
	int			col[SIFT_V6_MAX_FCOLS],
				nvec;

	return sift_v6_unnest_elem_clause(CurrentMemoryContext, list_make1(q), &clause, col, &nvec);
}

/* a comparison with a column of the row of test b, a conjunct (group 0) or an arm of disjunction group */
static bool
add_cmp(V6Plan *p, TestBuilt *b, const Node *clause, int group)
{
	SiftV6XCmp	cmp = {.group = group};

	if (!IsA(clause, OpExpr) || list_length(((const OpExpr *) clause)->args) != 2 || b->test.ncmps == SIFT_V6_XCMPS || !outer_cmp(p, b, (const OpExpr *) clause, &cmp))
		return false;
	b->test.cmp[b->test.ncmps++] = cmp;
	return true;
}

/* an OR whose arms compare with the columns of the row: those arms are comparisons of its group, the others quals over the columns of the element */
static bool
add_or(V6Plan *p, TestBuilt *b, const BoolExpr *or)
{
	const int	group = list_length(b->ors) + 1;
	List	   *arms = NIL;

	foreach_ptr(Node, arm, or->args)
	{
		Expr	   *q = agg_node_has_param(arm) ? NULL : sift_v6_unnest_elem_qual(p, arm);

		if (q == NULL && !add_cmp(p, b, arm, group))
			return false;
		if (q != NULL)
			arms = lappend(arms, q);
	}
	b->ors = lappend(b->ors, arms);
	return arms == NIL || elem_clause_ok(sift_v6_unnest_or_arms(arms));
}

static Node *
bind_case(Node *n, const TestCand *c)
{
	if (n != NULL && equal(n, c->bind))
		return (Node *) copyObject(c->value);
	return expression_tree_mutator(n, bind_case, unconstify(TestCand *, c));
}

/* a clause of the sub-select of test b with the constant of its split for the CASE the split binds */
static Node *
bound_clause(const TestBuilt *b, Node *clause)
{
	return b->cand->bind == NULL ? clause : bind_case(clause, b->cand);
}

/* the quals of the sub-select of an EXISTS over the columns of the elements, and its comparisons with the columns of the row */
static bool
exists_quals(V6Plan *p, TestBuilt *b)
{
	foreach_node(RestrictInfo, ri, b->elem_rel->baserestrictinfo)
	{
		Node	   *clause = bound_clause(b, (Node *) ri->clause);
		Expr	   *q;

		if (ri->pseudoconstant)
			return false;
		if (agg_node_has_param(clause))
		{
			if (!(is_orclause(clause) ? add_or(p, b, (const BoolExpr *) clause) : add_cmp(p, b, clause, 0)))
				return false;
			continue;
		}
		if ((q = sift_v6_unnest_elem_qual(p, clause)) == NULL)
			return false;
		b->quals = lappend(b->quals, q);
	}
	return true;
}

/*
 * The stream of the plan whose elements a test of kind fn over array reads: the one of the tests before it over the same array, or a new one (the first nfixed are the plan's own, which no test
 * joins); NULL when the source reads no more arrays of a row
 */
static SiftV6ElemRte *
stream_of(const V6Plan *p, SiftV6TestStreams *st, const Expr *array, SiftV6ElemFn fn, int *at)
{
	for (*at = st->nfixed; *at < st->n; (*at)++)
		if (st->rte[*at].fn == fn && equal(st->rte[*at].array, array))
			return &st->rte[*at];
	if (st->n == SIFT_V6_XSTREAMS || (st->n > 0 && (p->ops->jsonb == NULL || !p->ops->jsonb->streams)))
		return NULL;
	st->rte[st->n] = (SiftV6ElemRte) {0};
	return &st->rte[st->n++];
}

/* the declared columns of an EXISTS among the columns of the elements of its stream, which they join */
static bool
merge_declared(SiftV6ElemRte *rte, const SiftV6ElemRte *one, TestBuilt *b)
{
	int			c = 0;

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
build_exists(V6Plan *p, SiftV6TestStreams *st, TestBuilt *b)
{
	const TestCand *c = b->cand;
	SiftV6ElemRte one;
	SiftV6ElemRte *rte;

	b->elem_rel = sub_rel(c->sub);
	if (!sift_v6_unnest_exists_rte(planner_rt_fetch(b->elem_rel->relid, (PlannerInfo *) c->sub), c->sp, &one) || !sift_v6_unnest_array_ok(p->root, p->rel, one.array) ||
		(rte = stream_of(p, st, one.array, one.fn == SV6E_MEMBERS ? SV6E_MEMBERS : SV6E_RECORDSET, &b->test.stream)) == NULL)
		return false;
	if (one.fn == SV6E_MEMBERS && rte->array == NULL)
		*rte = one;
	return one.fn == SV6E_MEMBERS || merge_declared(rte, &one, b);
}

/* a jsonpath or containment test: over an index the quals of the elements of its array, over a table the call itself decides each row, which the scan reads again by its address */
static bool
build_path(V6Plan *p, SiftV6TestStreams *st, TestBuilt *b)
{
	const Node *e = b->cand->node;
	const Expr *array = sift_v6_unnest_path_array(e, p->is_table);
	SiftV6ElemRte *rte;

	if (array == NULL || !sift_v6_unnest_array_ok(p->root, p->rel, array) || (rte = stream_of(p, st, array, sift_v6_unnest_path_fn(e), &b->test.stream)) == NULL)
		return false;
	if (rte->array == NULL)
	{
		sift_v6_unnest_array_rte(array, rte);
		rte->fn = sift_v6_unnest_path_fn(e);
	}
	b->test.nullable = true;
	b->heap = copyObject(unconstify(Expr *, (const Expr *) e));
	p->heap_rows |= p->is_table;
	return p->is_table || (sift_v6_unnest_path_quals(p, rte, e, &b->parts) && (!sift_v6_unnest_path_keyed(e) || sift_v6_unnest_doc_leaf(p, rte, &b->test.doc_leaf) || b->test.need == 1));
}

/* the tests of the plan for the candidates: their streams, their columns among those of the elements of the stream, and their quals */
static bool
build_tests(V6Plan *p, const List *cands, SiftV6TestStreams *st)
{
	foreach_ptr(TestCand, c, cands)
	{
		TestBuilt  *b = palloc0(sizeof(TestBuilt));

		b->cand = c;
		b->index = list_length(p->tests);
		b->test = (SiftV6XTest) {.need = c->where == NULL ? 0 : c->negated ? 2 : 1,.joined = c->bind != NULL,.doc_leaf = -1};
		if (!(c->sp != NULL ? build_exists(p, st, b) : build_path(p, st, b)))
			return false;
		p->tests = lappend(p->tests, b);
	}
	return true;
}

/* the streams of the tests after the plan's own, seeded into its Ext */
static bool
seed_streams(V6Plan *p, const SiftV6TestStreams *st)
{
	SiftV6Ext  *x = p->xseed;

	for (int s = Max(st->nfixed, 1); s < st->n; s++)
	{
		List	   *names;

		if (!sift_v6_unnest_elem_seed(p, &st->rte[s], false, &x->streams[x->nstreams], &names))
			return false;
		x->stream_names = lappend(x->stream_names, names);
		x->stream_arrays = lappend(x->stream_arrays, copyObject(unconstify(Expr *, st->rte[s].array)));
		x->nstreams++;
	}
	return true;
}

/* the quals of the sub-selects, which need the Ext of the plan for the columns of the elements of their stream; the relation of the plan's own elements is its again after */
static bool
build_exists_quals(V6Plan *p)
{
	RelOptInfo *own = p->elem_rel;
	bool		ok = true;

	foreach_ptr(TestBuilt, b, p->tests)
	{
		p->elem_rel = b->elem_rel;
		p->elem_map = b->map;
		p->elem_stream = b->test.stream;
		ok = ok && (b->cand->sp == NULL || exists_quals(p, b));
	}
	p->elem_rel = own;
	p->elem_map = NULL;
	p->elem_stream = 0;
	return ok;
}

static bool
store_tests(V6Plan *p)
{
	SiftV6Ext  *x = p->xseed;

	foreach_ptr(TestBuilt, b, p->tests)
	{
		x->test[x->ntests++] = b->test;
		x->test_quals = lappend(x->test_quals, list_make3(b->cand->sp != NULL ? list_make1(b->quals) : b->parts, b->heap != NULL ? list_make1(b->heap) : NIL, b->ors));
	}
	return true;
}

/* the source reads the elements of several arrays of a row (SiftV6Ext.nstreams) */
static bool
source_streams(const V6Plan *p)
{
	return p->ops->jsonb != NULL && p->ops->jsonb->streams;
}

/* the batches of the source give the WHERE the flags of the tests to read (a table decides its quals in the scan of its store, which reads no flag) */
static bool
where_flags(const V6Plan *p)
{
	return source_streams(p) && !p->is_table;
}

void
sift_v6_unnest_tests_beside(V6Plan *p, const SiftV6ElemRte *own, RelOptInfo *out, GroupPathExtraData *extra, const List *splits)
{
	SiftV6TestStreams st = {.n = 1,.nfixed = 1,.rte = {*own}};
	const bool	streams = source_streams(p);
	const List *cands = collect_cands(p->root, p->rel, out, extra, where_flags(p), p->is_table, splits);
	SiftV6Ext  *x = p->xseed;

	if (cands == NIL || (streams && build_tests(p, cands, &st) && seed_streams(p, &st) && build_exists_quals(p) && store_tests(p)))
		return;
	p->tests = NIL;
	x->nstreams = x->ntests = 0;
	x->stream_names = x->stream_arrays = x->test_quals = NIL;
}

/* the test of the plan whose flag a FILTER reads that the node is, -1: none */
static int
test_flag(const V6Plan *p, const Node *node)
{
	foreach_ptr(TestBuilt, b, p->tests)
		if (b->cand->where == NULL && equal(b->cand->node, node))
			return b->index;
	return -1;
}

Expr *
sift_v6_unnest_test_qual(const V6Plan *p, const Node *conjunct)
{
	bool		negated;
	const Node *node = test_node(conjunct, p->is_table, &negated);
	const int	k = node != NULL ? test_flag(p, node) : -1;
	Var		   *flag = makeVar(INDEX_VAR, SIFT_V6_XFLAG_VATT + k, BOOLOID, -1, InvalidOid, 0);

	return k < 0 ? NULL : make_opclause(BooleanEqualOperator, BOOLOID, false, (Expr *) flag, (Expr *) makeBoolConst(!negated, false), InvalidOid, InvalidOid);
}

int
sift_v6_unnest_test_vatt(const V6Plan *p, const Expr *e)
{
	bool		negated;
	const Node *node = test_node((const Node *) e, p->is_table, &negated);
	const int	k = node != NULL && !negated && node == (const Node *) e ? test_flag(p, node) : -1;

	return k < 0 ? InvalidAttrNumber : SIFT_V6_XFLAG_VATT + k;
}

/* the document side of a member test (doc -> 'key') @> '[..]' (sift_v6_unnest_path_members) */
static Expr *
member_doc(const Expr *member)
{
	return linitial(((const OpExpr *) linitial(((const OpExpr *) member)->args))->args);
}

/* doc IS NULL */
static Expr *
is_null(const Expr *doc)
{
	NullTest   *nt = makeNode(NullTest);

	*nt = (NullTest) {.xpr.type = T_NullTest,.arg = copyObject(unconstify(Expr *, doc)),.nulltesttype = IS_NULL,.location = -1};
	return (Expr *) nt;
}

/*
 * A containment whose lists of strings the tests of the plan read, as the value it is: NULL exactly where its document is, else true where the flags of the tests and the containment of the
 * other members all are (where it is false the parts are false or NULL, a missing key)
 */
static Node *
split_mutator(Node *n, const V6Plan *p)
{
	Expr	   *rest;
	List	   *parts = n != NULL ? sift_v6_tests_contains_quals(p, (const Expr *) n, &rest) : NIL;
	BooleanTest *holds;
	CaseExpr   *value;
	CaseWhen   *null_doc;
	Expr	   *doc;

	if (parts == NIL)
		return expression_tree_mutator(n, split_mutator, (void *) p);
	parts = sift_v6_unnest_path_members(n, &rest);
	doc = member_doc(linitial(parts));
	holds = makeNode(BooleanTest);
	*holds = (BooleanTest) {.xpr.type = T_BooleanTest,.arg = make_ands_explicit(rest != NULL ? lappend(parts, rest) : parts),.booltesttype = IS_TRUE,.location = -1};
	if (IsA(doc, Var) && bms_is_member(((const Var *) doc)->varattno, sift_v6_plan_rel(p).notnull))
		return (Node *) holds;
	null_doc = makeNode(CaseWhen);
	*null_doc = (CaseWhen) {.xpr.type = T_CaseWhen,.expr = is_null(doc),.result = (Expr *) makeNullConst(BOOLOID, -1, InvalidOid),.location = -1};
	value = makeNode(CaseExpr);
	*value = (CaseExpr) {.xpr.type = T_CaseExpr,.casetype = BOOLOID,.args = list_make1(null_doc),.defresult = (Expr *) holds,.location = -1};
	return (Node *) value;
}

Expr *
sift_v6_tests_split(const V6Plan *p, const Expr *e)
{
	return p->tests == NIL ? (Expr *) e : (Expr *) split_mutator((Node *) e, p);
}

List *
sift_v6_tests_contains_quals(const V6Plan *p, const Expr *clause, Expr **rest)
{
	List	   *flags = NIL;

	*rest = NULL;
	foreach_ptr(Expr, member, p->tests == NIL ? NIL : sift_v6_unnest_path_members((const Node *) clause, rest))
	{
		Expr	   *q = sift_v6_unnest_test_qual(p, (const Node *) member);

		if (q == NULL)
			return NIL;
		flags = lappend(flags, q);
	}
	return flags;
}

int
sift_v6_unnest_split_flag(const V6Plan *p, const SubPlan *sp, const Const *value)
{
	foreach_ptr(TestBuilt, b, p->tests)
		if (b->cand->sp == sp && b->cand->bind != NULL && equal(b->cand->value, value))
			return b->index;
	return -1;
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

Cost
sift_v6_unnest_exists_cost(const RelOptInfo *rel)
{
	return rel->tuples * cpu_operator_cost * EXISTS_ROW_OPERATORS;
}

bool
sift_v6_unnest_tests_seed(V6Plan *p, GroupPathExtraData *extra, const List *splits)
{
	SiftV6TestStreams st = {0};
	const List *cands = collect_cands(p->root, p->rel, p->out, extra, where_flags(p), p->is_table, splits);

	if (cands == NIL)
		return true;
	if (!build_tests(p, cands, &st) || !sift_v6_unnest_seed(p, NULL, &st.rte[0], false) || !seed_streams(p, &st) || !build_exists_quals(p))
		return false;
	p->xseed->row_tests = true;
	return store_tests(p);
}

bool
sift_v6_unnest_exists_seed(V6Plan *p, GroupPathExtraData *extra)
{
	const bool	batch_ok = p->batch_ok;
	const bool	seeded = sift_v6_unnest_tests_seed(p, extra, NIL) && p->xseed != NULL;

	if (!seeded)
	{
		p->tests = NIL;
		p->xseed = NULL;
		p->batch_ok = batch_ok;
	}
	return seeded;
}

bool
sift_v6_unnest_exists_compile(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return sift_v6_unnest_compile(p, extra, group_exprs) && sift_v6_unnest_where(p);
}

static void
try_source(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs, const SiftV6RelSource *src)
{
	V6Plan		p = {.root = root,.rel = rel,.out = out};
	Relation	source = sift_v6_source_open(src->reloid, src->is_table, AccessShareLock, NULL);

	if (sift_v6_plan_bind(&p, src, source) && sift_v6_unnest_exists_seed(&p, extra) && sift_v6_unnest_exists_compile(&p, extra, group_exprs))
		sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, sift_v6_unnest_exists_cost(rel), rel->rows);
	sift_v6_source_close(source, src->is_table, NoLock);
}

void
sift_v6_unnest_exists_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs)
{
	SiftV6RelSource *src;
	int			nsrc;

	if (!sift_v6_plan_fact_ok(root, rel))
		return;
	src = sift_v6_rel_sources_alloc(root, rel, &nsrc);
	for (int i = 0; i < nsrc; i++)
		if (collect_cands(root, rel, out, extra, true, src[i].is_table, NIL) != NIL)
			try_source(root, rel, out, extra, group_exprs, &src[i]);
}
