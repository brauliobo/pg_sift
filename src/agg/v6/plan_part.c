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
#include "optimizer/paths.h"
#include "parser/parsetree.h"

#include "agg/v6/plan_int.h"

/* The attribute of the parent that the Var of the leaf's attribute stands for, 0 for a column of the leaf that is none of the parent's (a dropped one) */
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

/* The index of a leaf as an index of the parent, its keys the attributes of the parent; NULL when a key is a column that is not one of the parent's */
static IndexOptInfo *
index_as_parent(PlannerInfo *root, const RelOptInfo *leaf, const IndexOptInfo *ix)
{
	const AppendRelInfo *app = root->append_rel_array[leaf->relid];
	IndexOptInfo *as_parent = palloc(sizeof(IndexOptInfo));

	*as_parent = *ix;
	as_parent->indexkeys = palloc(sizeof(int) * ix->ncolumns);
	for (int i = 0; i < ix->ncolumns; i++)
	{
		as_parent->indexkeys[i] = parent_attno_of(app, ix->indexkeys[i]);
		if (as_parent->indexkeys[i] == InvalidAttrNumber)
			return NULL;
	}
	return as_parent;
}

static IndexOptInfo *
index_like(PlannerInfo *root, const RelOptInfo *leaf, const IndexOptInfo *like)
{
	foreach_ptr(IndexOptInfo, ix, leaf->indexlist)
	{
		IndexOptInfo *as_parent;

		if (!sift_v6_index_usable(ix) || ix->ncolumns != like->ncolumns)
			continue;
		as_parent = index_as_parent(root, leaf, ix);
		if (as_parent != NULL && memcmp(as_parent->indexkeys, like->indexkeys, sizeof(int) * like->ncolumns) == 0)
			return as_parent;
	}
	return NULL;
}

/* The live partitions of the parent that this plan can read: plain tables, no row security, children of the parent itself; NIL for any other partition */
static List *
live_leaves(PlannerInfo *root, const RelOptInfo *parent)
{
	List	   *leaves = NIL;
	int			i = -1;

	if (parent->part_rels == NULL)
		return NIL;
	while ((i = bms_next_member(parent->live_parts, i)) >= 0)
	{
		RelOptInfo *leaf = parent->part_rels[i];
		RangeTblEntry *rte;

		if (leaf == NULL || IS_DUMMY_REL(leaf))
			continue;
		rte = planner_rt_fetch(leaf->relid, root);
		if (rte->relkind != RELKIND_RELATION || rte->inh || rte->securityQuals != NIL || root->append_rel_array[leaf->relid]->parent_relid != parent->relid)
			return NIL;
		leaves = lappend(leaves, leaf);
	}
	return leaves;
}

/* Compiles the statement against the index of each leaf like the one of the first; the plan of the first, with every index, when all compile alike */
static bool
compile_alike(PlannerInfo *root, RelOptInfo *parent, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs, List *leaves, const IndexOptInfo *like, V6Plan *first)
{
	List	   *packed = NIL;
	Oid		   *indexes = palloc(sizeof(Oid) * list_length(leaves));
	int			n = 0;

	foreach_ptr(RelOptInfo, leaf, leaves)
	{
		IndexOptInfo *ix = index_like(root, leaf, like);
		V6Plan		p;
		Relation	irel;
		bool		ok;

		if (ix == NULL)
			return false;
		p = (V6Plan) {.root = root,.rel = parent,.out = out,.relid = ix->indexoid,.index = ix};
		irel = index_open(ix->indexoid, NoLock);
		p.colrel = sift_col_rel(irel);
		ok = !p.colrel->stale && sift_v6_compile_on(&p, extra, group_exprs);
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
	return true;
}

void
sift_agg_v6_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	List	   *leaves = live_leaves(root, input_rel);

	if (leaves == NIL)
		return;
	foreach_ptr(IndexOptInfo, ix, ((RelOptInfo *) linitial(leaves))->indexlist)
	{
		IndexOptInfo *like = sift_v6_index_usable(ix) ? index_as_parent(root, linitial(leaves), ix) : NULL;
		V6Plan		first;

		if (like != NULL && compile_alike(root, input_rel, output_rel, extra, group_exprs, leaves, like, &first))
		{
			sift_v6_add_group_path(&first, output_rel, group_exprs, (List *) extra->havingQual);
			return;
		}
	}
}
