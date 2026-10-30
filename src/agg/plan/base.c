/*
 * agg/plan/base.c - the base of a scan
 * The table and joins the query reads, the keys it groups by, and the spec made from them
 * (agg_base, agg_spec_new).
 */
#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "catalog/partition.h"
#include "utils/partcache.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"

#include "agg/agg.h"
#include "agg/plan/int.h"

/*
 * The leaf of the index that a leaf after the source's takes its values from: the value column of a member of an object, the array whose length it
 * is; -1 for the type of a path (the tags of its leaf say it, whatever the leaf holds). Whatever that leaf cannot store is read as NULL.
 */
int
vleaf_source(AggCtx *c, int j)
{
	const SiftAggVLeaf *v = &c->vleaf[j];

	if (v->count)
		return v->obj;
	return sift_aggvleaf_member(v) ? child_leaf(c, v->obj, "value", leaf_of(c, c->lay->nleaves - c->nvleaf + j)->typid) : -1;
}

/*
 * The FROM list of root's query as AggCtx: the scanned table with a column source, at most one lateral unnest of one of its leaves,
 * the tables that join to it by equality. input_rel (NULL: not the query being planned) is the join the planner made of it.
 */
/*
 * The key of `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`, NULL for any other outer join of the function. An object has one member
 * per key, so the join is the member k of each row (a row without it keeps NULL columns): a row level value, not an unnest. A JSON null and
 * a missing key are the same NULL there; before PostgreSQL 18 the cast of a JSON null raises where the missing key does not, so the plan stays
 * normal while some unit may hold one (reads_json_null_cast).
 */
static const char *
left_each_key(LeftJoin *lj, Index rti, FuncKind kind)
{
	OpExpr	   *op;
	Node	   *a,
			   *b;

	if (kind != FK_EACH || lj->kind != SJ_LEFT || list_length(lj->quals) != 1 || !IsA(linitial(lj->quals), OpExpr))
		return NULL;
	op = linitial_node(OpExpr, lj->quals);
	set_opfuncid(op);
	/* a non-deterministic collation equates keys that differ in bytes (k and K): the join then matches several members */
	if (op->opfuncid != F_TEXTEQ || !get_collation_isdeterministic(op->inputcollid))
		return NULL;
	a = strip_relabel(linitial(op->args));
	b = strip_relabel(lsecond(op->args));
	if (IsA(b, Var))
	{
		Node	   *t = a;

		a = b;
		b = t;
	}
	if (!IsA(a, Var) || ((Var *) a)->varno != rti || ((Var *) a)->varlevelsup != 0 || ((Var *) a)->varattno != 1 || !IsA(b, Const) ||
		((Const *) b)->constisnull)
		return NULL;
	return TextDatumGetCString(((Const *) b)->constvalue);
}

/*
 * LEFT JOIN LATERAL over the list of a row: its own conditions (the lateral's WHERE, the ON clause) say which elements exist, and a row with
 * none is one tuple with NULL element columns. A condition above the join on an element column that PostgreSQL did not turn into an inner join
 * (it is not strict) would see that NULL tuple: the plan stays normal.
 */
bool
left_lateral(AggCtx *c, FromScan *fs, LeftJoin *lj, Index rti)
{
	if (!sift_agg_left_lateral || lj->kind != SJ_LEFT || (c->func_kind != FK_RECORDSET && c->func_kind != FK_ELEMENTS))
		return false;
	foreach_ptr(Node, q, fs->quals)
		if (bms_is_member(rti, pull_varnos(c->root, q)))
			return false;
	fs->quals = list_concat(fs->quals, list_copy(lj->quals));
	return true;
}

/*
 * Does the group key hold every column of the partition key, as the column itself? A row of a group is then in the one partition that holds the
 * value of the key, and the groups of a partition are final when its units end. The partitions are read one after the other, so a partition
 * of a partition would end a group that goes on in the next.
 */
bool
group_has_partition_key(AggCtx *c, List *keys)
{
	Relation	rel;
	PartitionKey pk;
	bool		ok;

	if (c->nparts == 0)
		return false;
	/* even_if_detached: a partition the snapshot still sees may be mid-DETACH CONCURRENTLY, which the call otherwise refuses with an error */
	for (int i = 0; i < c->nparts; i++)
		if (get_partition_parent(c->partheap[i], true) != c->heapoid)
			return false;
	rel = table_open(c->heapoid, NoLock);
	pk = RelationGetPartitionKey(rel);
	ok = pk != NULL && pk->partnatts > 0;
	for (int i = 0; ok && i < pk->partnatts; i++)
	{
		AttrNumber	att = get_partition_col_attnum(pk, i);
		bool		hit = false;

		foreach_ptr(Node, e, keys)
		{
			Node	   *x = strip_relabel(e);

			hit |= att != 0 && IsA(x, Var) && ((Var *) x)->varno == c->relid && ((Var *) x)->varattno == att;
		}
		ok = hit;
	}
	table_close(rel, NoLock);
	return ok;
}

/* is a column of table a equal to a column of table b in the query (an equivalence class holds both)? */
static bool
eq_joined(PlannerInfo *root, int a, int b)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		bool		ina = false,
					inb = false;

		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			ina |= bms_is_member(a, em->em_relids);
			inb |= bms_is_member(b, em->em_relids);
		}
		if (ina && inb)
			return true;
	}
	return false;
}

/*
 * The tables joined to the scan, those joined to the scanned table itself first: a table keyed by a column of another lookup is loaded after the
 * one it takes its keys from (a customer listed before the orders that name it is read whole, row by row, by every row of the scan).
 */
static List *
joined_in_order(PlannerInfo *root, int scanned, List *joined)
{
	List	   *direct = NIL,
			   *rest = NIL;

	foreach_int(rti, joined)
		if (eq_joined(root, scanned, rti))
			direct = lappend_int(direct, rti);
		else
			rest = lappend_int(rest, rti);
	return list_concat(direct, rest);
}

static bool
agg_base_once(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	Query	   *parse = root->parse;
	IndexOptInfo *ii;
	Bitmapset  *allrtis;
	List	   *live_rtis = NIL,
			   *joined = NIL;
	int			nin = 0,
				skip = fs->skip;

	scan_from((Node *) parse->jointree, fs);
	allrtis = bms_copy(fs->hidden);
	if (!fs->ok)
		return agg_refuse_on(FROM_SHAPE, "the FROM tree has a join other than the inner, left, semi, anti or preserved joins of tables the scan reads");
	/*
	 * A semi, anti or preserved join whose table has no relation entry was not removed as a useless LEFT JOIN: PostgreSQL 18 merged it into another
	 * scan of the same table by a unique key (self-join elimination), and moved its conditions to the one it kept, which the FROM tree does not say.
	 */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->kind != SJ_LEFT && root->simple_rel_array[lj->rti] == NULL)
			return agg_refuse_on(FROM_SHAPE, "PostgreSQL merged a joined table into another scan of the same table (self-join elimination)");
	/* Join removal leaves the original FROM tree in place, but removes its base relations. */
	foreach_int(rti, fs->rtis)
		if (root->simple_rel_array[rti] != NULL)
			live_rtis = lappend_int(live_rtis, rti);
	fs->rtis = live_rtis;
	foreach_int(rti, fs->rtis)
		fs->ntables += rt_fetch(rti, parse->rtable)->rtekind == RTE_RELATION;
	c->root = root;
	c->group = -1;
	c->nulling = fs->ojrels;
	foreach_int(rti, fs->rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		allrtis = bms_add_member(allrtis, rti);
		if (e->rtekind == RTE_RELATION)
		{
			bool		part = e->inh && e->relkind == RELKIND_PARTITIONED_TABLE;

			if ((e->inh && !part) || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW && !part))
				return agg_refuse(TABLE_KIND);
			/*
			 * A table with row-level security read through a view of an owner the policies do not apply to: PostgreSQL adds no condition, and the scan,
			 * which checks row-level security for the invoking role, would refuse the query the normal plan answers.
			 */
			if (e->relkind == RELKIND_RELATION && e->perminfoindex > 0 && OidIsValid(getRTEPermissionInfo(parse->rteperminfos, e)->checkAsUser) &&
				check_enable_rls(e->relid, InvalidOid, true) == RLS_ENABLED)
				return agg_refuse_on(TABLE_KIND, "the table has row-level security and is read through a view");
			if (c->relid == 0 && skip == 0)
				c->relid = rti;
			else if (c->joinless)
				return agg_refuse_on(FROM_SHAPE, "a second table of the FROM list is read by a query that joins no table");
			else
			{
				joined = lappend_int(joined, rti);
				skip -= c->relid == 0;
			}
		}
		else if (e->rtekind == RTE_FUNCTION && e->lateral && c->func_rti == 0 && !e->funcordinality && list_length(e->functions) == 1 &&
				 func_kind_of(linitial_node(RangeTblFunction, e->functions)) != FK_NONE &&
				 !(fs->plain_unnest && func_kind_of(linitial_node(RangeTblFunction, e->functions)) == FK_UNNEST))
		{
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
			/* the scan unnests the elements that pass: a row without one is no row of an outer join (it keeps NULL columns) */
			foreach_ptr(LeftJoin, lj, fs->lefts)
				if (lj->rti == rti && (c->func_key = left_each_key(lj, rti, c->func_kind)) == NULL)
				{
					if (!left_lateral(c, fs, lj, rti))
						return agg_refuse(OUTER_JOIN);
					c->func_left = true;
				}
		}
		else if (input_rel == NULL || c->joinless ||
				 (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION && e->rtekind != RTE_CTE))
			return agg_refuse_on(FROM_SHAPE, "a FROM item is not a table, a lateral function the scan unnests, a sub-select, a VALUES list, a function or a CTE");
		else
			joined = lappend_int(joined, rti);
	}
	if (c->relid == 0)
		return agg_refuse_on(FROM_SHAPE, "the FROM list has no table to scan");
	if (!bms_is_subset(fs->nested, bms_make_singleton(c->func_rti)))
		return agg_refuse_on(FROM_SHAPE, "a LEFT JOIN holds a join of tables on its right side");
	/* a plan joined to the scan by an outer join keeps its own rows: the scanned table is never the nullable side */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == c->relid)
			return agg_refuse(OUTER_JOIN);
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	/* the joins come after the source: planning the sub-selects of a query with no column index to scan would be a waste */
	if (c->rel == NULL || !find_source(c, &ii))
		return agg_refuse(NO_SOURCE);
	if (list_length(joined) > 1 && list_length(joined) == list_length(fs->rtis) - 1 && fs->lefts == NIL)
		joined = joined_in_order(root, c->relid, joined);
	c->nsiblings = list_length(joined) + (c->func_rti != 0);
	foreach_int(rti, joined)
		if (!add_join(c, fs, rti))
			return agg_refuse(JOIN_CHILD);
	if (input_rel != NULL)
	{
		if (!bms_is_member(c->relid, input_rel->relids))
			return agg_refuse_on(FROM_SHAPE, "the scanned table is not an input of the aggregate");
		for (int m = -1; (m = bms_next_member(input_rel->relids, m)) >= 0;)
			if (rt_fetch(m, parse->rtable)->rtekind != RTE_JOIN)
			{
				if (!bms_is_member(m, allrtis))
					return agg_refuse_f(FROM_SHAPE, "the aggregate reads the table %s, which the FROM tree does not name", rt_fetch(m, parse->rtable)->eref->aliasname);
				nin++;
			}
		if (nin + list_length(c->joins) < list_length(fs->rtis))
			return agg_refuse_on(FROM_SHAPE, "a table of the FROM tree is not an input of the aggregate");
	}
	/* the rows of a preserved side that nothing matched are known at the end of a single scan of one join */
	if (fs->ojrels && input_rel == NULL)
		return agg_refuse(OUTER_JOIN);
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind) && jc != llast(c->joins))
			return agg_refuse(OUTER_JOIN);
	build_attexprs(c);
	if (c->func_rti)
	{
		Node	   *arg = linitial(((FuncExpr *) c->func->funcexpr)->args);
		int			leaf = c->func_kind == FK_UNNEST ? array_group(c, arg) : group_leaf_of(c, arg, func_leaf_kind(c->func_kind));

		if (leaf < 0)
			return agg_refuse(UNNEST_LEAF);
		if (c->func_key)
			c->func_obj = leaf;
		else
		{
			c->group = leaf;
			c->used = bms_add_member(c->used, leaf);
		}
	}
	return true;
}

/* agg_base_once, and an unnest(array) the index holds no list of read as the function PostgreSQL runs over the rows the scan hands it */
bool
agg_base(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	AggCtx		saved = *c;
	FromScan	fs0 = *fs;

	if (agg_base_once(c, root, input_rel, fs))
		return true;
	if (c->func_kind != FK_UNNEST || fs0.plain_unnest)
		return false;
	*c = saved;
	*fs = fs0;
	fs->plain_unnest = true;
	agg_refuse_retry();
	return agg_base_once(c, root, input_rel, fs);
}

/*
 * agg_base, then another table of the FROM list as the scanned one when the first did not give a plan: the other table of an outer join of two
 * tables (a preserved side), or, with inner joins only, each of the tables in turn (the one with the column index, which the others are joined to)
 */
bool
agg_base_either(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	List	   *planned;
	int			ntables;

	if (agg_base(c, root, input_rel, fs))
		return true;
	if (!fs->flippable)
	{
		ntables = fs->ntables;
		if (fs->lefts != NIL || c->joinless)
			return agg_refuse_on(FROM_SHAPE, "no other table of the FROM list can be the scanned one");
		planned = c->planned;
		for (int skip = 1; skip < ntables; skip++)
		{
			memset(c, 0, sizeof(AggCtx));
			c->planned = planned;
			agg_refuse_retry();
			*fs = (FromScan) {.ok = true,.skip = skip};
			if (agg_base(c, root, input_rel, fs))
				return true;
		}
		return agg_refuse_on(FROM_SHAPE, "no other table of the FROM list can be the scanned one");
	}
	planned = c->planned;
	memset(c, 0, sizeof(AggCtx));
	c->planned = planned;
	agg_refuse_retry();
	*fs = (FromScan) {.ok = true, .flip = true};
	return agg_base(c, root, input_rel, fs);
}

SiftAggSpec *
agg_spec_new(AggCtx *c)
{
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->nparts = c->nparts;
	s->partheap = c->partheap;
	s->partindex = c->partindex;
	s->partprune = c->partprune;
	s->group = c->group;
	s->left_lateral = c->func_left;
	return s;
}


/* the groups the scan makes: those of the keys, or of each grouping set */

/*
 * A set filter bound as one array parameter (`col = ANY($1)`) has no length in a generic plan: PostgreSQL assumes 10 elements and prices an
 * index or bitmap scan for them, while the scan of this node takes the whole array into a byte table when it starts and costs the same
 * for 10 or 100,000 elements. The paths that fetch the rows of such a set one by one are charged the elements sift.agg_param_set_size
 * assumes (their cost grows with the set), and only when this node can run the query, so that a query it cannot take keeps the plan it
 * has. A custom plan sees the array and needs none of this.
 */
int			sift_agg_param_set_size = 1000;
int			sift_agg_lookup_max_rows = -1;
int			sift_agg_lookup_max_kb = 256 * 1024;

int			agg_index_pick;
int			agg_index_usable;
double		agg_stage_normal = HUGE_VAL;

const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
