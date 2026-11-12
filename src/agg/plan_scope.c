/*
 * agg/plan_scope.c - the scope conjunct of a scan (col/scope.h, agg/scope.c)
 *
 * Two ways to the same SiftAggScope (scope_make). The testing marker sift.in_scope(index, key_name, fk, key or keys), or sift.in_scope_pairs for the meaning of a join (a row once for
 * each pair of its member with the keys): true for the rows whose fk has a pair with the key (one of the keys of the array) in the scope relation the scope key_name of the index
 * declares; its body, in the test flavour, reads that relation under the snapshot, so a statement the scan does not take is exact as written. And the join the marker stands for,
 * as an application writes it: `JOIN scope_rel s ON s.member = f.fk WHERE s.key = k`, `f.fk IN (SELECT member FROM scope_rel WHERE key = k)`, EXISTS, through a view, an inlined SQL
 * function or a security barrier view over the relation. PostgreSQL pulls most of them up into a join of the scope relation to the scanned table; the second pass of group_paths
 * (sift_agg_scope_wanted) plans the statement again with that join taken out of the FROM list (sift_agg_scope_take) and its conditions the scope conjunct, and the prices of the
 * two plans choose.
 *
 * The proof that the scope read gives the rows of the join, all of it at plan time:
 *  (a) the relation and its columns are the index's declared scope, and the join is the equality of its member column and the declared column of the scanned table;
 *  (b) the scope key is bound by one equality or IN list of constants or of parameters (a $n, an InitPlan's output, a stable expression: the scan evaluates them when it starts), the
 *      scope relation has no other table joined to it and no condition but the predicate of the btree that defines its pairs, which the statement must state (agg/scope_legs.c);
 *  (c) the statement reads no column of the scope relation but its key as a group key (`GROUP BY s.key`, "every tenant"), and a join is taken only where the recorded btree on (member, key) is
 *      unique: a row of the scanned table has at most one row with a key then, and the join, the semi join and the scope read count it the same; the group key is read as the derived leaf of the
 *      scope, which holds the key of each pair the scan reads (an aggregate argument, a select list expression, a HAVING that reads the key keeps the join). With a non-unique btree (a pair may be
 *      several rows: a multiset) the scope read counts a pair once, the semi joins (IN, EXISTS) too, and a join counts it once for each row: only the semi joins are taken;
 *  (d) a semi join (IN, EXISTS) counts a row once whichever keys it has, which the reader does (Scope Semantics: once); a join counts it once for each pair, which the reader reads
 *      per pair for one key, for several and for every key (no condition on the key, SiftAggScope.all). Where the units cannot answer (the reader's refusals, its changed pairs) or cost more, the
 *      node runs the join as written (agg/cost/scope.c: the plan holds it), so a read of several or of every key, and a read that groups by the key, is planned only where the first pass
 *      has that join (an array or a list of parameters may hold several keys: a join takes a scalar parameter only);
 *  (e) the other conjuncts and the aggregates are the scan's as ever.
 * Whether the index may be read at execution (its stamps, the journal, the write buffers) is the reader's verdict, and EXPLAIN says it (Scope Fallback). Every near miss is reported
 * (slug scope) with its reason, and the join runs as it is written. A marker that stays an ordinary filter says why in the spec (scope_why: EXPLAIN prints it as Scope Fallback), so that a scope
 * read is never lost without a word.
 *
 * Where the conjunct may come from (the rows of the scan before any join that keeps rows the scan does not read): the conditions inside the nullable input of an outer join (FromScan.scan_quals: a
 * sub-select's WHERE and the ON conjuncts that read only that input) filter its rows before the join, so a scan of that input takes them whatever the join keeps; the WHERE of the statement
 * (FromScan.quals) is a row filter of the whole result, exact for the scan unless an outer join of the scan can null its table (the conjunct then drops, or keeps, the rows the join made
 * of the other side's unmatched ones, which the scan does not read). The ON clause of a join that keeps the scanned rows filters the matches, not the rows.
 */
/* check-memory: allow-allocs the keys are the constants of one statement */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/plan_int.h"
#include "col/am.h"
#include "col/scope.h"
#include "core/cmp.h"

#define NOT_SCOPE	((const char *) 1)	/* take_item: the item is not the scope relation of the index */

/* why a join, or a read for each pair, is no scope read of a relation whose btree is not unique: the scope read counts a pair once */
#define SCOPE_MULTISET	"pairs are a multiset (non-unique btree): a join counts a fact once for each row"

/* is the scope key_name of the index, which it declares, one whose pairs may be several rows */
static bool
scope_multiset(Oid indexoid, const char *key_name)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	SiftScopeSpecs *sp = sift_scope_specs(index);
	bool		multiset = false;

	for (int i = 0; i < sp->n; i++)
		if (strcmp(sp->s[i]->key_name, key_name) == 0)
			multiset = sift_scope_pairs_multiset(index, sp->s[i]);
	index_close(index, NoLock);
	return multiset;
}

static void
scope_miss(const char *why)
{
	agg_report("scope", AGG_REFUSE_SCOPE, why, false, 0, 0);
}

/* the scope of the template (key_name, keys or param, once), read by the member key in leaf fk */
static SiftAggScope *
scope_make(AggCtx *c, const SiftAggScope *proto, int fk)
{
	SiftAggScope *sc = memcpy(palloc(sizeof(SiftAggScope)), proto, sizeof(SiftAggScope));

	sc->key_name = pstrdup(proto->key_name);
	sc->fk = fk;
	use_leaf(c, fk);
	return sc;
}

/* is key_name a scope of the index that holds its member key in the column of the index attribute att */
static bool
declared_on(Oid indexoid, const char *key_name, int att)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	SiftScopeSpecs *sp = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;
	bool		found = false;

	for (int i = 0; sp != NULL && i < sp->n; i++)
		found |= strcmp(sp->s[i]->key_name, key_name) == 0 &&
			strcmp(sp->s[i]->fk, NameStr(TupleDescAttr(RelationGetDescr(index), att - 1)->attname)) == 0;
	index_close(index, NoLock);
	return found;
}

/* NULL, with the reason */
static SiftAggScope *
no_scope(const char **why, const char *reason)
{
	*why = reason;
	return NULL;
}

/* the leaf of the index that holds the member key fk of the scope key_name; -1, with the reason */
static int
fk_leaf(AggCtx *c, Node *fk, const char *key_name, const char **why)
{
	int			leaf = match_leaf(c, fk);
	const SiftSrcLeaf *lf;

	if (leaf < 0)
		return (*why = "scope qual is not on the scanned relation", -1);
	lf = leaf_of(c, leaf);
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->path != NIL || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64))
		return (*why = "member key is not an integer column of the index", -1);
	if (!declared_on(c->indexoid, key_name, lf->attno))
		return (*why = "no scope of that name is declared on the member key", -1);
	return leaf;
}

/* ---- the testing marker ---- */

/* sift.<name>(index, key_name, fk, key or keys) of the testing extension */
static bool
is_func(Node *q, const char *name, int nargs)
{
	return IsA(q, FuncExpr) && list_length(((FuncExpr *) q)->args) == nargs && get_func_namespace(((FuncExpr *) q)->funcid) == get_namespace_oid("sift", true) &&
		strcmp(get_func_name(((FuncExpr *) q)->funcid), name) == 0;
}

static bool
is_marker(Node *q)
{
	return is_func(q, "in_scope", 4) || is_func(q, "in_scope_pairs", 4);
}

static bool
marker_walker(Node *n, void *ctx)
{
	return n != NULL && (is_marker(n) || expression_tree_walker(n, marker_walker, ctx));
}

static Const *
arg_const(FuncExpr *f, int i, Oid type)
{
	Node	   *a = strip_relabel(list_nth(f->args, i));

	return IsA(a, Const) && !((Const *) a)->constisnull && ((Const *) a)->consttype == type ? (Const *) a : NULL;
}

static SiftAggScope *
marker_scope(AggCtx *c, Node *q, const char **why)
{
	FuncExpr   *f = (FuncExpr *) q;
	Const	   *idx = arg_const(f, 0, REGCLASSOID),
			   *name = arg_const(f, 1, TEXTOID),
			   *key = arg_const(f, 3, INT8OID);
	int			leaf;
	SiftAggScope proto = {0};

	if (key == NULL)
		key = arg_const(f, 3, INT8ARRAYOID);
	if (idx == NULL || name == NULL || key == NULL)
		return no_scope(why, "scope index, name or keys are not constants");
	if (DatumGetObjectId(idx->constvalue) != c->indexoid)
		return no_scope(why, "scope index is not the scanned index");
	if ((leaf = fk_leaf(c, list_nth(f->args, 2), TextDatumGetCString(name->constvalue), why)) < 0)
		return NULL;
	if (key->consttype == INT8ARRAYOID && ARR_HASNULL(DatumGetArrayTypeP(key->constvalue)))
		return no_scope(why, "a scope key is NULL");
	proto.key_name = TextDatumGetCString(name->constvalue);
	proto.keys = sift_agg_scope_keys(key->consttype, key->constvalue, false, &proto.nkeys);
	proto.once = strcmp(get_func_name(f->funcid), "in_scope") == 0;
	if (!proto.once && scope_multiset(c->indexoid, proto.key_name))
		return no_scope(why, SCOPE_MULTISET);
	return scope_make(c, &proto, leaf);
}

/* ---- the join to the scope relation ---- */

/* the integer btree equality `=` (also between int2, int4 and int8): the scope key compares as the join does */
bool
integer_eq(Oid opno)
{
	return op_in_opfamily(opno, INTEGER_BTREE_FAM_OID) && get_op_opfamily_strategy(opno, INTEGER_BTREE_FAM_OID) == BTEqualStrategyNumber;
}

/* the relation r of a declaration is the table relid */
static bool
names_relation(Oid heapoid, const SiftScopeRel *r, Oid relid)
{
	Oid			nsp = r->schema ? get_namespace_oid(r->schema, true) : get_rel_namespace(heapoid);

	return OidIsValid(nsp) && get_relname_relid(r->name, nsp) == relid;
}

/*
 * The declared scope of the index whose pairs relation is relid; else the scope of two paths that has it as a relation of a path (its first or its joined one), whose pairs are not the rows of
 * either; else NULL.
 */
static const SiftScopeSpec *
declared_scope(Oid heapoid, const SiftScopeSpecs *specs, Oid relid)
{
	const SiftScopeSpec *two = NULL;

	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		const SiftScopeSpec *sp = specs->s[i];

		if (sp->joined == NULL && names_relation(heapoid, &sp->rel, relid))
			return sp;
		if (sp->joined != NULL && two == NULL && (names_relation(heapoid, &sp->rel, relid) || names_relation(heapoid, sp->joined, relid)))
			two = sp;
	}
	return two;
}

/* a select that only projects columns of its tables and filters them */
bool
plain_select(const Query *q)
{
	return !(q->hasAggs || q->hasWindowFuncs || q->hasSubLinks || q->groupClause != NIL || q->distinctClause != NIL || q->limitCount != NULL ||
			 q->limitOffset != NULL || q->setOperations != NULL || q->cteList != NIL || q->sortClause != NIL || q->rowMarks != NIL);
}

typedef struct ScopeSeen
{
	Oid			heapoid;
	const SiftScopeSpecs *specs;
} ScopeSeen;

/* does the tree n read a relation of a scope of the index (a table of any query inside it) */
static bool
scope_seen_walker(Node *n, ScopeSeen *s)
{
	if (n == NULL)
		return false;
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_RELATION && declared_scope(s->heapoid, s->specs, ((RangeTblEntry *) n)->relid) != NULL;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, scope_seen_walker, s, QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(n, scope_seen_walker, s);
}

/* why the item rti of the FROM list, a sub-select that is none of the scope read's items, reads a relation of a scope all the same; NOT_SCOPE: it reads none */
static const char *
item_strays(AggCtx *c, const SiftScopeSpecs *specs, Index rti)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, c->root);
	ScopeSeen	seen = {c->heapoid, specs};

	if (rte->rtekind != RTE_SUBQUERY || !scope_seen_walker((Node *) rte->subquery, &seen))
		return NOT_SCOPE;
	return bms_is_empty(c->root->simple_rel_array[rti]->lateral_relids) ?
		"a sub-select reads the scope relation and is no select of it alone: the relation, a view or a sub-select that projects its columns, and the union of the two paths of a scope, are read from the units" :
		"a lateral (correlated) sub-select reads the scope relation";
}

bool
sift_agg_reads_scope(Query *level, Node *tree)
{
	foreach_node(RangeTblEntry, rte, level->rtable)
	{
		Relation	heap;

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || !has_column_source(rte->relid))
			continue;
		heap = relation_open(rte->relid, AccessShareLock);
		foreach_oid(ixoid, RelationGetIndexList(heap))
		{
			Relation	index = index_open(ixoid, AccessShareLock);
			ScopeSeen	seen = {rte->relid, sift_col_is_index(index) ? sift_scope_specs(index) : NULL};
			bool		hit = seen.specs != NULL && scope_seen_walker(tree, &seen);

			index_close(index, NoLock);
			if (hit)
			{
				relation_close(heap, AccessShareLock);
				return true;
			}
		}
		relation_close(heap, AccessShareLock);
	}
	return false;
}

/*
 * What the item rti of the FROM list is as a scope relation: the table itself, or the one table of a sub-select PostgreSQL did not flatten (a security barrier view) that projects its
 * columns, which is then read through (map[resno] is the column of the table the sub-select outputs as resno, 0 for another expression; nmap entries), or the union of the two paths of a
 * scope (scope_union.c). why: it is that table's, but not one the join may be read through. sp NULL: it is none.
 */
static ScopeItem
scope_item(AggCtx *c, const SiftScopeSpecs *specs, Index rti)
{
	PlannerInfo *root = c->root;
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	RelOptInfo *rel = root->simple_rel_array[rti];
	ScopeItem	it = {0};
	Query	   *q;
	RangeTblEntry *base;

	if (rel == NULL)
		return it;
	if (rte->rtekind == RTE_RELATION)
	{
		if (rte->securityQuals != NIL)
			it.why = "row-level security applies to the scope relation";
		it.relid = rte->inh ? InvalidOid : rte->relid;
	}
	else if (rte->rtekind == RTE_SUBQUERY && bms_is_empty(rel->lateral_relids))
	{
		q = rte->subquery;
		if (q->setOperations != NULL)
			return scope_union_item(c, specs, q);
		if (list_length(q->rtable) != 1 || (base = linitial_node(RangeTblEntry, q->rtable))->rtekind != RTE_RELATION)
			return it;
		if (!plain_select(q))
			it.why = "the sub-select over the scope relation is not a plain projection of its columns";
		else if (q->jointree->quals != NULL || base->securityQuals != NIL)
			it.why = "the view over the scope relation has a condition of its own";
		else if (has_subclass(base->relid) || base->tablesample != NULL)
			it.why = "the view over the scope relation reads its child tables or a sample of it: their rows are rows of the join, not pairs";
		else
		{
			it.nmap = list_length(q->targetList) + 1;
			it.map = palloc0(it.nmap * sizeof(AttrNumber));
			foreach_node(TargetEntry, te, q->targetList)
				if (!te->resjunk && IsA(te->expr, Var) && ((Var *) te->expr)->varlevelsup == 0)
					it.map[te->resno] = ((Var *) te->expr)->varattno;
		}
		it.relid = base->relid;
	}
	it.sp = OidIsValid(it.relid) ? declared_scope(c->heapoid, specs, it.relid) : NULL;
	return it;
}

/* the column of the table that column att of the item is (InvalidAttrNumber: none) */
AttrNumber
item_attno(const AttrNumber *map, int nmap, AttrNumber att)
{
	return map == NULL ? att : att > 0 && att < nmap ? map[att] : InvalidAttrNumber;
}

/* the derived leaf of the scope the pass took out, when e is its key column (a bare Var of the item): what a group key `s.key` reads (match_leaf); -1 when it is not */
int
sift_agg_scope_key_leaf(AggCtx *c, Node *e)
{
	const ScopeTake *t = c->scope_take;
	const Var  *v = (const Var *) e;

	if (c->scope_key == NULL || !IsA(e, Var) || v->varno != t->rti || v->varlevelsup != 0 || item_attno(t->map, t->nmap, v->varattno) != t->key)
		return -1;
	return sift_agg_scope_leaf(c->lay, c->scope_key);
}

/* n is a plain column of the item rti */
Var *
item_var(Node *n, Index rti)
{
	Node	   *v = strip_relabel(n);

	return IsA(v, Var) && ((Var *) v)->varno == rti && ((Var *) v)->varlevelsup == 0 && ((Var *) v)->varnullingrels == NULL ? (Var *) v : NULL;
}

/* q equates a column of the item rti to a column of the scanned table: theirs, ours (the scanned table's Var) */
static bool
member_equality(AggCtx *c, Index rti, Node *q, Var **theirs, Var **ours)
{
	OpExpr	   *op = (OpExpr *) q;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2 || !integer_eq(op->opno))
		return false;
	*theirs = item_var(linitial(op->args), rti);
	*ours = item_var(lsecond(op->args), c->relid);
	if (*theirs == NULL)
	{
		*theirs = item_var(lsecond(op->args), rti);
		*ours = item_var(linitial(op->args), c->relid);
	}
	return *theirs != NULL && *ours != NULL;
}

/*
 * q binds the key column of the item: `key = value`, `value = key` or `key = ANY (array)` (IN (list)). The value, a Const when the statement knows it, else an expression of its
 * parameters; NULL when q is not such a condition.
 */
Node *
key_operand(Index rti, const AttrNumber *map, int nmap, AttrNumber key, Node *q)
{
	Node	   *col,
			   *val;

	if (IsA(q, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) q)->useOr && integer_eq(((ScalarArrayOpExpr *) q)->opno))
	{
		col = linitial(((ScalarArrayOpExpr *) q)->args);
		val = lsecond(((ScalarArrayOpExpr *) q)->args);
	}
	else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2 && integer_eq(((OpExpr *) q)->opno))
	{
		col = linitial(((OpExpr *) q)->args);
		val = lsecond(((OpExpr *) q)->args);
		if (item_var(col, rti) == NULL)
		{
			Node	   *t = col;

			col = val;
			val = t;
		}
	}
	else
		return NULL;
	if (item_var(col, rti) == NULL || item_attno(map, nmap, item_var(col, rti)->varattno) != key)
		return NULL;
	return strip_relabel(val);
}

/* a column of another table than the item rti (not under an outer join) */
static Var *
other_var(Node *n, Index rti)
{
	Node	   *v = strip_relabel(n);

	return IsA(v, Var) && ((Var *) v)->varno != rti && ((Var *) v)->varlevelsup == 0 && ((Var *) v)->varnullingrels == NULL ? (Var *) v : NULL;
}

/* q equates the key column of the item rti to a column of another table (a table of keys): that column; NULL when q is not such a condition */
static Var *
key_equality(Index rti, const AttrNumber *map, int nmap, AttrNumber key, Node *q)
{
	OpExpr	   *op = (OpExpr *) q;
	Var		   *mine,
			   *theirs;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2 || !integer_eq(op->opno))
		return NULL;
	mine = item_var(linitial(op->args), rti);
	theirs = other_var(lsecond(op->args), rti);
	if (mine == NULL || theirs == NULL)
	{
		mine = item_var(lsecond(op->args), rti);
		theirs = other_var(linitial(op->args), rti);
	}
	return mine != NULL && item_attno(map, nmap, mine->varattno) == key ? theirs : NULL;
}

/* the keys a generic plan expects of an array it does not know: the estimate of PostgreSQL's own for an ANY (array parameter) */
#define SCOPE_PARAM_KEYS	10

/* the keys of ARRAY(SELECT key FROM keys WHERE ..): the rows its sub-select is expected to give, at least two (an array may hold several whatever the plan expects: a join that counts a row for each pair refuses it) */
static int
initplan_rows(AggCtx *c, Param *prm)
{
	foreach_ptr(SubPlan, sp, c->root->init_plans)
		if (list_member_int(sp->setParam, prm->paramid))
			return (int) Min(Max(((Plan *) list_nth(c->root->glob->subplans, sp->plan_id - 1))->plan_rows, 2.0), 1000000.0);
	return 2;
}

/*
 * The keys the value of a key condition gives, into sc: its constants, or, for a statement with parameters, the expression the scan evaluates when it starts (sc->param: a $n, a list
 * of them, an InitPlan's output, a stable function; none that reads a row or an executor parameter of an outer loop, which change while the scan runs) with the number of keys the plan
 * expects of it (1 for an integer, the elements of a list, the rows of an InitPlan's sub-select, SCOPE_PARAM_KEYS for any other array). false: the value is neither.
 */
static bool
key_source(AggCtx *c, Node *val, SiftAggScope *sc)
{
	Oid			type = exprType(val);
	Oid			elem = type_is_array(type) ? get_element_type(type) : type;

	if (elem != INT2OID && elem != INT4OID && elem != INT8OID)
		return false;
	if (IsA(val, Const))
	{
		sc->keys = sift_agg_scope_keys(type, ((Const *) val)->constvalue, ((Const *) val)->constisnull, &sc->nkeys);
		return !((Const *) val)->constisnull;
	}
	sc->param_desc = expr_text(c, val);
	if (pseudo_const(val))
	{
		sc->param = val;
		sc->nkeys = elem == type ? 1 : IsA(val, ArrayExpr) ? list_length(((ArrayExpr *) val)->elements) : SCOPE_PARAM_KEYS;
	}
	else if (IsA(val, Param) && ((Param *) val)->paramkind == PARAM_EXEC && initplan_ok(c, (Param *) val))
	{
		sc->nkeys = elem == type ? 1 : initplan_rows(c, (Param *) val);
		sc->param = (Node *) initplan_param(c, (Param *) val);
	}
	else
		return false;
	return true;
}

/* does the statement read a column of the item above the scan: a group key, an aggregate argument, the select list (the conditions that bind it are not read above) */
bool
read_above(PlannerInfo *root, Index rti)
{
	return bms_is_member(rti, pull_varnos(root, (Node *) root->processed_tlist)) || bms_is_member(rti, pull_varnos(root, root->parse->havingQual));
}

/*
 * How the statement reads the item rti above the scan, for a join that counts a row for each pair (allowed): not at all (NULL, *key false), or its key column as a group key and
 * nothing else of it (NULL, *key true): the scan groups by the derived leaf, whose type is int8, so the Var's int2 or int4 key is the leaf narrowed to a value it holds, as the join's. Else why not: a
 * column read as a value (an aggregate argument, an expression, HAVING), grouping sets, DISTINCT ON, or no grouping at all.
 */
static const char *
key_above(AggCtx *c, Index rti, const AttrNumber *map, int nmap, AttrNumber key, bool allowed, bool *as_key)
{
	PlannerInfo *root = c->root;
	Query	   *parse = root->parse;

	*as_key = false;
	if (!read_above(root, rti))
		return NULL;
	if (!allowed)
		return "the statement reads a column of the scope relation (a group key, an aggregate argument), which a semi join does not give";
	if (parse->groupClause == NIL && parse->distinctClause == NIL)
		return "the statement reads a column of the scope relation, and groups by none";
	if (parse->groupingSets != NIL || parse->hasDistinctOn)
		return "the statement reads the key of the scope relation in grouping sets or DISTINCT ON";
	if (parse->havingQual != NULL && bms_is_member(rti, pull_varnos(root, parse->havingQual)))
		return "HAVING reads a column of the scope relation";
	foreach_node(TargetEntry, te, root->processed_tlist)
	{
		const Var  *v = (const Var *) te->expr;

		if (!bms_is_member(rti, pull_varnos(root, (Node *) te->expr)))
			continue;
		if (!IsA(te->expr, Var) || v->varno != rti || v->varnullingrels != NULL || item_attno(map, nmap, v->varattno) != key)
			return psprintf("the statement reads a column of the scope relation other than its key as a group key: %s", expr_text(c, (Node *) te->expr));
	}
	*as_key = true;
	return NULL;
}

/*
 * The key a union's arms bind and the one the statement binds (keyval, in the condition keyq on the item): the same, or the arms alone bind it, both of them. NULL: it is the scope's key, into sc
 * when the arms are what binds it; else why not.
 */
static const char *
arm_key(AggCtx *c, const ScopeItem *it, Node *keyq, Node *keyval, SiftAggScope *sc)
{
	if (keyq != NULL)
		return equal(keyval, it->key) ? NULL : "the arms of the union bind another key than the statement";
	if (!it->key_both)
		return "an arm of the union does not bind the scope key";
	return key_source(c, it->key, sc) ? NULL : "the arms of the union bind the scope key by other than constants or parameters";
}

/*
 * The item rti of the FROM list as the scope join of the index: what the pass takes out of it is in t. NULL: it is, and the pass has its conditions; NOT_SCOPE: it is not the scope relation
 * of the index; else why it is but not a join the scope read answers.
 */
static const char *
take_item(AggCtx *c, FromScan *fs, const SiftScopeSpecs *specs, Index rti, ScopeTake *t)
{
	PlannerInfo *root = c->root;
	ScopeItem	it = scope_item(c, specs, rti);
	const SiftScopeSpec *sp = it.sp;
	AttrNumber *map = it.map;
	int			nmap = it.nmap;
	const char *why = it.why;
	Oid			relid = it.relid;
	LeftJoin   *lj = NULL;
	Node	   *joinq = NULL,
			   *keyq = NULL,
			   *keyval = NULL,
			   *tabq = NULL;
	Var		   *fk = NULL,
			   *kvar = NULL;
	bool		key_read,
				once;
	SiftAggScope sc = {0};
	List	   *pool,
			   *rest = NIL;
	Relids		own,
				tables = NULL;

	if (sp == NULL)
		return item_strays(c, specs, rti);
	if (why != NULL)
		return why;
	if (sp->joined != NULL && !it.is_union)
		return "the pairs of the scope are the union of two paths: a join to one of its relations is not the pair set";
	foreach_ptr(LeftJoin, o, fs->lefts)
		if (o->rti == rti)
			lj = o;
	if (lj != NULL && lj->kind != SJ_SEMI)
		return "the scope relation is not joined by an inner or semi join (IN, EXISTS)";
	if (lj == NULL && scope_multiset(c->indexoid, sp->key_name))
		return SCOPE_MULTISET;
	once = lj != NULL || it.member_only;
	if (it.all && lj == NULL)
		return "a join to a UNION ALL of the two paths of the scope counts a member that both paths reach twice: the pairs are the distinct rows of a UNION";
	pool = list_concat_copy(list_concat_copy(fs->quals, fs->scan_quals), lj ? list_concat_copy(lj->quals, lj->inner) : NIL);
	if (lj != NULL && lj->tree != NULL && !scan_tree(lj->rarg, &tables, &pool))
		return "the sub-select of the semi join has a join other than a semi join";
	/* a semi join of a table to the scope relation's key is a table of keys (`s.key IN (SELECT ..)`); a join of another kind reading it is no scope read */
	foreach_ptr(LeftJoin, o, fs->lefts)
	{
		if (o != lj && o->kind == SJ_SEMI)
			pool = list_concat_copy(pool, o->quals);
		else
			foreach_ptr(Node, q, o->quals)
				if (o != lj && bms_is_member(rti, pull_varnos(root, q)))
					return psprintf("the join to %s reads a column of the scope relation", get_rel_name(planner_rt_fetch(o->rti, root)->relid));
	}
	foreach_int(o, fs->rtis)
		if (o != rti && root->simple_rel_array[o] != NULL && bms_is_member(rti, root->simple_rel_array[o]->lateral_relids))
			return "another item of the FROM list reads the scope relation";
	if ((why = key_above(c, rti, map, nmap, get_attnum(relid, sp->rel.b), lj == NULL, &key_read)) != NULL)
		return why;
	own = bms_add_member(bms_make_singleton(rti), c->relid);
	foreach_ptr(Node, q, pool)
	{
		Relids		v = pull_varnos(root, q);
		Var		   *theirs,
				   *ours;
		Node	   *val;

		if (!bms_is_member(rti, v))
			continue;
		if (!bms_is_subset(v, own))
		{
			if (tabq != NULL && equal(q, tabq))
				continue;
			if (tabq != NULL || (kvar = key_equality(rti, map, nmap, get_attnum(relid, sp->rel.b), q)) == NULL)
				return psprintf("a condition reads the scope relation and another table (not the equality of its key to the key column of a table): %s", expr_text(c, q));
			tabq = q;
			continue;
		}
		if (member_equality(c, rti, q, &theirs, &ours))
		{
			if (item_attno(map, nmap, theirs->varattno) != get_attnum(relid, sp->rel.a) || ours->varattno != get_attnum(c->heapoid, sp->fk))
				return psprintf("the join is not by the declared member column %s and the declared column %s of the scanned table", sp->rel.a, sp->fk);
			if (joinq != NULL && !equal(q, joinq))
				return "the scope relation is joined by more than one equality";
			joinq = q;
			fk = ours;
		}
		else if ((val = key_operand(rti, map, nmap, get_attnum(relid, sp->rel.b), q)) != NULL)
		{
			if (!key_source(c, val, &sc))
				return psprintf("the scope relation has another condition: %s", expr_text(c, q));
			if (keyq != NULL && !equal(q, keyq))
				return "the scope key is bound more than once";
			keyq = q;
			keyval = val;
		}
		else if (bms_membership(v) == BMS_SINGLETON)
			rest = lappend(rest, q);
		else
			return psprintf("the scope relation has another condition: %s", expr_text(c, q));
	}
	if (joinq == NULL)
		return psprintf("the scope relation is not joined by its member column %s to the column %s of the scanned table", sp->rel.a, sp->fk);
	if (lj != NULL && lj->tree != NULL && (tabq == NULL || !bms_equal(tables, bms_add_member(bms_make_singleton(rti), kvar->varno))))
		return "the scope relation is joined to other tables inside the sub-select of the semi join";
	if (tabq != NULL)
	{
		const char *miss = keyq != NULL || it.key != NULL ? "the scope key is bound more than once" : scope_key_table(c, fs, pool, tabq, kvar, rti, lj != NULL, &sc, t);

		if (miss != NULL)
			return miss;
		keyq = tabq;
	}
	if (it.key != NULL && (why = arm_key(c, &it, keyq, keyval, &sc)) != NULL)
		return why;
	keyq = keyq != NULL ? keyq : it.key;
	/* a join with no condition on the key reads every key, a row for each pair; a semi join of every key is not that, nor is a union of members */
	sc.all = keyq == NULL && lj == NULL && !it.member_only;
	if (keyq == NULL && !sc.all)
		return "the scope key is not bound by an equality or an IN list of constants or parameters";
	/* several keys or every key counted for each pair, or a group key: where the units cannot (the reader refuses them, or the join costs less) the node runs the join the first pass priced, so it needs one */
	if ((sc.all || sc.nkeys > 1 || key_read) && !once && t->alts == NIL)
		return "several keys or every key, or the key as a group key, and a row counts once for each pair: where the units cannot answer the node runs the join, which the first pass does not have";
	if (it.is_union && rest != NIL)
		return psprintf("the union of the two paths of the scope has another condition: %s", expr_text(c, linitial(rest)));
	if (!it.is_union && (why = scope_pairs_proof(c, specs, sp, rti, map, nmap, rest)) != NULL)
		return why;
	t->rti = rti;
	t->map = map;
	t->nmap = nmap;
	t->key = get_attnum(relid, sp->rel.b);
	t->key_read = key_read;
	t->fk = (Var *) copyObject(fk);
	sc.key_name = sp->key_name;
	sc.once = once;
	t->sc = sc;
	if (tabq == NULL)
		t->pairs = root->simple_rel_array[rti]->rows;
	scope_union_perms(root, &it);
	return NULL;
}

bool
sift_agg_scope_item(AggCtx *c, Index *rti, AttrNumber *key)
{
	Relation	index = index_open(c->indexoid, AccessShareLock);
	SiftScopeSpecs *specs = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;

	index_close(index, NoLock);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);
		const SiftScopeSpec *sp = rte->rtekind == RTE_RELATION ? declared_scope(c->heapoid, specs, rte->relid) : NULL;

		if (sp != NULL && sp->joined == NULL)
		{
			*rti = jc->rti;
			*key = get_attnum(rte->relid, sp->rel.b);
			return true;
		}
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery->setOperations != NULL)
		{
			ScopeItem	it = scope_union_item(c, specs, rte->subquery);

			for (int i = 1; it.is_union && !it.all && i < it.nmap; i++)
				if (it.map[i] == get_attnum(it.relid, it.sp->rel.b))
				{
					*rti = jc->rti;
					*key = i;
					return true;
				}
		}
	}
	return false;
}

/*
 * Why a relation of a scope of the index stays in the statement where the pass has no item of the FROM list to take: a table inside a join of the sub-select of a semi or anti join, or a
 * sub-select the planner runs as a plan of its own (NOT IN, OR, a scalar or an ARRAY sub-select, a MATERIALIZED CTE, the select list). NULL: none does.
 */
static const char *
scope_strays(AggCtx *c, const FromScan *fs, const SiftScopeSpecs *specs)
{
	ScopeSeen	seen = {c->heapoid, specs};
	int			plan = 0;

	for (int rti = -1; (rti = bms_next_member(fs->hidden, rti)) >= 0;)
		if (scope_seen_walker((Node *) planner_rt_fetch(rti, c->root), &seen))
			return "the scope relation is a table inside a join in the sub-select of a semi join: only a semi join to the scope relation itself is read from the units";
	foreach_ptr(PlannerInfo, sub, c->root->glob->subroots)
	{
		plan++;
		if (sub->parent_root != c->root || !scope_seen_walker((Node *) sub->parse, &seen))
			continue;
		foreach_ptr(SubPlan, sp, c->root->init_plans)
			if (sp->plan_id == plan && sp->subLinkType == ARRAY_SUBLINK)
				return "ARRAY(SELECT ..) over the scope relation is an InitPlan the planner runs before the scan, so this plan joins as written; the statement is also planned with IN (SELECT ..) in its place where the sub-select has no ORDER BY, LIMIT, DISTINCT or aggregate, and the cheaper plan runs (sift.explain_why of the IN form says why it is not read from the units)";
		return "a sub-select over the scope relation is a plan of its own (NOT IN, OR, a scalar sub-select, a MATERIALIZED CTE, the select list): only a join, IN or EXISTS that PostgreSQL pulls up into the join is read from the units";
	}
	return NULL;
}

/* Does the first pass scan an index that declares scopes, with tables joined to it: the statement may have a scope join for a second pass to take out; says why a relation of a scope that no pass can take stays in it */
bool
sift_agg_scope_wanted(AggCtx *c, const FromScan *fs)
{
	Relation	index;
	SiftScopeSpecs *specs;
	const char *why;
	Index		rti;
	AttrNumber	key;

	if (!sift_agg_scope || !OidIsValid(c->indexoid) || (c->joins == NIL && c->root->glob->subroots == NIL))
		return false;
	index = index_open(c->indexoid, AccessShareLock);
	specs = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;
	index_close(index, NoLock);
	if (specs == NULL)
		return false;
	if ((why = scope_strays(c, fs, specs)) != NULL)
		scope_miss(why);
	if (c->nparts > 0)
	{
		if (sift_agg_scope_item(c, &rti, &key))
			scope_miss("a scan of partitions reads no scope");
		return false;
	}
	return c->joins != NIL;
}

/* the item rti leaves the FROM list with the conditions that read it and its outer or semi join */
static void
drop_item(PlannerInfo *root, FromScan *fs, List **joined, Index rti)
{
	foreach_ptr(Node, q, fs->quals)
		if (bms_is_member(rti, pull_varnos(root, q)))
			fs->quals = foreach_delete_current(fs->quals, q);
	foreach_ptr(Node, q, fs->scan_quals)
		if (bms_is_member(rti, pull_varnos(root, q)))
			fs->scan_quals = foreach_delete_current(fs->scan_quals, q);
	foreach_ptr(LeftJoin, o, fs->lefts)
		if (o->rti == rti)
			fs->lefts = foreach_delete_current(fs->lefts, o);
	*joined = list_delete_int(*joined, rti);
	fs->rtis = list_delete_int(fs->rtis, rti);
}

/*
 * The second pass: the item of the FROM list that is the scope relation of the index, the join of the statement to the scanned table, leaves the FROM list with its conditions
 * (the member equality, the key binding), which are the scope conjunct sift_agg_scope_extract makes. false (with the reason reported when the statement has a join to the
 * relation that is none the scope read answers): the pass has nothing to plan.
 */
bool
sift_agg_scope_take(AggCtx *c, FromScan *fs, List **joined)
{
	ScopeTake  *t = fs->scope;
	Relation	index = index_open(c->indexoid, AccessShareLock);
	SiftScopeSpecs *specs = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;
	const char *why = NOT_SCOPE;

	index_close(index, NoLock);
	foreach_int(rti, *joined)
		if ((why = take_item(c, fs, specs, rti, t)) != NOT_SCOPE)
			break;
	if (why != NULL)
	{
		if (why != NOT_SCOPE)
			scope_miss(why);
		return false;
	}
	drop_item(c->root, fs, joined, t->rti);
	if (t->krti != 0)
		drop_item(c->root, fs, joined, t->krti);
	foreach_int(rti, *joined)
		if (scope_item(c, specs, rti).sp != NULL)
		{
			scope_miss("a scan reads one scope relation: another join to a scope relation stays a join");
			break;
		}
	c->scope_take = t;
	return true;
}

/* why a scan reads no scope at all; NULL when it may */
static const char *
scan_why(AggCtx *c)
{
	if (!sift_agg_scope)
		return "disabled";
	if (c->nparts > 0)
		return "partitioned scan";
	if (c->search != NULL)
		return "text search scan";
	return OidIsValid(c->indexoid) ? NULL : "scan of a table, not of a column index";
}

/*
 * The scope conjunct of quals into s->scope, out of the list; a conjunct that holds a marker and stays (the scan reads no scope in `held`'s words, it is not the conjunct itself,
 * a scope is read already, or marker_scope says why not) is the first reason of s->scope_why.
 */
static void
take_scope(AggCtx *c, SiftAggSpec *s, List **quals, const char *held)
{
	List	   *rest = NIL;

	foreach_ptr(Node, q, *quals)
	{
		const char *why = held;
		SiftAggScope *sc = NULL;

		if (!marker_walker(q, NULL))
		{
			rest = lappend(rest, q);
			continue;
		}
		if (why == NULL && !is_marker(q))
			why = "scope qual inside another expression";
		if (why == NULL && s->scope != NULL)
			why = "second scope qual: a scan reads one scope";
		if (why == NULL)
			sc = marker_scope(c, q, &why);
		if (sc != NULL)
		{
			s->scope = sc;
			continue;
		}
		if (s->scope_why == NULL)
			s->scope_why = why;
		rest = lappend(rest, q);
	}
	*quals = rest;
}

/* take_scope over the conjuncts of the scan that may hold one (see the head of the file); scan: why the scan reads no scope at all, or NULL */
static void
scope_scan(AggCtx *c, SiftAggSpec *s, FromScan *fs, const char *scan)
{
	const char *nulled = preserved_join(c) ? "scope qual under an outer join that can null it" : NULL;

	s->scope = NULL;
	s->scope_why = NULL;
	take_scope(c, s, &fs->scan_quals, scan);
	take_scope(c, s, &fs->quals, scan ? scan : nulled);
	foreach_ptr(JoinCtx, jc, c->joins)
		take_scope(c, s, &jc->clauses, scan ? scan : "scope qual in the ON clause of a join that keeps the scanned rows");
}

/*
 * The scope conjunct of the scan into s->scope. The second pass of a statement over a scope index (c->scope_take) has the one it took out of the FROM list, and false says there is no
 * scope read to plan (the reason is reported, slug scope). The first pass has the marker, out of the conjuncts of the scan; none the scan reads: they stay as filters and
 * s->scope_why says why.
 */
bool
sift_agg_scope_extract(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	ScopeTake  *t = c->scope_take;
	const char *why = scan_why(c);
	int			leaf;

	c->scope_key = NULL;
	if (t == NULL)
	{
		scope_scan(c, s, fs, why);
		return true;
	}
	if (why == NULL && (leaf = fk_leaf(c, (Node *) t->fk, t->sc.key_name, &why)) >= 0)
	{
		s->scope = scope_make(c, &t->sc, leaf);
		c->scope_key = t->key_read ? t->sc.key_name : NULL;
		return true;
	}
	scope_miss(why);
	return false;
}

/* A scan that builds its spec in another way reads no scope: a conjunct that holds a marker stays a filter, and s->scope_why is `why` */
void
sift_agg_scope_declined(AggCtx *c, SiftAggSpec *s, FromScan *fs, const char *why)
{
	c->scope_key = NULL;
	scope_scan(c, s, fs, why);
}
