/*
 * agg/cost/scope.c - what a scope read leaves of the table to read (agg/plan_scope.c)
 *
 * The scope units of a key hold exactly the rows of the members that have a pair with it (col/scope.h): a scope read of the keys reads those rows and no others, where the entity
 * path (the join the second pass took out) reads the units its key filter on the member column leaves, which hold the rows of other members too and, for facts ordered by time,
 * most units. The share is the one of the rows of the table: the pairs PostgreSQL expects the keys of the scope relation to have (its statistics of the key column, the most common
 * keys by their own frequency, not the join's estimate) over the distinct members of the scanned table, each member holding its share of the rows.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "core/compat.h"
#include "col/pg.h"
#include "agg/agg.h"
#include "agg/cost/int.h"

/* the share of the rows of the scanned table the scope conjunct of s leaves: 1 without one, or when the pass did not take the join out (the marker has no relation to ask) */
double
scope_share(AggCtx *c, const SiftAggSpec *s)
{
	const ScopeTake *t = c->scope_take;
	double		nd,
				share;

	if (s->scope == NULL || t == NULL)
		return 1.0;
	nd = column_distinct(c, t->fk, false);
	share = t->pairs / (nd > 0 ? nd : DEFAULT_NUM_DISTINCT);
	/* a read of every key has a row for each pair of its member: the pairs a member has on average, which may exceed 1 */
	return s->scope->all ? share : Min(share, 1.0);
}

/*
 * The tests a row of the scope units goes through besides the scan's own: its key, and for IN and EXISTS of several keys one arm for each key whose rows the dedupe thins
 * (the keys of a member in two of them are left out of the units of all but the least: col/scan/scope.c), the OR of which is the conjunct. An arm costs a row AGG_SCOPE_ARM
 * tests: 50 keys read 330,000 entries in 80 ms where the join takes 8, 20 keys in 16 where one key takes 2 (300,000 events, 5,000 entities, 50 tenants).
 */
double
scope_tests(const SiftAggSpec *s)
{
	return s->scope == NULL ? 0.0 : s->scope->once ? 1.0 + (s->scope->nkeys - 1) * AGG_SCOPE_ARM : 1.0;
}

/*
 * What the join as written costs the node of a scope read, in instructions, for the keys it has: the second pass's node runs the scope units, or the first pass's scan (the alternative,
 * SiftAggScope.alt) when the join costs less for the keys the execution finds (agg/scope.c scope_open). Each serial path of the first pass leaves its spec here with its price at
 * SIFT_AGG_SCOPE_KNOTS key counts: a key of the average size has the pairs of the relation over its keys (the statistics), and the rows PostgreSQL expects of the lookup and of the join
 * follow them. The price is the scan of the table's rows (AGG_ALT_ROW), the pairs the lookup table loads (AGG_ALT_PAIR) and the operators the model counts for the keys' rows beyond those of the first
 * key (AGG_ALT_OP, from agg_ops: the operators as the executor spends them, not the cost units, which carry the share against the native plan and a scale that differs with the plan it is compared with:
 * a cost unit took 2.5 times the instructions it was fitted at on a table a third the size). The executor does the arithmetic with the keys it really has: linear between the knots, flat past the last, which
 * is every key, and below the first knot. entry_instr is the same operators for each entry of the scope units, which they aggregate as the join does.
 */

/* the operators of the first pass's scan, as the executor spends them, for `pairs` rows of the scope relation (srel) and the join rows, each pair bringing `per_pair` of them */
static double
alt_ops(AggCtx *c, const SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *srel, double pairs, double per_pair, double rows_out)
{
	SiftAggSpec priced = *s;
	Path		p = {0};
	double		srows = srel->rows,
				irows = input_rel->rows;

	srel->rows = pairs;
	input_rel->rows = Max(pairs * per_pair, 1.0);
	pricing_scratch = true;
	PG_TRY();
	{
		agg_cost(c, &priced, input_rel, &p, rows_out);
	}
	PG_FINALLY();
	{
		pricing_scratch = false;
		srel->rows = srows;
		input_rel->rows = irows;
	}
	PG_END_TRY();
	return agg_ops;
}

/* the rows the nodes of a plan hand up: the scans of its relations (leaves) and every other node (joins, appends, aggregates, subquery scans) */
static void
plan_node_rows(Path *p, double *scans, double *others)
{
	List	   *kids = NIL;

	check_stack_depth();
	switch (nodeTag(p))
	{
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			kids = list_make2(((JoinPath *) p)->outerjoinpath, ((JoinPath *) p)->innerjoinpath);
			break;
		case T_AppendPath:
			kids = ((AppendPath *) p)->subpaths;
			break;
		case T_MergeAppendPath:
			kids = ((MergeAppendPath *) p)->subpaths;
			break;
		case T_SubqueryScanPath:
			kids = list_make1(((SubqueryScanPath *) p)->subpath);
			break;
		case T_AggPath:
			kids = list_make1(((AggPath *) p)->subpath);
			break;
		case T_SortPath:
			kids = list_make1(((SortPath *) p)->subpath);
			break;
		default:
			if (!sift_is_sorted_unique_path(p))
			{
				*scans += p->rows;
				return;
			}
			kids = list_make1(((SiftSortedUniquePath *) p)->subpath);
			break;
	}
	*others += p->rows;
	foreach_ptr(Path, k, kids)
		plan_node_rows(k, scans, others);
}

/*
 * What the join as written pays for the relation that makes the pairs of the scope. Where it is a table the lookup reads and loads the pairs of its keys: AGG_ALT_PAIR for each. Where
 * PostgreSQL makes it (a view of a union and a join, a join to a table of keys) the plan runs whole, whatever the keys: its scans read the rows of the relations (AGG_ALT_SCAN each, the same
 * for 3 pairs or 3,000), and the nodes above them hand up the pairs of the keys, AGG_ALT_NODE a row for each node (the rows that nodes other than scans hand up, over those the plan gives:
 * the nodes a pair passes). *fixed: the instructions of the scans; *per_pair: those of each pair of the keys.
 */
static void
alt_pair_instr(AggCtx *c, double *fixed, double *per_pair)
{
	double		scans = 0,
				others = 0,
				out = 0;

	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->cpath != NULL)
		{
			plan_node_rows(jc->cpath, &scans, &others);
			out += jc->cpath->rows;
		}
	*fixed = AGG_ALT_SCAN * scans;
	*per_pair = (out > 0 ? AGG_ALT_PLAN_PAIR : AGG_ALT_PAIR) + AGG_ALT_NODE * others / Max(out, 1.0);
}

/* does the node of the second pass run this spec in the place of its own: the plans its joins read are children it can hold (no parameter of an enclosing level, none of a query level of its own), and it has no spine */
static bool
alt_runs(AggCtx *c, const SiftAggSpec *s)
{
	if (s->scope != NULL || s->nworkers > 0 || c->level_params != NIL || c->spine != NULL || c->nparts > 0)
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->cpath != NULL && (jc->j.params != NIL || jc->j.subplan > 0 || c->detached))
			return false;
	return true;
}

void
scope_alt_record(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, List *pathkeys)
{
	ScopeTake  *t = c->scope_pass;
	static const double counts[] = {1, 3, 10, 30, 100};
	ScopeAlt   *a;
	RelOptInfo *srel;
	RangeTblEntry *rte;
	Index		rti;
	AttrNumber	att,
				fk;
	Oid			type,
				coll;
	int32		typmod;
	double		kall,
				per,
				nd,
				per_pair,
				pairs,
				fixed,
				pair_instr,
				ops,
				ops0 = 0;

	if (t == NULL || t->second || !t->try || !alt_runs(c, s) || !sift_agg_scope_item(c, &rti, &att, &fk))
		return;
	srel = c->root->simple_rel_array[rti];
	rte = planner_rt_fetch(rti, c->root);
	if (rte->rtekind == RTE_RELATION)
		get_atttypetypmodcoll(rte->relid, att, &type, &typmod, &coll);
	else
	{
		Expr	   *key = list_nth_node(TargetEntry, rte->subquery->targetList, att - 1)->expr;

		type = exprType((Node *) key);
		typmod = exprTypmod((Node *) key);
		coll = exprCollation((Node *) key);
	}
	kall = Max(estimate_num_groups(c->root, list_make1(makeVar(rti, att, type, typmod, coll, 0)), Max(srel->tuples, 1.0), NULL, NULL), 1.0);
	per = Max(srel->tuples, 1.0) / kall;
	a = palloc0(sizeof(ScopeAlt));
	a->spec = agg_spec_keep(s);
	a->pathkeys = pathkeys;
	a->startup = p->startup_cost;
	a->total = p->total_cost;
	/* the rows a pair brings (those of its member) from the statistics of the member column, not from the ratio of the join's estimate to the scope relation's, which holds the keys the statement
	 * names only where it names them as constants: a table of keys leaves the relation whole and the join its rows, and the ratio was the rows of a key divided by the pairs of all of them */
	nd = column_distinct(c, makeVar(c->relid, fk, InvalidOid, -1, InvalidOid, 0), false);
	per_pair = Max(c->rel->rows, 1.0) / (nd > 0 ? nd : DEFAULT_NUM_DISTINCT);
	a->knot_rows = Max(per * per_pair, 1.0);
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->cpath != NULL)
			a->children = lappend(a->children, jc->cpath);
	a->entry_instr = 0;
	alt_pair_instr(c, &fixed, &pair_instr);
	for (int i = 0; i <= lengthof(counts); i++)
	{
		double		k = i < lengthof(counts) ? counts[i] : kall;

		if (k > kall || (a->nknot > 0 && k <= a->knot_k[a->nknot - 1]))
			continue;
		pairs = Min(k * per, Max(srel->tuples, 1.0));
		ops = alt_ops(c, s, input_rel, srel, pairs, per_pair, s->est_groups);
		ops0 = a->nknot == 0 ? ops : ops0;
		a->knot_k[a->nknot] = k;
		a->knot_instr[a->nknot++] = AGG_ALT_ROW * Max(c->rel->tuples, 1.0) + fixed + pair_instr * pairs + AGG_ALT_OP * Max(ops - ops0, 0.0);
		a->entry_instr = a->nknot > 1 ? AGG_ALT_OP * (ops - ops0) / ((k - a->knot_k[0]) * a->knot_rows) : 0.0;
	}
	t->alts = lappend(t->alts, a);
}

bool
scope_alt_attach(AggCtx *c, SiftAggSpec *s, Path *p, List *pathkeys)
{
	ScopeTake  *t = c->scope_pass;
	ScopeAlt   *best = NULL;
	SiftAggScope *sc;
	SiftAggScope one_scope;
	SiftAggSpec one;

	if (t == NULL || !t->second || s->scope == NULL)
		return true;
	foreach_ptr(ScopeAlt, a, t->alts)
		if (equal(a->pathkeys, pathkeys) && s->nord == a->spec->nord && (best == NULL || a->total < best->total))
			best = a;
	/* a row for each pair of a member in several keys is what the join counts, and the units cannot when the member's pairs changed */
	if (best == NULL)
		return s->scope->once || (!s->scope->all && s->scope->nkeys <= 1 && !t->key_read);
	sc = memcpy(palloc(sizeof(SiftAggScope)), s->scope, sizeof(SiftAggScope));
	sc->alt = agg_spec_keep(best->spec);
	sc->alt->join = memcpy(palloc(best->spec->njoin * sizeof(SiftAggJoin)), best->spec->join, best->spec->njoin * sizeof(SiftAggJoin));
	((SiftAggPath *) p)->alt_children = best->children;
	sc->nknot = best->nknot;
	memcpy(sc->knot_k, best->knot_k, sizeof(sc->knot_k));
	memcpy(sc->knot_instr, best->knot_instr, sizeof(sc->knot_instr));
	for (int i = 0; t->key_read && i < sc->nknot; i++)
		sc->knot_instr[i] += AGG_ALT_JOIN * (sc->knot_k[i] - sc->knot_k[0]) * best->knot_rows;
	sc->knot_rows = best->knot_rows;
	sc->entry_instr = best->entry_instr;
	/* the units answer from their pre-aggregates, where the spec can use them (a unit with a changed member does not): a read per pair always, a read counted once of one key, which the execution may have of the many the plan was
	 * priced for (a read of several keys counted once leaves rows out of its units by the dedupe and reads them: sift_aggspec_preagg_why) */
	one = *s;
	one_scope = *s->scope;
	one_scope.nkeys = 1;
	one.scope = &one_scope;
	sc->rowwise = 1.0 - preagg_share(c, &one);
	s->scope = sc;
	/* the node costs what the cheaper of its two ways does, and a hair less than the join it holds: the plan that can only be that join has no reason to win */
	p->total_cost = Min(p->total_cost, best->total * (1.0 - 1e-6));
	p->startup_cost = Min(p->startup_cost, best->startup * (1.0 - 1e-6));
	return true;
}

void
scope_alt_children(SiftAggPath *ap, SiftAggSpec *s)
{
	SiftAggSpec *alt = s->scope != NULL ? s->scope->alt : NULL;
	int			shift = list_length(ap->path.custom_paths);

	if (alt == NULL)
		return;
	for (int i = 0; i < alt->njoin; i++)
		alt->join[i].child += alt->join[i].child >= 0 ? shift : 0;
	ap->path.custom_paths = list_concat_copy(ap->path.custom_paths, ap->alt_children);
}
