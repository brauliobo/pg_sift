/*
 * agg/plan/finish.c - the last steps of a spec
 * Parallel safety and workers, the spelling of keys, the lookups by parameter, and the
 * price and shape the spec is given (spec_finish, agg_finish).
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_proc.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/plan/int.h"

/* what a group table that cannot spill may be, as a multiple of hash memory, before the scan gives way to the normal plan (which spills) */
#define GROUP_NOSPILL_FACTOR	32
/* ... of the hash memory, or of the memory the defaults give (work_mem 4MB, hash_mem_multiplier 2) when that is more: a small work_mem is a choice for sorts */
#define GROUP_NOSPILL_BASE		(8 * 1024 * 1024)

/* the bytes of a row of a lookup table besides its columns: the hash entry and the row's header (about 48 in all for an integer key and a text column, measured) */
#define AGG_LOOKUP_ROW_OVERHEAD	64

/*
 * Do `rows` rows of `width` bytes fit the lookup tables the scan loads when it starts? A rows limit that was set (the older form of
 * sift.agg_lookup_max_kb) is compared with the rows alone.
 */
bool
agg_lookup_fits(double rows, int width)
{
	if (sift_agg_lookup_max_rows >= 0)
		return rows <= sift_agg_lookup_max_rows;
	return rows * (width + AGG_LOOKUP_ROW_OVERHEAD) <= (double) sift_agg_lookup_max_kb * 1024;
}

static bool
fn_unsafe(Oid funcid, void *context)
{
	return func_parallel(funcid) != PROPARALLEL_SAFE;
}

/* does a function of the packed spec (its expressions are Nodes among Integers and Strings), or of a query, run only in the leader? */
static bool
unsafe_in_worker(Node *n, void *context)
{
	if (n == NULL || IsA(n, Integer) || IsA(n, String) || IsA(n, IntList) || IsA(n, OidList))
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, unsafe_in_worker, context, 0);
	return check_functions_in_node(n, fn_unsafe, NULL) || expression_tree_walker(n, unsafe_in_worker, context);
}

/* can equal values of the wide leaf be spelled in more than one way (1.0 and 1.00, 1 month and 30 days)? A uuid, bytea or network address has one spelling */
bool
spelled_wide(const SiftSrcLeaf *lf)
{
	Oid			typid = OidIsValid(lf->wtypid) ? lf->wtypid : lf->typid;

	return typid != UUIDOID && typid != BYTEAOID && typid != INETOID && typid != CIDROID && typid != MACADDROID && typid != MACADDR8OID;
}

/*
 * Does a group key read a wide leaf whose equal values are spelled in more than one way? The group shows the spelling of the first row, which the workers' tables
 * merge in no order. The counts, sums, averages and variances over a wide leaf do not show it.
 */
static bool
key_spelling(AggCtx *c, const SiftAggSpec *s)
{
	for (int j = 0; j < s->nkeys; j++)
	{
		const SiftAggKey *k = &s->key[j];
		const SiftAggCombo *cb = k->kind == SAK_COMBO ? &s->combo[k->leaf] : NULL;

		for (int i = 0; i < (cb ? cb->nleaves : 1); i++)
		{
			int			leaf = cb ? cb->leaf[i] : k->leaf;

			if (leaf < 0 || (!cb && k->kind != SAK_LEAF && k->kind != SAK_LUT) || !leaf_of(c, leaf)->wide)
				continue;
			if (spelled_wide(leaf_of(c, leaf)))
				return true;
		}
	}
	return false;
}

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the spec's
 * expressions are safe to run in a worker and its tables can be merged (sift_aggspec_parallel_ok). The relations of the joins are not
 * asked: the leader reads them (a CTE, a sub-select) and shares the lookup tables; the plan of a correlated join runs in each worker.
 */
int
parallel_workers(AggCtx *c, SiftAggSpec *s)
{
	if (!sift_aggspec_parallel_ok(s) || !c->root->glob->parallelModeOK || !c->rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return 0;
	if (c->sub_serial || unsafe_in_worker((Node *) sift_aggspec_pack(s), NULL))
	{
		s->serial = "an expression of the query has a sub-select or a function that is not parallel safe";
		return 0;
	}
	/* a worker runs the plan of a correlated join itself (its own executor state, from the statement the leader sends) */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->j.params != NIL && (jc->subroot == NULL || unsafe_in_worker((Node *) jc->subroot->parse, NULL)))
		{
			s->serial = "the plan of a correlated join is not parallel safe";
			return 0;
		}
	if (key_spelling(c, s))
	{
		s->serial = "a group key shows one of the spellings of equal values (1.0, 1.00), which the workers read in no order";
		return 0;
	}
	/* the leader writes the lookup tables to the shared memory and each worker maps them: a scan of fewer rows than AGG_SHARE_RATIO for each of their rows does not pay for it */
	if (agg_share_rows(c, s) * AGG_SHARE_RATIO > Max(c->stats.nrows, c->rel->tuples))
		return 0;
	return compute_parallel_worker(c->rel, rel_pages(c), -1, max_parallel_workers_per_gather);
}

/* is leaf m the value column of an object whose members the spec reads only by key, all of them read again from the heap (member_recheck_ok)? */
static bool
member_source(AggCtx *c, const SiftAggSpec *s, int m)
{
	bool		found = false;

	for (int j = 0; j < s->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) == m)
		{
			if (s->vleaf[j].recheck == SRK_NONE)
				return false;
			found = true;
		}
	return found;
}

/*
 * Does the spec read a leaf that holds values the index cannot store? The scan raises an error when a row of its snapshot has one
 * (check_overflow), the normal plan reads them from the heap: the query keeps its plan. Unless the source reads the rows again as SQL reads
 * them (spec_recheck). A numeric leaf is not one: its only such value is NaN, which the scan evaluates (SiftSrcVec.ovf). Nor is the leaf of a
 * jsonb_typeof conjunct (SAP_TYPE): the source keeps the rows of the types and tells the type of each value it cannot store. Unless the query
 * reads the leaf too and the conjunct keeps a type the leaf cannot store (typeof_keeps_unstorable): such a row would be an error.
 */
static bool
reads_overflow(AggCtx *c, const SiftAggSpec *s)
{
	Bitmapset  *typed = NULL,
			   *leaves;
	int			m = -1;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE && !typeof_keeps_unstorable(c, s->pred[i]))
			typed = bms_add_member(typed, s->pred[i]->leaf);
	leaves = bms_union(c->used, c->func_ovf);
	for (int j = 0; j < c->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) >= 0)
			leaves = bms_add_member(leaves, vleaf_source(c, j));
	while ((m = bms_next_member(leaves, m)) >= 0)
		if (c->stats.ovf[m] && !bms_is_member(m, typed) && spec_recheck(c, s, m) == SRK_NONE && !member_source(c, s, m))
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not read \"%s\": some unit holds values the leaf cannot store", leaf_name(c, m))));
			if (bms_is_member(m, c->order_refused))
				agg_refused_f(DIRTY_ORDER, "%s: PostgreSQL decides whether the cast of a row that raises runs before another condition drops the row", leaf_name(c, m));
			else if (c->dirty_why && c->dirty_leaf == m)
				agg_refused_f(DIRTY_UNITS, "%s: %s", leaf_name(c, m), c->dirty_why);
			else
				agg_refused_on(DIRTY_UNITS, leaf_name(c, m));
			return true;
		}
	return false;
}

/*
 * Does the spec cast the value of a leaf that may hold a JSON null with the cast function of jsonb? Before PostgreSQL 18 the cast raises for it
 * (NULL since), as the scan, which reads the NULL slot, would not: the normal plan keeps the query. A leaf of a unit without JSON nulls keeps the scan.
 */
static bool
reads_json_null_cast(AggCtx *c)
{
	Bitmapset  *casts = bms_intersect(c->jcast, c->read);
	int			m = -1,
				first = c->lay->nleaves - c->nvleaf;

	while ((m = bms_next_member(casts, m)) >= 0)
	{
		int			src = m < first ? m : vleaf_source(c, m - first);

		if (src >= 0 && c->stats.jnull[src])
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not cast \"%s\": some unit may hold a JSON null there, which the cast raises for before PostgreSQL 18",
									leaf_name(c, src))));
			agg_refused_on(ERROR_PARITY, leaf_name(c, src));
			return true;
		}
	}
	return false;
}

/* the leaves the spec reads and the combos; false when the spec cannot be run. keys: the group expressions of root's query */
bool
spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys)
{
	double		lists;

	if (c->other_group)
		return agg_refuse_on(FROM_SHAPE, "the query reads the elements of a second list besides the one it unnests");
	if (types_guard_reads(c))
		return agg_refuse_on(ERROR_PARITY, "a jsonb_typeof guard (FILTER or CASE) wraps a read of the same path: the leaf cannot hold the values the guard skips");
	/* the NULL tuples are found before the late conjuncts run, so a late one on the elements could not give a row whose elements all fail it its tuple */
	if (s->left_lateral)
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->level == 1 && s->pred[i]->late)
				return agg_refuse_on(WHERE, "a condition of the LEFT JOIN LATERAL may raise an error and is evaluated after the elements are kept");
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return agg_refuse_f(FROM_SHAPE, "%s is an element of a list the query does not unnest", leaf_name(c, i));
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return agg_refuse_f(FROM_SHAPE, "the unnested list %s is read by no leaf", leaf_name(c, c->group));
	if (list_length(c->combos) > sift_limit(combos, SIFT_AGG_MAX_COMBOS))
		return agg_refuse_limit(SHAPE_WIDTH, combos, sift_limit(combos, SIFT_AGG_MAX_COMBOS), "%d value combinations", list_length(c->combos));
	if (list_length(c->joins) > sift_limit(joins, SIFT_AGG_MAX_JOINS))
		return agg_refuse_limit(SHAPE_WIDTH, joins, sift_limit(joins, SIFT_AGG_MAX_JOINS), "%d lookup-joined tables", list_length(c->joins));
	typeof_reads(c, s);
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	/* the columns a recordset declares and the query does not read are leaves of the spec too: one the source cannot read again keeps the plan */
	finish_leaves(c, s);
	if (reads_overflow(c, s) || reads_json_null_cast(c))
		return agg_refuse(DIRTY_UNITS);
	finish_combos(c, s);
	s->est_groups = set_groups(c, root, s, keys, input_rel->rows, false);
	s->est_rows = input_rel->rows;
	/*
	 * Where PostgreSQL's count of a lookup is a default guess (the lists are the output of a plan), the path reports the groups of the rows
	 * the lists can pass, at the geometric mean of PostgreSQL's count and all of them (as the normal plan's price takes the keys,
	 * lookup_set_ratio): a plan above that takes a CTE of one row for one nests loops on a null-safe key and runs the aggregate under it
	 * again for each row, while a count that is too large only makes it hash or materialize, and one as large as the distinct values of the key
	 * (the keys of a request name a few of them) makes the plan above a join of thousands of groups. A filter of the table that has no
	 * statistics keeps a guess of the rows as well (filter_rows_ratio).
	 */
	lists = sqrt(lookup_set_ratio(c, s, input_rel, true) * filter_rows_ratio(c));
	c->path_rows = lists > 1.0 ? Min(set_groups(c, root, s, keys, input_rel->rows * lists, true), s->est_groups + AGG_LIFT_GROUPS) : 0;
	/* the floor PostgreSQL gives an unnest over a lookup it cannot count is not the groups of the table's elements (agg/cost/model.c) */
	c->path_rows = Max(c->path_rows, tuple_group_count(c, s, input_rel));
	if (c->search)
		s->search = c->search->spec;

	spec_deps(root, s);
	return true;
}

static double
param_set_ratio(AggCtx *c)
{
	double		f = 1.0;

	if (sift_agg_param_set_size <= 0)
		return 1.0;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		ScalarArrayOpExpr *sa;
		Node	   *arr;
		double		n0;

		if (!IsA(ri->clause, ScalarArrayOpExpr))
			continue;
		sa = (ScalarArrayOpExpr *) ri->clause;
		if (!sa->useOr || list_length(sa->args) != 2)
			continue;
		arr = strip_relabel(lsecond(sa->args));
		if (!IsA(arr, Param) || ((Param *) arr)->paramkind != PARAM_EXTERN || !IsA(strip_relabel(linitial(sa->args)), Var) ||
			match_leaf(c, linitial(sa->args)) < 0)
			continue;
		n0 = estimate_array_length(c->root, arr);
		if (sift_agg_param_set_size > n0)
			f *= sift_agg_param_set_size / n0;
	}
	return Min(f, Max(c->rel->tuples / Max(c->rel->rows, 1.0), 1.0));
}

/* is the path a scan of the table the node scans (the plan has other ranges of it: a CTE read twice, an inlined copy)? */
static bool
scans_table(AggCtx *c, const Path *p)
{
	return p->parent->reloptkind == RELOPT_BASEREL &&
		planner_rt_fetch(p->parent->relid, c->root)->relid == planner_rt_fetch(c->relid, c->root)->relid;
}

/*
 * the scan of the rows of the table a plan reads them with, when it is an index or bitmap scan under aggregation, sorts and gathers,
 * or the probes a join makes of one per key (a nested loop, or the input of a merge or hash join above it)
 */
static Path *
row_scan_of(Path *p, AggCtx *c)
{
	for (;;)
		switch (nodeTag(p))
		{
			case T_IndexPath:
			case T_BitmapHeapPath:
				return scans_table(c, p) ? p : NULL;
			case T_AggPath:
				p = ((AggPath *) p)->subpath;
				break;
			case T_GroupPath:
				p = ((GroupPath *) p)->subpath;
				break;
			case T_SortPath:
				p = ((SortPath *) p)->subpath;
				break;
			case T_IncrementalSortPath:
				p = ((IncrementalSortPath *) p)->spath.subpath;
				break;
			case T_GatherPath:
				p = ((GatherPath *) p)->subpath;
				break;
			case T_GatherMergePath:
				p = ((GatherMergePath *) p)->subpath;
				break;
			case T_ProjectionPath:
				p = ((ProjectionPath *) p)->subpath;
				break;
			case T_MaterialPath:
				p = ((MaterialPath *) p)->subpath;
				break;
			case T_NestPath:
			case T_MergePath:
			case T_HashPath:
				{
					/* the keys of a lookup drive the probes of an index or bitmap scan of the table: the plan costs a probe a key */
					JoinPath   *jp = (JoinPath *) p;
					Path	   *inner = jp->innerjoinpath,
							   *found;

					if (IsA(inner, MemoizePath))
						inner = ((MemoizePath *) inner)->subpath;
					if ((IsA(inner, IndexPath) || IsA(inner, BitmapHeapPath)) && inner->param_info != NULL && scans_table(c, inner))
						return inner;
					found = row_scan_of(jp->outerjoinpath, c);
					return found ? found : row_scan_of(jp->innerjoinpath, c);
				}
			default:
				return NULL;
		}
}

/* the paths that fetch the rows one set element or key at a time are charged the elements of the array parameters and the keys of the lookups that sift.agg_param_set_size and lookup_set_ratio assume */
static void
param_set_penalty(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	/*
	 * a few keys are probes of the btree, which the normal plan prices as they run: the rows the table holds for them are no lost estimate. Keys that are
	 * sure to be few but leave a handful of units are charged the skew of the key alone (excess): the plan that probes them reads the rows of the key, not
	 * those the table's filters keep, and a charge for those made the scan win for a key of a few hundred rows
	 */
	double		key_rows,
				excess;
	bool		sure,
				few_keys = few_keys_lookup(c, s, &key_rows, &excess, &sure);
	double		f = param_set_ratio(c) * (sure ? 1.0 : lookup_set_ratio(c, s, input_rel, false)),
				rows = few_keys ? Max(input_rows_ratio(c, s, input_rel, key_rows), excess) :
					sure ? excess : input_rows_ratio(c, s, input_rel, c->rel->rows);

	if (f <= 1.0 && rows <= 1.0)
		return;
	foreach_ptr(Path, p, output_rel->pathlist)
	{
		double		g = (row_scan_of(p, c) != NULL ? f : 1.0) * (IsA(p, CustomPath) ? 1.0 : rows);

		p->startup_cost *= g;
		p->total_cost *= g;
	}
}

/* the cost; adds the paths to output_rel */
void
agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *keys, List *scan_tlist)
{
	int			nworkers,
				nospill = (int) sift_limit(group_nospill_factor, GROUP_NOSPILL_FACTOR);
	double		state,
				table;

	if (!spec_finish(c, s, root, input_rel, keys))
		return;
	/* only a count(DISTINCT) carries a set per group, which is what makes the table of a partition's groups worth finishing early: the tuples of the others cost more than the table */
	if (c->part_key && sift_aggspec_spillable(s) && !(sift_aggspec_float_sums(s) && !sift_aggspec_workers_only(s)))
		for (int i = 0; i < s->naggs; i++)
			s->part_final |= s->agg[i].kind == SAA_COUNT_DISTINCT;
	if (!first_fits(s))
	{
		agg_refused(FIRST_MEMORY);
		return;
	}
	table = group_table_bytes(s, group_count(c, s), &state);
	/* a scan that cannot spill holds all its groups: the estimate may go far past hash memory (an OOM kill is the other end of it) but not to the sizes of a table of millions of groups */
	if (table > (double) nospill * Max((double) get_hash_memory_limit(), (double) GROUP_NOSPILL_BASE) && !sift_aggspec_spillable(s))
	{
		agg_refused_on(GROUP_MEMORY, psprintf("about %.0f groups need %.0f kB, a hash aggregate gets %.0f kB, and this scan cannot spill them past %d times %.0f kB",
											  group_count(c, s), table / 1024.0, (double) get_hash_memory_limit() / 1024.0, nospill,
											  Max((double) get_hash_memory_limit(), (double) GROUP_NOSPILL_BASE) / 1024.0));
		return;
	}
	if (!child_paths(c, s))
		return;
	param_set_penalty(c, s, input_rel, output_rel);
	nworkers = preagg_share(c, s) >= AGG_PREAGG_EXACT ? 0 : parallel_workers(c, s);
	/* the table that spills is the process's alone: the workers' would each hold their own, and the leader's the sum */
	if (table > (double) get_hash_memory_limit() && sift_aggspec_spillable(s))
		nworkers = 0;
	add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, 0, NIL);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, nworkers, NIL);
	agg_ordered_paths(c, s, root, input_rel, output_rel, target, scan_tlist, keys, table, nworkers);
}
