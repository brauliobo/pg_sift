/*
 * agg/plan_from.c - the FROM of the query and the sources of partitioned tables
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- the query ---------- */

/* the table of a join's right side: a table, or a pulled up sub-select over one (a FromExpr of one table with a WHERE) */
static RangeTblRef *
right_table(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? (RangeTblRef *) n : NULL;
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
			   *r = IsA(j->rarg, RangeTblRef) ? (RangeTblRef *) j->rarg : NULL;
	FromScan	right = {.ok = true};
	LeftJoin   *lj;
	Index		scanned,
				dim;

	if ((!full && j->jointype != JOIN_LEFT) || l == NULL || (full && r == NULL))
		return false;
	f->flippable = true;
	if (!full && !f->flip)
		return false;
	if (f->flip)
	{
		scan_from(j->rarg, &right);
		if (!right.ok || right.ojrels != NULL || right.rtis == NIL)
			return false;
	}
	scanned = f->flip ? linitial_int(right.rtis) : l->rtindex;
	dim = f->flip ? l->rtindex : r->rtindex;
	lj = palloc(sizeof(LeftJoin));
	lj->kind = full ? SJ_FULL : SJ_RIGHT;
	lj->rti = dim;
	lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
	/* Filter the nullable input before matching; these conditions never test its preserved NULL rows. */
	f->scan_quals = list_concat(f->scan_quals, right.quals);
	f->lefts = list_concat(f->lefts, right.lefts);
	f->rtis = f->flip ? list_concat(f->rtis, right.rtis) : lappend_int(f->rtis, scanned);
	f->rtis = lappend_int(f->rtis, dim);
	f->lefts = lappend(f->lefts, lj);
	f->ojrels = bms_add_member(f->ojrels, j->rtindex);
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
	else if (IsA(n, JoinExpr) && (((JoinExpr *) n)->jointype == JOIN_LEFT || ((JoinExpr *) n)->jointype == JOIN_SEMI ||
								  ((JoinExpr *) n)->jointype == JOIN_ANTI) && right_table(((JoinExpr *) n)->rarg) != NULL)
	{
		JoinExpr   *j = (JoinExpr *) n;
		LeftJoin   *lj = palloc(sizeof(LeftJoin));
		RangeTblRef *r = right_table(j->rarg);

		scan_from(j->larg, f);
		lj->kind = j->jointype == JOIN_LEFT ? SJ_LEFT : j->jointype == JOIN_SEMI ? SJ_SEMI : SJ_ANTI;
		lj->rti = r->rtindex;
		lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
		/* the WHERE of a pulled up sub-select that is the right side filters the table's rows before the join: more of the ON clause */
		if (IsA(j->rarg, FromExpr) && ((FromExpr *) j->rarg)->quals)
			lj->quals = list_concat(lj->quals, list_copy((List *) ((FromExpr *) j->rarg)->quals));
		f->rtis = lappend_int(f->rtis, lj->rti);
		f->lefts = lappend(f->lefts, lj);
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

FuncKind
func_kind_of(RangeTblFunction *f)
{
	FuncExpr   *fe;

	if (!IsA(f->funcexpr, FuncExpr))
		return FK_NONE;
	fe = (FuncExpr *) f->funcexpr;
	switch (fe->funcid)
	{
		case F_JSONB_TO_RECORDSET:
			return FK_RECORDSET;
		case F_JSONB_ARRAY_ELEMENTS:
			return FK_ELEMENTS;
		case F_JSONB_ARRAY_ELEMENTS_TEXT:
			return FK_ELEMENTS_TEXT;
		case F_JSONB_EACH:
			return FK_EACH;
		case F_JSONB_EACH_TEXT:
			return FK_EACH_TEXT;
		default:
			return FK_NONE;
	}
}

/* the kind of group leaf a set-returning function reads */
SiftSrcLeafKind
func_leaf_kind(FuncKind k)
{
	return k == FK_EACH || k == FK_EACH_TEXT ? SCL_OBJECT : SCL_ARRAY;
}

/* the group leaf of the given kind the keys below an expression lead to: the jsonb array (object) leaf an unnested function reads */
int
group_leaf_below(AggCtx *c, Node *arg, List *keys, SiftSrcLeafKind kind)
{
	JsonRef		r = {0};

	if (!json_access(c, arg, &r) || r.as_text || r.att == 0)
		return -1;
	r.path = list_concat_copy(r.path, keys);
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && paths_equal(l->path, r.path) && l->kind == kind)
			return i;
	}
	return -1;
}

int
group_leaf_of(AggCtx *c, Node *arg, SiftSrcLeafKind kind)
{
	return group_leaf_below(c, arg, NIL, kind);
}

/* ---------- partitioned tables ---------- */

/* a column index of a partition: its layout in the terms of the partitioned table's columns, its statistics and pre-aggregates */
typedef struct PartCand
{
	Oid			indexoid;
	SiftSrcLayout *lay;
	SiftSrcStats stats;
	SiftSrcPreagg pa;
} PartCand;

typedef struct PartVars
{
	PlannerInfo *root;
	Index		rti;			/* the partition */
	Index		top;			/* the partitioned table */
	bool		ok;
} PartVars;

/* the attribute of the parent that has the partition's attribute att (0: none) */
static AttrNumber
parent_attno(AppendRelInfo *ai, AttrNumber att)
{
	foreach_ptr(Node, tv, ai->translated_vars)
		if (tv != NULL && IsA(tv, Var) && ((Var *) tv)->varattno == att)
			return foreach_current_index(tv) + 1;
	return 0;
}

/* A column of a partition as the column of the partitioned table, through every level (the attribute numbers differ after a dropped column) */
static Node *
part_vars_mutator(Node *n, PartVars *pv)
{
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, part_vars_mutator, pv);
	v = copyObject((Var *) n);
	for (Index r = pv->rti; r != pv->top && v->varattno > 0; r = pv->root->append_rel_array[r]->parent_relid)
		v->varattno = parent_attno(pv->root->append_rel_array[r], v->varattno);
	pv->ok &= v->varattno > 0;
	return (Node *) v;
}

/* The candidates of a leaf partition: its sift_col indexes, with the layouts in the terms of the partitioned table; none when it cannot be read */
static List *
part_candidates(AggCtx *c, RelOptInfo *leaf)
{
	RangeTblEntry *rte = planner_rt_fetch(leaf->relid, c->root);
	List	   *cands = NIL;

	if (rte->relkind != RELKIND_RELATION || get_rel_relam(rte->relid) != HEAP_TABLE_AM_OID || rte->securityQuals != NIL)
		return NIL;
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
	{
		PartCand   *pc;
		PartVars	pv = {c->root, leaf->relid, c->relid, true};
		Relation	rel;
		List	   *exprs = NIL;

		if (!sift_colsrc_can_part(ii->relam) || ii->hypothetical || ii->indpred != NIL)
			continue;
		pc = palloc0(sizeof(PartCand));
		pc->indexoid = ii->indexoid;
		rel = index_open(ii->indexoid, AccessShareLock);
		pc->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &pc->stats);
		sift_colsrc_preagg(rel, &pc->pa);
		index_close(rel, AccessShareLock);
		foreach_ptr(Node, e, pc->lay->attexprs)
			exprs = lappend(exprs, part_vars_mutator(e, &pv));
		pc->lay->attexprs = exprs;
		if (pv.ok)
			cands = lappend(cands, pc);
	}
	return cands;
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

/* the names of the columns the partitions are split by, at every level */
static void
part_key_names(AggCtx *c, RelOptInfo *rel, List **names)
{
	Oid			relid = planner_rt_fetch(rel->relid, c->root)->relid;

	for (int k = 0; k < rel->part_scheme->partnatts; k++)
		foreach_node(Var, v, pull_var_clause((Node *) rel->partexprs[k], 0))
			*names = list_append_unique(*names, makeString(get_attname(relid, v->varattno, false)));
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
		if (rel->part_rels[i]->part_scheme != NULL)
			part_key_names(c, rel->part_rels[i], names);
}

/*
 * A parameter in a condition on a column the partitions are split by prunes them when the plan runs, which the scan does not do (it reads every
 * partition PostgreSQL's planning kept, and applies the condition to the rows): the normal plan keeps that pruning.
 */
static bool
part_runtime_pruning(AggCtx *c)
{
	List	   *names = NIL;

	part_key_names(c, c->rel, &names);
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			foreach_node(Var, v, pull_var_clause((Node *) ri->clause, 0))
				if (list_member(names, makeString(get_attname(c->heapoid, v->varattno, false))))
					return true;
	return false;
}

/*
 * The source of a partitioned table: the live partitions after PostgreSQL's pruning, each read through a sift_col index of the same layout, the
 * columns in the terms of the table (the query is compiled once, against them). A partition without such an index, of another storage, or whose
 * index differs from the others' keeps the normal plan, and so does a partitioned table whose partitions are pruned when the plan runs.
 */
static bool
part_source(AggCtx *c)
{
	List	   *leaves = NIL,
			   *first;
	int			n;

	if (c->rel->part_scheme == NULL || c->rel->nparts == 0)
		return false;
	part_leaves(c->rel, &leaves);
	n = list_length(leaves);
	if (n == 0 || n > SIFT_AGG_MAX_PARTS || part_runtime_pruning(c))
		return false;
	first = part_candidates(c, linitial_node(RelOptInfo, leaves));
	foreach_ptr(PartCand, f, first)
	{
		List	   *picked = list_make1(f);

		for (int i = 1; i < n && picked != NIL; i++)
		{
			PartCand   *m = NULL;

			foreach_ptr(PartCand, pc, part_candidates(c, list_nth_node(RelOptInfo, leaves, i)))
				if (sift_aggpart_same_layout(f->lay, pc->lay) && memcmp(&f->pa, &pc->pa, sizeof(f->pa)) == 0)
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
			sift_aggpart_add_stats(&c->stats, &pc->stats);
		}
		c->lay = f->lay;
		c->pa = f->pa;
		c->indexoid = f->indexoid;
		return true;
	}
	return false;
}

/* Opens the source of the rel and its layout; false if none */
bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;

	*ixout = NULL;
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return part_source(c);
	if (sift_colsrc_is_table(get_rel_relam(rte->relid)))
	{
		rel = table_open(rte->relid, NoLock);
		c->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &c->stats);
		sift_colsrc_preagg(rel, &c->pa);
		table_close(rel, NoLock);
		c->indexoid = InvalidOid;
		return true;
	}
	foreach_node(IndexOptInfo, ii, c->rel->indexlist)
	{
		if (!sift_colsrc_is_index(ii->relam) || ii->hypothetical || ii->indpred != NIL)
			continue;
		rel = index_open(ii->indexoid, AccessShareLock);
		c->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &c->stats);
		sift_colsrc_preagg(rel, &c->pa);
		index_close(rel, AccessShareLock);
		c->indexoid = ii->indexoid;
		*ixout = ii;
		return true;
	}
	return false;
}

static List *
vars_of_rel(AggCtx *c, List *exprs)
{
	List	   *out = NIL;

	foreach_ptr(Node, e, exprs)
	{
		Node	   *x = e ? copyObject(e) : NULL;

		if (x)
			ChangeVarNodes(x, 1, c->relid, 0);
		out = lappend(out, x);
	}
	return out;
}

void
build_attexprs(AggCtx *c)
{
	c->attexpr = vars_of_rel(c, c->lay->attexprs);
	c->attalias = vars_of_rel(c, c->lay->attalias);
}

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

static bool recordset_recheck_ok(AggCtx *c);
static Bitmapset *recordset_columns(AggCtx *c);

void
finish_leaves(AggCtx *c, SiftAggSpec *s)
{
	int			n = 0,
				m = -1;

	/* the columns of jsonb_to_recordset that nothing reads but a value they cannot store makes it raise: the source reads those rows again */
	if (recordset_recheck_ok(c))
	{
		Bitmapset  *cols = recordset_columns(c);

		while ((m = bms_next_member(cols, m)) >= 0)
			if (c->stats.ovf[m])
			{
				c->used = bms_add_member(c->used, m);
				c->rec_ok = bms_add_member(c->rec_ok, m);
			}
		m = -1;
	}
	s->leaves = palloc(Max(bms_num_members(c->used), 1) * sizeof(int));
	while ((m = bms_next_member(c->used, m)) >= 0)
		s->leaves[n++] = m;
	s->nleaves = n;
	qsort(s->leaves, n, sizeof(int), cmp_int);
	s->recheck = palloc0(Max(n, 1));
	for (int i = 0; i < n; i++)
		s->recheck[i] = s->leaves[i] < c->lay->nleaves ? spec_recheck(c, s, s->leaves[i]) : SRK_NONE;
}

/* can SQL read a leaf of this type from the text of a scalar through its input function (the leaf types of the shredder)? */
static bool
text_io_leaf(const SiftSrcLeaf *l)
{
	if (l->wide || l->kind != SCL_SCALAR)
		return false;
	switch (l->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
			return true;
		default:
			return false;
	}
}

/*
 * Does jsonb_to_recordset read the unnested list alone: nothing else reads it or one of its columns, and each column the query reads is read as
 * a column of the record (and has a type SQL reads from the text of a scalar)? Then a row whose document holds a value the leaves cannot store is
 * read again by the source as the function reads it.
 */
static bool
recordset_recheck_ok(AggCtx *c)
{
	int			m = -1;

	if (c->func_kind != FK_RECORDSET || c->group < 0 || c->search || bms_is_member(c->group, c->rec_bad) ||
		!OidIsValid(c->indexoid) || !sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return false;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].obj == c->group)	/* the length of the list: a row that raises has no length */
			return false;
	while ((m = bms_next_member(c->used, m)) >= 0)
		if (leaf_of(c, m)->parent == c->group && (!bms_is_member(m, c->rec_ok) || bms_is_member(m, c->rec_bad) || !text_io_leaf(leaf_of(c, m))))
			return false;
	return true;
}

/*
 * The columns the function declares that have a leaf in the index: it reads all of them for every element, whether the query does or not, and
 * raises for a value its type does not take.
 */
static Bitmapset *
recordset_columns(AggCtx *c)
{
	Bitmapset  *cols = NULL;
	RangeTblFunction *f = c->func;

	for (int i = 0; i < list_length(f->funccolnames); i++)
	{
		int			leaf = child_leaf(c, c->group, strVal(list_nth(f->funccolnames, i)), list_nth_oid(f->funccoltypes, i));

		if (leaf >= 0 && text_io_leaf(leaf_of(c, leaf)))
			cols = bms_add_member(cols, leaf);
	}
	return cols;
}

/* How the query reads leaf m when the source holds a value it cannot store (SRK_NONE: it does not, such a value is an error for the scan) */
static SiftSrcRecheck
leaf_recheck(AggCtx *c, int m)
{
	if (!recordset_recheck_ok(c))
		return SRK_NONE;
	if (m == c->group)
		return SRK_RECORDSET;
	return leaf_of(c, m)->parent == c->group && (bms_is_member(m, c->used) || bms_is_member(m, recordset_columns(c))) ? SRK_TEXT : SRK_NONE;
}

/* does a conjunct of the rows read one of the leaves? Those rows would be dropped before the error of the list they hold is raised */
static bool
pred_reads_any(const SiftAggPred *p, const Bitmapset *leaves)
{
	if (p->leaf >= 0 && bms_is_member(p->leaf, leaves))
		return true;
	if (p->leaf2 >= 0 && bms_is_member(p->leaf2, leaves))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_any(p->child[i], leaves))
			return true;
	return false;
}

/* SRK_NONE unless leaf m is read in a way the source can read again, and no conjunct of the rows reads the unnested list or its columns or is applied after the others (it may drop a row that the error would reach first) */
SiftSrcRecheck
spec_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	Bitmapset  *list = NULL;

	if (leaf_recheck(c, m) == SRK_NONE)
		return SRK_NONE;
	list = bms_add_member(list, c->group);
	for (int i = 0; i < c->lay->nleaves; i++)
		if (leaf_of(c, i)->parent == c->group)
			list = bms_add_member(list, i);
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 0 && (s->pred[i]->late || pred_reads_any(s->pred[i], list)))
			return SRK_NONE;
	return leaf_recheck(c, m);
}
