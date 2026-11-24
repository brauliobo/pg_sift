/*
 * agg/v6/v6_pg_qual.c - the Boolean PostgreSQL expressions the batch quals of the scan fall back to (agg/v6/v6_pg_qual.h).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_pg_qual.h"

struct SiftV6PgQual
{
	SiftV6PgExpr *x;
	int			nin;
	int			slot[SIFT_V6_MAX_COLS];
};

/* the column of the input that holds the table column attno (an expression column of an index holds none): its position, -1 for none */
static int
input_pos(const SiftV6Input *in, AttrNumber attno)
{
	if (in->is_table)
		return attno > 0 && attno <= in->nkeys ? attno - 1 : -1;
	for (int i = 0; i < in->nkeys && attno > 0; i++)
		if (in->keys[i] == attno)
			return i;
	return -1;
}

/* What a statement reads of a key of a document (its type, its `->>` text or the float8 cast of it): the column is the virtual attribute of the key's leaf */
static int
add_json_column(SiftV6Input *in, const Var *v)
{
	for (int k = 0; k < in->ncols; k++)
		if (in->col[k].attno == v->varattno)
			return k;
	if (in->ncols == SIFT_V6_MAX_COLS)
		return -1;
	in->col[in->ncols] = (SiftV6InCol) {.attno = v->varattno,.typid = v->vartype,.type = sift_v6_result_vec_type(v->vartype)};
	return in->ncols++;
}

/* The slot of the input column that holds v, added when it is not one yet; -1 for a column the format does not hold in leaves the scan decodes. A numeric and a type the native vectors do not hold are read as datums: a NaN is a value of them */
static int
add_column(SiftV6Input *in, const Var *v)
{
	const int	pos = input_pos(in, v->varattno);
	const bool	datum = v->vartype == NUMERICOID || sift_v6_class(v->vartype) == SV6C_NONE;
	const SiftV6Class cls = datum ? SV6C_DATUM : sift_v6_class(v->vartype);
	SiftV6InCol col = {.typid = v->vartype,.type = datum ? SVEC_DATUM : sift_v6_result_vec_type(v->vartype),.scale = datum ? SIFT_V6_SCALE_DATUM : 0};

	if (sift_v6_vatt_is_json(v->varattno))
		return add_json_column(in, v);
	if (pos < 0 || v->vartype == JSONBOID)
		return -1;
	col.attno = pos + 1;
	for (int k = 0; k < in->ncols; k++)
		if (in->col[k].attno == col.attno)
			return k;
	if (in->ncols == SIFT_V6_MAX_COLS || !sift_v6_format_bound() || !sift_v6_format()->leaf_holds(in->indexoid, col.attno, cls))
		return -1;
	in->col[in->ncols] = col;
	return in->ncols++;
}

static bool
columns_ok(SiftV6Input *in, const List *vars)
{
	foreach_ptr(Var, v, vars)
		if (add_column(in, v) < 0)
			return false;
	return true;
}

bool
sift_v6_pgqual_plan_ok(PlannerInfo *root, const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	List	   *vars;

	return exprType((const Node *) clause) == BOOLOID && sift_v6_pgexpr_plan_ok(root, clause) && sift_v6_pgexpr_vars(clause, SIFT_V6_BQ_RELID, 0, &vars) && columns_ok(&scratch, vars);
}

/* the type of the column the executor gets of Var v: what add_column makes of it */
static SiftV6PgIn
pg_in(const Var *v)
{
	SiftV6InCol col = {.typid = v->vartype};

	if (v->vartype == NUMERICOID)
		sift_v6_vec_type(v->vartype, v->vartypmod, &col);
	return (SiftV6PgIn) {.typid = v->vartype,.scale = col.scale};
}

static Expr *
conjunction(const List *quals, List **vars)
{
	Expr	   *e = make_ands_explicit(list_copy(quals));

	SIFT_INVARIANT(sift_v6_pgexpr_vars(e, SIFT_V6_BQ_RELID, 0, vars), "the batch quals of the aggregate scan read at most %d columns", SIFT_V6_MAX_COLS);
	return e;
}

static SiftV6PgExpr *
open_expr(MemoryContext cx, PlanState *ps, Expr *e, const List *vars)
{
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

	for (int i = 0; i < list_length(vars); i++)
		pin[i] = pg_in(list_nth(vars, i));
	return sift_v6_pgexpr_open(cx, ps, sift_v6_pgexpr_renumber(e, SIFT_V6_BQ_RELID, 0, vars), list_length(vars), pin);
}

SiftV6PgExpr *
sift_v6_pgqual_prepare(MemoryContext cx, PlanState *ps, const List *quals)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	List	   *xq = NIL,
			   *vars;
	SiftV6PgExpr *x = NULL;

	foreach_ptr(Expr, clause, quals)
		if (sift_v6_pgexpr_exec((const Node *) clause))
			xq = lappend(xq, clause);
	if (xq != NIL)
	{
		Expr	   *e = conjunction(xq, &vars);

		x = open_expr(cx, ps, e, vars);
	}
	MemoryContextSwitchTo(old);
	return x;
}

SiftV6PgQual *
sift_v6_pgqual_open(MemoryContext cx, const List *quals, SiftV6Input *in, SiftV6PgExpr *prepared)
{
	SiftV6PgQual *q = MemoryContextAllocZero(cx, sizeof(SiftV6PgQual));
	MemoryContext old = MemoryContextSwitchTo(cx);
	List	   *vars;
	Expr	   *e = conjunction(quals, &vars);

	q->nin = list_length(vars);
	for (int i = 0; i < q->nin; i++)
	{
		const int	slot = add_column(in, list_nth(vars, i));

		SIFT_INVARIANT(slot >= 0, "the scan decodes the columns of its batch quals of relation %u", in->indexoid);
		q->slot[i] = slot;
	}
	q->x = prepared != NULL ? prepared : open_expr(cx, NULL, e, vars);
	MemoryContextSwitchTo(old);
	return q;
}

void
sift_v6_pgqual_filter(SiftV6PgQual *q, SiftBatchSel *bs, const SiftVec *const *src)
{
	const SiftVec *in[SIFT_V6_MAX_COLS];
	uint16		left = 0;

	if (bs->n_active == 0)
		return;
	for (int i = 0; i < q->nin; i++)
		in[i] = src[q->slot[i]];
	sift_v6_pgexpr_eval(q->x, in, bs);
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
	{
		uint64		keep = 0;

		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			keep |= (uint64) (!q->x->isnull[r] && DatumGetBool(q->x->vals[r])) << (r & 63);
		}
		bs->mask[w] = keep;
		left += (uint16) __builtin_popcountll(keep);
	}
	bs->n_active = left;
	bs->all_active = left == bs->n_raw;
	bs->has_sel = false;
}
