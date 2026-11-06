/*
 * agg/flat/probe.c - an EXISTS, NOT EXISTS, IN or a join to a large table as a lateral probe
 * `WHERE EXISTS (SELECT 1 FROM big b WHERE b.k = x.k AND ..)` is a semi join to `big`, which the scan reads whole or, for the keys of its IN list, by one btree scan. When the
 * keys are the scan's own (no list), the table can only be read whole, though a probe of the btree for each distinct key (a correlated child, taken by the scan
 * directly: agg/combo/probe.c) reads the entries of the keys the scan meets and no others. The same query written with the sub-select as a LATERAL child
 * (`JOIN LATERAL (SELECT true AS m FROM big b WHERE b.k = x.k AND .. LIMIT 1) p ON true`; `LEFT JOIN LATERAL .. WHERE p.m IS NULL` for NOT EXISTS) has the same rows; the
 * planner hook plans it too and keeps it where it runs in the scan and costs less. An inner or left join to the table (`x JOIN big b ON b.k = x.k AND ..`) is the same with every
 * row of the key (OFFSET 0 for LIMIT 1): see probe_joins below.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

bool		sift_agg_probe_rewrite = true;

/*
 * The child of a probe is walked in its btree by the scan (agg/combo/probe.c), which takes an Index (Only) Scan; a Bitmap Heap Scan is a run of the child that
 * the scan cannot take apart, and costs more instructions (10 to 14 percent more over 60 and 1,000 keys of rows fetched from the heap). The tables of such a
 * child have no bitmap scan: the sub-select is marked by the length of its statement, which nothing else reads.
 */
#define PROBE_MARK	(-4242)

static void
probe_no_bitmap(PlannerInfo *root, RelOptInfo *rel)
{
	if (root->parent_root != NULL && root->parse->stmt_len == PROBE_MARK)
		foreach_node(IndexOptInfo, ii, rel->indexlist)
			ii->amhasgetbitmap = false;
}

/* up to PostgreSQL 18 get_relation_info_hook; 19 has build_simple_rel_hook instead (as in col/amscan/cost.c) */
#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
probe_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	probe_no_bitmap(root, rel);
}

void
sift_agg_probe_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = probe_get_relation_info;
}
#else
static build_simple_rel_hook_type prev_simple_rel_hook;

static void
probe_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	probe_no_bitmap(root, rel);
}

void
sift_agg_probe_init(void)
{
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = probe_build_simple_rel;
}
#endif
bool		sift_agg_probe_applied;
bool		sift_agg_probe_mixed;

/* the tables smaller than this are read whole at the load of the scan: a probe for each key does not beat that */
#define PROBE_MIN_ROWS	10000.0

typedef struct Probe
{
	Node	   *conj;			/* the conjunct of the WHERE */
	SubLink    *sl;
	bool		anti;
} Probe;

/* does a valid btree without a predicate start with column att of the table? */
static bool
btree_on(Oid relid, AttrNumber att)
{
	Relation	rel = relation_open(relid, NoLock);
	bool		found = false;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);

		found = idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisvalid && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			idx->rd_index->indkey.values[0] == att;
		index_close(idx, AccessShareLock);
		if (found)
			break;
	}
	relation_close(rel, NoLock);
	return found;
}

/* is `column = expr` of the table (entry 1 of the sub-select) with expr over the query around it alone, and a btree on the column? */
static bool
tie_column(Query *sub, Node *col, Node *other, Oid opno)
{
	RangeTblEntry *rte = linitial_node(RangeTblEntry, sub->rtable);

	while (IsA(col, RelabelType))
		col = (Node *) ((RelabelType *) col)->arg;
	return IsA(col, Var) && ((Var *) col)->varlevelsup == 0 && ((Var *) col)->varno == 1 && ((Var *) col)->varattno > 0 &&
		op_mergejoinable(opno, exprType(col)) && !contain_vars_of_level(other, 0) && contain_vars_of_level(other, 1) &&
		btree_on(rte->relid, ((Var *) col)->varattno);
}

/* does the WHERE of the sub-select tie its table to the query around it by an equality on a column a btree starts with? */
static bool
tied(Query *sub)
{
	foreach_ptr(Node, a, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		OpExpr	   *op;

		if (!IsA(a, OpExpr) || list_length(((OpExpr *) a)->args) != 2)
			continue;
		op = (OpExpr *) a;
		if (tie_column(sub, linitial(op->args), lsecond(op->args), op->opno) || tie_column(sub, lsecond(op->args), linitial(op->args), op->opno))
			return true;
	}
	return false;
}

/* a plain select of one large table, that reads the query around it and nothing above */
static bool
probe_sub(Query *sub)
{
	RangeTblEntry *rte;

	if (sub->commandType != CMD_SELECT || sub->jointree == NULL || sub->hasAggs || sub->hasWindowFuncs || sub->hasTargetSRFs || sub->hasDistinctOn || sub->hasSubLinks ||
		sub->groupClause != NIL || sub->groupingSets != NIL || sub->distinctClause != NIL || sub->havingQual != NULL || sub->limitCount != NULL || sub->limitOffset != NULL ||
		sub->setOperations != NULL || sub->cteList != NIL || sub->rowMarks != NIL || list_length(sub->rtable) != 1 || list_length(sub->jointree->fromlist) != 1)
		return false;
	rte = linitial_node(RangeTblEntry, sub->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample != NULL || rte->securityQuals != NIL)
		return false;
	if (contain_volatile_functions((Node *) sub->jointree) || contain_volatile_functions((Node *) sub->targetList) ||
		contain_vars_of_level((Node *) sub->jointree, 2) || contain_vars_of_level((Node *) sub->targetList, 2))
		return false;
	return sift_agg_table_rows(rte->relid) >= PROBE_MIN_ROWS;
}

/* the parameter of the sub-select's column in an IN's test expression, replaced by the column */
static Node *
column_for_param(Node *n, Node *col)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_SUBLINK)
		return copyObject(col);
	return expression_tree_mutator(n, column_for_param, col);
}

/* `x IN (SELECT c FROM t WHERE ..)` is `EXISTS (SELECT 1 FROM t WHERE .. AND c = x)` where it is a conjunct of the WHERE: the test expression for the sub-select's column */
static Node *
in_test(SubLink *sl, Query *sub)
{
	OpExpr	   *op;
	Node	   *t;
	TargetEntry *te;

	if (sl->subLinkType != ANY_SUBLINK || !IsA(sl->testexpr, OpExpr) || list_length(sub->targetList) != 1)
		return NULL;
	op = (OpExpr *) sl->testexpr;
	te = linitial_node(TargetEntry, sub->targetList);
	if (list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType((Node *) te->expr)) || contain_vars_of_level((Node *) te->expr, 1))
		return NULL;
	t = copyObject(sl->testexpr);
	IncrementVarSublevelsUp(t, 1, 0);
	return column_for_param(t, (Node *) te->expr);
}

/* is the column of the Var among the nonnullable ones of find_nonnullable_vars (a list of the attnos, as bits, of each range table entry, by its number) */
static bool
in_nonnullable(List *vars, Var *v)
{
	return v->varno < list_length(vars) && bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, list_nth(vars, v->varno));
}

/* does the column of a table have no NULL: declared so, or a condition of the WHERE that a NULL fails? */
static bool
not_null(Query *q, Node *e)
{
	List	   *vars;

	while (IsA(e, RelabelType))
		e = (Node *) ((RelabelType *) e)->arg;
	if (IsA(e, Const))
		return !((Const *) e)->constisnull;
	if (!IsA(e, Var) || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno <= 0)
		return false;
	if (bms_is_empty(((Var *) e)->varnullingrels))
	{
		RangeTblEntry *rte = rt_fetch(((Var *) e)->varno, q->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			HeapTuple	tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(rte->relid), Int16GetDatum(((Var *) e)->varattno));
			bool		declared = HeapTupleIsValid(tp) && ((Form_pg_attribute) GETSTRUCT(tp))->attnotnull;

			if (HeapTupleIsValid(tp))
				ReleaseSysCache(tp);
			if (declared)
				return true;
		}
	}
	vars = find_nonnullable_vars((Node *) q->jointree->quals);
	return in_nonnullable(vars, (Var *) e);
}

/*
 * `x NOT IN (SELECT c FROM t WHERE ..)` is NOT EXISTS of the equality where neither x nor c can be NULL (a NULL among the rows, or as x, makes NOT IN not true): the sub-select
 * is the test of a NOT EXISTS then. Otherwise it is the PostgreSQL's own plan (a hashed sub-plan) that runs.
 */
static bool
not_in_safe(Query *q, SubLink *sl, Query *sub)
{
	OpExpr	   *op = (OpExpr *) sl->testexpr;
	List	   *vars;

	if (!IsA(op, OpExpr) || list_length(op->args) != 2 || !not_null(q, linitial(op->args)))
		return false;
	vars = find_nonnullable_vars((Node *) sub->jointree->quals);
	{
		Node	   *c = (Node *) linitial_node(TargetEntry, sub->targetList)->expr;
		RangeTblEntry *rte = linitial_node(RangeTblEntry, sub->rtable);

		while (IsA(c, RelabelType))
			c = (Node *) ((RelabelType *) c)->arg;
		if (!IsA(c, Var) || ((Var *) c)->varattno <= 0)
			return false;
		if (in_nonnullable(vars, (Var *) c))
			return true;
		{
			HeapTuple	tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(rte->relid), Int16GetDatum(((Var *) c)->varattno));
			bool		declared = HeapTupleIsValid(tp) && ((Form_pg_attribute) GETSTRUCT(tp))->attnotnull;

			if (HeapTupleIsValid(tp))
				ReleaseSysCache(tp);
			return declared;
		}
	}
}

/* the conjuncts of the WHERE that probe_sub takes: EXISTS and IN, NOT EXISTS, NOT IN where it is NOT EXISTS */
static List *
probe_found(Query *q)
{
	List	   *found = NIL;

	foreach_ptr(Node, a, make_ands_implicit((Expr *) q->jointree->quals))
	{
		Probe	   *p = palloc0(sizeof(Probe));
		Node	   *n = a;
		Query	   *sub;

		if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == NOT_EXPR)
		{
			n = linitial(((BoolExpr *) n)->args);
			p->anti = true;
		}
		if (!IsA(n, SubLink) || !IsA(((SubLink *) n)->subselect, Query))
			continue;
		p->sl = (SubLink *) n;
		sub = castNode(Query, p->sl->subselect);
		if (p->sl->subLinkType == EXISTS_SUBLINK ? !(probe_sub(sub) && tied(sub)) : (!probe_sub(sub) || in_test(p->sl, sub) == NULL || (p->anti && !not_in_safe(q, p->sl, sub))))
			continue;
		p->conj = a;
		found = lappend(found, p);
	}
	return found;
}

/* the sub-select as the lateral child: one row of `true` for the first row that holds */
static Query *
probe_child(SubLink *sl)
{
	Query	   *sub = copyObject(castNode(Query, sl->subselect));

	if (sl->subLinkType == ANY_SUBLINK)
	{
		Node	   *t = in_test(sl, castNode(Query, sl->subselect));

		sub->jointree->quals = (Node *) make_ands_explicit(lappend(make_ands_implicit((Expr *) sub->jointree->quals), t));
	}
	sub->targetList = list_make1(makeTargetEntry((Expr *) makeBoolConst(true, false), 1, "m", false));
	sub->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(1), false, FLOAT8PASSBYVAL);
	sub->stmt_len = PROBE_MARK;
	return sub;
}

/* the FROM list as one item (an inner join on true of its items, tables or joins), for the left join of a NOT EXISTS around all of them */
static bool
fold_from(Query *q)
{
	FromExpr   *f = q->jointree;

	for (int i = 1; i < list_length(f->fromlist); i++)
		if (!IsA(list_nth(f->fromlist, i), RangeTblRef))
			return false;
	while (list_length(f->fromlist) > 1)
	{
		Node	   *right = lsecond(f->fromlist);
		JoinExpr   *j = flat_join_item(q, linitial(f->fromlist), ((RangeTblRef *) right)->rtindex, JOIN_INNER, (Node *) makeBoolConst(true, false));

		f->fromlist = lcons(j, list_delete_first(list_delete_first(f->fromlist)));
	}
	return true;
}

static bool keys_level(Query *q);
static bool reads_scan_table(Query *q);
static bool item_reads_scan(RangeTblEntry *rte);

bool
probe_sublinks(Query *q, bool apply)
{
	List	   *found,
			   *rest = NIL;
	bool		changed = false;

	if (!sift_agg_probe_rewrite || !q->hasSubLinks || q->commandType != CMD_SELECT || q->jointree == NULL || q->jointree->fromlist == NIL || q->setOperations != NULL ||
		q->rowMarks != NIL || keys_level(q))
		return false;
	found = probe_found(q);
	if (found == NIL || !apply)
		return found != NIL;
	/* the anti joins first: each is a left join around the whole FROM list, which the semi joins then join to */
	for (int pass = 0; pass < 2; pass++)
		foreach_ptr(Probe, p, found)
		{
			Index		rti;

			if (p->anti != (pass == 0) || (p->anti && !fold_from(q)))
				continue;
			rti = list_length(q->rtable) + 1;
			flat_add_subquery(q, probe_child(p->sl), SIFT_PROBE_ALIAS, true);
			if (p->anti)
			{
				JoinExpr   *j = flat_join_item(q, linitial(q->jointree->fromlist), rti, JOIN_LEFT, (Node *) makeBoolConst(true, false));
				NullTest   *t = makeNode(NullTest);

				linitial(q->jointree->fromlist) = j;
				t->arg = (Expr *) add_nulling_relids((Node *) makeVar(rti, 1, BOOLOID, -1, InvalidOid, 0), NULL, bms_make_singleton(j->rtindex));
				t->nulltesttype = IS_NULL;
				t->location = -1;
				rest = lappend(rest, t);
			}
			else
			{
				RangeTblRef *r = makeNode(RangeTblRef);

				r->rtindex = rti;
				q->jointree->fromlist = lappend(q->jointree->fromlist, r);
			}
			p->conj = NULL;
			changed = true;
		}
	foreach_ptr(Node, a, make_ands_implicit((Expr *) q->jointree->quals))
	{
		bool		taken = false;

		foreach_ptr(Probe, p, found)
			taken |= p->conj == NULL && p->sl != NULL && (Node *) p->sl == (IsA(a, BoolExpr) ? linitial(((BoolExpr *) a)->args) : a);
		if (!taken)
			rest = lappend(rest, a);
	}
	q->jointree->quals = rest == NIL ? NULL : (Node *) make_ands_explicit(rest);
	if (changed)
	{
		sift_agg_by_cost = true;
		sift_agg_probe_applied = true;
	}
	return changed;
}

/*
 * An inner or left join to a large table on a btree column, `x JOIN big b ON b.k = x.k AND ..` or `FROM x, big b WHERE b.k = x.k AND ..`, as the lateral child that has the same
 * rows (`x JOIN LATERAL (SELECT b.c, .. FROM big b WHERE b.k = x.k AND .. OFFSET 0) b ON true`): the entry of the table becomes the sub-select's, whose output has the columns
 * the query reads, so only the numbers of the columns in the Vars of the table change (a whole-row Var is a column of the output). The scan probes the btree for each distinct
 * key it meets; the planner keeps the plan where that costs less.
 */
typedef struct ProbeJoin
{
	Query	   *q;
	Index		rti;			/* the entry of the table */
	JoinExpr   *join;			/* its join, or NULL: it is an item of the FROM list, tied by the WHERE */
	FromExpr   *from;
	List	   *child;			/* the conjuncts of the ON or WHERE the sub-select takes */
	List	   *rest;			/* those that stay: on the other side alone, of an inner join */
	Bitmapset  *used;			/* the columns of the table the query reads (0: the row) */
	int		   *pos;			/* column of the table -> column of the output */
	int			top;			/* the highest column of the table the query reads */
	int			level;			/* the depth of the sub-select the walk is in */
	bool		bad;			/* a system column */
} ProbeJoin;

static bool used_walker(Node *n, ProbeJoin *c);

/* the columns of the table that the expression reads through the entry of a join or of the grouping */
static void
used_through(ProbeJoin *c, Node *alias)
{
	int			level = c->level;

	c->level = 0;
	used_walker(alias, c);
	c->level = level;
}

static bool
used_walker(Node *n, ProbeJoin *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varlevelsup == c->level)
		{
			RangeTblEntry *r = rt_fetch(v->varno, c->q->rtable);

			if (v->varno == c->rti)
			{
				if (v->varattno < 0)
					c->bad = true;
				else
					c->used = bms_add_member(c->used, v->varattno);
			}
			else if (r->rtekind == RTE_JOIN && v->varattno > 0)
				used_through(c, list_nth(r->joinaliasvars, v->varattno - 1));
		}
		return false;
	}
	if (IsA(n, Query))
	{
		bool		done;

		c->level++;
		done = query_tree_walker((Query *) n, used_walker, c, 0);
		c->level--;
		return done;
	}
	return expression_tree_walker(n, used_walker, c);
}

typedef struct VarRefs
{
	Index		rti;
	bool		table,			/* a Var of the table */
				other,			/* a Var of another entry of the query */
				outer;			/* a Var of a query around */
} VarRefs;

static bool
refs_walker(Node *n, VarRefs *r)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varlevelsup > 0)
			r->outer = true;
		else if (v->varno == r->rti)
			r->table = true;
		else
			r->other = true;
		return false;
	}
	return expression_tree_walker(n, refs_walker, r);
}

static VarRefs
refs(Node *n, Index rti)
{
	VarRefs		r = {rti};

	refs_walker(n, &r);
	return r;
}

/* the items other than RTI that the expression reads (Vars of this query level) */
static bool
partners_walker(Node *n, Bitmapset **partners)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
	{
		if (((Var *) n)->varlevelsup == 0)
			*partners = bms_add_member(*partners, ((Var *) n)->varno);
		return false;
	}
	return expression_tree_walker(n, partners_walker, partners);
}

/* the conjunct of the ON as the sub-select reads it: the Vars of the table are its own, those of the others the query around it's */
static Node *
child_vars(Node *n, Index *rti)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0)
	{
		Var		   *v = copyObject((Var *) n);

		if (v->varno == *rti)
			v->varno = v->varnosyn = 1;
		else
			v->varlevelsup = 1;
		return (Node *) v;
	}
	return expression_tree_mutator(n, child_vars, rti);
}

/* the Vars of the table in the query around it, by the column of the sub-select's output (a column nothing was asked of is NULL: only an entry of a join lists it) */
static Node *
output_vars(Node *n, ProbeJoin *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == c->level && ((Var *) n)->varno == c->rti)
	{
		Var		   *v = (Var *) n;

		if (v->varattno > c->top || c->pos[v->varattno] == 0)
			return (Node *) makeNullConst(v->vartype, v->vartypmod, v->varcollid);
		v = copyObject(v);
		v->varattno = c->pos[v->varattno];
		return (Node *) v;
	}
	if (IsA(n, Query))
	{
		Query	   *sub;

		c->level++;
		sub = query_tree_mutator((Query *) n, output_vars, c, 0);
		c->level--;
		return (Node *) sub;
	}
	return expression_tree_mutator(n, output_vars, c);
}

/* `column = expr` of the table over the other side: a btree starts with the column */
static bool
join_tie(RangeTblEntry *rte, Index rti, Node *a)
{
	OpExpr	   *op = (OpExpr *) a;

	if (!IsA(a, OpExpr) || list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType(linitial(op->args))))
		return false;
	for (int i = 0; i < 2; i++)
	{
		Node	   *col = list_nth(op->args, i);
		VarRefs		o = refs(list_nth(op->args, 1 - i), rti);

		while (IsA(col, RelabelType))
			col = (Node *) ((RelabelType *) col)->arg;
		if (IsA(col, Var) && ((Var *) col)->varlevelsup == 0 && ((Var *) col)->varno == rti && ((Var *) col)->varattno > 0 && !o.table && !o.outer && o.other &&
			btree_on(rte->relid, ((Var *) col)->varattno))
			return true;
	}
	return false;
}

/* does the query read a table that has a column index (a scan), in its own tables, its sub-selects and the CTEs it names or reads? The query is the first of flat_levels (as at the call
 * from flatten_query), and the levels follow a sub-select or a CTE body into it */
static bool
reads_scan_table(Query *q)
{
	foreach_node(RangeTblEntry, r, q->rtable)
		if ((r->rtekind == RTE_RELATION && has_column_source(r->relid)) || ((r->rtekind == RTE_SUBQUERY || r->rtekind == RTE_CTE) && item_reads_scan(r)))
			return true;
	foreach_node(CommonTableExpr, cte, q->cteList)
	{
		bool		found;

		flat_levels = lcons(cte->ctequery, flat_levels);
		found = reads_scan_table(castNode(Query, cte->ctequery));
		flat_levels = list_delete_first(flat_levels);
		if (found)
			return true;
	}
	return false;
}

/* does a sub-select or a CTE reference of the first level of flat_levels read a table with a column index? A CTE that cannot be found is taken as one that does */
static bool
item_reads_scan(RangeTblEntry *rte)
{
	List	   *saved = flat_levels;
	bool		found;

	if (rte->rtekind == RTE_SUBQUERY)
	{
		flat_levels = lcons(rte->subquery, flat_levels);
		found = reads_scan_table(rte->subquery);
		flat_levels = list_delete_first(flat_levels);
		return found;
	}
	if (!rte->self_reference && rte->ctelevelsup < list_length(saved))
		foreach_node(CommonTableExpr, cte, ((Query *) list_nth(saved, rte->ctelevelsup))->cteList)
			if (strcmp(cte->ctename, rte->ctename) == 0 && !cte->cterecursive)
			{
				flat_levels = lcons(cte->ctequery, list_copy_tail(saved, rte->ctelevelsup));
				found = reads_scan_table(castNode(Query, cte->ctequery));
				flat_levels = saved;
				return found;
			}
	return true;
}

/* a query level that reads no table with a column index and no column of the query around it: it names keys, no scan reads it, and PostgreSQL plans its joins and sub-selects as written
 * (a probe made there leaves a table with no join condition of its own once the level is flattened into the statement that reads it) */
static bool
keys_level(Query *q)
{
	return !reads_scan_table(q) && !contain_vars_of_level((Node *) q, 1);
}

/* a CTE, a sub-select or a VALUES list that reads no table with a column index: the keys a statement names, which no scan reads */
static bool
keys_item(RangeTblEntry *rte)
{
	return rte->rtekind == RTE_VALUES || ((rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE) && !item_reads_scan(rte));
}

/* a plain table with many rows that no row-level policy restricts */
static bool
join_table(Query *q, Node *n)
{
	RangeTblEntry *rte;

	if (!IsA(n, RangeTblRef))
		return false;
	rte = rt_fetch(((RangeTblRef *) n)->rtindex, q->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample != NULL || rte->securityQuals != NIL)
		return false;
	return sift_agg_table_rows(rte->relid) >= PROBE_MIN_ROWS && !has_column_source(rte->relid);
}

/* what is left of the ON, or of the WHERE of a FROM list, after the sub-select took its conjuncts */
static Node *
join_on(ProbeJoin *c)
{
	if (c->rest == NIL)
		return c->join != NULL ? (Node *) makeBoolConst(true, false) : NULL;
	return (Node *) make_ands_explicit(c->rest);
}

static Node **
join_slot(ProbeJoin *c)
{
	return c->join != NULL ? &c->join->quals : &c->from->quals;
}

/*
 * Whether the join is taken: one conjunct of its ON (or of the WHERE, of an item of the FROM list) ties a column a btree starts with to the other side by an equality; nothing
 * in the ON is a sub-select, a volatile function or an aggregate; and the query reads no system column of the table. What the sub-select takes: every conjunct that reads the
 * table, and, of a left join, every one (a conjunct that stays would not make the NULLs of a row without a match).
 */
static bool
join_split(Query *q, ProbeJoin *c, JoinType type)
{
	RangeTblEntry *rte = rt_fetch(c->rti, q->rtable);
	Node	  **slot = join_slot(c);
	Node	   *on = *slot;
	bool		tie = false;
	Bitmapset  *partners = NULL;

	c->child = c->rest = NIL;
	c->used = NULL;
	c->bad = false;
	c->level = 0;
	c->q = q;
	foreach_ptr(Node, a, make_ands_implicit((Expr *) on))
	{
		VarRefs		r = refs(a, c->rti);

		if (r.outer || checkExprHasSubLink(a) || contain_volatile_functions(a) || contain_agg_clause(a))
			return false;
		if (r.table && join_tie(rte, c->rti, a))
		{
			tie = true;
			partners_walker(a, &partners);
			partners = bms_del_member(partners, c->rti);
		}
		if (r.table || type == JOIN_LEFT)
			c->child = lappend(c->child, a);
		else
			c->rest = lappend(c->rest, a);
	}
	if (!tie)
		return false;
	/*
	 * A table that an inner join ties only to a CTE (the keys a statement names) is probed by those keys by the planner's own nested loop, and
	 * as a lateral child of the CTE it leaves the CTE with no join condition of its own once the statement is flattened into the query that
	 * reads the table's columns (the scan finds no lookup to key it by): the keys' CTE is the driver, the table the one joined by them.
	 */
	if (type == JOIN_INNER && !bms_is_empty(partners))
	{
		bool		driven = true;

		for (int m = -1; (m = bms_next_member(partners, m)) >= 0;)
			driven &= keys_item(rt_fetch(m, q->rtable));
		if (driven)
			return false;
	}
	foreach_node(RangeTblEntry, r, q->rtable)
	{
		/* the items of a FROM list are in an order that a lateral reference to the table needs: the table goes last */
		if (r->lateral && c->join == NULL)
			return false;
	}
	*slot = join_on(c);
	query_tree_walker(q, used_walker, c, QTW_IGNORE_RANGE_TABLE);
	/* the expressions of the GROUP BY are those of the entry for the grouping, which the Vars of the query read; the entries that read the table as a lateral reference */
	foreach_node(RangeTblEntry, r, q->rtable)
	{
		switch (r->rtekind)
		{
#if PG_VERSION_NUM >= 180000
			case RTE_GROUP:
				used_walker((Node *) r->groupexprs, c);
				break;
#endif
			case RTE_SUBQUERY:
				if (r->lateral)
					used_walker((Node *) r->subquery, c);
				break;
			case RTE_FUNCTION:
				used_walker((Node *) r->functions, c);
				break;
			case RTE_VALUES:
				used_walker((Node *) r->values_lists, c);
				break;
			default:
				break;
		}
	}
	*slot = on;
	return !c->bad;
}

static bool
join_find(Query *q, Node *n, ProbeJoin *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;

		foreach_ptr(Node, item, f->fromlist)
			if (join_find(q, item, c))
				return true;
		if (list_length(f->fromlist) > 1)
			foreach_ptr(Node, item, f->fromlist)
				if (join_table(q, item))
				{
					c->rti = ((RangeTblRef *) item)->rtindex;
					c->join = NULL;
					c->from = f;
					if (join_split(q, c, JOIN_INNER))
						return true;
				}
	}
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		if (join_find(q, j->larg, c) || join_find(q, j->rarg, c))
			return true;
		if ((j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT) && join_table(q, j->rarg))
		{
			c->rti = ((RangeTblRef *) j->rarg)->rtindex;
			c->join = j;
			return join_split(q, c, j->jointype);
		}
	}
	return false;
}

static void
join_probe(Query *q, ProbeJoin *c)
{
	RangeTblEntry *rte = rt_fetch(c->rti, q->rtable);
	Query	   *sub = makeNode(Query);
	RangeTblRef *ref = makeNode(RangeTblRef);
	RangeTblEntry *tab = copyObject(rte);
	List	   *tl = NIL,
			   *names = NIL,
			   *qual = NIL;
	int			n = 0,
				att = -1;

	*join_slot(c) = join_on(c);
	if (c->join == NULL)
	{
		/* the table goes last in the FROM list: it reads the items before it */
		foreach_ptr(Node, item, c->from->fromlist)
			if (IsA(item, RangeTblRef) && ((RangeTblRef *) item)->rtindex == c->rti)
			{
				c->from->fromlist = lappend(list_delete_ptr(c->from->fromlist, item), item);
				break;
			}
	}
	c->top = Max(bms_prev_member(c->used, -1), 0);
	c->pos = palloc0((c->top + 1) * sizeof(int));
	while ((att = bms_next_member(c->used, att)) >= 0)
	{
		Oid			type,
					coll;
		int32		mod;
		char	   *name = att == 0 ? rte->eref->aliasname : get_attname(rte->relid, att, false);

		if (att == 0)
		{
			type = get_rel_type_id(rte->relid);
			mod = -1;
			coll = InvalidOid;
		}
		else
			get_atttypetypmodcoll(rte->relid, att, &type, &mod, &coll);
		tl = lappend(tl, makeTargetEntry((Expr *) makeVar(1, att, type, mod, coll, 0), ++n, name, false));
		names = lappend(names, makeString(name));
		c->pos[att] = n;
	}
	if (tl == NIL)
	{
		tl = list_make1(makeTargetEntry((Expr *) makeBoolConst(true, false), 1, "m", false));
		names = list_make1(makeString("m"));
	}
	foreach_ptr(Node, a, c->child)
		qual = lappend(qual, child_vars(a, &c->rti));
	tab->perminfoindex = 1;
	ref->rtindex = 1;
	sub->commandType = CMD_SELECT;
	sub->querySource = QSRC_ORIGINAL;
	sub->canSetTag = true;
	sub->stmt_len = PROBE_MARK;
	sub->rtable = list_make1(tab);
	sub->rteperminfos = list_make1(copyObject(getRTEPermissionInfo(q->rteperminfos, rte)));
	sub->jointree = makeFromExpr(list_make1(ref), (Node *) make_ands_explicit(qual));
	sub->targetList = tl;
	sub->limitOffset = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL);
	c->level = 0;
	q->targetList = (List *) output_vars((Node *) q->targetList, c);
	q->jointree = (FromExpr *) output_vars((Node *) q->jointree, c);
	q->havingQual = output_vars(q->havingQual, c);
	q->limitCount = output_vars(q->limitCount, c);
	q->limitOffset = output_vars(q->limitOffset, c);
	foreach_node(RangeTblEntry, r, q->rtable)
	{
		switch (r->rtekind)
		{
#if PG_VERSION_NUM >= 180000
			case RTE_GROUP:
				r->groupexprs = (List *) output_vars((Node *) r->groupexprs, c);
				break;
#endif
			case RTE_SUBQUERY:
				if (r->lateral)
					r->subquery = (Query *) output_vars((Node *) r->subquery, c);
				break;
			case RTE_FUNCTION:
				r->functions = (List *) output_vars((Node *) r->functions, c);
				break;
			case RTE_VALUES:
				r->values_lists = (List *) output_vars((Node *) r->values_lists, c);
				break;
			case RTE_JOIN:
				/* the entry of a join lists the table's columns: those the query does not read are NULL, as nothing reads them */
				r->joinaliasvars = (List *) output_vars((Node *) r->joinaliasvars, c);
				break;
			default:
				break;
		}
	}
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = sub;
	rte->lateral = true;
	rte->relid = InvalidOid;
	rte->relkind = 0;
	rte->rellockmode = 0;
	rte->inh = false;
	rte->perminfoindex = 0;
	rte->eref = makeAlias(SIFT_PROBE_ALIAS, names);
}

bool
sift_agg_probe_joins(Query *q, bool apply)
{
	bool		changed = false;
	ProbeJoin	c = {0};

	if (!sift_agg_probe_rewrite || q->commandType != CMD_SELECT || q->jointree == NULL || q->setOperations != NULL || q->rowMarks != NIL || keys_level(q))
		return false;
	while (join_find(q, (Node *) q->jointree, &c))
	{
		if (!apply)
			return true;
		join_probe(q, &c);
		changed = true;
	}
	if (changed)
	{
		sift_agg_by_cost = true;
		sift_agg_probe_applied = true;
	}
	return changed;
}
