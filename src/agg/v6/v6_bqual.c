/*
 * agg/v6/v6_bqual.c - the quals of the scan that no scan key says, over the vectors of a batch (agg/v6/v6_bqual.h): the clauses the machine of agg/vexpr.h runs, then the ones it does not, which
 * PostgreSQL's executor evaluates for the rows that are left (agg/v6/v6_pg_qual.h).
 */
#include "postgres.h"

#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"

#include "agg/v6/v6_bqual.h"
#include "agg/v6/v6_pg_qual.h"

struct SiftV6Bq
{
	SiftV6Vexpr vx;
	bool		has_vx;
	SiftV6PgQual *pg;
	SiftVexprCtx *ctx;
	SiftV6Input in;
	const SiftVec *src[SIFT_V6_MAX_COLS];
	SiftVec		col[SIFT_V6_MAX_COLS];
	int32	   *narrow[SIFT_V6_MAX_COLS];
};

static bool
compile(MemoryContext cx, const List *quals, SiftV6Input *in, SiftV6Vexpr *vx)
{
	return sift_v6_format_bound() && sift_v6_vexpr_quals(cx, quals, SIFT_V6_BQ_RELID, NULL, in, vx) && in->nmasks == 0 && vx->zone == NULL;
}

/* the machine runs the clause: it has no stable function either (what it reads of the session or the snapshot is not known to a program made with the path) */
static bool
machine_runs(const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	SiftV6Vexpr vx;

	return !contain_mutable_functions((Node *) clause) && compile(CurrentMemoryContext, list_make1((void *) clause), &scratch, &vx);
}

bool
sift_v6_bq_plan_ok(const SiftV6Input *in, const Expr *clause)
{
	return machine_runs(in, clause) || sift_v6_pgqual_plan_ok(in, clause);
}

SiftV6Bq *
sift_v6_bq_open(MemoryContext cx, const List *quals, const SiftV6Input *in, SiftV6BqCol col, void *arg)
{
	SiftV6Bq   *bq = MemoryContextAllocZero(cx, sizeof(SiftV6Bq));
	MemoryContext old = MemoryContextSwitchTo(cx);
	List	   *machine = NIL,
			   *pg = NIL;

	bq->in = *in;
	foreach_ptr(Expr, clause, quals)
		if (machine_runs(in, clause))
			machine = lappend(machine, clause);
		else
			pg = lappend(pg, clause);
	bq->has_vx = machine != NIL;
	if (bq->has_vx && !compile(cx, machine, &bq->in, &bq->vx))
		SIFT_INVARIANT(false, "the batch quals of the aggregate scan of relation %u are not compiled", in->indexoid);
	if (pg != NIL)
		bq->pg = sift_v6_pgqual_open(cx, pg, &bq->in);
	bq->ctx = sift_vexpr_ctx_new(cx);
	for (int i = 0; i < bq->in.ncols; i++)
	{
		bq->src[i] = col(arg, &bq->in.col[i]);
		SIFT_INVARIANT(bq->src[i] != NULL, "the scan decodes the columns of its batch quals");
		if (bq->in.col[i].type == SVEC_I32)
			bq->narrow[i] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int32) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	MemoryContextSwitchTo(old);
	return bq;
}

static void
filter_machine(SiftV6Bq *bq, SiftBatchSel *bs)
{
	uint64		out[SIFT_VEC_WORDS];
	const SiftV6Batch b = {.n = bs->n_raw,.col = bq->col,.mask = bs->mask};
	uint16		left;

	for (int i = 0; i < bq->in.ncols; i++)
	{
		const SiftV6InCol *c = &bq->in.col[i];

		bq->col[i] = *bq->src[i];
		bq->col[i].type = c->type;
		bq->col[i].scale = c->scale;
		if (c->type == SVEC_I32)
		{
			const int64 *wide = bq->src[i]->data;

			for (uint16 r = 0; r < bs->n_raw; r++)
				bq->narrow[i][r] = (int32) wide[r];
			bq->col[i].data = bq->narrow[i];
		}
	}
	left = sift_v6_vexpr_filter(bq->ctx, &bq->vx, &b, out);
	memcpy(bs->mask, out, sizeof(uint64) * ((bs->n_raw + 63) / 64));
	bs->n_active = left;
	bs->all_active = left == bs->n_raw;
	bs->has_sel = false;
}

void
sift_v6_bq_filter(SiftV6Bq *bq, SiftBatchSel *bs)
{
	if (bs->n_active == 0)
		return;
	if (bq->has_vx)
		filter_machine(bq, bs);
	if (bq->pg != NULL)
		sift_v6_pgqual_filter(bq->pg, bs, bq->src);
}
