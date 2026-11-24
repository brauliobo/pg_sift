/*
 * agg/v6/plan_part.c - SiftAggV6 over a partitioned table: the partitions the planner left after pruning are each a sift_table of Format 2.0 or have a sift_col index of format major 2, and
 * the aggregate reads them one after the other into one set of groups, where an Append over one aggregate for each partition would hash every group once for each. The statement is compiled
 * against a source of the first live partition, with the columns of an index named by the attributes of the parent, and again against a source of every other: the path is made when each
 * has one that compiles to the very same plan, so that the plan names a column of the source and not of a partition. What the statement is compiled as is the caller's (SiftV6CompileFn):
 * the groups of an aggregate, the best rows, or either of those over the join of a partitioned fact to its dimensions.
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
#include "agg/v6/v6_source_rel.h"

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

/* A sift_table names the columns of a leaf by its own attributes: the plan of the parent holds for it when they are the parent's, at every level up to the top table */
static bool
table_as_parent(PlannerInfo *root, const RelOptInfo *leaf, Index top)
{
	const AppendRelInfo *app = root->append_rel_array[leaf->relid];
	int			attno = 0;

	foreach_ptr(Node, v, app->translated_vars)
	{
		attno++;
		if (v != NULL && (!IsA(v, Var) || ((Var *) v)->varattno != attno))
			return false;
	}
	return app->parent_relid == top || table_as_parent(root, find_base_rel(root, app->parent_relid), top);
}

typedef struct LeafSources
{
	SiftV6RelSource *src;
	int			n;
} LeafSources;

/* What the plan of the parent can read of a leaf: its sift_table, and its indexes with their keys the attributes of the parent */
static LeafSources
leaf_sources(PlannerInfo *root, RelOptInfo *leaf, Index top)
{
	LeafSources ls = {.src = sift_v6_rel_sources_alloc(root, leaf, &ls.n)};
	int			kept = 0;

	for (int i = 0; i < ls.n; i++)
	{
		SiftV6RelSource *s = &ls.src[i];

		if (s->is_table ? !table_as_parent(root, leaf, top) : !sift_v6_index_usable(s->ix) || (s->ix = index_as_parent(root, leaf, top, s->ix)) == NULL)
			continue;
		ls.src[kept++] = *s;
	}
	ls.n = kept;
	return ls;
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
part_compile(V6Plan *p, const void *arg)
{
	const PartStmt *stmt = arg;

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

typedef struct PartPlan
{
	PlannerInfo *root;
	RelOptInfo *parent;
	RelOptInfo *out;
	SiftV6CompileFn compile;
	const void *stmt;
	bool		rows;			/* the best rows: the node fetches the columns of the heap rows, whose attributes each partition numbers as it was made */
	List	   *leaves;
} PartPlan;

static bool
compile_source(const PartPlan *pp, const SiftV6RelSource *s, V6Plan *p)
{
	Relation	rel = sift_v6_source_open(s->reloid, s->is_table, AccessShareLock, NULL);
	bool		ok;

	*p = (V6Plan) {.root = pp->root,.rel = pp->parent,.out = pp->out};
	ok = sift_v6_plan_bind(p, s, rel) && pp->compile(p, pp->stmt);
	sift_v6_source_close(rel, s->is_table, NoLock);
	return ok;
}

/* What the plan is, whichever relation of a partition it reads */
static List *
layout_of(const V6Plan *p)
{
	SiftV6Spec	spec = p->spec;

	spec.relid = InvalidOid;
	spec.is_table = false;
	return list_make2(sift_v6_spec_pack(&spec), p->scan_tlist);
}

/* The source of the leaf whose plan is the layout, planned in p; NULL when it has none */
static const SiftV6RelSource *
source_alike(const PartPlan *pp, const LeafSources *ls, List *layout, V6Plan *p)
{
	for (int i = 0; i < ls->n; i++)
		if (compile_source(pp, &ls->src[i], p) && equal(layout_of(p), layout))
			return &ls->src[i];
	return NULL;
}

/* The path when every leaf has a source that compiles like the template's, the one the first leaf compiled; the plan of the template, with the source of each partition */
static bool
compile_alike(const PartPlan *pp, const LeafSources *leaves, int nleaves, const SiftV6RelSource *template, V6Plan *first)
{
	Oid		   *rels = palloc(nleaves * sizeof(Oid));
	bool	   *is_table = palloc(nleaves * sizeof(bool));
	List	   *layout;

	if (!compile_source(pp, template, first))
		return false;
	layout = layout_of(first);
	for (int k = 0; k < nleaves; k++)
	{
		V6Plan		p;
		const SiftV6RelSource *s = k == 0 ? template : source_alike(pp, &leaves[k], layout, &p);

		if (s == NULL)
			return false;
		rels[k] = s->reloid;
		is_table[k] = s->is_table;
	}
	first->spec.relid = rels[0];
	first->spec.is_table = false;
	first->spec.nparts = nleaves;
	first->spec.part_index = rels;
	first->spec.part_is_table = is_table;
	if (pp->rows)
		leaf_attnos(pp->root, first, pp->parent->relid, pp->leaves);
	return true;
}

bool
sift_v6_compile_partitions(PlannerInfo *root, RelOptInfo *parent, RelOptInfo *out, SiftV6CompileFn compile, const void *stmt, bool rows, V6Plan *first, SiftV6RelSource *like)
{
	PartPlan	pp = {root, parent, out, compile, stmt, rows, live_leaves(root, parent)};
	LeafSources *ls;
	int			n = 0;

	if (pp.leaves == NIL || list_length(pp.leaves) > UINT16_MAX)
		return false;
	ls = palloc(list_length(pp.leaves) * sizeof(LeafSources));
	foreach_ptr(RelOptInfo, leaf, pp.leaves)
		ls[n++] = leaf_sources(root, leaf, parent->relid);
	for (int i = 0; i < ls[0].n; i++)
		if (compile_alike(&pp, ls, n, &ls[0].src[i], first))
		{
			*like = ls[0].src[i];
			return true;
		}
	return false;
}

void
sift_agg_v6_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs)
{
	const PartStmt stmt = {.extra = extra,.group_exprs = group_exprs};
	SiftV6RelSource like;
	V6Plan		first;

	if (sift_v6_compile_partitions(root, input_rel, output_rel, part_compile, &stmt, false, &first, &like))
		sift_v6_add_group_path(&first, output_rel, group_exprs, sift_v6_having(extra));
}

bool
sift_v6_partitioned_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	return rel->reloptkind == RELOPT_BASEREL && rel->part_scheme != NULL && rte->inh && rte->relkind == RELKIND_PARTITIONED_TABLE && rte->tablesample == NULL &&
		rel->lateral_relids == NULL && !agg_query_has_rls(root);
}

bool
sift_v6_join_fact_ok(PlannerInfo *root, RelOptInfo *fact)
{
	return sift_v6_plan_fact_ok(root, fact) || sift_v6_partitioned_rel_ok(root, fact);
}

void
sift_v6_fact_paths(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, SiftV6CompileFn compile, SiftV6AddFn add, const void *stmt)
{
	SiftV6RelSource *src;
	int			nsrc;

	if (sift_v6_partitioned_rel_ok(root, fact))
	{
		SiftV6RelSource like;
		V6Plan		first;

		if (sift_v6_compile_partitions(root, fact, out, compile, stmt, false, &first, &like))
			add(&first, stmt);
		return;
	}
	src = sift_v6_rel_sources_alloc(root, fact, &nsrc);
	for (int i = 0; i < nsrc; i++)
	{
		V6Plan		p = {.root = root,.rel = fact,.out = out};
		Relation	rel = sift_v6_source_open(src[i].reloid, src[i].is_table, AccessShareLock, NULL);

		if (sift_v6_plan_bind(&p, &src[i], rel) && compile(&p, stmt))
			add(&p, stmt);
		sift_v6_source_close(rel, src[i].is_table, NoLock);
	}
}

void
sift_v6_add_rows_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k)
{
	const PartStmt stmt = {.topk = k};
	SiftV6RelSource like;
	V6Plan		first;
	Cost		open,
				scan;

	if (!sift_v6_compile_partitions(root, input_rel, output_rel, part_compile, &stmt, true, &first, &like))
		return;
	sift_v6_source_scan_price(root, input_rel, &like, 0, &open, &scan);
	sift_v6_rows_add(&first, k, scan, NIL, 0, 0);
}
