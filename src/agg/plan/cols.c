/*
 * agg/plan/cols.c - the columns of a statement SiftAggV6 reads: a column of the host (the index or the table), a datum, an index or generated expression, and what the scan computes from them.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/selfuncs.h"
#include "utils/timestamp.h"

#include "agg/agg.h"
#include "agg/ht/ht.h"
#include "agg/plan/int.h"
#include "agg/par/pg.h"
#include "agg/ext.h"
#include "agg/engine.h"
#include "agg/join/push.h"
#include "agg/cost/card.h"
#include "agg/vexpr.h"
#include "agg/expr/num.h"
#include "agg/argext/argext.h"
#include "agg/argext/sql.h"
#include "agg/cost/card.h"
#include "agg/cost/cost.h"
#include "agg/filter/pg.h"
#include "agg/gen/gen.h"
#include "agg/pct.h"
#include "agg/plan/rewrite.h"
#include "agg/unnest/exists.h"
#include "agg/unnest/jsonb.h"
#include "agg/join/plan.h"
#include "agg/plan/shape.h"
#include "agg/datum.h"
#include "agg/expr/pg.h"
#include "agg/qual/qual.h"
#include "col/am.h"
#include "col/am/preds.h"
#include "col/col.h"
#include "table/dispatch.h"
#include "core/catname.h"


/* The attribute of the host a column of the table is: the column of the index, or the attribute of the table; 0: the host does not hold it */
static int
host_attno(const V6Plan *p, AttrNumber attno)
{
	if (p->is_table)
		return attno > 0 && attno <= p->sch.natts ? attno : 0;
	for (int i = 0; attno > 0 && i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == attno)
			return i + 1;
	return 0;
}

/* the relation holds attribute att in one leaf the scan decodes as the class says */
static bool
host_holds(const V6Plan *p, int att, SiftV6Class cls)
{
	return att > 0 && p->ops->leaf_holds(p->relid, p->is_table, att, cls);
}

/* the digits after the point of the scaled integer a numeric attribute is stored as */
static int
host_scale(const V6Plan *p, int att)
{
	return p->sch.leaves != NULL ? p->sch.leaves->cols[p->sch.att_first[att]].scale : p->sch.atts[att - 1].scale;
}

Oid
sift_v6_plan_opfamily(const V6Plan *p, int att)
{
	return p->is_table ? p->sch.att_opfamily[att] : p->index->opfamily[att - 1];
}

bool
sift_v6_plan_table(V6Plan *p, Oid relid)
{
	p->ops = sift_store_ops_for_oid(relid, true);
	p->is_table = p->ops != NULL && p->ops->schema(relid, true, &p->sch);
#ifdef SIFT_TESTING
	if (sift_test_col_rel_inval)
		RelationCacheInvalidateEntry(relid);	/* the catalog lookups of the rest of the plan can take a lock that reloads the entry and drops its store */
#endif
	return p->is_table;
}

void
sift_v6_plan_index(V6Plan *p, Relation irel)
{
	p->colrel = sift_col_is_index(irel) ? sift_col_rel_private(irel, CurrentMemoryContext) : NULL;
	p->ops = p->colrel != NULL && p->colrel->stale ? NULL : sift_store_ops_for_oid(RelationGetRelid(irel), false);
	if (p->ops != NULL && !p->ops->schema(RelationGetRelid(irel), false, &p->sch))
		p->ops = NULL;
#ifdef SIFT_TESTING
	sift_col_test_inval(irel);	/* the catalog lookups of the rest of the plan can take a lock that reloads the entry */
#endif
}

static bool
column_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	Oid			typid;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0)
		return false;
	typid = getBaseType(v->vartype);
	if (typid == NUMERICOID && !numeric_ok)
		return false;
	if (sift_v6_vatt_is_json(v->varattno))
	{
		const SiftV6Rel rel = sift_v6_plan_rel(p);

		return sift_v6_json_type_col(&rel, v, out);
	}
	att = host_attno(p, v->varattno);
	if (att == 0 || !host_holds(p, att, sift_v6_class(typid)))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = typid,.scale = typid == NUMERICOID ? host_scale(p, att) : 0};
	return true;
}

bool
sift_v6_plan_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(p, e, numeric_ok, out);
}

/* A column of a type no native vector holds, or a numeric of any form: its values are read as datums, so a NaN, an infinity or a numeric of any digits is the value it is */
static bool
datum_column_of(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || sift_v6_type_native(v->vartype) || v->vartype == JSONBOID)
		return false;
	att = host_attno(p, v->varattno);
	if (att == 0 || !host_holds(p, att, SV6C_DATUM))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = v->vartype,.scale = SIFT_V6_SCALE_DATUM};
	return true;
}

/* A column of the kernels' classes: the values the scan holds in a native vector, which an ordering of them follows */
bool
sift_v6_plan_native_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	return column_of(p, e, false, out);
}

/* A column the scan reads as a value of any type: the columns of the kernels, the datums, and the payload of a dimension or the element of the unnest */
bool
sift_v6_plan_read_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(p, e, numeric_ok, out) || datum_column_of(p, e, out) ||
		sift_v6_star_column(p, e, out) || (exprType((Node *) e) != NUMERICOID && sift_v6_unnest_column(p, e, out));
}

Expr *
sift_v6_plan_json_vars(const V6Plan *p, const Expr *e, bool values)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return p->xseed != NULL ? (Expr *) e : sift_v6_json_vars(&rel, e, values);
}

/* An expression of the query that is a column of the index (its indexprs entry, which PostgreSQL has built over the same range table entry): the scan reads what the index stored of it */
static bool
index_expr_of(const V6Plan *p, const Expr *e, SiftV6Col *out)
{
	int			k = 0;
	const Oid	typid = exprType((const Node *) e);

	if (p->index == NULL || p->index->indexprs == NIL || typid == NUMERICOID || sift_v6_class(typid) == SV6C_NONE)
		return false;
	for (int i = 0; i < p->index->ncolumns; i++)
	{
		if (p->index->indexkeys[i] != 0)
			continue;
		if (equal(e, list_nth(p->index->indexprs, k++)))
		{
			if (!host_holds(p, i + 1, sift_v6_class(typid)))
				return false;
			*out = (SiftV6Col) {.attno = i + 1,.typid = typid};
			return true;
		}
	}
	return false;
}

/* An expression of the query that is the generation expression of a stored generated column of the table: the scan reads the column, which holds what the expression gives */
static bool
generated_expr_of(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	Relation	rel = p->is_table && !IsA(e, Var) ? table_open(p->relid, NoLock) : NULL;
	bool		found = false;

	for (int a = 1; rel != NULL && a <= RelationGetNumberOfAttributes(rel) && !found; a++)
	{
		const Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), a - 1);
		Node	   *gen;

		if (att->attgenerated != ATTRIBUTE_GENERATED_STORED || att->attisdropped)
			continue;
		gen = (Node *) expression_planner((Expr *) build_column_default(rel, a));
		ChangeVarNodes(gen, 1, p->rel->relid, 0);
		found = equal(e, gen) &&
			column_of(p, (Expr *) makeVar(p->rel->relid, a, att->atttypid, att->atttypmod, att->attcollation, 0), false, out);
	}
	if (rel != NULL)
		table_close(rel, NoLock);
	return found;
}

/* the float8 of a member of a map of a document: the column, with the keys that lead to the member in the spec */
static bool
member_of(V6Plan *p, const SiftV6Rel *rel, const Expr *e, SiftV6Col *out)
{
	List	   *path;
	int			at = 0;

	if (!p->members || !sift_v6_json_map_member(rel, e, out, &path))
		return false;
	foreach_ptr(List, known, p->spec.jmembers)
	{
		at++;
		if (equal(known, path))
		{
			out->jmember = at;
			return true;
		}
	}
	p->spec.jmembers = lappend(p->spec.jmembers, path);
	out->jmember = list_length(p->spec.jmembers);
	return true;
}

/* A column the scan reads as a value: a column of the index, the text or the float8 of a key of a jsonb document, a payload column of a dimension or the element of the unnest */
bool
sift_v6_read_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return column_of(p, e, numeric_ok, out) || index_expr_of(p, e, out) || generated_expr_of(p, e, out) ||
		sift_v6_json_read(&rel, e, out) || member_of(p, &rel, e, out) || sift_v6_plan_read_column(p, e, numeric_ok, out);
}

bool
sift_v6_resolve_column(void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return column_of(ctx, e, numeric_ok, out) || sift_v6_unnest_column(ctx, e, out);
}

/* A jsonb document attribute the format hands over whole, as the Datum of its row, to the expressions PostgreSQL evaluates over it */
static bool
document_of(const V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	int			att;

	if (e == NULL || !IsA(e, Var) || v->varno != p->rel->relid || v->varlevelsup != 0 || v->vartype != JSONBOID)
		return false;
	att = host_attno(p, v->varattno);
	if (att == 0 || !host_holds(p, att, SV6C_DATUM))
		return false;
	*out = (SiftV6Col) {.attno = att,.typid = JSONBOID};
	return true;
}

/* the columns of an expression PostgreSQL computes: a numeric and a type the native vectors do not hold are datums, so a NaN is the value it is */
static bool
resolve_datum_column(void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out)
{
	return datum_column_of(ctx, e, out) || document_of(ctx, e, out) || sift_v6_plan_read_column(ctx, e, numeric_ok, out) || sift_v6_unnest_column(ctx, e, out);
}

static int
add_derive(V6Plan *p, const SiftV6Derive *d)
{
	for (int i = 0; i < p->spec.nderive; i++)
		if (memcmp(&p->spec.derive[i], d, sizeof(SiftV6Derive)) == 0)
			return i + 1;
	if (p->spec.nderive == SIFT_V6_MAX_DERIVE)
		return 0;
	p->spec.derive[p->spec.nderive] = *d;
	return ++p->spec.nderive;
}

/* the derivations the pipe of a join carries are those of PostgreSQL's evaluator (v6_pg_expr.h) over columns of the fact table: the pipe has a vector for each, which the rows of a RIGHT or FULL join's
 * dimension that no fact row matched have NULL, as an expression that is NULL on NULL inputs has them */
static bool
join_derive_ok(const V6Plan *p, const Expr *e)
{
	const SiftV6Ext *x = p->xseed;
	bool		preserved = false;

	if (x == NULL || x->unnest)
		return true;
	for (int d = 0; d < x->ndims; d++)
		preserved |= sift_star_preserves_dim(x->dim[d].mode);
	if (preserved && contain_nonstrict_functions((Node *) e))
		return false;
	foreach_ptr(Var, v, pull_var_clause((Node *) e, PVC_RECURSE_PLACEHOLDERS))
		if (v->varno != p->rel->relid)
			return false;
	return true;
}

/* the pipe of a star join carries the vector of a derivation through the dimensions, so it needs one type for the whole scan (the unnest's carries its own) */
static bool
pipe_carries(const V6Plan *p, const SiftV6Derive *d)
{
	return p->xseed == NULL || p->xseed->unnest || sift_v6_derive_vec_typed(d);
}

/* e is a key or argument the scan computes (v6_plan_rewrite.h): col is its column, typed as e */
bool
sift_v6_derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col)
{
	const SiftV6Resolver rs = {sift_v6_resolve_column, p, p->xseed != NULL && p->xseed->unnest};
	SiftV6Derive d;
	Oid			restype;
	int			at;

	if (p->pg_derive || !join_derive_ok(p, e) || !sift_v6_derive_of(&rs, e, key, &d, &restype) || !pipe_carries(p, &d) || (at = add_derive(p, &d)) == 0)
		return false;
	*col = (SiftV6Col) {.attno = d.src.attno,.typid = restype,.scale = d.kind == SVD_NUM_EXPR ? (int) d.aux : 0,.derive = at};
	return true;
}

/* a use of a jsonb document in e that is no chain of -> and ->> over a path the index declares: the index does not hold what it reads (a format that declares no paths holds the keys it shreds); a format that cannot read the value of a key its leaf cannot hold leaves every use to its legacy planner */
static bool
undeclared_doc_walker(Node *n, const V6Plan *p)
{
	List	   *path;
	const Var  *doc;

	if (n == NULL)
		return false;
	doc = sift_v6_doc_chain((const Expr *) n, &path);
	if (doc == NULL || doc->varno != p->rel->relid || doc->varlevelsup != 0)
		return expression_tree_walker(n, undeclared_doc_walker, (void *) p);
	if (p->ops->jsonb == NULL || p->ops->jsonb->path_declared == NULL)
		return false;
	return path == NIL || !p->ops->jsonb->path_declared(p->relid, p->is_table, host_attno(p, doc->varattno), path);
}

/* e is a key or argument PostgreSQL's executor computes from columns of the scan (v6_pg_expr.h): col is its column, typed as e; an argument has a native vector, a key may be of any type that can be grouped */
bool
sift_v6_pg_derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col)
{
	const SiftV6Resolver rs = {p->elem_rel != NULL ? sift_v6_resolve_column : resolve_datum_column, p, false, p->elem_rel != NULL ? p->elem_rel->relid : 0};
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	SiftV6Derive d = {0};
	Oid			restype;
	int			at;

	if (undeclared_doc_walker((Node *) e, p))
		return false;
	e = sift_v6_json_vars(&rel, e, true);
	if (!join_derive_ok(p, e) || !sift_v6_pgexpr_derive_of(p->batch_ok ? p->root : NULL, &rs, p->rel->relid, e, key || p->xseed != NULL, &d, &restype, &p->spec.pgexpr) ||
		!(sift_v6_type_native(restype) || (p->xseed == NULL && key && sift_v6_type_generic(restype) && sift_genkey_supported(restype))) || (at = add_derive(p, &d)) == 0)
		return false;
	*col = (SiftV6Col) {.attno = d.src.attno,.typid = restype,.derive = at,.collid = exprCollation((Node *) e)};
	return true;
}
