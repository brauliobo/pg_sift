/*
 * agg/v6/plan_part.c - SiftAggV6 over a partitioned table: the partitions the planner left after pruning each have a sift_col index of format major 2, and the aggregate reads them one
 * after the other into one set of groups, where an Append over one aggregate for each partition would hash every group once for each. The statement is compiled against the index of the
 * first live partition, with the columns of the index named by the attributes of the parent, and again against the index of every other: the path is made when each compiles to the
 * very same plan, so that the plan names a column of the index and not of a partition.
 * check-memory: allow-allocs every array here follows the live partitions of one statement
 */
#include "postgres.h"

#include "access/genam.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"

#include "agg/plan/safety.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_card.h"

/* The attribute of the parent of app that the Var of the child's attribute stands for, 0 for a column of the child that is none of the parent's (a dropped one) */
static AttrNumber
parent_attno_of(const AppendRelInfo *app, AttrNumber leaf_attno)
{
	int			attno = 0;

	foreach_ptr(Node, v, app->translated_vars)
	{
		attno++;
		if (v != NULL && IsA(v, Var) && ((Var *) v)->varattno == leaf_attno)
			return (AttrNumber) attno;
	}
	return InvalidAttrNumber;
}

/* The attribute of the top partitioned table for an attribute of one of its partitions, 0 for a column that is none of the table's */
static AttrNumber
top_attno_of(PlannerInfo *root, const RelOptInfo *leaf, Index top, AttrNumber attno)
{
	const AppendRelInfo *app = root->append_rel_array[leaf->relid];

	attno = parent_attno_of(app, attno);
	return attno == InvalidAttrNumber || app->parent_relid == top ? attno : top_attno_of(root, find_base_rel(root, app->parent_relid), top, attno);
}

/* The attribute of a partition for an attribute of the top partitioned table */
static AttrNumber
leaf_attno_of(PlannerInfo *root, const RelOptInfo *leaf, Index top, AttrNumber attno)
{
	const AppendRelInfo *app = root->append_rel_array[leaf->relid];

	if (app->parent_relid != top)
		attno = leaf_attno_of(root, find_base_rel(root, app->parent_relid), top, attno);
	return castNode(Var, list_nth(app->translated_vars, attno - 1))->varattno;
}

/* The index of a leaf as an index of the top table, its keys the attributes of the table; NULL when a key is a column that is not one of the table's */
static IndexOptInfo *
index_as_parent(PlannerInfo *root, const RelOptInfo *leaf, Index top, const IndexOptInfo *ix)
{
	IndexOptInfo *as_parent = palloc(sizeof(IndexOptInfo));

	*as_parent = *ix;
	as_parent->indexkeys = palloc(sizeof(int) * ix->ncolumns);
	for (int i = 0; i < ix->ncolumns; i++)
	{
		as_parent->indexkeys[i] = top_attno_of(root, leaf, top, ix->indexkeys[i]);
		if (as_parent->indexkeys[i] == InvalidAttrNumber)
			return NULL;
	}
	return as_parent;
}

static IndexOptInfo *
index_like(PlannerInfo *root, const RelOptInfo *leaf, Index top, const IndexOptInfo *like)
{
	foreach_ptr(IndexOptInfo, ix, leaf->indexlist)
	{
		IndexOptInfo *as_parent;

		if (!sift_v6_index_usable(ix) || ix->ncolumns != like->ncolumns)
			continue;
		as_parent = index_as_parent(root, leaf, top, ix);
		if (as_parent != NULL && memcmp(as_parent->indexkeys, like->indexkeys, sizeof(int) * like->ncolumns) == 0)
			return as_parent;
	}
	return NULL;
}

/* The live partitions of the parent that this plan can read, those of its sub-partitioned partitions too: plain tables, no row security; NIL for any other partition */
static bool
add_leaves(PlannerInfo *root, const RelOptInfo *parent, List **leaves)
{
	int			i = -1;

	if (parent->part_rels == NULL)
		return false;
	while ((i = bms_next_member(parent->live_parts, i)) >= 0)
	{
		RelOptInfo *leaf = parent->part_rels[i];
		RangeTblEntry *rte;

		if (leaf == NULL || IS_DUMMY_REL(leaf))
			continue;
		rte = planner_rt_fetch(leaf->relid, root);
		if (rte->securityQuals != NIL || root->append_rel_array[leaf->relid]->parent_relid != parent->relid)
			return false;
		if (rte->relkind == RELKIND_PARTITIONED_TABLE && rte->inh)
		{
			if (!add_leaves(root, leaf, leaves))
				return false;
		}
		else if (rte->relkind == RELKIND_RELATION && !rte->inh)
			*leaves = lappend(*leaves, leaf);
		else
			return false;
	}
	return true;
}

static List *
live_leaves(PlannerInfo *root, const RelOptInfo *parent)
{
	List	   *leaves = NIL;

	return add_leaves(root, parent, &leaves) ? leaves : NIL;
}

typedef struct PartStmt
{
	GroupPathExtraData *extra;
	List	   *group_exprs;
	uint32		topk;			/* ORDER BY .. LIMIT of the rows when above 0, else the groups */
} PartStmt;

static bool
part_compile(V6Plan *p, const PartStmt *stmt)
{
	return stmt->topk > 0 ? sift_v6_rows_compile(p, stmt->topk) : sift_v6_compile_on(p, stmt->extra, stmt->group_exprs);
}

/* The attribute of each partition for each column the node fetches: the partitions number their columns as they were made, the plan names those of the parent */
static void
leaf_attnos(PlannerInfo *root, V6Plan *first, Index top, List *leaves)
{
	int			at = 0;

	first->spec.part_ncols = list_length(first->scan_tlist);
	first->spec.part_attno = palloc(sizeof(AttrNumber) * first->spec.part_ncols * list_length(leaves));
	foreach_ptr(RelOptInfo, leaf, leaves)
	{
		foreach_node(TargetEntry, te, first->scan_tlist)
			first->spec.part_attno[at++] = leaf_attno_of(root, leaf, top, ((Var *) te->expr)->varattno);
	}
}

/* Compiles the statement against the index of each leaf like the one of the first; the plan of the first, with every index, when all compile alike */
static bool
compile_alike(PlannerInfo *root, RelOptInfo *parent, RelOptInfo *out, const PartStmt *stmt, List *leaves, const IndexOptInfo *like, V6Plan *first)
{
	List	   *packed = NIL;
	Oid		   *indexes = palloc(sizeof(Oid) * list_length(leaves));
	int			n = 0;

	foreach_ptr(RelOptInfo, leaf, leaves)
	{
		IndexOptInfo *ix = index_like(root, leaf, parent->relid, like);
		V6Plan		p;
		Relation	irel;
		bool		ok;

		if (ix == NULL)
			return false;
		p = (V6Plan) {.root = root,.rel = parent,.out = out,.relid = ix->indexoid,.index = ix};
		irel = index_open(ix->indexoid, AccessShareLock);
		sift_v6_plan_index(&p, irel);
		ok = p.ops != NULL && part_compile(&p, stmt);
		index_close(irel, NoLock);
		if (!ok)
			return false;
		indexes[n++] = ix->indexoid;
		p.spec.relid = indexes[0];
		if (packed == NIL)
		{
			*first = p;
			packed = list_make2(sift_v6_spec_pack(&p.spec), p.scan_tlist);
		}
		else if (!equal(packed, list_make2(sift_v6_spec_pack(&p.spec), p.scan_tlist)))
			return false;
	}
	first->spec.nparts = n;
	first->spec.part_index = indexes;
	if (stmt->topk > 0)
		leaf_attnos(root, first, parent->relid, leaves);
	first->spec.part_is_table = palloc0(n * sizeof(bool));
	return true;
}

/* The plan of the statement over the partitions, and the index of the parent it was compiled against */
static bool
compile_partitions(PlannerInfo *root, RelOptInfo *parent, RelOptInfo *out, const PartStmt *stmt, V6Plan *first, IndexOptInfo **like)
{
	List	   *leaves = live_leaves(root, parent);

	if (leaves == NIL || list_length(leaves) > UINT16_MAX)
		return false;
	foreach_ptr(IndexOptInfo, ix, ((RelOptInfo *) linitial(leaves))->indexlist)
	{
		*like = sift_v6_index_usable(ix) ? index_as_parent(root, linitial(leaves), parent->relid, ix) : NULL;
		if (*like != NULL && compile_alike(root, parent, out, stmt, leaves, *like, first))
			return true;
	}
	return false;
}

void
sift_agg_v6_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	const PartStmt stmt = {.extra = extra,.group_exprs = group_exprs};
	IndexOptInfo *like;
	V6Plan		first;

	if (compile_partitions(root, input_rel, output_rel, &stmt, &first, &like))
		sift_v6_add_group_path(&first, output_rel, group_exprs, (List *) extra->havingQual);
}

bool
sift_v6_partitioned_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	return rel->reloptkind == RELOPT_BASEREL && rel->part_scheme != NULL && rte->inh && rte->relkind == RELKIND_PARTITIONED_TABLE && rte->tablesample == NULL &&
		rel->lateral_relids == NULL && !agg_query_has_rls(root);
}

void
sift_v6_add_rows_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k)
{
	const PartStmt stmt = {.topk = k};
	IndexOptInfo *like;
	V6Plan		first;
	Cost		open,
				scan;

	if (!compile_partitions(root, input_rel, output_rel, &stmt, &first, &like))
		return;
	sift_v6_scan_price(root, input_rel, like, 0, &open, &scan);
	sift_v6_rows_add(&first, k, scan, NIL, 0, 0);
}
