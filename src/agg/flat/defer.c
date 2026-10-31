/*
 * agg/flat/defer.c - the one-row lookups of a page run for the rows of the page
 * `FROM events e JOIN .. LEFT JOIN LATERAL (SELECT .. FROM dim_a WHERE dim_a.id = e.dim_a_id) a ON true ORDER BY e.d DESC LIMIT 3` looks dim_a up for every row
 * that matches, and sorts the joined rows: the lookups of the rows the sort drops are the work of the query. A left join to something that has at most one row
 * for a row of its left side (a sub-select of aggregates without GROUP BY, with LIMIT 1, or of a table by a unique key; a table joined by its unique key) neither
 * adds nor drops rows, so it can wait for the LIMIT: the joins below it are a sub-select with the ORDER BY and the LIMIT, and the left joins read its columns
 * and sort its few rows again. PostgreSQL does not move a join above a LIMIT.
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

#define MAX_KEY_COLUMNS 8

/*
 * The columns of table rti that the conditions equate with an expression that does not read the table: `rti.col = expr` in an AND list, of the
 * default equality of the column's type (what a unique index on the column holds once).
 */
static int
equated_columns(Node *quals, Index rti, int *atts)
{
	List	   *conds;
	int			n = 0;

	if (quals == NULL)
		return 0;
	conds = IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR ? ((BoolExpr *) quals)->args : list_make1(quals);
	foreach_ptr(Node, c, conds)
	{
		OpExpr	   *op;
		Node	   *sides[2];

		if (!IsA(c, OpExpr) || list_length(((OpExpr *) c)->args) != 2)
			continue;
		op = (OpExpr *) c;
		sides[0] = linitial(op->args);
		sides[1] = lsecond(op->args);
		for (int i = 0; i < 2; i++)
		{
			Node	   *col = sides[i],
					   *other = sides[1 - i];
			Var		   *v;

			while (IsA(col, RelabelType))
				col = (Node *) ((RelabelType *) col)->arg;
			while (IsA(other, RelabelType))
				other = (Node *) ((RelabelType *) other)->arg;
			if (!IsA(col, Var))
				continue;
			v = (Var *) col;
			if (v->varno != rti || v->varlevelsup != 0 || v->varattno <= 0 || exprType(other) != v->vartype ||
				lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr != op->opno ||
				bms_is_member(rti, pull_varnos(NULL, other)) || contain_volatile_functions(other))
				continue;
			if (n < MAX_KEY_COLUMNS)
				atts[n++] = v->varattno;
			break;
		}
	}
	return n;
}

static bool
plain_table(RangeTblEntry *rte)
{
	return rte->rtekind == RTE_RELATION && (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_PARTITIONED_TABLE) && rte->tablesample == NULL;
}

/* at most one row for any values of the columns it reads from the query around it, and no effect but its rows */
static bool
at_most_one_row(Query *sq)
{
	FromExpr   *f = sq->jointree;
	RangeTblEntry *rte;
	int			atts[MAX_KEY_COLUMNS],
				n;

	if (sq->commandType != CMD_SELECT || sq->setOperations != NULL || sq->rowMarks != NIL || sq->hasModifyingCTE || sq->hasTargetSRFs || sq->hasWindowFuncs ||
		sq->cteList != NIL || sq->limitOffset != NULL || contain_volatile_functions((Node *) sq))
		return false;
	if (sq->hasAggs && sq->groupClause == NIL && sq->groupingSets == NIL)
		return true;
	if (one_row_query(sq))
		return true;
	if (f != NULL && f->fromlist == NIL)
		return true;
	if (f == NULL || list_length(f->fromlist) != 1 || !IsA(linitial(f->fromlist), RangeTblRef))
		return false;
	rte = rt_fetch(((RangeTblRef *) linitial(f->fromlist))->rtindex, sq->rtable);
	if (rte->rtekind == RTE_SUBQUERY)
		return at_most_one_row(rte->subquery);
	if (!plain_table(rte))
		return false;
	n = equated_columns(f->quals, ((RangeTblRef *) linitial(f->fromlist))->rtindex, atts);
	return n > 0 && sift_agg_unique_key(rte->relid, n, 0, atts, NULL);
}

/* a left join that keeps the rows of its left side exactly once, whatever it matches */
static bool
one_row_join(Query *q, JoinExpr *j)
{
	Index		rti;
	RangeTblEntry *rte;
	int			atts[MAX_KEY_COLUMNS],
				n;

	if (j->jointype != JOIN_LEFT || !IsA(j->rarg, RangeTblRef) || contain_volatile_functions(j->quals))
		return false;
	rti = ((RangeTblRef *) j->rarg)->rtindex;
	rte = rt_fetch(rti, q->rtable);
	if (rte->rtekind == RTE_SUBQUERY)
		return at_most_one_row(rte->subquery);
	if (!plain_table(rte))
		return false;
	n = equated_columns(j->quals, rti, atts);
	return n > 0 && sift_agg_unique_key(rte->relid, n, 0, atts, NULL);
}

typedef struct Reads
{
	Bitmapset  *leaves;			/* the tables of the part that stays under the LIMIT */
	Bitmapset  *joins;			/* the joins of that part, whose aliases hold their columns */
	int			level;
	bool		unsupported;
} Reads;

/* what the lookups read of the part below: a system column or a join alias cannot be read from its sub-select's output */
static bool
reads_walker(Node *n, Reads *c)
{
	if (n == NULL || c->unsupported)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varlevelsup == c->level && ((bms_is_member(v->varno, c->leaves) && v->varattno < 0) || bms_is_member(v->varno, c->joins)))
			c->unsupported = true;
		return false;
	}
	if (IsA(n, Query))
	{
		bool		r;

		c->level++;
		r = query_tree_walker((Query *) n, reads_walker, c, 0);
		c->level--;
		return r;
	}
	return expression_tree_walker(n, reads_walker, c);
}

static void
tree_members(Node *jt, Bitmapset **leaves, Bitmapset **joins)
{
	if (IsA(jt, RangeTblRef))
		*leaves = bms_add_member(*leaves, ((RangeTblRef *) jt)->rtindex);
	else if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		if (j->rtindex != 0)
			*joins = bms_add_member(*joins, j->rtindex);
		tree_members(j->larg, leaves, joins);
		tree_members(j->rarg, leaves, joins);
	}
	else if (IsA(jt, FromExpr))
		foreach_ptr(Node, x, ((FromExpr *) jt)->fromlist)
			tree_members(x, leaves, joins);
}

/* for each table of the part below, the outer joins in it that null its columns (a column read above them carries them in varnullingrels) */
static void
nulled_by(Node *jt, Bitmapset *acc, Bitmapset **by_leaf)
{
	if (IsA(jt, RangeTblRef))
		by_leaf[((RangeTblRef *) jt)->rtindex] = acc;
	else if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;
		Bitmapset  *with = j->rtindex != 0 ? bms_add_member(bms_copy(acc), j->rtindex) : acc;
		bool		left_nulled = j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL,
					right_nulled = j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL;

		nulled_by(j->larg, left_nulled ? with : acc, by_leaf);
		nulled_by(j->rarg, right_nulled ? with : acc, by_leaf);
	}
	else if (IsA(jt, FromExpr))
		foreach_ptr(Node, x, ((FromExpr *) jt)->fromlist)
			nulled_by(x, acc, by_leaf);
}

/* the sub-select moves one query level down: the common table expressions it reads are one level further up */
static bool
cte_level_walker(Node *n, int *depth)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		Query	   *sq = (Query *) n;

		foreach_node(RangeTblEntry, rte, sq->rtable)
			if (rte->rtekind == RTE_CTE && rte->ctelevelsup >= *depth)
				rte->ctelevelsup++;
		(*depth)++;
		query_tree_walker(sq, cte_level_walker, depth, 0);
		(*depth)--;
		return false;
	}
	return expression_tree_walker(n, cte_level_walker, depth);
}

/* the outer joins that null a column of the sub-select are inside it: the columns the lookups read from it are not nulled by any join of this query */
typedef struct Nulling
{
	Index		rti;
	int			level;
} Nulling;

static bool
nulling_walker(Node *n, Nulling *c)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varno == c->rti && v->varlevelsup == c->level)
			v->varnullingrels = NULL;
		return false;
	}
	if (IsA(n, Query))
	{
		bool		r;

		c->level++;
		r = query_tree_walker((Query *) n, nulling_walker, c, 0);
		c->level--;
		return r;
	}
	return expression_tree_walker(n, nulling_walker, c);
}

static RangeTblEntry *
dead_entry(void)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);

	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);
	return rte;
}

/* the query as a sub-select of the joins that wait: the rows under the LIMIT, with every column the lookups and the select list read */
static bool
defer_level(Query *q)
{
	Node	   *top;
	JoinExpr   *last = NULL;
	Bitmapset  *waiting = NULL,
			   *leaves = NULL,
			   *joins = NULL,
			   *used;
	List	   *lookups = NIL;
	Reads		reads;
	Query	   *in;
	RangeTblEntry *rte_in;
	RangeTblRef *ref;
	List	   *per_leaf = NIL,
			   *colnames = NIL,
			   *leaf_list = NIL;
	int			x;
	Bitmapset **nulling;
	Index		kidx = list_length(q->rtable) + 1;
	ListCell   *lc;
	int			depth = 0;

	if (q->commandType != CMD_SELECT || q->limitCount == NULL || q->limitOption != LIMIT_OPTION_COUNT || q->sortClause == NIL || q->setOperations != NULL ||
		q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasRecursive || q->hasModifyingCTE || q->groupClause != NIL || q->groupingSets != NIL ||
		q->distinctClause != NIL || q->havingQual != NULL || q->rowMarks != NIL || q->jointree == NULL || list_length(q->jointree->fromlist) != 1)
		return false;
	top = linitial(q->jointree->fromlist);
	while (IsA(top, JoinExpr) && one_row_join(q, (JoinExpr *) top))
	{
		JoinExpr   *j = (JoinExpr *) top;

		last = j;
		lookups = lappend(lookups, j);
		waiting = bms_add_member(waiting, ((RangeTblRef *) j->rarg)->rtindex);
		if (j->rtindex != 0)
			waiting = bms_add_member(waiting, j->rtindex);
		top = j->larg;
	}
	if (last == NULL || contain_volatile_functions((Node *) q))
		return false;

	/* the conditions and the order read only what stays under the LIMIT */
	used = pull_varnos(NULL, q->jointree->quals);
	foreach_node(TargetEntry, te, q->targetList)
		if (te->ressortgroupref != 0)
			used = bms_add_members(used, pull_varnos(NULL, (Node *) te->expr));
	if (bms_overlap(used, waiting))
		return false;
	tree_members(top, &leaves, &joins);
	for (x = -1; (x = bms_next_member(leaves, x)) >= 0;)
		leaf_list = lappend_int(leaf_list, x);
	nulling = palloc0((list_length(q->rtable) + 1) * sizeof(Bitmapset *));
	nulled_by(top, NULL, nulling);
	reads = (Reads) {leaves, joins, 0, false};
	reads_walker((Node *) q->targetList, &reads);
	foreach_ptr(JoinExpr, j, lookups)
	{
		reads_walker(j->quals, &reads);
		reads_walker((Node *) rt_fetch(((RangeTblRef *) j->rarg)->rtindex, q->rtable)->subquery, &reads);
	}
	if (reads.unsupported)
		return false;

	/* the sub-select: the columns of every table below, then the sort expressions (the ORDER BY of the sub-select reads them) */
	in = makeNode(Query);
	in->commandType = CMD_SELECT;
	in->querySource = QSRC_ORIGINAL;
	in->canSetTag = true;
	in->rtable = copyObject(q->rtable);
	in->rteperminfos = copyObject(q->rteperminfos);
	in->jointree = makeFromExpr(list_make1(copyObject(top)), copyObject(q->jointree->quals));
	in->sortClause = copyObject(q->sortClause);
	in->limitCount = copyObject(q->limitCount);
	in->limitOffset = copyObject(q->limitOffset);
	in->limitOption = q->limitOption;
	in->hasSubLinks = q->hasSubLinks;
	foreach(lc, in->rtable)
		if (bms_is_member(foreach_current_index(lc) + 1, waiting))
			lfirst(lc) = dead_entry();
	foreach_int(rti, leaf_list)
	{
		RangeTblEntry *rte = rt_fetch(rti, q->rtable);
		List	   *names = NIL,
				   *vars = NIL,
				   *tl = NIL;
		ListCell   *lv,
				   *ln;

		sift_expand_rte_dropped(rte, rti, &names, &vars);
		forboth(lv, vars, ln, names)
		{
			Node	   *x = lfirst(lv);

			if (IsA(x, Var))
			{
				Var		   *v = copyObject((Var *) x);
				int			pos = list_length(in->targetList) + 1;

				v->varnullingrels = bms_copy(nulling[rti]);
				in->targetList = lappend(in->targetList, makeTargetEntry((Expr *) v, pos, psprintf("c%d", pos), false));
				colnames = lappend(colnames, makeString(psprintf("c%d", pos)));
				tl = lappend(tl, makeTargetEntry((Expr *) makeVar(kidx, pos, v->vartype, v->vartypmod, v->varcollid, 0), list_length(tl) + 1, NULL, false));
			}
			else
				tl = lappend(tl, makeTargetEntry((Expr *) copyObject(x), list_length(tl) + 1, NULL, false));
		}
		per_leaf = lappend(per_leaf, tl);
	}
	foreach_node(TargetEntry, te, q->targetList)
		if (te->ressortgroupref != 0)
		{
			TargetEntry *c = copyObject(te);

			c->resno = list_length(in->targetList) + 1;
			c->resjunk = true;
			in->targetList = lappend(in->targetList, c);
		}
	IncrementVarSublevelsUp((Node *) in, 1, 1);
	cte_level_walker((Node *) in, &depth);

	/* the joins that waited read the sub-select; its rows come ordered, and are sorted again for the joins' output */
	ref = makeNode(RangeTblRef);
	ref->rtindex = kidx;
	last->larg = (Node *) ref;
	q->jointree = makeFromExpr(list_make1(linitial(q->jointree->fromlist)), NULL);
	q->limitCount = NULL;
	q->limitOffset = NULL;
	lc = list_head(per_leaf);
	foreach_int(rti, leaf_list)
	{
		*q = *(Query *) sift_replace_vars_from_targetlist((Node *) q, rti, rt_fetch(rti, q->rtable), lfirst(lc));
		lc = lnext(per_leaf, lc);
	}
	{
		Nulling		nc = {kidx, 0};

		query_tree_walker(q, nulling_walker, &nc, 0);
	}
	rte_in = makeNode(RangeTblEntry);
	rte_in->rtekind = RTE_SUBQUERY;
	rte_in->subquery = in;
	rte_in->eref = makeAlias("deferred", colnames);
	rte_in->inFromCl = true;
	q->rtable = lappend(q->rtable, rte_in);
	return true;
}

static bool
defer_sublinks(Node *n, void *context)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		sift_agg_defer_lookups((Query *) n);
		return false;
	}
	return expression_tree_walker(n, defer_sublinks, context);
}

void
sift_agg_defer_lookups(Query *q)
{
	if (q->commandType != CMD_SELECT || q->rowMarks != NIL)
		return;
	foreach_node(CommonTableExpr, cte, q->cteList)
		sift_agg_defer_lookups(castNode(Query, cte->ctequery));
	if (q->setOperations == NULL)
		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_SUBQUERY && !rte->security_barrier)
				sift_agg_defer_lookups(rte->subquery);
	if (q->hasSubLinks)
		query_tree_walker(q, defer_sublinks, NULL, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
	defer_level(q);
}
