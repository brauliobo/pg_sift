/*
 * col/amscan/ordpath.c - the paths of an ordered index-only scan
 * A sift_col index whose first column is an integer, a date or a timestamp can give that column in order (ordered.c) to a plan that reads it alone: a Sort above the
 * unordered scan of the index, or of a merge join's side, is what the path replaces. The path has the pathkey of the first column only (never the order of the
 * index's other columns), the direction of the scan says to the access method that it is to give the order, and the plan asks no index condition: a condition on the
 * column is the node's own qual.
 */
#include "postgres.h"
#include "access/sdir.h"
#include "access/genam.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "col/am.h"
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

void
sift_col_add_ordered_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *added = NIL;

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
		if (!bms_is_empty(bms_del_member(used, ii->indexkeys[0] - FirstLowInvalidHeapAttributeNumber)))
			continue;
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
