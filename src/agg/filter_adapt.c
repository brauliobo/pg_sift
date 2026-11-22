/*
 * agg/filter_adapt.c - the learned order of the conjuncts of a filter (agg/filter_adapt.h).
 */
#include "core/host.h"

#include "core/popcount.h"

#include "agg/filter_adapt.h"

double
sift_cost_class_ns(SiftCostClass c)
{
	static const double ns[] = {0.1, 0.2, 0.3, 0.8, 1.2, 1.5, 3.0, 4.0, 15.0};

	return c <= SIFT_CC_STR_BLOCK ? ns[c] : 15.0;
}

double
sift_afilt_rank(const SiftAdaptiveFilter *af, const uint16 *order, int pos)
{
	const SiftConjunctState *s = &af->st[order[pos]];
	double		w = af->io_cold ? SIFT_AFILT_W_IO_COLD : SIFT_AFILT_W_IO_WARM,
				io = s->io;

	for (int p = 0; p < pos; p++)
		if (af->st[order[p]].col == s->col)
		{
			io = 0.0;
			break;
		}
	return (s->cpu + w * io) / Max(0.01, 1.0 - s->pass);
}

static void
swap_pos(uint16 *order, int a, int b)
{
	uint16		t = order[a];

	order[a] = order[b];
	order[b] = t;
}

/* Insertion sort of each group: a conjunct moves ahead of its neighbour only when its rank is below hysteresis times the neighbour's */
static void
sort_groups(SiftAdaptiveFilter *af, double hysteresis)
{
	for (int a = 0; a < af->n; a = af->seg_end[a])
	{
		int			b = af->seg_end[a];

		for (int j = a + 1; j < b; j++)
			for (int k = j; k > a && sift_afilt_rank(af, af->order, k) < sift_afilt_rank(af, af->order, k - 1) * hysteresis; k--)
			{
				swap_pos(af->order, k, k - 1);
				af->n_reorders++;
			}
	}
}

/* The conjuncts of one column side by side, in the place of the first of them: the later ones cost no read */
static void
cluster_columns(SiftAdaptiveFilter *af)
{
	for (int a = 0; a < af->n; a = af->seg_end[a])
	{
		int			b = af->seg_end[a];

		for (int pos = a; pos < b; pos++)
			for (int q = pos + 2; q < b; q++)
				if (af->st[af->order[q]].col == af->st[af->order[pos]].col)
				{
					uint16		moved = af->order[q];

					memmove(&af->order[pos + 2], &af->order[pos + 1], (q - pos - 1) * sizeof(uint16));
					af->order[pos + 1] = moved;
					af->n_reorders++;
					break;
				}
	}
}

static void
set_groups(SiftAdaptiveFilter *af)
{
	int			i = 0;

	while (i < af->n)
	{
		int			j = i + 1;

		if (af->st[i].barrier_group == 0)
			while (j < af->n && af->st[j].barrier_group == 0)
				j++;
		for (int k = i; k < j; k++)
		{
			af->seg_start[k] = i;
			af->seg_end[k] = j;
		}
		i = j;
	}
}

void
sift_afilt_init(SiftAdaptiveFilter *af, const SiftConjunctInit *init, int n, bool io_cold)
{
	*af = (SiftAdaptiveFilter) {.n = Min(n, SIFT_AFILT_MAX),.io_cold = io_cold,.explore_slot = -1,.window = 0xFFFF};
	for (int i = 0; i < af->n; i++)
	{
		af->st[i] = (SiftConjunctState) {.col = init[i].col,.barrier_group = init[i].barrier_group,.pass = Max(0.001, Min(0.99, init[i].pass_prior)),
		.cpu = init[i].cpu_ns_row,.io = init[i].io_bytes_row};
		af->order[i] = i;
	}
	set_groups(af);
	sort_groups(af, 1.0);
	cluster_columns(af);
	af->n_reorders = 0;
	memcpy(af->cur, af->order, sizeof(af->order));
}

const uint16 *
sift_afilt_order(const SiftAdaptiveFilter *af)
{
	return af->cur;
}

bool
sift_afilt_sample_cpu(const SiftAdaptiveFilter *af)
{
	return af->granules_seen < SIFT_AFILT_WARMUP || af->granules_seen % SIFT_AFILT_SAMPLE == 0;
}

static double
ewma(double old, double sample, bool seen)
{
	return seen ? old + SIFT_AFILT_ALPHA * (sample - old) : sample;
}

void
sift_afilt_record(SiftAdaptiveFilter *af, int slot, uint32 rows_in, uint32 rows_out, double cpu_ns, uint32 io_bytes, uint32 granule_rows)
{
	SiftConjunctState *s;

	if (slot < 0 || slot >= af->n || rows_in == 0)
		return;
	s = &af->st[slot];
	s->pass = ewma(s->pass, (double) rows_out / rows_in, s->seen);
	if (cpu_ns > 0.0)
	{
		s->cpu = ewma(s->cpu, cpu_ns / rows_in, s->seen_cpu);
		s->seen_cpu = true;
	}
	if (io_bytes > 0 && granule_rows > 0)
		s->io = ewma(s->io, (double) io_bytes / granule_rows, s->seen);
	s->seen = true;
	s->total_rows_in += rows_in;
	s->total_rows_out += rows_out;
}

void
sift_afilt_skipped(SiftAdaptiveFilter *af, int slot)
{
	if (slot >= 0 && slot < af->n)
		af->st[slot].total_skipped++;
}

/* The next conjunct, from the cursor on, that is free to move and is not at the front of its group */
static int
pick_explore(SiftAdaptiveFilter *af)
{
	for (int k = 0; k < af->n; k++)
	{
		int			pos = (af->explore_cursor + k) % af->n;

		if (af->st[af->order[pos]].barrier_group == 0 && pos != af->seg_start[pos])
		{
			af->explore_cursor = (pos + 1) % af->n;
			return pos;
		}
	}
	return -1;
}

bool
sift_afilt_dense(const SiftAdaptiveFilter *af)
{
	return sift_pc64(af->window) * 2 >= SIFT_AFILT_WINDOW;
}

void
sift_afilt_granule_done(SiftAdaptiveFilter *af, bool survived)
{
	int			pos = -1;

	af->window = (af->window << 1) | (survived ? 1U : 0U);
	af->granules_seen++;
	if (af->granules_seen == SIFT_AFILT_WARMUP || (af->granules_seen > SIFT_AFILT_WARMUP && (af->granules_seen - SIFT_AFILT_WARMUP) % SIFT_AFILT_EPOCH == 0))
	{
		sort_groups(af, SIFT_AFILT_HYSTERESIS);
		cluster_columns(af);
	}
	memcpy(af->cur, af->order, sizeof(af->order));
	af->explore_slot = -1;
	if (af->n > 1 && af->granules_seen % SIFT_AFILT_EXPLORE == 0)
		pos = pick_explore(af);
	if (pos >= 0)
	{
		int			front = af->seg_start[pos];
		uint16		moved = af->order[pos];

		memmove(&af->cur[front + 1], &af->order[front], (pos - front) * sizeof(uint16));
		af->cur[front] = moved;
		af->explore_slot = moved;
	}
}
