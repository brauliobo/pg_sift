/*
 * agg/from/scan.c - the FROM of the query
 * Which relations the scan reads, which an outer join preserves, the trees of a semi-join and
 * the lateral heads.
 */
#include "postgres.h"

#include "optimizer/optimizer.h"
#include "rewrite/rewriteManip.h"

#include "agg/part.h"
#include "agg/from/int.h"

/* the table of a join's right side: a table, or a pulled up sub-select over one (a FromExpr of one table with a WHERE) */
static RangeTblRef *
right_table(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? (RangeTblRef *) n : NULL;
}

/* the first item of a join's right side that is itself a LEFT JOIN of items (a list a row unnests, with the lookups of its elements): the one the outer join is of */
static RangeTblRef *
lateral_head(Node *n)
{
	JoinExpr   *j = IsA(n, JoinExpr) ? (JoinExpr *) n : NULL;

	return j != NULL && j->jointype == JOIN_LEFT && IsA(j->larg, RangeTblRef) ? (RangeTblRef *) j->larg : NULL;
}

/* a condition that reads no column of the preserved table rti, and no sub-select or volatile function */
static bool
right_only(Node *q, Index rti)
{
	if (checkExprHasSubLink(q) || contain_volatile_functions(q))
		return false;
	foreach_ptr(Var, v, pull_var_clause(q, PVC_RECURSE_PLACEHOLDERS))
		if (v->varno == rti)
			return false;
	return true;
}

/*
 * A LEFT JOIN with the scanned table on its right (the left side is preserved: flip), or a FULL JOIN of two tables: the table that is
 * not scanned is the join, with all of its rows kept. The ON clause is the join's; whatever the WHERE says of the scanned table is not.
 */
static bool
scan_preserved(JoinExpr *j, FromScan *f)
{
	bool		full = j->jointype == JOIN_FULL;
	RangeTblRef *l = IsA(j->larg, RangeTblRef) ? (RangeTblRef *) j->larg : NULL,
			   *r = right_table(j->rarg);
	FromScan	right = {.ok = true},
				left = {.ok = true};
	LeftJoin   *lj;
	Index		scanned,
				dim;

	/* a FULL JOIN of a join of tables to a table: the join is the scanned side (its tables are joined to the first) */
	if ((!full && j->jointype != JOIN_LEFT) || (full && r == NULL) || (l == NULL && (!full || f->flip)))
		return false;
	f->flippable = true;
	if (!full && !f->flip)
		return false;
	if (l == NULL)
	{
		scan_from(j->larg, &left);
		if (!left.ok || left.ojrels != NULL || left.rtis == NIL)
			return false;
	}
	if (f->flip)
	{
		scan_from(j->rarg, &right);
		if (!right.ok || right.ojrels != NULL || right.rtis == NIL)
			return false;
	}
	scanned = f->flip ? linitial_int(right.rtis) : l ? l->rtindex : linitial_int(left.rtis);
	dim = f->flip ? l->rtindex : r->rtindex;
	lj = palloc(sizeof(LeftJoin));
	lj->kind = full ? SJ_FULL : SJ_RIGHT;
	lj->tree = NULL;
	lj->rti = dim;
	lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
	/*
	 * PostgreSQL moves the WHERE of a sub-select on the nullable side into the ON clause: what does not read the preserved table filters the
	 * nullable input before the match, like the conditions inside it (a lookup whose filter matches nothing then ends the scan).
	 */
	if (!full)
	{
		List	   *on = NIL;

		foreach_ptr(Node, q, lj->quals)
			if (right_only(q, dim))
				f->scan_quals = lappend(f->scan_quals, q);
			else
				on = lappend(on, q);
		lj->quals = on;
	}
	/* Filter the nullable input before matching; these conditions never test its preserved NULL rows. */
	f->scan_quals = list_concat(list_concat(f->scan_quals, right.quals), left.quals);
	f->lefts = list_concat(list_concat(f->lefts, right.lefts), left.lefts);
	f->rtis = f->flip ? list_concat(f->rtis, right.rtis) : l ? lappend_int(f->rtis, scanned) : list_concat(f->rtis, left.rtis);
	f->rtis = lappend_int(f->rtis, dim);
	f->lefts = lappend(f->lefts, lj);
	f->ojrels = bms_add_member(f->ojrels, j->rtindex);
	return true;
}

/* the tables and the outer joins of a tree of joins */
static void
tree_relids(Node *n, Relids *tables, Relids *ojs)
{
	if (n == NULL)
		return;
	if (IsA(n, RangeTblRef))
		*tables = bms_add_member(*tables, ((RangeTblRef *) n)->rtindex);
	else if (IsA(n, FromExpr))
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
			tree_relids(x, tables, ojs);
	else if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;

		tree_relids(j->larg, tables, ojs);
		tree_relids(j->rarg, tables, ojs);
		if (j->jointype == JOIN_LEFT || j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL)
			*ojs = bms_add_member(*ojs, j->rtindex);
	}
}

/*
 * A semi or anti join (IN, EXISTS, NOT EXISTS) whose right side is a tree of joined tables: PostgreSQL planned the tree as a join of its own, and
 * its rows are the keys the join looks up. The ON clause reads the columns of one table of the tree, which stands for it; the other tables of the
 * tree are only in its plan (hidden).
 */
static bool
scan_semi_tree(JoinExpr *j, FromScan *f)
{
	Relids		tables = NULL,
				ojs = NULL;
	Relids		read = NULL;
	List	   *quals = NIL;
	LeftJoin   *lj;

	tree_relids(j->rarg, &tables, &ojs);
	if (bms_membership(tables) != BMS_MULTIPLE)
		return false;
	/* a condition of the ON clause that reads only the tree's tables (an anti join keeps it there) is already in the plan PostgreSQL made of the tree */
	foreach_ptr(Node, q, j->quals == NULL ? NIL : IsA(j->quals, List) ? (List *) j->quals : list_make1(j->quals))
	{
		List	   *vars = pull_var_clause(q, PVC_RECURSE_PLACEHOLDERS);
		bool		outside = vars == NIL;

		if (checkExprHasSubLink(q))
			return false;
		foreach_ptr(Var, v, vars)
			outside |= !bms_is_member(v->varno, tables) || v->varlevelsup != 0;
		if (!outside)
			continue;
		quals = lappend(quals, q);
		foreach_ptr(Var, v, vars)
			if (bms_is_member(v->varno, tables))
				read = bms_add_member(read, v->varno);
	}
	if (bms_membership(read) != BMS_SINGLETON)
		return false;
	scan_from(j->larg, f);
	lj = palloc(sizeof(LeftJoin));
	lj->kind = j->jointype == JOIN_SEMI ? SJ_SEMI : SJ_ANTI;
	lj->rti = bms_next_member(read, -1);
	lj->quals = quals;
	lj->tree = bms_union(tables, ojs);
	f->rtis = lappend_int(f->rtis, lj->rti);
	f->hidden = bms_join(f->hidden, bms_del_member(bms_copy(tables), lj->rti));
	f->lefts = lappend(f->lefts, lj);
	return true;
}

void
scan_from(Node *n, FromScan *f)
{
	if (n == NULL)
		return;
	if (IsA(n, RangeTblRef))
		f->rtis = lappend_int(f->rtis, ((RangeTblRef *) n)->rtindex);
	else if (IsA(n, FromExpr))
	{
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
			scan_from(x, f);
		if (((FromExpr *) n)->quals)
			f->quals = list_concat(f->quals, list_copy((List *) ((FromExpr *) n)->quals));
	}
	else if (IsA(n, JoinExpr) && scan_preserved((JoinExpr *) n, f))
		return;
	else if (IsA(n, JoinExpr) && right_table(((JoinExpr *) n)->rarg) != NULL &&
			 (((JoinExpr *) n)->jointype == JOIN_LEFT || ((JoinExpr *) n)->jointype == JOIN_SEMI || ((JoinExpr *) n)->jointype == JOIN_ANTI ||
			  (((JoinExpr *) n)->jointype == JOIN_INNER && bms_is_member(right_table(((JoinExpr *) n)->rarg)->rtindex, f->outer))))
	{
		JoinExpr   *j = (JoinExpr *) n;
		LeftJoin   *lj = palloc(sizeof(LeftJoin));
		RangeTblRef *r = right_table(j->rarg);

		scan_from(j->larg, f);
		lj->kind = j->jointype == JOIN_SEMI ? SJ_SEMI : j->jointype == JOIN_ANTI ? SJ_ANTI : SJ_LEFT;
		lj->rti = r->rtindex;
		lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
		/* an inner join the scan joins outer: what it says of the other tables only is the WHERE of the scan, not the ON of the outer join */
		if (j->jointype == JOIN_INNER)
			foreach_ptr(Node, q, list_copy(lj->quals))
				if (!bms_is_member(r->rtindex, pull_varnos(NULL, q)))
				{
					lj->quals = list_delete_ptr(lj->quals, q);
					f->quals = lappend(f->quals, q);
				}
		/* the WHERE of a pulled up sub-select that is the right side filters the table's rows before the join: more of the ON clause */
		if (IsA(j->rarg, FromExpr) && ((FromExpr *) j->rarg)->quals)
			lj->quals = list_concat(lj->quals, list_copy((List *) ((FromExpr *) j->rarg)->quals));
		lj->tree = NULL;
		f->rtis = lappend_int(f->rtis, lj->rti);
		f->lefts = lappend(f->lefts, lj);
	}
	else if (IsA(n, JoinExpr) && (((JoinExpr *) n)->jointype == JOIN_SEMI || ((JoinExpr *) n)->jointype == JOIN_ANTI) && scan_semi_tree((JoinExpr *) n, f))
		return;
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_LEFT && lateral_head(((JoinExpr *) n)->rarg) != NULL)
	{
		JoinExpr   *j = (JoinExpr *) n;
		RangeTblRef *head = lateral_head(j->rarg);
		LeftJoin   *lj = palloc(sizeof(LeftJoin));

		scan_from(j->larg, f);
		scan_from(j->rarg, f);
		lj->kind = SJ_LEFT;
		lj->rti = head->rtindex;
		lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
		lj->tree = NULL;
		f->lefts = lappend(f->lefts, lj);
		f->nested = bms_add_member(f->nested, head->rtindex);
		f->ojrels = bms_add_member(f->ojrels, j->rtindex);
	}
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER)
	{
		JoinExpr   *j = (JoinExpr *) n;

		scan_from(j->larg, f);
		scan_from(j->rarg, f);
		if (j->quals)
			f->quals = list_concat(f->quals, list_copy(IsA(j->quals, List) ? (List *) j->quals : list_make1(j->quals)));
	}
	else
		f->ok = false;
}

