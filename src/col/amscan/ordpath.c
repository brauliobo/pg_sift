/*
 * col/amscan/ordpath.c - the paths of an ordered index-only scan
 * A sift_col index whose first column is an integer, a date or a timestamp can give that column in order (ordered.c) to a plan that reads it alone: a Sort above the
 * unordered scan of the index, or of a merge join's side, is what the path replaces. The path has the pathkey of the first column only (never the order of the
 * index's other columns), the direction of the scan says to the access method that it is to give the order, and the plan asks no index condition: a condition on the
 * column is the node's own qual.
 *
 * An index whose second column is ordered too and has the second order grouped by the first (order_array_group, whole chunks) gives the pair: the rows in the order of (first, second),
 * what a b-tree on the two columns gives to ORDER BY g, k and to a merge of partitions on both. The plan reads those two columns alone, and the first has no NULL (a row with no group
 * is not in the second order). The path has both pathkeys; its node is marked COL_ORDERED_PAIR_MARK.
 */
#include "postgres.h"
#include "access/sdir.h"
#include "access/genam.h"
#include "access/stratnum.h"
#include "access/htup_details.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_attribute.h"
#include "utils/syscache.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "col/am.h"
#include "col/pg.h"
#include "col/amscan/amscan_int.h"

#if PG_VERSION_NUM < 170000
#define EXPR_PATHKEY(root, expr, opno, rel) build_expression_pathkey(root, expr, NULL, opno, rel, false)
#else
#define EXPR_PATHKEY(root, expr, opno, rel) build_expression_pathkey(root, expr, opno, rel, false)
#endif

/* The order array of the index's first column is written whole (option order_layout): the scan merges it as it is, else the units give the rows and they are sorted here (ordered.c) */
bool
sift_col_ordered_whole(Oid indexoid)
{
	Relation	irel = index_open(indexoid, NoLock);
	int			leaf = sift_col_rel(irel)->first[1];
	bool		whole = leaf >= 0 && sift_col_rel(irel)->pagg.glayout[leaf] == SIFT_CGORD_OL_WHOLE;

	index_close(irel, NoLock);
	return whole;
}

/* Does the table of the index declare the column NOT NULL: the second order grouped by it holds the rows with a group only */
static bool
column_not_null(Oid relid, AttrNumber attno)
{
	HeapTuple	tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(attno));
	bool		notnull;

	if (!HeapTupleIsValid(tp))
		return false;
	notnull = ((Form_pg_attribute) GETSTRUCT(tp))->attnotnull;
	ReleaseSysCache(tp);
	return notnull;
}

/*
 * The second order of the second column of the index grouped by its first, from the statistics of the leaves: its entries are all the rows, and its chunks hold the exact keys (a
 * compact chunk holds a bucket of the key, which an index-only scan cannot give). 1: the scan reads it group after group; 0: the scan would have to sort the rows.
 */
int
sift_col_ordered_pair_mode(Oid indexoid, double tuples)
{
	Relation	irel = index_open(indexoid, NoLock);
	const int  *first = sift_col_rel(irel)->first;
	const SiftColLeafStats *ls = sift_col_leaf_stats(irel);
	int			mode = 0;

	if (ls != NULL && first[1] >= 0 && first[2] >= 0 && ls[first[2]].gord_gcol == first[1] && ls[first[2]].gord_grp_rows >= Max(tuples, 1.0) * 0.95 && ls[first[2]].gord_compact_chunks <= 0)
		mode = 1;
	index_close(irel, NoLock);
	return mode;
}

/* The clause g = value of the restrictions of the relation, the value free of the relation's columns and of volatile functions (a constant, a parameter): the pair scan reads the one group */
static RestrictInfo *
group_equality(RelOptInfo *rel, Var *gvar, Oid gfam)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		OpExpr	   *op;
		Node	   *other;

		if (!IsA(ri->clause, OpExpr) || ri->pseudoconstant || !bms_equal(ri->required_relids, rel->relids))
			continue;
		op = (OpExpr *) ri->clause;
		if (list_length(op->args) != 2 || !equal(linitial(op->args), gvar) || op->opno != get_opfamily_member(gfam, gvar->vartype, gvar->vartype, BTEqualStrategyNumber))
			continue;
		other = lsecond(op->args);
		if (exprType(other) == gvar->vartype && !contain_vars_of_level(other, 0) && !contain_volatile_functions(other))
			return ri;
	}
	return NULL;
}

static void
add_pair_paths(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, Var *gvar, Bitmapset *used, List **added)
{
	Var		   *kvar;
	Oid			gfam,
				kfam,
				gcls,
				kcls;
	IndexOptInfo *oi = NULL;
	RestrictInfo *eq;
	List	   *clauses = NIL;

	if (ii->nkeycolumns < 2 || ii->indexkeys[1] <= 0 || !ii->canreturn[1] || ii->indexkeys[1] == ii->indexkeys[0])
		return;
	kvar = (Var *) ((TargetEntry *) lsecond(ii->indextlist))->expr;
	if (!IsA(kvar, Var) || !sift_col_ordered_type(kvar->vartype))
		return;
	used = bms_del_member(bms_del_member(bms_copy(used), ii->indexkeys[0] - FirstLowInvalidHeapAttributeNumber), ii->indexkeys[1] - FirstLowInvalidHeapAttributeNumber);
	if (!bms_is_empty(used))
		return;
	gcls = GetDefaultOpClass(gvar->vartype, BTREE_AM_OID);
	kcls = GetDefaultOpClass(kvar->vartype, BTREE_AM_OID);
	if (!OidIsValid(gcls) || !OidIsValid(kcls))
		return;
	gfam = get_opclass_family(gcls);
	kfam = get_opclass_family(kcls);
	/* WHERE g = x: the one group is read, in the order of k (g is the same in every row, so the order asked is k alone) */
	eq = group_equality(rel, gvar, gfam);
	if (eq != NULL)
	{
		IndexClause *ic = makeNode(IndexClause);

		ic->rinfo = eq;
		ic->indexquals = list_make1(eq);
		ic->lossy = false;
		ic->indexcol = 0;
		ic->indexcols = NIL;
		clauses = list_make1(ic);
	}
	/* the paths whose order the query can use come first: the catalog and the statistics are read for those only (planning every statement that reads two columns of the index pays for them otherwise) */
	for (int d = 0; d < 2; d++)
	{
		ScanDirection dir = d == 0 ? ForwardScanDirection : BackwardScanDirection;
		int			strat = d == 0 ? BTLessStrategyNumber : BTGreaterStrategyNumber;
		Oid			gop = get_opfamily_member(gfam, gvar->vartype, gvar->vartype, strat),
					kop = get_opfamily_member(kfam, kvar->vartype, kvar->vartype, strat);
		List	   *pathkeys;

		if (!OidIsValid(gop) || !OidIsValid(kop))
			continue;
		if (eq != NULL)
			pathkeys = EXPR_PATHKEY(root, (Expr *) kvar, kop, rel->relids);
		else
		{
			pathkeys = EXPR_PATHKEY(root, (Expr *) gvar, gop, rel->relids);
			if (pathkeys != NIL)
				pathkeys = list_concat(pathkeys, EXPR_PATHKEY(root, (Expr *) kvar, kop, rel->relids));
		}
		pathkeys = truncate_useless_pathkeys(root, rel, pathkeys);
		if (pathkeys == NIL)
			continue;
		if (oi == NULL)
		{
			if (!column_not_null(planner_rt_fetch(rel->relid, root)->relid, ii->indexkeys[0]) || sift_col_ordered_pair_mode(ii->indexoid, rel->tuples) == 0)
				return;
			oi = makeNode(IndexOptInfo);
			memcpy(oi, ii, sizeof(IndexOptInfo));
			oi->amhasgettuple = true;
			oi->indextlist = NIL;
			foreach_node(TargetEntry, te, ii->indextlist)
			{
				te = flatCopyTargetEntry(te);
				if (oi->indextlist == NIL)
					te->resname = pstrdup(COL_ORDERED_PAIR_MARK);
				oi->indextlist = lappend(oi->indextlist, te);
			}
		}
		*added = lappend(*added, create_index_path(root, oi, clauses, NIL, NIL, pathkeys, dir, true, NULL, 1.0, false));
	}
}

void
sift_col_add_ordered_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *added = NIL;

	/* the parent of partitions or of an inheritance tree has no rows of its own to scan (a partitioned table lists its partitioned indexes, which have no access method routine): its children have the paths */
	if (root->simple_rte_array[rel->relid]->inh)
		return;
	/* no ORDER BY, window or merge join can use an order: truncate_useless_pathkeys would drop every path below, after a pathkey built for each index of each partition (a count(*) of 4 partitions: 261k instructions) */
	if (!has_useful_pathkeys(root, rel))
		return;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		Var		   *var;
		Oid			type,
					opclass,
					opfamily;
		Bitmapset  *used = NULL;
		IndexOptInfo *oi;

		/* amhasgettuple is not asked: cost.c clears it on the copy of the index of a statement that SiftAgg may answer (an aggregate, a window), and the order is what a merge join or a window under it reads */
		if (ii->relam != sift_col_am_oid() || ii->hypothetical || ii->indpred != NIL || ii->indexkeys[0] <= 0 || !ii->canreturn[0])
			continue;
		var = (Var *) ((TargetEntry *) linitial(ii->indextlist))->expr;
		if (!IsA(var, Var) || !sift_col_ordered_type(var->vartype))
			continue;
		type = var->vartype;
		/* the plan reads the first column alone: the others are not in the order array (and a row of the heap costs what a b-tree's does) */
		pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &used);
		foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
			pull_varattnos((Node *) ri->clause, rel->relid, &used);
		if (!bms_is_empty(bms_del_member(bms_copy(used), ii->indexkeys[0] - FirstLowInvalidHeapAttributeNumber)))
		{
			add_pair_paths(root, rel, ii, var, used, &added);
			continue;
		}
		opclass = GetDefaultOpClass(type, BTREE_AM_OID);
		if (!OidIsValid(opclass))
			continue;
		opfamily = get_opclass_family(opclass);
		/*
		 * The plan node keeps the index's target list: the copy of the index the path is made of names its first entry, which tells the scan that its plan wants the order. The list keeps every column: the scan's
		 * tuples have the columns of the index (xs_itupdesc), and the executor stores them in a slot of as many as the node's list, so a shorter one has it write past the slot, over its null flags.
		 */
		oi = makeNode(IndexOptInfo);
		memcpy(oi, ii, sizeof(IndexOptInfo));
		oi->amhasgettuple = true;
		oi->indextlist = NIL;
		foreach_node(TargetEntry, te, ii->indextlist)
		{
			te = flatCopyTargetEntry(te);
			if (oi->indextlist == NIL)
				te->resname = pstrdup(COL_ORDERED_MARK);
			oi->indextlist = lappend(oi->indextlist, te);
		}
		for (int d = 0; d < 2; d++)
		{
			ScanDirection dir = d == 0 ? ForwardScanDirection : BackwardScanDirection;
			Oid			opno = get_opfamily_member(opfamily, type, type, d == 0 ? BTLessStrategyNumber : BTGreaterStrategyNumber);
			List	   *pathkeys;

			if (!OidIsValid(opno))
				continue;
			pathkeys = EXPR_PATHKEY(root, (Expr *) var, opno, rel->relids);
			pathkeys = truncate_useless_pathkeys(root, rel, pathkeys);
			if (pathkeys == NIL)
				continue;
			added = lappend(added, create_index_path(root, oi, NIL, NIL, NIL, pathkeys, dir, true, NULL, 1.0, false));
		}
	}
	foreach_ptr(Path, p, added)
		add_path(rel, p);
}
