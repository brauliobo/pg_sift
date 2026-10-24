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
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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
#include "core/limits.h"

/* the table of a join's right side: a table, or a pulled up sub-select over one (a FromExpr of one table with a WHERE) */
static RangeTblRef *
right_table(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? (RangeTblRef *) n : NULL;
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
		/* the WHERE of a pulled up sub-select that is the right side filters the table's rows before the join: more of the ON clause */
		if (IsA(j->rarg, FromExpr) && ((FromExpr *) j->rarg)->quals)
			lj->quals = list_concat(lj->quals, list_copy((List *) ((FromExpr *) j->rarg)->quals));
		lj->tree = NULL;
		f->rtis = lappend_int(f->rtis, lj->rti);
		f->lefts = lappend(f->lefts, lj);
	}
	else if (IsA(n, JoinExpr) && (((JoinExpr *) n)->jointype == JOIN_SEMI || ((JoinExpr *) n)->jointype == JOIN_ANTI) && scan_semi_tree((JoinExpr *) n, f))
		return;
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
	const SiftPartStats *stats;
	const SiftSrcPreagg *pa;
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

/*
 * The layout, statistics and pre-aggregates of a partition's index are read once for a plan, however many arms and CTEs scan the table: the
 * planner hook opens a memo (sift_agg_memo_begin) that its plans share and deletes it when the last of them is made. They are kept for the
 * backend as well, in the context "sift cache partition facts", and read again only when the index's stamp moved (its directory, the pages of its
 * table and those the visibility map says are all-visible) or the catalog entry of the index or of its table was invalidated. The memo gives a plan an
 * index once, so an entry read again is never one that plan holds; the cache is emptied whole, by the next outermost plan, once it has grown past
 * FACTS_CACHE (with the entries read again, whose old facts stay until then).
 */
typedef struct PartMemo
{
	Oid			indexoid;
	PartCand   *pc;
} PartMemo;

typedef struct FactsWatch
{
	Oid			relid;			/* an index or a table the facts depend on */
	uint32		gen;			/* bumped by every invalidation of its catalog entry */
} FactsWatch;

typedef struct PartFacts
{
	Oid			indexoid;
	SiftSrcStamp stamp;
	FactsWatch *index,
			   *table;
	uint32		index_gen,
				table_gen,
				all_gen;
	PartCand	pc;				/* pc.lay NULL: not read (yet) */
} PartFacts;

#define FACTS_CACHE		SIFT_AGG_MAX_PARTS	/* the indexes of the largest scan */

static MemoryContext part_memo_cx;
static HTAB *part_memo;
static MemoryContext facts_cx;
static HTAB *facts;
static HTAB *facts_watch;
static uint32 facts_all;		/* bumped by an invalidation of every relation */
static int	facts_replaced;		/* entries read again since the cache was emptied: their old facts stay until it is */

static void
facts_inval(Datum arg, Oid relid)
{
	FactsWatch *w;

	if (relid == InvalidOid)
		facts_all++;
	else if (facts_watch != NULL && (w = hash_search(facts_watch, &relid, HASH_FIND, NULL)) != NULL)
		w->gen++;
}

static void
facts_empty(void)
{
	if (facts_cx != NULL)
		MemoryContextReset(facts_cx);
	facts = facts_watch = NULL;
	facts_replaced = 0;
}

static FactsWatch *
facts_watch_of(Oid relid)
{
	bool		found;
	FactsWatch *w = hash_search(facts_watch, &relid, HASH_ENTER, &found);

	if (!found)
		w->gen = 0;
	return w;
}

MemoryContext
sift_agg_memo_begin(void)
{
	if (part_memo_cx != NULL)
		return NULL;
	if (facts != NULL && hash_get_num_entries(facts) + facts_replaced >= FACTS_CACHE)
		facts_empty();
	part_memo_cx = AllocSetContextCreate(CurrentMemoryContext, "sift plan memo", ALLOCSET_DEFAULT_SIZES);
	return part_memo_cx;
}

void
sift_agg_memo_end(MemoryContext cx)
{
	if (cx == NULL)
		return;
	part_memo_cx = NULL;
	part_memo = NULL;
	MemoryContextDelete(cx);
}

/* sift.plan_scale: the source has that many times the rows, units and bytes */
static void
stats_scaled(Relation rel, SiftSrcStats *st)
{
	sift_colsrc_stats(rel, st);
	st->nrows *= sift_plan_scale;
	st->nunits *= sift_plan_scale;
	st->bytes *= sift_plan_scale;
}

/* what is read of a partition's index: its layout in the terms of the partition, and the statistics and pre-aggregates, packed, in cx */
static void
read_facts(Relation rel, MemoryContext cx, PartCand *pc)
{
	SiftSrcStats *full = palloc(sizeof(SiftSrcStats));
	SiftSrcPreagg *pa;
	MemoryContext old;

	stats_scaled(rel, full);
	old = MemoryContextSwitchTo(cx);
	pc->indexoid = RelationGetRelid(rel);
	pc->lay = sift_colsrc_layout(rel);
	pc->stats = sift_aggpart_pack_stats(full, pc->lay->nleaves);
	pc->pa = pa = palloc(sizeof(SiftSrcPreagg));
	sift_colsrc_preagg(rel, pa);
	MemoryContextSwitchTo(old);
	pfree(full);
}

static PartCand *
cached_facts(Relation rel)
{
	Oid			indexoid = RelationGetRelid(rel);
	SiftSrcStamp stamp;
	PartFacts  *e;
	PartCand	fresh;
	bool		found;

	if (facts == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(PartFacts)};

		if (facts_cx == NULL)
		{
			facts_cx = AllocSetContextCreate(CacheMemoryContext, "sift cache partition facts", ALLOCSET_DEFAULT_SIZES);
			CacheRegisterRelcacheCallback(facts_inval, (Datum) 0);
		}
		ctl.hcxt = facts_cx;
		facts = hash_create("sift partition facts", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		ctl.entrysize = sizeof(FactsWatch);
		facts_watch = hash_create("sift partition facts watch", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	sift_colsrc_stamp(rel, &stamp);
	e = hash_search(facts, &indexoid, HASH_ENTER, &found);
	if (found && e->pc.lay != NULL && memcmp(&e->stamp, &stamp, sizeof(stamp)) == 0 && e->index->gen == e->index_gen &&
		e->table->gen == e->table_gen && e->all_gen == facts_all)
		return &e->pc;
	if (found)
		facts_replaced++;
	/* the generations as of before the facts are read: an invalidation while they are is seen at the next plan */
	e->index = facts_watch_of(indexoid);
	e->table = facts_watch_of(rel->rd_index->indrelid);
	e->index_gen = e->index->gen;
	e->table_gen = e->table->gen;
	e->all_gen = facts_all;
	e->pc.lay = NULL;
	read_facts(rel, facts_cx, &fresh);
	e->pc = fresh;
	e->stamp = stamp;
	return &e->pc;
}

/* what is read of a partition's index, in the memo when there is one; its layout's expressions are those of the partition and nothing of it is to be changed */
static const PartCand *
part_index_facts(Oid indexoid)
{
	PartMemo   *m = NULL;
	PartCand   *pc;
	Relation	rel;

	if (part_memo_cx != NULL)
	{
		bool		found;

		if (part_memo == NULL)
		{
			HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(PartMemo), .hcxt = part_memo_cx};

			part_memo = hash_create("sift plan memo", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}
		m = hash_search(part_memo, &indexoid, HASH_ENTER, &found);
		if (found)
			return m->pc;
	}
	rel = index_open(indexoid, AccessShareLock);
	if (part_memo_cx != NULL && (facts == NULL || hash_get_num_entries(facts) < FACTS_CACHE))
		pc = cached_facts(rel);
	else
	{
		MemoryContext cx = part_memo_cx ? part_memo_cx : CurrentMemoryContext;

		pc = MemoryContextAllocZero(cx, sizeof(PartCand));
		read_facts(rel, cx, pc);
	}
	index_close(rel, AccessShareLock);
	if (m != NULL)
		m->pc = pc;
	return pc;
}

/* a partition a column index can be read through: a heap table without row security, and an index of the kind that is usable now */
static bool
part_readable(AggCtx *c, RelOptInfo *leaf)
{
	RangeTblEntry *rte = planner_rt_fetch(leaf->relid, c->root);

	return rte->relkind == RELKIND_RELATION && get_rel_relam(rte->relid) == HEAP_TABLE_AM_OID && rte->securityQuals == NIL;
}

static bool
part_index_usable(const IndexOptInfo *ii)
{
	return sift_colsrc_can_part(ii->relam) && !ii->hypothetical && (ii->indpred == NIL || ii->predOK);
}

/* does the partition have a column index at all? Only its list of indexes is looked at, nothing of the index is read */
static bool
part_has_index(AggCtx *c, RelOptInfo *leaf)
{
	if (!part_readable(c, leaf))
		return false;
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
		if (part_index_usable(ii))
			return true;
	return false;
}

/* The candidates of a leaf partition: its sift_col indexes, with the layouts in the terms of the partitioned table; none when it cannot be read */
static List *
part_candidates(AggCtx *c, RelOptInfo *leaf)
{
	List	   *cands = NIL;

	if (!part_readable(c, leaf))
		return NIL;
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
	{
		const PartCand *facts;
		PartCand   *pc;
		PartVars	pv = {c->root, leaf->relid, c->relid, true};
		List	   *exprs = NIL;

		if (!part_index_usable(ii))
			continue;
		facts = part_index_facts(ii->indexoid);
		pc = palloc(sizeof(PartCand));
		*pc = *facts;
		pc->lay = palloc(sizeof(SiftSrcLayout));
		*pc->lay = *facts->lay;
		foreach_ptr(Node, e, facts->lay->attexprs)
			exprs = lappend(exprs, part_vars_mutator(e, &pv));
		pc->lay->attexprs = exprs;
		if (pv.ok)
			cands = lappend(cands, pc);
	}
	return cands;
}

/*
 * Do two partitions write the same pre-aggregates? The key lists of the integer leaves (key_columns = auto) follow the data of each partition, and a
 * unit without one is read by its rows: acc keeps the leaves every partition so far lists, and what else differs is a layout of its own.
 */
static bool
part_preagg_common(SiftSrcPreagg *acc, const SiftSrcPreagg *pa)
{
	SiftSrcPreagg a,
				b;
	int			n = 0;

	memcpy(&a, acc, sizeof(a));
	memcpy(&b, pa, sizeof(b));
	a.npres = b.npres = 0;
	memset(a.pres, 0, sizeof(a.pres));
	memset(b.pres, 0, sizeof(b.pres));
	if (memcmp(&a, &b, sizeof(a)) != 0)
		return false;
	for (int p = 0; p < acc->npres; p++)
		for (int q = 0; q < pa->npres; q++)
			if (acc->pres[p] == pa->pres[q])
				acc->pres[n++] = acc->pres[p];
	memset(acc->pres + n, 0, (acc->npres - n) * sizeof(acc->pres[0]));
	acc->npres = n;
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
static bool
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
				if (sift_aggpart_same_layout(f->lay, pc->lay) && part_preagg_common(&common, pc->pa))
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

/* Opens the source of the rel and its layout, the agg_index_pick-th of its column indexes (agg_index_usable: how many there are); false if none */
bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;
	int			n = 0;

	*ixout = NULL;
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return part_source(c) || agg_refuse(PARTITION_SOURCE);
	if (sift_colsrc_is_table(get_rel_relam(rte->relid)))
	{
		rel = table_open(rte->relid, NoLock);
		c->lay = sift_colsrc_layout(rel);
		stats_scaled(rel, &c->stats);
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
	stats_scaled(rel, &c->stats);
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
	s->gate = palloc0(Max(n, 1) * sizeof(uint64));
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

typedef struct Unconditional
{
	AggCtx	   *c;
	int			leaf;			/* the scalar leaf whose read (leaf_read) is looked for; -1: the cast of a member's jsonb value to c->cast_want */
	bool		as_text;		/* leaf_read: `(doc ->> 'k')::T`, else the cast of the jsonb value `(doc -> 'k')::T` */
	int			nread;			/* count_leaf_reads: the reads it found */
} Unconditional;

/* is the node the read of the scalar leaf: the cast of `doc -> 'k'` or `doc ->> 'k'` to the leaf's type, the path of the leaf? */
static bool
leaf_read(Node *n, const Unconditional *u)
{
	const SiftSrcLeaf *l = leaf_of(u->c, u->leaf);
	JsonRef		r = {0};
	Node	   *arg;

	if (u->as_text && IsA(n, CoerceViaIO))
		arg = (Node *) ((CoerceViaIO *) n)->arg;
	else if (!u->as_text && IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) != InvalidOid)
		arg = linitial(((FuncExpr *) n)->args);
	else
		return false;
	if (exprType(n) != l->typid || !json_access(u->c, arg, &r) || r.as_text != u->as_text)
		return false;
	/* the member of an element of the unnested array: `p ->> 'k'` of jsonb_array_elements */
	if (l->parent >= 0)
		return r.att == 0 && r.fcol == 1 && u->c->func_kind == FK_ELEMENTS && list_length(r.path) == 1 && strcmp(strVal(linitial(r.path)), l->name) == 0;
	return r.att == l->attno && paths_equal(r.path, l->path);
}

/* does the expression evaluate what u looks for, for every row that reaches it? A CASE branch, a FILTER, the later operands of AND, OR and COALESCE do for some only */
static bool
unconditional(Node *n, Unconditional *u)
{
	AggCtx	   *c = u->c;

	if (n == NULL)
		return false;
	if (u->leaf >= 0 && leaf_read(n, u))
		return true;
	if (u->leaf < 0 && IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) == c->cast_want)
	{
		Node	   *arg = strip_relabel(linitial(((FuncExpr *) n)->args));

		return IsA(arg, Var) && ((Var *) arg)->varno == c->func_rti && ((Var *) arg)->varattno == 2 && ((Var *) arg)->varlevelsup == 0;
	}
	if (IsA(n, Aggref))
	{
		Aggref	   *a = (Aggref *) n;

		/* the FILTER is tested for every row that reaches the aggregate; the arguments for the rows it keeps */
		if (unconditional((Node *) a->aggfilter, u))
			return true;
		if (a->aggfilter != NULL)
			return false;
		foreach_ptr(Node, x, list_concat_copy(a->aggdirectargs, a->args))
			if (unconditional(IsA(x, TargetEntry) ? (Node *) ((TargetEntry *) x)->expr : x, u))
				return true;
		return false;
	}
	if (IsA(n, CaseExpr))
		return unconditional((Node *) ((CaseExpr *) n)->arg, u);
	if (IsA(n, CoalesceExpr))
		return unconditional(linitial(((CoalesceExpr *) n)->args), u);
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop != NOT_EXPR)
		return unconditional(linitial(((BoolExpr *) n)->args), u);
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		return false;
	return expression_tree_walker(n, unconditional, u);
}

/* counts the reads of the leaf in the tree, a sub-select's and a function's arguments too */
static bool
count_leaf_reads(Node *n, Unconditional *u)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		/* PostgreSQL 18 keeps the grouping expressions in an entry of the range table too: the same ones the targets have */
		return query_tree_walker((Query *) n, count_leaf_reads, u,
#if PG_VERSION_NUM >= 180000
								 QTW_IGNORE_GROUPEXPRS
#else
								 0
#endif
			);
	if (leaf_read(n, u))
	{
		u->nread++;
		return false;
	}
	return expression_tree_walker(n, count_leaf_reads, u);
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
	bool		mixed;			/* ... it is cast to two types */
	bool		bad;			/* ... or it is read bare, or where the walk does not see (a sub-select), or cast to two types */
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
			w->mixed = w->bad = true;
		w->to = to;
		return false;
	}
	if (is_member_value(n, w->c))
		w->bad = true;
	return expression_tree_walker(n, member_casts, w);
}

/*
 * Can leaf m, read only through the cast of a jsonb_each member's value to float8, be read again from the heap, so that a row with a value of another
 * type raises where PostgreSQL's cast does? The query casts the value to that one type: the error names the type of the cast that raises first, in
 * the order the aggregates evaluate them, which the row read again does not know (the message of another cast would be float8's).
 */
static bool
cast_recheck_ok(AggCtx *c, int m)
{
	Query	   *q = c->root->parse;
	MemberCasts w = {c, InvalidOid, false, false};
	Unconditional u = {c, -1};

	c->cast_want = FLOAT8OID;
	/* a leaf of the index, not the member of one key (a virtual leaf): the source reads the rows with a bad value in any member again, and the error is the row's, not the key's */
	return c->func_kind == FK_EACH && m < c->lay->nleaves - c->nvleaf && leaf_of(c, m)->parent >= 0 && bms_is_member(m, c->cast_ok) && !bms_is_member(m, c->cast_bad) &&
		leaf_of(c, m)->typid == FLOAT8OID && !c->search && OidIsValid(c->indexoid) && sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true) &&
		(unconditional((Node *) q->targetList, &u) || unconditional(q->havingQual, &u)) &&
		!member_casts((Node *) q, &w) && !w.mixed && w.to == FLOAT8OID;
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
	MemberCasts w = {c, InvalidOid, false, false};
	Unconditional u = {c, -1};

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
	if (!unconditional((Node *) q->targetList, &u) && !unconditional(q->havingQual, &u))
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

/* How the query reads leaf m when the source holds a value it cannot store (SRK_NONE: it does not, such a value is an error for the scan) */
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

/* a comparison, IN list or NULL test of the members' leaf alone: PostgreSQL casts the value of every member that reaches it */
static bool
leaf_test(const SiftAggPred *p, int leaf)
{
	return p->level == 1 && !p->late && p->nchild == 0 && p->leaf == leaf && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NULL);
}

/* the types of a scalar path whose rows are read again through the input function of the type (the narrower float and integer types are not tested with the read) */
static bool
scalar_text_type(Oid typid)
{
	return typid == BOOLOID || typid == INT4OID || typid == INT8OID || typid == FLOAT8OID;
}

/* a comparison, IN list, NULL test or boolean expression of the leaf alone, over the rows, that cannot raise by itself: a row the leaf cannot read passes it (pass_poison) */
static bool
scalar_test(const SiftAggPred *p, int leaf)
{
	return p->level == 0 && !p->late && p->nchild == 0 && p->leaf == leaf && p->leaf2 < 0 && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NULL || p->kind == SAP_LUT);
}

/* the leaf is left to the normal plan because of the order of the conditions, which reads_overflow names */
static SiftSrcRecheck
order_refused(AggCtx *c, int m)
{
	c->order_refused = bms_add_member(c->order_refused, m);
	return SRK_NONE;
}

/*
 * Does PostgreSQL test every condition of the WHERE that does not read the leaf (u) before each one that does? A scan sorts its conditions by their
 * cost, the written order among equals (order_qual_clauses), and stops at the first that drops the row, so a row another condition drops never
 * reaches the cast of the leaf, however the plan reaches the table. The scan raises for the rows that every other condition keeps, so the two raise
 * for the same rows exactly when the others come first.
 */
static bool
others_first(AggCtx *c, const Unconditional *u)
{
	Node	   *quals = (Node *) c->root->parse->jointree->quals;
	List	   *conds = quals == NULL ? NIL : IsA(quals, List) ? (List *) quals : list_make1(quals);
	int			n = list_length(conds);
	double	   *cost = palloc(Max(n, 1) * sizeof(double));
	bool	   *reads = palloc(Max(n, 1) * sizeof(bool));

	for (int i = 0; i < n; i++)
	{
		Unconditional t = *u;
		QualCost	qc;

		t.nread = 0;
		count_leaf_reads(list_nth(conds, i), &t);
		reads[i] = t.nread > 0;
		cost_qual_eval_node(&qc, list_nth(conds, i), c->root);
		cost[i] = qc.per_tuple;
	}
	for (int o = 0; o < n; o++)
		for (int r = 0; r < n; r++)
			if (!reads[o] && reads[r] && !(cost[o] < cost[r] || (cost[o] == cost[r] && o < r)))
				return false;
	return true;
}

/* counts the reads of the leaf in the arguments of the aggregates with a FILTER that does not read it: the aggregate evaluates them for the rows the FILTER keeps */
static bool
filtered_reads(Node *n, Unconditional *u)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref) && ((Aggref *) n)->aggfilter != NULL)
	{
		Aggref	   *a = (Aggref *) n;
		Unconditional t = *u;

		t.nread = 0;
		count_leaf_reads((Node *) a->aggfilter, &t);
		if (t.nread == 0)
		{
			count_leaf_reads((Node *) a->args, u);
			count_leaf_reads((Node *) a->aggdirectargs, u);
		}
		return false;
	}
	return expression_tree_walker(n, filtered_reads, u);
}

/*
 * The aggregates that read leaf m (bit j: agg[j]), when each has a FILTER that cannot raise and reads no leaf that holds a bad value: its row raises
 * where one of them keeps it. 0: there is one without, or too many aggregates for the bits.
 */
static uint64
gate_mask(AggCtx *c, const SiftAggSpec *s, int m)
{
	uint64		mask = 0;
	Bitmapset  *dirty = NULL;

	if (s->naggs > 64)
		return 0;
	for (int i = 0; i < c->lay->nleaves - c->nvleaf; i++)
		if (c->stats.ovf[i])
			dirty = bms_add_member(dirty, i);
	for (int j = 0; j < s->naggs; j++)
		if (bms_is_member(m, s->agg[j].reads))
		{
			const SiftAggPred *f = s->agg[j].filter;

			if (f == NULL || f->late || f->level != 0 || pred_reads_set(c, f, dirty))
				return 0;
			mask |= UINT64CONST(1) << j;
		}
	return mask;
}

/*
 * Can leaf m, a number or boolean path of the document that the query reads only as `(doc ->> 'k')::T` (SRK_IOCAST) or, for float8, as the
 * cast of `doc -> 'k'` (SRK_CAST), be read again from the heap? A row whose value the leaf cannot store is read as SQL reads it: the text of the
 * scalar through the input function of T, or the number of the jsonb value, and the error SQL raises for another value is raised where the row
 * reaches the aggregate (recheck_late), which is where PostgreSQL evaluates the argument of an aggregate or a grouping expression. So every
 * read of the leaf is that one cast, in a key or an aggregate that every row reaching it evaluates, and nothing the rows go through before the
 * aggregates may drop a row after PostgreSQL has evaluated the cast, which a function in the FROM or a join could.
 *
 * A condition of the WHERE may read it too, as a comparison, IN list or NULL test of the cast: a row of an unreadable value passes it and goes on
 * to the other conditions, and raises only if they keep it. That is what PostgreSQL does when it tests the others first (others_first); a plan
 * that tests the cast before a condition that drops the row raises for it. A condition that may raise itself (late) is left to the normal plan.
 * The rows the condition keeps have a readable value, so the aggregates may read the leaf in any way.
 */
static SiftSrcRecheck
scalar_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	const SiftSrcLeaf *l;
	Query	   *q = c->root->parse;
	Unconditional u = {c, m};
	Unconditional all;
	SiftSrcRecheck kind;
	Bitmapset  *read;
	bool		tested = false,
				filtered = false;
	uint64		gate = 0;

	if (m >= c->lay->nleaves - c->nvleaf || (c->group >= 0) != (c->func_kind == FK_ELEMENTS) || (c->func_rti != 0) != (c->group >= 0) || c->joins != NIL ||
		c->search || !OidIsValid(c->indexoid) || !sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return SRK_NONE;
	l = leaf_of(c, m);
	if (l->kind != SCL_SCALAR || l->parent != c->group || (c->group < 0 && l->path == NIL) || l->wide)
		return SRK_NONE;
	if (bms_is_member(m, c->cast_ok) && !bms_is_member(m, c->cast_bad) && l->typid == FLOAT8OID)
		kind = SRK_CAST;
	else if (bms_is_member(m, c->io_ok) && !bms_is_member(m, c->io_bad) && scalar_text_type(l->typid))
	{
		kind = SRK_IOCAST;
		u.as_text = true;
	}
	else
		return SRK_NONE;
	/* the error of an element is kept for its row, not for the element: a condition on the elements may drop the one that raises and keep another */
	for (int i = 0; c->group >= 0 && i < s->npred; i++)
		if (s->pred[i]->level != 0)
		{
			c->dirty_leaf = m;
			c->dirty_why = "a condition on the elements of the list may drop the element whose value raises: the error of an element is kept for its row";
			return SRK_NONE;
		}
	read = bms_make_singleton(m);
	for (int i = 0; i < s->npred; i++)
		if (pred_reads_set(c, s->pred[i], read))
		{
			if (!scalar_test(s->pred[i], m))
			{
				c->dirty_leaf = m;
				c->dirty_why = "a condition on the cast is none of a comparison with a constant, an IN list of integers and a NULL test (an OR, an expression of it, a boolean)";
				return SRK_NONE;
			}
			tested = true;
		}
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].filter != NULL && pred_reads_set(c, s->agg[j].filter, read))
		{
			if (!scalar_test(s->agg[j].filter, m))
			{
				c->dirty_leaf = m;
				c->dirty_why = "the FILTER of an aggregate tests the cast in a way that is none of a comparison with a constant, an IN list of integers and a NULL test";
				return SRK_NONE;
			}
			filtered = true;
		}
	if (tested)
	{
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->late)
				return order_refused(c, m);
		if (!others_first(c, &u))
			return order_refused(c, m);
	}
	else if (!unconditional((Node *) q->targetList, &u) && !unconditional(q->havingQual, &u))
	{
		/* not every row evaluates it: the rows that the FILTER of every aggregate that reads it keeps do */
		gate = gate_mask(c, s, m);
		if (gate == 0)
		{
			c->dirty_leaf = m;
			c->dirty_why = "the cast runs for some rows only (a CASE branch), or in an aggregate whose FILTER may raise, reads another path that holds a bad value or is missing";
			return SRK_NONE;
		}
	}
	/* every read is in the conditions and the targets (in the arguments of the aggregates with a FILTER when it is gated): the one the walk found is in the same tree as the others */
	all = u;
	all.nread = 0;
	count_leaf_reads((Node *) q, &all);
	u.nread = 0;
	if (gate)
	{
		filtered_reads((Node *) q->targetList, &u);
		filtered_reads(q->havingQual, &u);
	}
	else
	{
		count_leaf_reads((Node *) q->targetList, &u);
		count_leaf_reads(q->havingQual, &u);
		count_leaf_reads(q->jointree->quals, &u);
	}
	if (all.nread != u.nread)
		return SRK_NONE;
	for (int i = 0; gate && i < s->nleaves; i++)
		if (s->leaves[i] == m)
			s->gate[i] = gate;
	for (int i = 0; i < s->npred; i++)
		if (pred_reads_set(c, s->pred[i], read))
			s->pred[i]->pass_poison = true;
	for (int j = 0; filtered && j < s->naggs; j++)
		if (s->agg[j].filter != NULL && pred_reads_set(c, s->agg[j].filter, read))
			s->agg[j].filter->pass_poison = true;
	return kind;
}

/* SRK_NONE unless leaf m is read in a way the source can read again, and no conjunct of the rows reads the unnested list or its columns (it may drop a row that the error would reach first) */
SiftSrcRecheck
spec_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	Bitmapset  *list = NULL;
	int			grp;
	SiftSrcRecheck scalar = scalar_recheck(c, s, m);

	if (scalar != SRK_NONE)
		return scalar;
	if (leaf_recheck(c, m) == SRK_NONE)
		return SRK_NONE;
	/*
	 * the row raises where it reaches the aggregate, none of the members dropped before it. A conjunct that reads the member of a key taken from the
	 * leaf (the cast of the conjunct would raise before the aggregates, with the message of its own type, which the row read again does not know) or
	 * anything but the leaf's own value keeps the normal plan. A conjunct that tests the leaf alone casts every member of a row that reaches the
	 * members, with the cast of the aggregates, and no other test of the members may drop one first: the cast raises before the test (SRK_CAST_TESTED).
	 * Where it runs relative to a join or a conjunct that may raise is the planner's choice, so none of them is in the query then.
	 */
	if (leaf_recheck(c, m) == SRK_CAST)
	{
		Bitmapset  *read = bms_make_singleton(m);
		bool		tested = false;

		for (int j = 0; j < c->nvleaf; j++)
			if (vleaf_source(c, j) == m)
				read = bms_add_member(read, c->lay->nleaves - c->nvleaf + j);
		for (int i = 0; i < s->npred; i++)
			if (pred_reads_set(c, s->pred[i], read))
			{
				if (!leaf_test(s->pred[i], m))
					return SRK_NONE;
				tested = true;
			}
			else if (s->pred[i]->level != 0)
				return SRK_NONE;
		if (!tested)
			return SRK_CAST;
		if (c->joins != NIL)
			return SRK_NONE;
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->late)
				return SRK_NONE;
		return SRK_CAST_TESTED;
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
		if (s->pred[i]->level == 0 && pred_reads_any(s->pred[i], list) && !(s->pred[i]->pass_poison && s->pred[i]->leaf == grp))
			return SRK_NONE;
	if (m == grp && grp != c->group && list_read_by_tuple_tests(s, grp))
		return SRK_RECORDSET_LAZY;
	return leaf_recheck(c, m);
}
