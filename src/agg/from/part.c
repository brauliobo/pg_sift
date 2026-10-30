/*
 * agg/from/part.c - the column indexes of the partitions of a table
 * Their candidates, what they have in common, and the clauses that prune them.
 * The facts they are made from are read by facts.c.
 */
#include "postgres.h"

#include "catalog/pg_am.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/from/int.h"

/* a partition a column index or the table itself can be read through: a heap table without row security, or a sift_table */
static bool
part_readable(AggCtx *c, RelOptInfo *leaf)
{
	RangeTblEntry *rte = planner_rt_fetch(leaf->relid, c->root);

	return rte->relkind == RELKIND_RELATION && (get_rel_relam(rte->relid) == HEAP_TABLE_AM_OID || sift_colsrc_can_part(get_rel_relam(rte->relid), false)) &&
		rte->securityQuals == NIL;
}

static bool
part_is_table(AggCtx *c, RelOptInfo *leaf)
{
	return get_rel_relam(planner_rt_fetch(leaf->relid, c->root)->relid) != HEAP_TABLE_AM_OID;
}

static bool
part_index_usable(const IndexOptInfo *ii)
{
	return sift_colsrc_can_part(ii->relam, true) && !ii->hypothetical && (ii->indpred == NIL || ii->predOK);
}

/* does the partition have a column index at all (a sift_table is its own source)? Only its list of indexes is looked at, nothing of the index is read */
static bool
part_has_index(AggCtx *c, RelOptInfo *leaf)
{
	if (!part_readable(c, leaf))
		return false;
	if (part_is_table(c, leaf))
		return true;
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
		if (part_index_usable(ii))
			return true;
	return false;
}

/* the per-attribute expressions of a sift_table, which stand at the attribute numbers of its partition, at those of the partitioned table (NULL: dropped) */
static List *
part_attlist(List *in, PartVars *pv, int natts)
{
	Node	  **at = palloc0(natts * sizeof(Node *));
	List	   *out = NIL;

	foreach_ptr(Node, e, in)
		if (e != NULL)
		{
			AttrNumber	att = part_attno(pv, foreach_current_index(e) + 1);

			if (att > 0)
				at[att - 1] = part_vars_mutator(e, pv);
			else
				pv->ok = false;
		}
	for (int i = 0; i < natts; i++)
		out = lappend(out, at[i]);
	return out;
}

/* the facts of a partition's source, with the layout in the terms of the partitioned table; added to cands unless they cannot be */
static List *
part_cand(AggCtx *c, RelOptInfo *leaf, List *cands, Oid relid, bool table)
{
	const PartCand *facts = part_facts(relid, table);
	PartCand   *pc = palloc(sizeof(PartCand));
	PartVars	pv = {c->root, leaf->relid, c->relid, true};
	SiftSrcLayout *lay = palloc(sizeof(SiftSrcLayout));

	*pc = *facts;
	pc->lay = lay;
	*lay = *facts->lay;
	if (table)
	{
		lay->natts = c->rel->max_attr;
		lay->attexprs = part_attlist(facts->lay->attexprs, &pv, lay->natts);
		lay->attalias = facts->lay->attalias ? part_attlist(facts->lay->attalias, &pv, lay->natts) : NIL;
		lay->leaf = memcpy(palloc(lay->nleaves * sizeof(SiftSrcLeaf)), facts->lay->leaf, lay->nleaves * sizeof(SiftSrcLeaf));
		for (int i = 0; i < lay->nleaves; i++)
			if ((lay->leaf[i].attno = part_attno(&pv, lay->leaf[i].attno)) <= 0)
				pv.ok = false;
	}
	else
	{
		lay->attexprs = NIL;
		foreach_ptr(Node, e, facts->lay->attexprs)
			lay->attexprs = lappend(lay->attexprs, part_vars_mutator(e, &pv));
	}
	return pv.ok ? lappend(cands, pc) : cands;
}

/* The candidates of a leaf partition: its sift_col indexes or the sift_table itself, with the layouts in the terms of the partitioned table; none when it cannot be read */
static List *
part_candidates(AggCtx *c, RelOptInfo *leaf)
{
	List	   *cands = NIL;

	if (!part_readable(c, leaf))
		return NIL;
	if (part_is_table(c, leaf))
		return part_cand(c, leaf, NIL, planner_rt_fetch(leaf->relid, c->root)->relid, true);
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
		if (part_index_usable(ii))
			cands = part_cand(c, leaf, cands, ii->indexoid, false);
	return cands;
}

/*
 * Do two partitions write the same pre-aggregates? The key lists of the integer leaves (key_columns = auto) and the rollups (rollup_dims = auto) follow the
 * data of each partition, and a unit without one is read by its rows: acc keeps the leaves and the rollups every partition so far lists, and what else
 * differs is a layout of its own.
 */
static bool
part_rollup_same(const SiftSrcPreagg *x, int i, const SiftSrcPreagg *y, int j)
{
	return x->rollup[i].list == y->rollup[j].list && x->rollup[i].ndims == y->rollup[j].ndims && x->rollup[i].nmeas == y->rollup[j].nmeas &&
		memcmp(x->rollup[i].dim, y->rollup[j].dim, x->rollup[i].ndims * sizeof(int)) == 0 &&
		memcmp(x->rollup[i].meas, y->rollup[j].meas, x->rollup[i].nmeas * sizeof(int)) == 0;
}

/*
 * The measures a dim holds follow the key columns too: the key table of a key column adds its measures to the dim of the column (agg/pre.c), in the partitions
 * that keep one. The dims both have (a leaf and its calendar cells are two) with the measures both hold: a key table alone answers whole segments, a dim that
 * is a cube in one partition is not a key table in all.
 */
static void
part_dims_common(SiftSrcPreagg *a, SiftSrcPreagg *b, const SiftSrcPreagg *acc, const SiftSrcPreagg *pa)
{
	int			n = 0;

	for (int i = 0; i < acc->ndims; i++)
		for (int j = 0; j < pa->ndims; j++)
		{
			int			kept = 0;

			if (acc->dim[i].leaf != pa->dim[j].leaf || acc->dim[i].cal != pa->dim[j].cal)
				continue;
			a->dim[n] = acc->dim[i];
			for (int m = 0; m < acc->dim[i].nmeas; m++)
				for (int k = 0; k < pa->dim[j].nmeas; k++)
					if (acc->dim[i].meas[m] == pa->dim[j].meas[k])
					{
						a->dim[n].meas[kept++] = acc->dim[i].meas[m];
						break;
					}
			memset(a->dim[n].meas + kept, 0, (SIFT_SRC_MAX_MEAS - kept) * sizeof(int));
			a->dim[n].nmeas = kept;
			a->dim[n].minmax &= pa->dim[j].minmax;
			a->dim[n].ktab |= pa->dim[j].ktab;
			b->dim[n] = a->dim[n];
			n++;
			break;
		}
	memset(a->dim + n, 0, (SIFT_SRC_MAX_DIMS - n) * sizeof(a->dim[0]));
	memset(b->dim + n, 0, (SIFT_SRC_MAX_DIMS - n) * sizeof(b->dim[0]));
	a->ndims = b->ndims = n;
}

static bool
part_preagg_common(SiftSrcPreagg *acc, const SiftSrcPreagg *pa)
{
	SiftSrcPreagg a,
				b;
	int			n = 0,
				kept = 0;

	memcpy(&a, acc, sizeof(a));
	memcpy(&b, pa, sizeof(b));
	memset(a.rollup, 0, sizeof(a.rollup));
	for (int i = 0; i < acc->nrollups; i++)
		for (int j = 0; j < pa->nrollups; j++)
			if (part_rollup_same(acc, i, pa, j))
			{
				a.rollup[kept++] = acc->rollup[i];
				break;
			}
	a.nrollups = b.nrollups = kept;
	memcpy(b.rollup, a.rollup, sizeof(b.rollup));
	part_dims_common(&a, &b, acc, pa);
	a.npres = b.npres = 0;
	memset(a.pres, 0, sizeof(a.pres));
	memset(b.pres, 0, sizeof(b.pres));
	memset(a.hll, 0, sizeof(a.hll));
	memset(b.hll, 0, sizeof(b.hll));
	if (memcmp(&a, &b, sizeof(a)) != 0)
		return false;
	for (int p = 0; p < acc->npres; p++)
		for (int q = 0; q < pa->npres; q++)
			if (acc->pres[p] == pa->pres[q])
				acc->pres[n++] = acc->pres[p];
	memset(acc->pres + n, 0, (acc->npres - n) * sizeof(acc->pres[0]));
	for (int c = 0; c < SIFT_SRC_CAL; c++)
		acc->hll[c] &= pa->hll[c];
	acc->npres = n;
	acc->ndims = a.ndims;
	memcpy(acc->dim, a.dim, sizeof(acc->dim));
	acc->nrollups = kept;
	memcpy(acc->rollup, a.rollup, sizeof(acc->rollup));
	return true;
}

/* the partitions that the planner kept, the partitioned ones replaced by theirs: false when a partition is not one a column index reads */
static void
part_leaves(RelOptInfo *rel, List **leaves)
{
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
	{
		RelOptInfo *p = rel->part_rels[i];

		if (IS_DUMMY_REL(p))
			continue;
		if (p->part_scheme != NULL)
			part_leaves(p, leaves);
		else
			*leaves = lappend(*leaves, p);
	}
}

/* the names of the columns the partitions are split by, at every level (hash: only at the levels split by hash) */
static void
part_key_names(AggCtx *c, RelOptInfo *rel, bool hash, List **names)
{
	Oid			relid = planner_rt_fetch(rel->relid, c->root)->relid;

	if (!hash || rel->part_scheme->strategy == PARTITION_STRATEGY_HASH)
		for (int k = 0; k < rel->part_scheme->partnatts; k++)
			foreach_node(Var, v, pull_var_clause((Node *) rel->partexprs[k], 0))
				*names = list_append_unique(*names, makeString(get_attname(relid, v->varattno, false)));
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
		if (rel->part_rels[i]->part_scheme != NULL)
			part_key_names(c, rel->part_rels[i], hash, names);
}

static bool
exec_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if ((IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC) || IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
		return true;
	return expression_tree_walker(node, exec_param_walker, context);
}

static bool
mentions_key(Node *clause, Oid relid, List *names)
{
	foreach_node(Var, v, pull_var_clause(clause, 0))
		if (list_member(names, makeString(get_attname(relid, v->varattno, false))))
			return true;
	return false;
}

/*
 * Does the hash split of rel, or of a level under it, take this condition on its column as sift_aggpart_prune computes it, an equality of the
 * key's type, operator family and collation? A plan that cannot prune what PostgreSQL prunes when it runs keeps the normal plan.
 */
static bool
hash_clause_handled(AggCtx *c, RelOptInfo *rel, Var *v, Node *operand, Oid opno, Oid collid, bool any)
{
	PartitionScheme sc = rel->part_scheme;
	Oid			relid = planner_rt_fetch(rel->relid, c->root)->relid;

	if (sc->strategy == PARTITION_STRATEGY_HASH)
		for (int k = 0; k < sc->partnatts; k++)
		{
			Node	   *e = linitial(rel->partexprs[k]);

			if (IsA(e, Var) && strcmp(get_attname(relid, ((Var *) e)->varattno, false), get_attname(c->heapoid, v->varattno, false)) == 0)
				return v->vartype == ((Var *) e)->vartype && collid == sc->partcollation[k] &&
					get_op_opfamily_strategy(opno, sc->partopfamily[k]) == HTEqualStrategyNumber &&
					(any ? get_element_type(exprType(operand)) : exprType(operand)) == v->vartype;
		}
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
		if (rel->part_rels[i]->part_scheme != NULL && hash_clause_handled(c, rel->part_rels[i], v, operand, opno, collid, any))
			return true;
	return false;
}

/*
 * A condition on a column the partitions are split by whose operand is a parameter of a generic plan or a stable function (PostgreSQL prunes
 * the partitions with it when the plan starts) is kept in c->partprune, in the terms of the table with varno 1: the scan evaluates the operands
 * when it starts and drops the partitions the partition bounds refute (sift_aggpart_prune). So is an equality on a column a hash level splits by,
 * whatever its operand, as the remainder needs the values of every column of the level. False when a hash level's column is compared by a
 * condition that is not an equality of its type: the bounds cannot refute a hash remainder, and the normal plan keeps the pruning.
 */
static bool
part_prune_clauses(AggCtx *c)
{
	List	   *names = NIL,
			   *hashed = NIL;

	c->partprune = NIL;
	part_key_names(c, c->rel, false, &names);
	part_key_names(c, c->rel, true, &hashed);
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		Node	   *cl = (Node *) ri->clause;
		bool		dynamic = extern_param_walker(cl, NULL) || contain_mutable_functions(cl);

		if (contain_volatile_functions(cl) || exec_param_walker(cl, NULL) || !mentions_key(cl, c->heapoid, names))
			continue;
		if (mentions_key(cl, c->heapoid, hashed))
		{
			Var		   *v;
			Node	   *operand;
			Oid			opno,
						collid;
			bool		any;

			if (!sift_aggpart_key_clause(cl, &v, &operand, &opno, &collid, &any) || !hash_clause_handled(c, c->rel, v, operand, opno, collid, any))
			{
				if (dynamic)
					return false;
				continue;
			}
		}
		else if (!dynamic)
			continue;
		cl = copyObject(cl);
		ChangeVarNodes(cl, c->relid, 1, 0);
		c->partprune = lappend(c->partprune, cl);
	}
	return true;
}

/*
 * The source of a partitioned table: the live partitions after PostgreSQL's pruning, each read through a sift_col index of the same layout, the
 * columns in the terms of the table (the query is compiled once, against them). A partition without such an index, of another storage, or whose
 * index differs from the others' keeps the normal plan, and so does a table split by hash on a column that a parameter is compared with other than by
 * an equality. The partitions a parameter or a stable function prunes when the plan runs are dropped when the scan starts.
 */
bool
part_source(AggCtx *c)
{
	List	   *leaves = NIL,
			   *first;
	int			n;

	if (c->rel->part_scheme == NULL || c->rel->nparts == 0)
		return agg_refuse_on(PARTITION_SOURCE, "the table is not partitioned, or has no partition");
	part_leaves(c->rel, &leaves);
	n = list_length(leaves);
	if (n == 0)
		return agg_refuse_on(PARTITION_SOURCE, "no partition is left after the pruning");
	if (n > sift_limit(partitions, SIFT_AGG_MAX_PARTS))
		return agg_refuse_limit(PARTITION_COUNT, partitions, sift_limit(partitions, SIFT_AGG_MAX_PARTS), "%d partitions are left after the pruning", n);
	/* a partition without a column index is the reason, whatever the cost: the one a user can act on */
	foreach_ptr(RelOptInfo, leaf, leaves)
		if (!part_has_index(c, leaf))
			return agg_refuse_on(PARTITION_SOURCE, "a partition has no column index the plan can use, or the partitions do not share a layout");
	/* opening the partitions alone can cost more than the normal plan: nothing of them is read to find that out */
	if (agg_open_cost(n) > agg_stage_normal && !sift_agg_force)
		return agg_refuse_f(COST, "opening %d partitions costs the scan %.2f, the normal plan %.2f", n, agg_open_cost(n), agg_stage_normal);
	if (!part_prune_clauses(c))
		return agg_refuse_on(PARTITION_SOURCE, "a condition on the partition key cannot be evaluated when the scan starts");
	first = part_candidates(c, linitial_node(RelOptInfo, leaves));
	foreach_ptr(PartCand, f, first)
	{
		List	   *picked = list_make1(f);
		SiftSrcPreagg common;

		memcpy(&common, f->pa, sizeof(common));
		for (int i = 1; i < n && picked != NIL; i++)
		{
			PartCand   *m = NULL;

			foreach_ptr(PartCand, pc, part_candidates(c, list_nth_node(RelOptInfo, leaves, i)))
				if (OidIsValid(pc->indexoid) == OidIsValid(f->indexoid) && sift_aggpart_same_layout(f->lay, pc->lay) && part_preagg_common(&common, pc->pa))
				{
					m = pc;
					break;
				}
			picked = m ? lappend(picked, m) : NIL;
		}
		if (picked == NIL)
			continue;
		c->nparts = n;
		c->partheap = palloc(n * sizeof(Oid));
		c->partindex = palloc(n * sizeof(Oid));
		memset(&c->stats, 0, sizeof(c->stats));
		for (int i = 0; i < n; i++)
		{
			RelOptInfo *leaf = list_nth_node(RelOptInfo, leaves, i);
			PartCand   *pc = list_nth(picked, i);

			c->partheap[i] = planner_rt_fetch(leaf->relid, c->root)->relid;
			c->partindex[i] = pc->indexoid;
			c->part_pages += leaf->pages;
			sift_aggpart_add_stats(&c->stats, pc->stats);
		}
		c->lay = f->lay;
		c->pa = common;
		c->indexoid = f->indexoid;
		return true;
	}
	return agg_refuse_on(PARTITION_SOURCE, "a partition has no column index the plan can use, or the partitions do not share a layout");
}
