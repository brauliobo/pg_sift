/*
 * agg/unnest/plan.c - CROSS JOIN LATERAL unnest(col) AS u(elem), jsonb_to_recordset(col) AS u(c1 t1, ..) (or LEFT JOIN LATERAL ... ON true) over an array column of the fact table as part of the
 * aggregate scan of SiftAggV6: when the paths of the index keep the array of the column as a list (col[] text, ids[] int4, items[]{sku text, price float8}: a leaf of its lengths and a leaf of its
 * elements or of each key of its objects), the node takes the elements from the index and the keys and aggregates of the statement may name the elements beside the columns of the table
 * (agg/v6_unnest_run.h). The WHERE clauses of the table and of the elements are evaluated on the batches, which have no scan keys.
 *
 * The statement reads no other relation. The planning of the lateral relation itself is agg/unnest/jsonb.h.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/bytes.h"
#include "agg/filter/pg.h"
#include "agg/plan/int.h"
#include "agg/cost/card.h"
#include "agg/cost/card_est.h"
#include "agg/vexpr.h"
#include "agg/expr/pg.h"
#include "agg/ext.h"
#include "agg/pipe.h"
#include "agg/safety.h"
#include "agg/source_rel.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/late.h"

/* the join is inner or a left one of the unnest with no condition; any other special join is not ours */
static bool
join_kind(PlannerInfo *root, Index elem_rti, bool *left)
{
	*left = false;
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		if (sj->jointype != JOIN_LEFT || !bms_equal(sj->min_righthand, bms_make_singleton(elem_rti)) || *left)
			return false;
		*left = true;
	}
	return true;
}

/* what the batches of the scan compute is the month of a date column (the one binning of a key the scan of an unnest does), float8 arithmetic of arguments (agg/plan/rewrite.h) and the PostgreSQL
 * expressions of keys and arguments over the columns of the pairs (v6_pg_expr.h), and no other */
static bool
computes_only_months(const SiftV6Spec *s)
{
	for (int b = 0; b < s->nbins; b++)
		if (s->bin[b].stride_us != SIFT_DATEBIN_MONTH || s->key[s->bin[b].key].typid != DATEOID)
			return false;
	for (int k = 0; k < s->nderive; k++)
		if (s->derive[k].kind != SVD_ARITH && s->derive[k].kind != SVD_PG_EXPR)
			return false;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].derive != 0 && s->derive[s->key[i].derive - 1].kind != SVD_PG_EXPR)
			return false;
	return true;
}

List *
sift_v6_quals_in_order(PlannerInfo *root, List *infos)
{
	List	   *ordered = NIL;

	foreach_node(RestrictInfo, ri, infos)
	{
		QualCost	c;
		int			at = 0;

		cost_qual_eval_node(&c, (Node *) ri->clause, root);
		for (; at < list_length(ordered); at++)
		{
			QualCost	o;

			cost_qual_eval_node(&o, (Node *) ((RestrictInfo *) list_nth(ordered, at))->clause, root);
			if (c.per_tuple < o.per_tuple)
				break;
		}
		ordered = list_insert_nth(ordered, at, ri);
	}
	return ordered;
}

/* a sift_table's rows reach the pairs as the vectors of its scan, which hold the native classes: no datum and no binning */
static bool
table_rows_ok(const SiftV6Spec *s)
{
	if (s->nbins > 0)
		return false;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].src == SV6SRC_ATTR && sift_v6_col_class(&s->key[i]) == SV6C_DATUM)
			return false;
	for (int v = 0; v < sift_v6_spec_nvec(s); v++)
		if (sift_v6_vec_col(s, v) != NULL && sift_v6_vec_col(s, v)->src == SV6SRC_ATTR && sift_v6_col_class(sift_v6_vec_col(s, v)) == SV6C_DATUM)
			return false;
	return true;
}

typedef bool (*ColTest) (const SiftV6Col *c);

/* a column the statement reads from the table, its fact or the elements, passes test: the keys, the arguments, the FILTER columns, the inputs of what the scan computes and of the residual of a join */
static bool
any_col(const SiftV6Spec *s, ColTest test)
{
	for (int i = 0; i < s->nkeys; i++)
		if (test(&s->key[i]))
			return true;
	for (int v = 0; v < sift_v6_spec_nvec(s); v++)
		if (sift_v6_vec_col(s, v) != NULL && test(sift_v6_vec_col(s, v)))
			return true;
	for (int i = 0; i < s->nfcols; i++)
		if (test(&s->fcol[i]))
			return true;
	for (int i = 0; s->ext != NULL && i < s->ext->nrcol; i++)
		if (test(&s->ext->rcol[i]))
			return true;
	for (int k = 0; k < s->nderive; k++)
	{
		SiftV6PgItem it;

		if (test(&s->derive[k].src))
			return true;
		for (int c = 0; c < s->derive[k].ncond; c++)
			if (test(&s->derive[k].cond[c]))
				return true;
		if (s->derive[k].kind != SVD_PG_EXPR)
			continue;
		sift_v6_pgexpr_item(s, &s->derive[k], &it);
		for (int c = 0; c < it.nin; c++)
			if (test(&it.col[c]))
				return true;
	}
	return false;
}

/* the scan of an unnest reads the values of the leaves of its columns, not the type each row's chunk keeps of a key (jsonb_typeof(doc -> 'key')) */
static bool
is_type_tag(const SiftV6Col *c)
{
	return c->jtype;
}

/* a column of the table that no typed leaf holds: a whole document, a numeric read as datums, a type no vector holds (the flag of a test of the elements is no column of the table) */
static bool
is_untyped(const SiftV6Col *col)
{
	const SiftV6Col c = sift_v6_vatt_is_json(col->attno) ? sift_v6_vatt_col(col->attno, col->attno, col->typid, col->scale) : *col;
	const SiftV6Class cls = sift_v6_col_class(&c);

	if (col->src != SV6SRC_ATTR || sift_v6_col_is_flag(col))
		return false;
	return c.jtype || (c.attno >= SIFT_V6_JLEAF_VATT && c.jleaf == 0) || cls == SV6C_DATUM || cls == SV6C_NONE;
}

/* the store scans the elements of an array (SiftJsonbOps) and reads the columns of the table the statement names beside them */
static bool
store_reads_cols(const V6Plan *p)
{
	return p->ops->jsonb != NULL && (p->ops->jsonb->exact_values || !any_col(&p->spec, is_untyped));
}

/* every row of a table the scan reads again by its address: its arrays are read as the rows the leaves cannot hold */
static void
rows_from_heap(SiftV6Ext *x)
{
	x->elem.group_leaf = -1;
	for (int s = 0; s < x->nstreams; s++)
		x->streams[s].group_leaf = -1;
}

bool
sift_v6_unnest_where(V6Plan *p)
{
	SiftClause	clause;

	if (p->heap_rows)
		rows_from_heap(p->spec.ext);
	foreach_node(RestrictInfo, ri, sift_v6_quals_in_order(p->root, p->rel->baserestrictinfo))
		if (!sift_v6_unnest_test_ri(p, ri))
			p->spec.ext->heap_where = lappend(p->spec.ext->heap_where, copyObject(ri->clause));
	p->spec.ext->late_probe = sift_v6_late_probe_plan(p);
	if (p->spec.indexqual == NIL)
		return true;
	if (!sift_v6_clause_make_params(CurrentMemoryContext, p->spec.indexqual, &p->spec, &clause, NULL))
		return false;
	p->spec.ext->where = p->spec.indexqual;
	p->spec.indexqual = NIL;
	return true;
}

/* the compiled statement is one the scan of an unnest computes over the columns its source gives */
static bool
unnest_computes(const V6Plan *p)
{
	return (!p->is_table || table_rows_ok(&p->spec)) && computes_only_months(&p->spec) && p->spec.jsonqual == NIL && store_reads_cols(p);
}

/* the statement compiled on the index to what the scan of an unnest computes */
bool
sift_v6_unnest_compile(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return sift_v6_compile_on(p, extra, group_exprs) && unnest_computes(p);
}

/* what the unnest scan needs of the compiled statement besides its columns: the quals of the element and of the table run on the batches */
bool
sift_v6_unnest_ready(V6Plan *p)
{
	return unnest_computes(p) && !any_col(&p->spec, is_type_tag) && sift_v6_unnest_elem_quals(p) && sift_v6_unnest_where(p);
}

/* the pairs the filter of an aggregate drops are not skipped by an argument PostgreSQL computes for every pair: as the CASE of its filter, the executor leaves them out of the argument; the members of jsonb_each of a map are float8 leaves, a cast of one that raises is the statement's error only where an expression reads it (agg/unnest/late.h), and its filter is a clause of the pairs */
Aggref *
sift_v6_unnest_filter_as_case(const V6Plan *p, Aggref *a)
{
	TargetEntry *te;
	CaseWhen   *when;
	CaseExpr   *c;
	Aggref	   *plain;

	if (p->elem_rel == NULL || (p->xseed != NULL && p->xseed->elem.fn >= SV6E_EACH) || a->aggfilter == NULL || list_length(a->args) != 1 || a->aggdistinct != NIL || a->aggorder != NIL)
		return a;
	te = linitial_node(TargetEntry, a->args);
	if (IsA(te->expr, Var) || exprType((Node *) te->expr) != FLOAT8OID ||
		!bms_equal(pull_varnos(p->root, (Node *) list_make2(a->aggfilter, te->expr)), bms_make_singleton(p->elem_rel->relid)))
		return a;
	when = makeNode(CaseWhen);
	when->expr = a->aggfilter;
	when->result = te->expr;
	c = makeNode(CaseExpr);
	c->casetype = FLOAT8OID;
	c->args = list_make1(when);
	c->defresult = (Expr *) makeNullConst(FLOAT8OID, -1, InvalidOid);
	plain = copyObject(a);
	plain->aggfilter = NULL;
	linitial_node(TargetEntry, plain->args)->expr = (Expr *) c;
	return plain;
}

/* one operator per pair, and each group of a distinct count or a percentile keeps the values it sees (7 times a pair that folds into a counter, as measured on 3.7M pairs in 60,000 groups) */
#define HEAVY_AGG_PAIR_FACTOR	6

/* the quals of the table whose values the plan knows: the selectivity of a qual of $n is the guess of a generic plan, which the normal plan is priced with too */
static List *
known_quals(const RelOptInfo *rel)
{
	List	   *known = NIL;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (!agg_node_has_extern_param((Node *) ri->clause))
			known = lappend(known, ri);
	return known;
}

/* the lengths leaf of the array the elements come from: the one the scan reads, or of a table whose rows it reads again by their address, the leaf of the declared list of the column */
static int
lengths_leaf(const V6Plan *p)
{
	const SiftV6XElem *e = &p->xseed->elem;

	return e->group_leaf >= 0 || p->leaves.schema == NULL ? e->group_leaf : sift_v6_unnest_group_of(&p->leaves, e->attno, NULL);
}

/*
 * the rows the join of a row with its elements makes: PostgreSQL prices a set-returning function at a constant count of rows a call (100 for the jsonb ones), the lengths leaf of the
 * array in the index or the table says how many elements a row has (rows with no array have none)
 */
double
sift_v6_unnest_rows(const V6Plan *p, const RelOptInfo *input_rel)
{
	const SiftCardStatsView *v = sift_v6_rel_card_view(p->relid, p->is_table);
	const int	gl = lengths_leaf(p);
	const SiftColCardStats *cs = v != NULL && gl >= 0 && (uint32) gl < v->ncols ? &v->cols[gl] : NULL;
	const double mean = cs != NULL && cs->total_rows > 0 ? sift_card_mean(cs) : -1;

	if (mean < 0 || p->elem_rel->tuples <= 0)
		return input_rel->rows;
	return clamp_row_est(input_rel->rows / p->elem_rel->tuples * mean * (1.0 - (double) cs->null_rows / cs->total_rows));
}

/* the tuples an element of a row read again costs beyond the one its function makes: read back and deformed, then its columns made the cells of the vectors of the pairs (measured: at 1,500 and
 * 20,000 rows of 1.6 elements, jsonb_to_recordset read so runs at 1.0 to 1.4 times the normal plan) */
#define REREAD_ELEM_TUPLES	2

/*
 * the rows of a sift_table whose arrays no leaves hold, read again by their address: each is fetched and its array taken apart by the function of the statement, priced as the normal plan prices a
 * call of the function and the tuples of its rows; a qual only the row decides is decided after the fetch, so every row of the table is read again for it
 */
static Cost
reread_cost(const V6Plan *p)
{
	const double rows = p->heap_rows ? p->rel->tuples : p->rel->rows;
	const Path *fn = p->elem_rel->cheapest_total_path;

	if (p->xseed->elem.group_leaf >= 0)
		return 0;
	return rows * (sift_v6_fetch_cost(p->is_table) + fn->total_cost + p->elem_rel->rows * cpu_tuple_cost * REREAD_ELEM_TUPLES);
}

/* the pairs of the whole table: the scan skips only the granules the zones of the quals of the table rule out, which a column the rows are not ordered by does not narrow, so a selective statement costs what the table does */
Cost
sift_v6_unnest_pairs_cost(const V6Plan *p, const RelOptInfo *input_rel)
{
	const double sel = Max(clauselist_selectivity(p->root, known_quals(p->rel), 0, JOIN_INNER, NULL), 1.0e-6);
	int			heavy = 0;

	for (int j = 0; p->spec.nkeys > 0 && j < p->spec.naggs; j++)
		heavy += sift_v6_kind_distinct(p->spec.agg[j].kind) || p->spec.agg[j].kind == SV6_PCT_CONT;
	return input_rel->rows / sel * cpu_operator_cost * (1 + HEAVY_AGG_PAIR_FACTOR * heavy) + reread_cost(p);
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem_rel, GroupPathExtraData *extra, List *group_exprs)
{
	SiftV6ElemRte rte;
	SiftV6RelSource *src;
	int			nsrc;
	bool		left;

	if (!sift_v6_plan_fact_ok(root, fact) || !sift_v6_unnest_rte(root, fact, elem_rel, &rte) || !join_kind(root, elem_rel->relid, &left))
		return;
	src = sift_v6_rel_sources_alloc(root, fact, &nsrc);
	for (int i = 0; i < nsrc; i++)
	{
		V6Plan		p = {.root = root,.rel = fact,.out = output_rel};
		Relation	rel = sift_v6_source_open(src[i].reloid, src[i].is_table, AccessShareLock, NULL);

		if (sift_v6_plan_bind(&p, &src[i], rel) && sift_v6_unnest_seed(&p, elem_rel, &rte, left))
		{
			sift_v6_unnest_tests_beside(&p, &rte, output_rel, extra);
			if (sift_v6_compile_on(&p, extra, group_exprs) && sift_v6_unnest_ready(&p))
				sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, sift_v6_unnest_pairs_cost(&p, input_rel), sift_v6_unnest_rows(&p, input_rel));
		}
		sift_v6_source_close(rel, src[i].is_table, NoLock);
	}
}

void
sift_v6_unnest_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs;
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	int			a,
				b;

	if (!sift_v6_plan_query_ok(root, output_rel) || bms_num_members(base) != 2)
		return;
	a = bms_next_member(base, -1);
	b = bms_next_member(base, a);
	group_exprs = sift_v6_group_exprs_of(root);
	try_fact(root, input_rel, output_rel, root->simple_rel_array[a], root->simple_rel_array[b], (GroupPathExtraData *) extra, group_exprs);
	try_fact(root, input_rel, output_rel, root->simple_rel_array[b], root->simple_rel_array[a], (GroupPathExtraData *) extra, group_exprs);
}
