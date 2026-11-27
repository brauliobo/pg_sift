/*
 * agg/qual/bqual.c - the quals of the scan that no scan key says, over the vectors of a batch (agg/qual/bqual.h): the clauses the machine of agg/vexpr/vexpr.h runs, then the ones it does not, which
 * PostgreSQL's executor evaluates for the rows that are left (agg/qual/pg.h). A clause of a calendar step of a timestamp column (agg/qual/step.h) is evaluated for the first row of each run of rows
 * in one step, and its verdict holds for the rest of the run.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"

#include "agg/text_qual.h"
#include "agg/qual/bqual.h"
#include "agg/pg_exec.h"
#include "agg/qual/pg.h"
#include "agg/vexpr/str.h"
#include "agg/qual/step.h"
#include "core/popcount.h"

#define BQ_TEXT_MAX		8
#define BQ_STEP_MAX		8

typedef struct BqText
{
	int			slot;			/* the column of the input that holds the strings */
	SiftTextQual q;
} BqText;

/* a clause of a calendar step: the program or the expression that evaluates it, the input column of the step, and the step */
typedef struct BqStep
{
	SiftV6Vexpr vx;
	SiftV6PgQual *pg;
	int			slot;
	SiftV6Step	step;
} BqStep;

struct SiftV6Bq
{
	SiftV6Vexpr vx;
	bool		has_vx;
	int			nstep;
	BqStep		step[BQ_STEP_MAX];
	SiftV6PgQual *pg;
	SiftV6PgQual *pgx;			/* the clauses that need the node's executor */
	int			ntext;			/* the clauses over the bytes of a text column (text_qual.h), which no executor runs */
	BqText		text[BQ_TEXT_MAX];
	SiftVexprCtx *ctx;
	SiftV6Input in;
	const SiftVec *src[SIFT_V6_MAX_COLS];
	SiftVec		col[SIFT_V6_MAX_COLS];
	int32	   *narrow[SIFT_V6_MAX_COLS];
};

static bool
compile(MemoryContext cx, const List *quals, SiftV6Input *in, SiftV6Vexpr *vx)
{
	return sift_store_ops_any() && sift_v6_vexpr_quals(cx, quals, SIFT_V6_BQ_RELID, NULL, in, vx) && in->nmasks == 0 && vx->zone == NULL;
}

/* the machine runs the clause: it has no stable function either (what it reads of the session or the snapshot is not known to a program made with the path), nor a sub-plan or executor parameter (the node's) */
static bool
reads_numeric(Node *n, void *context)
{
	return n != NULL && ((IsA(n, Var) && ((Var *) n)->vartype == NUMERICOID) || expression_tree_walker(n, reads_numeric, context));
}

/* a numeric is not run by the machine: it reads scaled integers and has no NaN, which PostgreSQL's own functions on the datums do */
static bool
machine_runs(const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	SiftV6Vexpr vx;

	return !contain_mutable_functions((Node *) clause) && !sift_v6_pgexpr_exec((const Node *) clause) && !reads_numeric((Node *) clause, NULL) && compile(CurrentMemoryContext, list_make1((void *) clause), &scratch, &vx);
}

/* the clause is a text qual of the bytes, and the input decodes its column as strings: the slot of the column in in, else -1 */
static int
text_slot(SiftV6Input *in, const Expr *clause, SiftTextQual *q)
{
	const Var  *column;

	return sift_text_qual_of((const Node *) clause, SIFT_V6_BQ_RELID, q, &column) ? sift_v6_input_add_text(in, column) : -1;
}

bool
sift_v6_bq_plan_ok(PlannerInfo *root, const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	SiftTextQual q;

	return text_slot(&scratch, clause, &q) >= 0 || machine_runs(in, clause) || sift_v6_pgqual_plan_ok(root, in, clause);
}

int64
sift_v6_bq_step(const Expr *clause)
{
	const Var  *col;
	SiftTextQual q;
	const Var  *column;
	bool		local;

	return sift_v6_pgexpr_exec((const Node *) clause) || sift_text_qual_of((const Node *) clause, SIFT_V6_BQ_RELID, &q, &column) ? 0 :
		sift_v6_calendar_step((const Node *) clause, SIFT_V6_BQ_RELID, &col, &local);
}

/* the clause of a calendar step, opened over the input: the machine runs it when it can, else PostgreSQL's executor */
static void
step_open(SiftV6Bq *bq, MemoryContext cx, Expr *clause, BqStep *st)
{
	const Var  *col;

	sift_v6_step_open(cx, (const Node *) clause, SIFT_V6_BQ_RELID, &st->step, &col);
	if (machine_runs(&bq->in, clause))
	{
		if (!compile(cx, list_make1(clause), &bq->in, &st->vx))
			SIFT_INVARIANT(false, "the batch quals of the aggregate scan of relation %u are not compiled", bq->in.indexoid);
	}
	else
		st->pg = sift_v6_pgqual_open(cx, list_make1(clause), &bq->in, NULL);
	st->slot = sift_v6_input_add_var(&bq->in, col);
	SIFT_INVARIANT(st->slot >= 0, "the scan decodes the column of a calendar step of relation %u", bq->in.indexoid);
}

bool
sift_v6_bq_runs_pg(const SiftV6Input *in, const Expr *clause)
{
	SiftV6Input scratch = *in;
	SiftTextQual q;

	return text_slot(&scratch, clause, &q) < 0 && (sift_v6_pgexpr_exec((const Node *) clause) || !machine_runs(in, clause));
}

SiftV6Bq *
sift_v6_bq_open(MemoryContext cx, const List *quals, const SiftV6Input *in, SiftV6BqCol col, void *arg, const struct SiftV6Exec *exec)
{
	SiftV6Bq   *bq = MemoryContextAllocZero(cx, sizeof(SiftV6Bq));
	MemoryContext old = MemoryContextSwitchTo(cx);
	List	   *machine = NIL,
			   *pg = NIL,
			   *needs_exec = NIL;

	bq->in = *in;
	foreach_ptr(Expr, clause, quals)
		if (bq->ntext < BQ_TEXT_MAX && (bq->text[bq->ntext].slot = text_slot(&bq->in, clause, &bq->text[bq->ntext].q)) >= 0)
			bq->ntext++;
		else if (bq->nstep < BQ_STEP_MAX && sift_v6_bq_step(clause) > 0)
			step_open(bq, cx, clause, &bq->step[bq->nstep++]);
		else if (sift_v6_pgexpr_exec((const Node *) clause))
			needs_exec = lappend(needs_exec, clause);
		else if (machine_runs(in, clause))
			machine = lappend(machine, clause);
		else
			pg = lappend(pg, clause);
	bq->has_vx = machine != NIL;
	if (bq->has_vx && !compile(cx, machine, &bq->in, &bq->vx))
		SIFT_INVARIANT(false, "the batch quals of the aggregate scan of relation %u are not compiled", in->indexoid);
	if (pg != NIL)
		bq->pg = sift_v6_pgqual_open(cx, pg, &bq->in, NULL);
	if (needs_exec != NIL)
	{
		SIFT_INVARIANT(exec != NULL && exec->qual != NULL, "the node of the aggregate scan of relation %u prepared its batch quals that need the executor", in->indexoid);
		bq->pgx = sift_v6_pgqual_open(cx, needs_exec, &bq->in, exec->qual);
	}
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

/* the rows of bs whose string passes the qual; a NULL string passes none */
static void
filter_text(const BqText *t, const SiftVec *v, SiftBatchSel *bs)
{
	const SiftStrView *views = v->data;
	uint16		left = 0;

	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
	{
		uint64		keep = 0;

		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));
			const bool	valid = v->all_valid || ((v->valid[w] >> (r & 63)) & 1);

			keep |= (uint64) (valid && sift_text_qual_holds(&t->q, (const uint8 *) sift_vstr_bytes(&views[r]), views[r].len)) << (r & 63);
		}
		bs->mask[w] = keep;
		left += (uint16) sift_pc64(keep);
	}
	bs->n_active = left;
	bs->all_active = left == bs->n_raw;
	bs->has_sel = false;
}

/* the rows of bs whose run's first row passes the clause */
static void
filter_step(SiftV6Bq *bq, BqStep *st, SiftBatchSel *bs)
{
	SiftBatchSel heads;
	uint16		head_of[SIFT_VEC_LEN];
	uint64		pass[SIFT_VEC_WORDS];
	uint16		left = 0;

	sift_v6_step_runs(&st->step, bq->src[st->slot], bs, &heads, head_of);
	if (st->pg != NULL)
	{
		sift_v6_pgqual_filter(st->pg, &heads, bq->src);
		memcpy(pass, heads.mask, sizeof(pass));
	}
	else
	{
		const SiftV6Batch b = {.n = bs->n_raw,.col = bq->col,.mask = heads.mask};

		bq->col[st->slot] = *bq->src[st->slot];
		bq->col[st->slot].type = bq->in.col[st->slot].type;
		sift_v6_vexpr_filter(bq->ctx, &st->vx, &b, pass);
	}
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
	{
		uint64		keep = 0;

		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			keep |= ((pass[head_of[r] >> 6] >> (head_of[r] & 63)) & 1) << (r & 63);
		}
		bs->mask[w] = keep;
		left += (uint16) sift_pc64(keep);
	}
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
	for (int i = 0; i < bq->nstep && bs->n_active > 0; i++)
		filter_step(bq, &bq->step[i], bs);
	for (int i = 0; i < bq->ntext && bs->n_active > 0; i++)
		filter_text(&bq->text[i], bq->src[bq->text[i].slot], bs);
	if (bq->pg != NULL)
		sift_v6_pgqual_filter(bq->pg, bs, bq->src);
	if (bq->pgx != NULL)
		sift_v6_pgqual_filter(bq->pgx, bs, bq->src);
}
