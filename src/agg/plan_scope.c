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
 *  (b) the scope key is bound by one equality or IN list of constants or of parameters (a $n, an InitPlan's output, a stable expression: the scan evaluates them when it starts), and the
 *      scope relation has no other condition and no other table joined to it;
 *  (c) the statement reads no column of the scope relation (a group key, an aggregate argument): the declaration needs a unique btree on (member, key), so a row of the scanned table
 *      has at most one pair with a key and the join, the semi join and the scope read count it the same;
 *  (d) a semi join (IN, EXISTS) counts a row once whichever keys it has, which the reader does (Scope Semantics: once); a join counts it once for each pair, which the reader reads
 *      per pair only for one key: of several, a member whose pairs changed needs the join (docs/COL-SCOPE.md), so the plan keeps it (an array or a list of parameters may hold several: a
 *      join takes a scalar parameter only);
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
#include "access/stratnum.h"
#include "catalog/namespace.h"
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

/* sift.<name>(index, key_name, fk [, keys]) of the testing extension */
static bool
is_func(Node *q, const char *name, int nargs)
{
	return IsA(q, FuncExpr) && list_length(((FuncExpr *) q)->args) == nargs && get_func_namespace(((FuncExpr *) q)->funcid) == get_namespace_oid("sift", true) &&
		strcmp(get_func_name(((FuncExpr *) q)->funcid), name) == 0;
}

static bool
is_marker(Node *q)
{
	return is_func(q, "in_scope", 4) || is_func(q, "in_scope_pairs", 4) || is_func(q, "in_scope_all", 3);
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
	bool		all = list_length(f->args) == 3;
	Const	   *idx = arg_const(f, 0, REGCLASSOID),
			   *name = arg_const(f, 1, TEXTOID),
			   *key = all ? NULL : arg_const(f, 3, INT8OID);
	int			leaf;
	SiftAggScope proto = {0};

	if (key == NULL && !all)
		key = arg_const(f, 3, INT8ARRAYOID);
	if (idx == NULL || name == NULL || (key == NULL && !all))
		return no_scope(why, "scope index, name or keys are not constants");
	if (DatumGetObjectId(idx->constvalue) != c->indexoid)
		return no_scope(why, "scope index is not the scanned index");
	if ((leaf = fk_leaf(c, list_nth(f->args, 2), TextDatumGetCString(name->constvalue), why)) < 0)
		return NULL;
	if (!all && key->consttype == INT8ARRAYOID && ARR_HASNULL(DatumGetArrayTypeP(key->constvalue)))
		return no_scope(why, "a scope key is NULL");
	proto.key_name = TextDatumGetCString(name->constvalue);
	proto.all = all;
	if (!all)
		proto.keys = sift_agg_scope_keys(key->consttype, key->constvalue, false, &proto.nkeys);
	proto.once = strcmp(get_func_name(f->funcid), "in_scope") == 0;
	return scope_make(c, &proto, leaf);
}

/*
 * The expression that stands for the derived key of a scope the scan reads every key of: the testing marker sift.scope_key(index, key_name, fk), in a group key or an aggregate. Its leaf is the one
 * of the derived key "<key_name>#scope", whose value is the key of each row the scan reads (the reader gives only rows that have one).
 * RELEASE-TODO: the recognition of the join replaces it: the key column of the scope relation in a GROUP BY of the join, read as this leaf, of the type of that column.
 */
int
sift_agg_scope_key_leaf(AggCtx *c, Node *e)
{
	FuncExpr   *f = (FuncExpr *) e;
	Const	   *idx,
			   *name;
	const char *why;

	if (c->scope_all == NULL || !is_func(e, "scope_key", 3))
		return -1;
	idx = arg_const(f, 0, REGCLASSOID);
	name = arg_const(f, 1, TEXTOID);
	if (idx == NULL || name == NULL || DatumGetObjectId(idx->constvalue) != c->indexoid || strcmp(TextDatumGetCString(name->constvalue), c->scope_all) != 0 ||
		fk_leaf(c, list_nth(f->args, 2), c->scope_all, &why) < 0)
		return -1;
	return sift_agg_scope_leaf(c->lay, c->scope_all);
}

/* ---- the join to the scope relation ---- */

/* the integer btree equality `=` (also between int2, int4 and int8): the scope key compares as the join does */
static bool
integer_eq(Oid opno)
{
	return op_in_opfamily(opno, INTEGER_BTREE_FAM_OID) && get_op_opfamily_strategy(opno, INTEGER_BTREE_FAM_OID) == BTEqualStrategyNumber;
}

/* the declared scope of the index that names the relation, else NULL */
static const SiftScopeSpec *
declared_scope(Oid heapoid, const SiftScopeSpecs *specs, Oid relid)
{
	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		const SiftScopeSpec *sp = specs->s[i];
		Oid			nsp = sp->rel.schema ? get_namespace_oid(sp->rel.schema, true) : get_rel_namespace(heapoid);

		/* the pairs of a scope of two paths are not the rows of its first relation: a join to it is not the scope */
		if (sp->joined == NULL && OidIsValid(nsp) && get_relname_relid(sp->rel.name, nsp) == relid)
			return sp;
	}
	return NULL;
}

/*
 * The table an item of the FROM list is: itself, or the one table of a sub-select PostgreSQL did not flatten (a security barrier view) that projects its columns, which is then read
 * through (map[resno] is the column of the table the sub-select outputs as resno, 0 for another expression; *nmap entries). *why: it is that table's, but not one the join may be
 * read through. InvalidOid: it is neither.
 */
static Oid
scope_table(PlannerInfo *root, Index rti, AttrNumber **map, int *nmap, const char **why)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	RelOptInfo *rel = root->simple_rel_array[rti];
	Query	   *q;
	RangeTblEntry *base;

	if (rel == NULL)
		return InvalidOid;
	if (rte->rtekind == RTE_RELATION)
	{
		if (rte->securityQuals != NIL)
			*why = "row-level security applies to the scope relation";
		return rte->inh ? InvalidOid : rte->relid;
	}
	if (rte->rtekind != RTE_SUBQUERY || !bms_is_empty(rel->lateral_relids))
		return InvalidOid;
	q = rte->subquery;
	if (list_length(q->rtable) != 1 || (base = linitial_node(RangeTblEntry, q->rtable))->rtekind != RTE_RELATION)
		return InvalidOid;
	if (q->hasAggs || q->hasWindowFuncs || q->hasSubLinks || q->groupClause != NIL || q->distinctClause != NIL || q->limitCount != NULL ||
		q->limitOffset != NULL || q->setOperations != NULL || q->cteList != NIL || q->sortClause != NIL || q->rowMarks != NIL)
		*why = "the sub-select over the scope relation is not a plain projection of its columns";
	else if (q->jointree->quals != NULL || base->securityQuals != NIL)
		*why = "the view over the scope relation has a condition of its own";
	else
	{
		*nmap = list_length(q->targetList) + 1;
		*map = palloc0(*nmap * sizeof(AttrNumber));
		foreach_node(TargetEntry, te, q->targetList)
			if (!te->resjunk && IsA(te->expr, Var) && ((Var *) te->expr)->varlevelsup == 0)
				(*map)[te->resno] = ((Var *) te->expr)->varattno;
	}
	return base->relid;
}

/* the column of the table that column att of the item is (InvalidAttrNumber: none) */
static AttrNumber
item_attno(const AttrNumber *map, int nmap, AttrNumber att)
{
	return map == NULL ? att : att > 0 && att < nmap ? map[att] : InvalidAttrNumber;
}

/* n is a plain column of the item rti */
static Var *
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
static Node *
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
static bool
read_above(PlannerInfo *root, Index rti)
{
	return bms_is_member(rti, pull_varnos(root, (Node *) root->processed_tlist)) || bms_is_member(rti, pull_varnos(root, root->parse->havingQual));
}

/*
 * The item rti of the FROM list as the scope join of the index: what the pass takes out of it is in t. NULL: it is, and the pass has its conditions; NOT_SCOPE: it is not the scope relation
 * of the index; else why it is but not a join the scope read answers.
 */
static const char *
take_item(AggCtx *c, FromScan *fs, const SiftScopeSpecs *specs, Index rti, ScopeTake *t)
{
	PlannerInfo *root = c->root;
	AttrNumber *map = NULL;
	int			nmap = 0;
	const char *why = NULL;
	Oid			relid = scope_table(root, rti, &map, &nmap, &why);
	const SiftScopeSpec *sp = OidIsValid(relid) ? declared_scope(c->heapoid, specs, relid) : NULL;
	LeftJoin   *lj = NULL;
	Node	   *joinq = NULL,
			   *keyq = NULL;
	Var		   *fk = NULL;
	SiftAggScope sc = {0};
	Relids		own;

	if (sp == NULL)
		return NOT_SCOPE;
	if (why != NULL)
		return why;
	foreach_ptr(LeftJoin, o, fs->lefts)
		if (o->rti == rti)
			lj = o;
	if (lj != NULL && lj->kind != SJ_SEMI)
		return "the scope relation is not joined by an inner or semi join (IN, EXISTS)";
	if (lj != NULL && lj->tree != NULL)
		return "the scope relation is joined to other tables inside the sub-select of the semi join";
	foreach_ptr(LeftJoin, o, fs->lefts)
		foreach_ptr(Node, q, o->quals)
			if (o != lj && bms_is_member(rti, pull_varnos(root, q)))
				return psprintf("the join to %s reads a column of the scope relation", get_rel_name(planner_rt_fetch(o->rti, root)->relid));
	foreach_int(o, fs->rtis)
		if (o != rti && root->simple_rel_array[o] != NULL && bms_is_member(rti, root->simple_rel_array[o]->lateral_relids))
			return "another item of the FROM list reads the scope relation";
	if (read_above(root, rti))
		return "the statement reads a column of the scope relation (a group key, an aggregate argument), which the scope read does not give";
	own = bms_add_member(bms_make_singleton(rti), c->relid);
	foreach_ptr(Node, q, list_concat_copy(list_concat_copy(fs->quals, fs->scan_quals), lj ? lj->quals : NIL))
	{
		Relids		v = pull_varnos(root, q);
		Var		   *theirs,
				   *ours;
		Node	   *val;

		if (!bms_is_member(rti, v))
			continue;
		if (!bms_is_subset(v, own))
			return psprintf("a condition reads the scope relation and another table (a table of keys, a join): %s", expr_text(c, q));
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
		}
		else
			return psprintf("the scope relation has another condition: %s", expr_text(c, q));
	}
	if (joinq == NULL)
		return psprintf("the scope relation is not joined by its member column %s to the column %s of the scanned table", sp->rel.a, sp->fk);
	if (keyq == NULL)
		return "the scope key is not bound by an equality or an IN list of constants or parameters";
	if (sc.nkeys > 1 && lj == NULL)
		return "several keys, and a row counts once for each pair: a member whose pairs changed needs the join, which counts them";
	t->rti = rti;
	t->fk = (Var *) copyObject(fk);
	sc.key_name = sp->key_name;
	sc.once = lj != NULL;
	t->sc = sc;
	t->pairs = root->simple_rel_array[rti]->rows;
	return NULL;
}

/* Does the first pass scan an index that declares scopes, with tables joined to it: the statement may have a scope join for a second pass to take out */
bool
sift_agg_scope_wanted(AggCtx *c)
{
	Relation	index;
	bool		declared;

	if (!sift_agg_scope || c->nparts > 0 || c->joins == NIL || !OidIsValid(c->indexoid))
		return false;
	index = index_open(c->indexoid, AccessShareLock);
	declared = sift_col_is_index(index) && sift_scope_specs(index) != NULL;
	index_close(index, NoLock);
	return declared;
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
	foreach_ptr(Node, q, fs->quals)
		if (bms_is_member(t->rti, pull_varnos(c->root, q)))
			fs->quals = foreach_delete_current(fs->quals, q);
	foreach_ptr(Node, q, fs->scan_quals)
		if (bms_is_member(t->rti, pull_varnos(c->root, q)))
			fs->scan_quals = foreach_delete_current(fs->scan_quals, q);
	foreach_ptr(LeftJoin, o, fs->lefts)
		if (o->rti == t->rti)
			fs->lefts = foreach_delete_current(fs->lefts, o);
	*joined = list_delete_int(*joined, t->rti);
	fs->rtis = list_delete_int(fs->rtis, t->rti);
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

	c->scope_all = NULL;
	if (t == NULL)
	{
		scope_scan(c, s, fs, why);
		c->scope_all = s->scope != NULL && s->scope->all ? s->scope->key_name : NULL;
		return true;
	}
	if (why == NULL && (leaf = fk_leaf(c, (Node *) t->fk, t->sc.key_name, &why)) >= 0)
	{
		s->scope = scope_make(c, &t->sc, leaf);
		return true;
	}
	scope_miss(why);
	return false;
}

/* A scan that builds its spec in another way reads no scope: a conjunct that holds a marker stays a filter, and s->scope_why is `why` */
void
sift_agg_scope_declined(AggCtx *c, SiftAggSpec *s, FromScan *fs, const char *why)
{
	c->scope_all = NULL;
	scope_scan(c, s, fs, why);
}
