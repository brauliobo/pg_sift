/*
 * core/adapt_pipe.c - the adaptive order of the conjuncts of core/adapt_pipe.h.
 */
#include "core/adapt_pipe.h"

/* the nanoseconds a row costs in a stage that has not been measured */
static const double PRIOR_CPU_NS[] = {2, 5, 12, 25, 200};

static double
ewma(double cur, double x)
{
	return cur + SIFT_APIPE_EWMA_ALPHA * (x - cur);
}

static double
rank_of(const SiftAdaptivePipeline *p, const SiftAdaptOp *op, const SiftAdaptOp *prev)
{
	const double w_io = p->io_cold ? SIFT_APIPE_IO_WEIGHT_COLD : SIFT_APIPE_IO_WEIGHT_WARM;
	const double io = prev && op->col >= 0 && prev->col == op->col ? 0 : op->ewma_io_bytes;
	const double drop = 1.0 - op->ewma_pass;

	return (op->ewma_cpu_ns + w_io * io) / (drop > 0.01 ? drop : 0.01);
}

/* The conjuncts that may trade places: the same group of errors, and both of the granule phase or both of the row phase */
static bool
same_block(const SiftAdaptOp *a, const SiftAdaptOp *b)
{
	return a->barrier_group == b->barrier_group && (a->stage == SPSTAGE_SKIP_INDEX) == (b->stage == SPSTAGE_SKIP_INDEX);
}

static int
block_key(const SiftAdaptOp *op)
{
	return (op->stage == SPSTAGE_SKIP_INDEX ? 0 : 256) + op->barrier_group;
}

void
sift_apipe_init(SiftAdaptivePipeline *p)
{
	memset(p, 0, sizeof(*p));
	p->gran_survival = 1.0;
	p->next_explore_at = SIFT_APIPE_EXPLORE_GRAN;
}

uint16
sift_apipe_add(SiftAdaptivePipeline *p, uint16 op_idx, int16 col, SiftPredStage stage, uint8 barrier_group)
{
	SiftAdaptOp *op = &p->ops[p->n_ops];

	Assert(p->n_ops < SIFT_APIPE_MAX_OPS && (stage != SPSTAGE_SKIP_INDEX || barrier_group == 0));
	op->op_idx = op_idx;
	op->col = col;
	op->stage = stage;
	op->barrier_group = barrier_group;
	op->ewma_pass = 0.5;
	op->ewma_cpu_ns = PRIOR_CPU_NS[stage];
	return p->n_ops++;
}

static void
make_next(SiftAdaptivePipeline *p)
{
	p->n_active = 0;
	for (uint16 i = 0; i < p->n_ops; i++)
		if (!p->ops[p->order[i]].disabled)
			p->next[p->n_active++] = p->order[i];
}

void
sift_apipe_start(SiftAdaptivePipeline *p)
{
	for (uint16 i = 0; i < p->n_ops; i++)
	{
		const SiftAdaptOp *op = &p->ops[i];
		uint16		at = i;

		while (at > 0 && (block_key(&p->ops[p->order[at - 1]]) > block_key(op) ||
						  (block_key(&p->ops[p->order[at - 1]]) == block_key(op) &&
						   rank_of(p, &p->ops[p->order[at - 1]], NULL) > rank_of(p, op, NULL))))
		{
			p->order[at] = p->order[at - 1];
			at--;
		}
		p->order[at] = i;
	}
	make_next(p);
}

void
sift_apipe_record(SiftAdaptivePipeline *p, uint16 slot, uint32 rows_in, uint32 rows_out, double cpu_ns, double io_bytes)
{
	SiftAdaptOp *op = &p->ops[slot];
	const double n = rows_in;

	Assert(slot < p->n_ops && rows_out <= rows_in);
	if (rows_in == 0)
		return;
	if (!op->seen)
	{
		op->ewma_pass = rows_out / n;
		op->ewma_cpu_ns = cpu_ns / n;
		op->ewma_io_bytes = io_bytes / n;
		op->seen = true;
	}
	else
	{
		op->ewma_pass = ewma(op->ewma_pass, rows_out / n);
		op->ewma_cpu_ns = ewma(op->ewma_cpu_ns, cpu_ns / n);
		op->ewma_io_bytes = ewma(op->ewma_io_bytes, io_bytes / n);
	}
	op->rows_in += rows_in;
	op->rows_out += rows_out;
	op->early_exits += rows_out == 0;
}

/* Adjacent conjuncts of one block trade places while the later has a rank below SIFT_APIPE_HYSTERESIS of the earlier's */
static void
reorder(SiftAdaptivePipeline *p)
{
	bool		changed = true;
	bool		swapped = false;

	for (uint16 pass = 0; changed && pass < p->n_ops; pass++)
	{
		changed = false;
		for (uint16 i = 0; i + 1 < p->n_ops; i++)
		{
			const SiftAdaptOp *a = &p->ops[p->order[i]],
					   *b = &p->ops[p->order[i + 1]],
					   *prev = i > 0 ? &p->ops[p->order[i - 1]] : NULL;

			if (!same_block(a, b) || !(a->seen || b->seen))
				continue;
			if (rank_of(p, b, prev) < SIFT_APIPE_HYSTERESIS * rank_of(p, a, prev))
			{
				const uint16 t = p->order[i];

				p->order[i] = p->order[i + 1];
				p->order[i + 1] = t;
				changed = swapped = true;
			}
		}
	}
	p->reorders += swapped;
}

/* The conjunct of the cursor runs first of its block for one granule */
static void
explore(SiftAdaptivePipeline *p)
{
	const uint16 want = p->explore_cursor++ % p->n_ops;
	uint16		at = p->n_active,
				first = p->n_active;

	for (uint16 i = 0; i < p->n_active; i++)
	{
		if (p->next[i] == want)
			at = i;
	}
	if (at == p->n_active || p->ops[want].stage == SPSTAGE_SKIP_INDEX)
		return;
	for (uint16 i = at; i > 0 && same_block(&p->ops[p->next[i - 1]], &p->ops[want]); i--)
		first = i - 1;
	if (first == p->n_active)
		return;
	memmove(&p->next[first + 1], &p->next[first], (at - first) * sizeof(uint16));
	p->next[first] = want;
}

void
sift_apipe_granule_done(SiftAdaptivePipeline *p, bool survived)
{
	p->gran_survival = p->granules_seen == 0 ? survived : ewma(p->gran_survival, survived);
	p->granules_seen++;
	if (p->granules_seen == SIFT_APIPE_WARMUP_GRAN ||
		(p->granules_seen > SIFT_APIPE_WARMUP_GRAN && (p->granules_seen - SIFT_APIPE_WARMUP_GRAN) % SIFT_APIPE_EPOCH_GRAN == 0))
		reorder(p);
	make_next(p);
	if (p->n_ops > 1 && p->granules_seen >= p->next_explore_at)
	{
		explore(p);
		p->next_explore_at += SIFT_APIPE_EXPLORE_GRAN;
	}
}

const uint16 *
sift_apipe_order(const SiftAdaptivePipeline *p, uint16 *n)
{
	*n = p->n_active;
	return p->next;
}

void
sift_apipe_set_disabled(SiftAdaptivePipeline *p, uint16 slot, bool disabled)
{
	p->ops[slot].disabled = disabled;
	make_next(p);
}

bool
sift_apipe_dense(const SiftAdaptivePipeline *p)
{
	return p->gran_survival >= SIFT_APIPE_DENSE_SURVIVAL;
}
