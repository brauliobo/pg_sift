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

		if (!sift_colsrc_can_part(ii->relam) || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
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
	if (n == 0 || n > SIFT_AGG_MAX_PARTS || !part_prune_clauses(c))
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

/* Opens the source of the rel and its layout, the agg_index_pick-th of its column indexes (agg_index_usable: how many there are); false if none */
bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;
	int			n = 0;

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
		if (sift_colsrc_is_index(ii->relam) && !ii->hypothetical && (ii->indpred == NIL || ii->predOK) && n++ == agg_index_pick)
			*ixout = ii;
	agg_index_usable = Max(agg_index_usable, n);
	if (*ixout == NULL)
		return false;
	rel = index_open((*ixout)->indexoid, AccessShareLock);
	c->lay = sift_colsrc_layout(rel);
	sift_colsrc_stats(rel, &c->stats);
	sift_colsrc_preagg(rel, &c->pa);
	index_close(rel, AccessShareLock);
	c->indexoid = (*ixout)->indexoid;
	return true;
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

static Oid member_recheck(AggCtx *c, const SiftAggSpec *s, int j);
static bool recordset_recheck_ok(AggCtx *c, int grp);
static Bitmapset *recordset_columns(AggCtx *c, int grp);
static List *recordset_funcs(AggCtx *c, int grp);
static Bitmapset *function_columns(AggCtx *c, int grp, RangeTblFunction *f);
static Bitmapset *recordset_groups(AggCtx *c);

void
finish_leaves(AggCtx *c, SiftAggSpec *s)
{
	int			n = 0,
				m = -1;

	/* the columns of jsonb_to_recordset that nothing reads but a value they cannot store makes it raise: the source reads those rows again */
	for (int g = -1; (g = bms_next_member(recordset_groups(c), g)) >= 0;)
		if (recordset_recheck_ok(c, g))
		{
			Bitmapset  *cols = recordset_columns(c, g);

			for (m = -1; (m = bms_next_member(cols, m)) >= 0;)
				if (c->stats.ovf[m])
				{
					c->used = bms_add_member(c->used, m);
					c->rec_ok = bms_add_member(c->rec_ok, m);
				}
		}
	m = -1;
	s->leaves = palloc(Max(bms_num_members(c->used), 1) * sizeof(int));
	while ((m = bms_next_member(c->used, m)) >= 0)
		s->leaves[n++] = m;
	s->nleaves = n;
	qsort(s->leaves, n, sizeof(int), cmp_int);
	s->recheck = palloc0(Max(n, 1));
	for (int i = 0; i < n; i++)
		s->recheck[i] = s->leaves[i] < c->lay->nleaves ? spec_recheck(c, s, s->leaves[i]) : SRK_NONE;
	for (int j = 0; j < s->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && OidIsValid(s->vleaf[j].cast = member_recheck(c, s, j)))
			s->vleaf[j].recheck = SRK_CAST;
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
recordset_recheck_ok(AggCtx *c, int grp)
{
	int			m = -1;

	if (!bms_is_member(grp, recordset_groups(c)) || c->search || bms_is_member(grp, c->rec_bad) ||
		!OidIsValid(c->indexoid) || !sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return false;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].obj == grp)	/* the length of the list: a row that raises has no length */
			return false;
	/* the error of a row is the list's, whichever call reads it: calls that declare other columns raise for other values */
	foreach_ptr(RangeTblFunction, f, recordset_funcs(c, grp))
		if (!bms_equal(function_columns(c, grp, f), function_columns(c, grp, linitial(recordset_funcs(c, grp)))))
			return false;
	while ((m = bms_next_member(c->used, m)) >= 0)
		if (leaf_of(c, m)->parent == grp && (!bms_is_member(m, c->rec_ok) || bms_is_member(m, c->rec_bad) || !text_io_leaf(leaf_of(c, m))))
			return false;
	return true;
}

/* the lists a jsonb_to_recordset unnests: the one of the FROM, and those the EXISTS tests of the query read */
static Bitmapset *
recordset_groups(AggCtx *c)
{
	Bitmapset  *groups = NULL;

	if (c->func_kind == FK_RECORDSET && c->group >= 0)
		groups = bms_add_member(groups, c->group);
	foreach_ptr(RecFunc, rf, c->rec_funcs)
		groups = bms_add_member(groups, rf->grp);
	return groups;
}

/* the calls of jsonb_to_recordset that unnest list grp: the FROM's, and those of the EXISTS tests */
static List *
recordset_funcs(AggCtx *c, int grp)
{
	List	   *funcs = NIL;

	if (c->func_kind == FK_RECORDSET && grp == c->group)
		funcs = list_make1(c->func);
	foreach_ptr(RecFunc, rf, c->rec_funcs)
		if (rf->grp == grp)
			funcs = lappend(funcs, rf->f);
	return funcs;
}

/* the columns one call declares that have a leaf in the index */
static Bitmapset *
function_columns(AggCtx *c, int grp, RangeTblFunction *f)
{
	Bitmapset  *cols = NULL;

	for (int i = 0; i < list_length(f->funccolnames); i++)
	{
		int			leaf = child_leaf(c, grp, strVal(list_nth(f->funccolnames, i)), list_nth_oid(f->funccoltypes, i));

		if (leaf >= 0 && text_io_leaf(leaf_of(c, leaf)))
			cols = bms_add_member(cols, leaf);
	}
	return cols;
}

/*
 * The columns the function declares that have a leaf in the index: it reads all of them for every element, whether the query does or not, and
 * raises for a value its type does not take.
 */
static Bitmapset *
recordset_columns(AggCtx *c, int grp)
{
	Bitmapset  *cols = NULL;

	foreach_ptr(RangeTblFunction, f, recordset_funcs(c, grp))
		cols = bms_join(cols, function_columns(c, grp, f));
	return cols;
}

/* does the expression evaluate the cast of a member's jsonb value to float8 for every row that reaches it? A CASE branch, a FILTER, the later operands of AND, OR and COALESCE do for some only */
static bool
unconditional_cast(Node *n, AggCtx *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) == c->cast_want)
	{
		Node	   *arg = strip_relabel(linitial(((FuncExpr *) n)->args));

		return IsA(arg, Var) && ((Var *) arg)->varno == c->func_rti && ((Var *) arg)->varattno == 2 && ((Var *) arg)->varlevelsup == 0;
	}
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		if (a->aggfilter != NULL)
			return false;
		foreach_ptr(Node, x, list_concat_copy(a->aggdirectargs, a->args))
			if (unconditional_cast(IsA(x, TargetEntry) ? (Node *) ((TargetEntry *) x)->expr : x, c))
				return true;
		return false;
	}
	if (IsA(n, CaseExpr))
		return unconditional_cast((Node *) ((CaseExpr *) n)->arg, c);
	if (IsA(n, CoalesceExpr))
		return unconditional_cast(linitial(((CoalesceExpr *) n)->args), c);
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop != NOT_EXPR)
		return unconditional_cast(linitial(((BoolExpr *) n)->args), c);
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		return false;
	return expression_tree_walker(n, unconditional_cast, c);
}

/* can leaf m, read only through the cast of a jsonb_each member's value to float8, be read again from the heap, so that a row with a value of another type raises where PostgreSQL's cast does? */
static bool
cast_recheck_ok(AggCtx *c, int m)
{
	Query	   *q = c->root->parse;

	c->cast_want = FLOAT8OID;
	/* a leaf of the index, not the member of one key (a virtual leaf): the source reads the rows with a bad value in any member again, and the error is the row's, not the key's */
	return c->func_kind == FK_EACH && m < c->lay->nleaves - c->nvleaf && bms_is_member(m, c->cast_ok) && !bms_is_member(m, c->cast_bad) && leaf_of(c, m)->typid == FLOAT8OID &&
		!c->search && OidIsValid(c->indexoid) && sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true) &&
		(unconditional_cast((Node *) q->targetList, c) || unconditional_cast(q->havingQual, c));
}

/* does a conjunct of the rows read the leaf, directly or in an expression of a combo? */
static bool
pred_reads_leaf(AggCtx *c, const SiftAggPred *p, int leaf)
{
	if (p->kind == SAP_COMBO)
	{
		const ComboCtx *cb = list_nth(c->combos, p->leaf);

		for (int k = 0; k < cb->nl; k++)
			if (cb->leaf[k] == leaf)
				return true;
		return false;
	}
	if ((p->leaf >= 0 && p->leaf == leaf) || (p->leaf2 >= 0 && p->leaf2 == leaf))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_leaf(c, p->child[i], leaf))
			return true;
	return false;
}

typedef struct MemberCasts
{
	AggCtx	   *c;
	Oid			to;				/* the type the value of the member is cast to */
	bool		bad;			/* ... it is cast to two types, read bare, or read where the walk does not see */
} MemberCasts;

static bool
is_member_value(Node *n, AggCtx *c)
{
	return IsA(n, Var) && ((Var *) n)->varno == c->func_rti && ((Var *) n)->varattno == 2;
}

/* the casts of the value column of the jsonb_each of the FROM, in the query and nowhere else: a sub-select could read it where the walk cannot tell */
static bool
member_casts(Node *n, MemberCasts *w)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE)
				w->bad = true;
		return query_tree_walker(q, member_casts, w, QTW_IGNORE_RT_SUBQUERIES);
	}
	if (IsA(n, SubLink) || IsA(n, SubPlan))
	{
		w->bad = true;
		return false;
	}
	if (IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) != InvalidOid && is_member_value(strip_relabel(linitial(((FuncExpr *) n)->args)), w->c))
	{
		Oid			to = json_cast_target(((FuncExpr *) n)->funcid);

		if (w->to != InvalidOid && w->to != to)
			w->bad = true;
		w->to = to;
		return false;
	}
	if (is_member_value(n, w->c))
		w->bad = true;
	return expression_tree_walker(n, member_casts, w);
}

/*
 * Can the member of one key (vleaf j: `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`) be read again from the heap, so that a row whose member of
 * that key holds a value that is no number raises where the cast of PostgreSQL does, and a dirty member of another key raises nothing? The
 * member is read only through casts to one type that every row reaching the aggregates evaluates, no conjunct reads it, and no other read of the
 * value column of the object (all the members) shares the rows read again.
 */
static Oid
member_recheck(AggCtx *c, const SiftAggSpec *s, int j)
{
	int			v = c->lay->nleaves - c->nvleaf + j,
				src = vleaf_source(c, j);
	Query	   *q = c->root->parse;
	MemberCasts w = {c, InvalidOid, false};

	if (!sift_aggvleaf_member(&c->vleaf[j]) || src < 0 || c->func_kind != FK_EACH || c->func_key == NULL || c->vleaf[j].obj != c->func_obj ||
		bms_is_member(src, c->used) || leaf_of(c, v)->typid != FLOAT8OID || c->search || !OidIsValid(c->indexoid) ||
		!sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return InvalidOid;
	/* the integer casts start from the float8 member (jsonb_int_mutator): the message of the error names the type the query casts to */
	if (member_casts((Node *) q, &w) || w.bad || (w.to != FLOAT8OID && w.to != INT4OID && w.to != INT8OID))
		return InvalidOid;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level != 0 || pred_reads_leaf(c, s->pred[i], v))
			return InvalidOid;
	c->cast_want = w.to;
	if (!unconditional_cast((Node *) q->targetList, c) && !unconditional_cast(q->havingQual, c))
		return InvalidOid;
	return w.to;
}

/* can leaf m, a text path read only as `doc ->> 'k'`, be read again from the heap? The text of a value cannot raise: a container where a scalar is, a document of another kind than an object (a key of an array or scalar is NULL) */
static bool
text_recheck_ok(AggCtx *c, int m)
{
	const SiftSrcLeaf *l = leaf_of(c, m);

	return m < c->lay->nleaves - c->nvleaf && bms_is_member(m, c->txt_ok) && !bms_is_member(m, c->txt_bad) && l->kind == SCL_SCALAR && l->parent < 0 &&
		l->path != NIL && l->typid == TEXTOID && !l->wide && !c->search && OidIsValid(c->indexoid) && sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true);
}

/* the list leaf m is, or is a column of; -1 when it is neither */
static int
recordset_group(AggCtx *c, int m)
{
	return bms_is_member(m, recordset_groups(c)) ? m : leaf_of(c, m)->parent;
}

/* How the query reads leaf m when the source holds a value it cannot store (SRK_NONE: it does not, such a value is an error for the scan); docs/COL-DIRTY-UNITS.md */
static SiftSrcRecheck
leaf_recheck(AggCtx *c, int m)
{
	int			grp;

	if (cast_recheck_ok(c, m))
		return SRK_CAST;
	if (text_recheck_ok(c, m))
		return SRK_TEXT;
	grp = recordset_group(c, m);
	if (grp < 0 || !recordset_recheck_ok(c, grp))
		return SRK_NONE;
	if (m == grp)
		return SRK_RECORDSET;
	return bms_is_member(m, c->used) || bms_is_member(m, recordset_columns(c, grp)) ? SRK_TEXT : SRK_NONE;
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

/* does a conjunct read one of the leaves, also through the leaves of a combo (an expression over them), which its `leaf` is the number of? */
static bool
pred_reads_set(AggCtx *c, const SiftAggPred *p, const Bitmapset *leaves)
{
	if (p->kind == SAP_COMBO || p->kind == SAP_ELEM_EXISTS || p->kind == SAP_MATCHED)
	{
		const ComboCtx *cb = list_nth(c->combos, p->leaf);

		for (int k = 0; k < cb->nl; k++)
			if (cb->leaf[k] >= 0 && bms_is_member(cb->leaf[k], leaves))
				return true;
	}
	else if ((p->leaf >= 0 && bms_is_member(p->leaf, leaves)) || (p->leaf2 >= 0 && bms_is_member(p->leaf2, leaves)))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_set(c, p->child[i], leaves))
			return true;
	return false;
}

/* is every read of list grp a test of the joined tuples (SAP_ELEM_EXISTS), which raises the error of a row it reaches itself? */
static bool
list_read_by_tuple_tests(const SiftAggSpec *s, int grp)
{
	bool		found = false;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		if (p->kind == SAP_ELEM_EXISTS && p->leaf2 == grp && p->pass_poison)
			found = true;
		else if (p->kind == SAP_ANY_ELEM && p->leaf == grp)
			return false;
	}
	return found;
}

/* SRK_NONE unless leaf m is read in a way the source can read again, and no conjunct of the rows reads the unnested list or its columns or is applied after the others (it may drop a row that the error would reach first) */
SiftSrcRecheck
spec_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	Bitmapset  *list = NULL;
	int			grp;

	if (leaf_recheck(c, m) == SRK_NONE)
		return SRK_NONE;
	/*
	 * the row raises where it reaches the aggregate: no conjunct reads the leaf, nor the member of a key taken from it (the cast of the conjunct would
	 * raise before the aggregates, with the message of its own type, which the row read again does not know), and none of the members is dropped before it
	 */
	if (leaf_recheck(c, m) == SRK_CAST)
	{
		Bitmapset  *read = bms_make_singleton(m);

		for (int j = 0; j < c->nvleaf; j++)
			if (vleaf_source(c, j) == m)
				read = bms_add_member(read, c->lay->nleaves - c->nvleaf + j);
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->level != 0 || pred_reads_set(c, s->pred[i], read))
				return SRK_NONE;
		return SRK_CAST;
	}
	/* no error to raise, so no order to keep: the rows read again give the text any conjunct and key reads */
	if (leaf_of(c, m)->parent < 0 && leaf_recheck(c, m) == SRK_TEXT)
		return SRK_TEXT;
	grp = recordset_group(c, m);
	list = bms_add_member(list, grp);
	for (int i = 0; i < c->lay->nleaves; i++)
		if (leaf_of(c, i)->parent == grp)
			list = bms_add_member(list, i);
	/* an EXISTS over the list passes the row that raises: the other conjuncts decide whether it reaches the error */
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 0 && (s->pred[i]->late || (pred_reads_any(s->pred[i], list) && !(s->pred[i]->pass_poison && s->pred[i]->leaf == grp))))
			return SRK_NONE;
	if (m == grp && grp != c->group && list_read_by_tuple_tests(s, grp))
		return SRK_RECORDSET_LAZY;
	return leaf_recheck(c, m);
}
