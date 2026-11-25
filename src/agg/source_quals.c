/*
 * agg/source_quals.c - the WHERE quals of an operator over a source: the machine's clauses go to the source, the clauses over text narrow its batches (v6_source_quals.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"

#include "agg/qual/bqual.h"
#include "agg/source_quals.h"
#include "core/sift.h"

static bool
has_param(Node *n, void *context)
{
	return n != NULL && (IsA(n, Param) || expression_tree_walker(n, has_param, context));
}

static bool
text_clause(const Node *clause, Index relid)
{
	List	   *vars = pull_var_clause((Node *) clause, 0);

	if (vars == NIL || contain_volatile_functions((Node *) clause) || has_param((Node *) clause, NULL))
		return false;
	foreach_ptr(Var, v, vars)
		if (v->varno != relid || v->varlevelsup != 0 || (v->vartype != TEXTOID && v->vartype != VARCHAROID))
			return false;
	return true;
}

bool
sift_v6_split_quals(MemoryContext cx, PlannerInfo *root, const List *quals, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6SplitQuals *out)
{
	List	   *machine = NIL;

	*out = (SiftV6SplitQuals) {.text = NIL};
	foreach_ptr(Node, clause, quals)
	{
		SiftV6Input scratch = *in;
		SiftV6Vexpr probe;

		if (sift_v6_vexpr_quals(cx, list_make1(clause), relid, params, &scratch, &probe))
			machine = lappend(machine, clause);
		else if (text_clause(clause, relid))
		{
			Node	   *batch = copyObject(clause);

			ChangeVarNodes(batch, relid, SIFT_V6_BQ_RELID, 0);
			if (root != NULL && !sift_v6_bq_plan_ok(root, in, (Expr *) batch))
				return false;
			out->text = lappend(out->text, batch);
		}
		else
			return false;
	}
	out->has_machine = machine != NIL;
	return machine == NIL || sift_v6_vexpr_quals(cx, machine, relid, params, in, &out->machine);
}

void
sift_v6_split_quals_columns(SiftV6Input *in, const SiftV6SplitQuals *sq)
{
	foreach_ptr(Node, clause, sq->text)
		foreach_ptr(Var, v, pull_var_clause(clause, 0))
		{
			const int	slot = sift_v6_input_add_text(in, v);

			SIFT_INVARIANT(slot >= 0, "the source decodes the text columns of its quals");
		}
}

typedef struct QualSource
{
	SiftV6Source src;
	SiftV6Source *inner;
	SiftV6Bq   *bq;
	SiftV6Input in;
	SiftVec		held[SIFT_V6_MAX_COLS];
	SiftBatchSel *sel;
	uint64		mask[SIFT_VEC_WORDS];
} QualSource;

static const SiftVec *
held_col(void *arg, const SiftV6InCol *col)
{
	QualSource *q = arg;

	for (int i = 0; i < q->in.ncols; i++)
		if (q->in.col[i].attno == col->attno)
			return &q->held[i];
	return NULL;
}

static void
take_rows(SiftBatchSel *sel, const SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;

	sel->n_raw = b->n;
	for (int w = 0; w < words; w++)
		sel->mask[w] = b->mask != NULL ? b->mask[w] : (w * 64 + 64 <= b->n ? ~UINT64CONST(0) : (UINT64CONST(1) << (b->n & 63)) - 1);
	sel->n_active = 0;
	for (int w = 0; w < words; w++)
		sel->n_active += (uint16) pg_popcount64(sel->mask[w]);
	sel->all_active = sel->n_active == b->n;
	sel->has_sel = false;
}

static bool
qual_next(SiftV6Source *s, SiftV6Batch *out)
{
	QualSource *q = (QualSource *) s;

	if (!q->inner->next(q->inner, out))
		return false;
	for (int i = 0; i < q->in.ncols; i++)
		q->held[i] = out->col[i];
	take_rows(q->sel, out);
	sift_v6_bq_filter(q->bq, q->sel);
	memcpy(q->mask, q->sel->mask, sizeof(uint64) * ((out->n + 63) / 64));
	out->mask = q->mask;
	return true;
}

static void
qual_skip(SiftV6Source *s, const int64 *part_key, uint8 part_nulls)
{
	QualSource *q = (QualSource *) s;

	if (q->inner->skip != NULL)
		q->inner->skip(q->inner, part_key, part_nulls);
}

static bool
qual_fetch(SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	QualSource *q = (QualSource *) s;

	return q->inner->fetch(q->inner, tag, attnos, n, values, isnull);
}

static void
qual_close(SiftV6Source *s)
{
	QualSource *q = (QualSource *) s;

	q->inner->close(q->inner);
}

SiftV6Source *
sift_v6_source_over_quals(MemoryContext cx, SiftV6Source *inner, const SiftV6Input *in, const SiftV6SplitQuals *sq)
{
	QualSource *q = MemoryContextAllocZero(cx, sizeof(QualSource));

	if (sq->text == NIL)
		return inner;
	q->inner = inner;
	q->in = *in;
	q->sel = MemoryContextAllocAligned(cx, sizeof(SiftBatchSel), 64, MCXT_ALLOC_ZERO);
	q->bq = sift_v6_bq_open(cx, sq->text, &q->in, held_col, q, NULL);
	q->src = (SiftV6Source) {.next = qual_next,.skip = inner->skip != NULL ? qual_skip : NULL,.fetch = qual_fetch,.close = qual_close};
	return &q->src;
}
