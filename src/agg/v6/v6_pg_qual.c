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

/* The slot of the input column that holds v, added when it is not one yet; -1 for a column the format does not hold in leaves the scan decodes. A numeric and a type the native vectors do not hold are read as datums: a NaN is a value of them */
static int
add_column(SiftV6Input *in, const Var *v)
{
	const int	pos = input_pos(in, v->varattno);
	const bool	datum = v->vartype == NUMERICOID || sift_v6_class(v->vartype) == SV6C_NONE;
	const SiftV6Class cls = datum ? SV6C_DATUM : sift_v6_class(v->vartype);
	SiftV6InCol col = {.typid = v->vartype,.type = datum ? SVEC_DATUM : sift_v6_result_vec_type(v->vartype),.scale = datum ? SIFT_V6_SCALE_DATUM : 0};

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
sift_v6_pgqual_plan_ok(const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	List	   *vars;

	return exprType((const Node *) clause) == BOOLOID && sift_v6_pgexpr_plan_ok(clause) && sift_v6_pgexpr_vars(clause, SIFT_V6_BQ_RELID, &vars) && columns_ok(&scratch, vars);
}

SiftV6PgQual *
sift_v6_pgqual_open(MemoryContext cx, const List *quals, SiftV6Input *in)
{
	SiftV6PgQual *q = MemoryContextAllocZero(cx, sizeof(SiftV6PgQual));
	MemoryContext old = MemoryContextSwitchTo(cx);
	Expr	   *e = make_ands_explicit(list_copy(quals));
	List	   *vars;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

	SIFT_INVARIANT(sift_v6_pgexpr_vars(e, SIFT_V6_BQ_RELID, &vars), "the batch quals of the aggregate scan of relation %u read at most %d columns", in->indexoid, SIFT_V6_MAX_COLS);
	q->nin = list_length(vars);
	for (int i = 0; i < q->nin; i++)
	{
		const int	slot = add_column(in, list_nth(vars, i));

		SIFT_INVARIANT(slot >= 0, "the scan decodes the columns of its batch quals of relation %u", in->indexoid);
		q->slot[i] = slot;
		pin[i] = (SiftV6PgIn) {.typid = in->col[slot].typid,.scale = in->col[slot].scale};
	}
	q->x = sift_v6_pgexpr_open(cx, sift_v6_pgexpr_renumber(e, SIFT_V6_BQ_RELID, vars), q->nin, pin);
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
