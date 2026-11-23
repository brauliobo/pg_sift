/*
 * agg/gpu_v6.c - the Vulkan path of SiftAggV6 (agg/gpu_v6.h, Spec 39): the settings, the choice between the device and the CPU, the program of the query as the device runs it, the batches of
 * parts sent to it and the groups it gives back folded into those of the CPU.
 *
 * The choice (sift.gpu = auto) is a cost (agg/gpu_cost.c, fitted to the measurements of BENCHMARK-RESULTS.md): what the device would take (a fixed start, the kernel over the rows, the readback of
 * the groups, and for chunks it does not hold the copy, the transfer and the decode) against what the CPU scan would take, and the device is used only when it wins by SIFT_GPU_MARGIN. Whatever the device does not finish (lost, timeout, a chunk it cannot decode, a table too small) discards what it folded and the scan runs on the CPU.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "agg/gpu_cost.h"
#include "agg/gpu_emit.h"
#include "agg/gpu_shm.h"
#include "agg/gpu_v6.h"
#include "agg/nth.h"
#include "agg/vk/layout.h"
#include "col/f2/f2_cache.h"

#ifdef SIFT_VULKAN
#define GPU_BUILT			true
#else
#define GPU_BUILT			false
#endif

static const struct config_enum_entry gpu_mode_options[] = {
	{"off", SIFT_GPU_OFF, false},
	{"auto", SIFT_GPU_AUTO, false},
	{"on", SIFT_GPU_ON, false},
	{"true", SIFT_GPU_ON, true},
	{"false", SIFT_GPU_OFF, true},
	{NULL, 0, false}
};

#define GPU_MIN_ROWS_DEFAULT	1000000
#define GPU_CACHE_MB_DEFAULT	256

int			sift_gpu_mode = SIFT_GPU_OFF;
int			sift_gpu_min_rows = GPU_MIN_ROWS_DEFAULT;
char	   *sift_gpu_device = NULL;
int			sift_gpu_cache_mb = GPU_CACHE_MB_DEFAULT;

void
sift_gpu_v6_init_gucs(void)
{
	DefineCustomEnumVariable("sift.gpu", "Runs aggregates and top-N scans on a Vulkan device: off, auto (when it is cheaper) or on.", "A build without Vulkan (make VULKAN=1) runs on the CPU.",
							 &sift_gpu_mode, SIFT_GPU_OFF, gpu_mode_options, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.gpu_min_rows", "Rows below which sift.gpu = auto runs on the CPU without asking what the device would cost.", NULL, &sift_gpu_min_rows,
							GPU_MIN_ROWS_DEFAULT, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.gpu_device", "The Vulkan device sift.gpu uses: its index, a PCI bus and device (BB:DD), or a part of its name. Empty: the best one.", NULL, &sift_gpu_device, "",
							   PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.gpu_cache_mb", "Device memory, in MB, for the decoded column chunks that stay on the Vulkan device between queries (shared by the server).", NULL,
							&sift_gpu_cache_mb, GPU_CACHE_MB_DEFAULT, 16, 1 << 20, PGC_SUSET, GUC_UNIT_MB, NULL, NULL, NULL);
}

#define ROUTER_UNKNOWN_GROUPS		100000	/* modelled: the groups assumed when the planner has no estimate */

#define DENSE_MAX_GROUPS			65536
#define HASH_EST_FACTOR				1.5
#define CACHE_USE					0.75	/* of the device cache one job's decoded chunks may take: none of them is then evicted by the job itself */
#define GRANULES_PER_JOB_SHARED		1024
#define GRANULES_PER_JOB_LOCAL		16384
typedef struct Exec
{
	const SiftGpuQuery *q;
	SiftGpuRun *run;
	SiftGpuEmit *em;
	SiftVkJob	job;
	SiftGpuPlan *plan;
	SiftGpuConn *conn;
	double		groups;
	uint32		granules_per_job;
	uint64		cache_limit;
	SiftVkKey  *keys;
	uint8	   *resident;
} Exec;

/* MEM[MEM-VK-11]: gpu_v6 decline path (sift_gpu_f2_free gives the whole plan back before the CPU scan runs: nothing of the plan stays in the query's context) */
static bool pg_attribute_printf(2, 3)
decline(SiftGpuRun *run, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(run->why, sizeof(run->why), fmt, ap);
	va_end(ap);
	run->used = false;
	if (run->plan != NULL)
	{
		sift_gpu_f2_free(run->plan);
		run->plan = NULL;
	}
	return false;
}

static void
choose_kernel(Exec *x)
{
	const SiftGpuPlan *p = x->plan;
	SiftVkJob  *j = &x->job;
	uint64		span[2],
				total = 1;
	bool		dense = j->nkeys >= 1 && j->nkeys <= 2;

	if (j->mode == SIFT_VK_TOPN)
		return;
	j->mode = j->nkeys == 0 ? SIFT_VK_UNGROUPED : SIFT_VK_HASH;
	x->groups = 1;
	for (uint32 i = 0; i < j->nkeys && dense; i++)
	{
		int			c = x->em->key_slot[i];

		dense = c >= 0 && p->zrange[c] && (uint64) p->zmax[c] - (uint64) p->zmin[c] < DENSE_MAX_GROUPS;
		if (dense)
		{
			span[i] = (uint64) p->zmax[c] - (uint64) p->zmin[c] + 2;
			total *= span[i];
		}
	}
	if (j->nkeys > 0 && dense && total <= DENSE_MAX_GROUPS)
	{
		j->mode = SIFT_VK_DENSE;
		for (uint32 i = 0; i < j->nkeys; i++)
		{
			j->dense_kmin[i] = p->zmin[x->em->key_slot[i]];
			j->dense_span[i] = (uint32) span[i];
		}
		x->groups = (double) total;
	}
	else if (j->nkeys > 0)
	{
		x->groups = x->q->est_groups > 0 ? x->q->est_groups : ROUTER_UNKNOWN_GROUPS;
		j->est_groups = (uint64) (x->q->est_groups > 0 ? x->q->est_groups * HASH_EST_FACTOR : (double) Min(p->rows, (uint64) 1 << 20));
	}
}

static SiftGpuCostIn
cost_in(const Exec *x, double pcie_gbps, double cold_share)
{
	const SiftVkJob *j = &x->job;

	return (SiftGpuCostIn) {.mode = j->mode,.fast = sift_vk_job_is_fast(j),.rows = x->plan->rows,.scan_rows = x->plan->scan_rows,.enc_bytes = x->plan->enc_bytes,.cold_share = cold_share,
		.pcie_gbps = pcie_gbps,.groups = x->groups,.ncols = x->q->ncols,.nops = (int) j->nops,.nkeys = (int) j->nkeys,.naggs = (int) j->naggs,
		.parallel_workers = x->q->parallel_workers};
}

static double
resident_share(Exec *x)
{
	uint64		n = (uint64) x->plan->ngran * x->plan->ncols,
				hit = 0;

	x->keys = palloc(Max(n, 1) * sizeof(SiftVkKey));
	x->resident = palloc0(Max(n, 1));
	sift_gpu_f2_keys(x->plan, x->keys);
	sift_gpu_conn_resident(x->conn, x->keys, (uint32) n, x->resident);
	for (uint64 i = 0; i < n; i++)
		hit += x->resident[i];
	return n == 0 ? 0 : (double) hit / n;
}

/* False, and why in the run, when the CPU is to run the scan: auto asks the cost, on asks only whether the device can */
static bool
route(Exec *x)
{
	SiftGpuRun *run = x->run;
	SiftGpuCostIn in = cost_in(x, SIFT_GPU_PCIE_DEFAULT_GBPS, 0);
	double		cpu = sift_gpu_cost_cpu_ms(&in),
				best = sift_gpu_cost_gpu_ms(&in),
				share,
				pcie;
	const char *why;
	const SiftVkCaps *caps;

	run->cpu_ms = cpu;
	run->gpu_ms = best;
	if (sift_gpu_mode == SIFT_GPU_AUTO && best * SIFT_GPU_MARGIN >= cpu)
		return decline(run, "the CPU is cheaper: %.2f ms against at best %.2f ms on the device (it must win by %.2fx)", cpu, best, SIFT_GPU_MARGIN);
	if ((x->conn = sift_gpu_conn_open(sift_gpu_mode == SIFT_GPU_ON, &why)) == NULL)
		return decline(run, "no usable Vulkan device (%s)", why);
	caps = sift_gpu_conn_caps(x->conn);
	if (caps->software && sift_gpu_mode != SIFT_GPU_ON)
		return decline(run, "the only device is a software renderer, which only sift.gpu = on uses");
	share = resident_share(x);
	pcie = caps->pcie_gbps > 0 ? caps->pcie_gbps : SIFT_GPU_PCIE_DEFAULT_GBPS;
	in = cost_in(x, pcie, 1 - share);
	run->gpu_ms = sift_gpu_cost_gpu_ms(&in);
	if (sift_gpu_mode == SIFT_GPU_AUTO && run->gpu_ms * SIFT_GPU_MARGIN >= cpu)
		return decline(run, "the CPU is cheaper: %.2f ms against %.2f ms on the device with %.0f%% of the chunks on it (it must win by %.2fx)", cpu, run->gpu_ms, share * 100, SIFT_GPU_MARGIN);
	x->cache_limit = caps->cache_bytes > 0 ? (uint64) (caps->cache_bytes * CACHE_USE) : (uint64) sift_gpu_cache_mb * ((uint64) 1 << 20) * 3 / 4;
	x->granules_per_job = sift_gpu_conn_shared(x->conn) ? GRANULES_PER_JOB_SHARED : GRANULES_PER_JOB_LOCAL;
	run->shared = sift_gpu_conn_shared(x->conn);
	run->pcie_gbps = caps->pcie_gbps;
	run->rebar = caps->rebar;
	run->kernel = x->job.mode;
	strlcpy(run->device, caps->name, sizeof(run->device));
	if (x->job.mode == SIFT_VK_HASH && x->job.est_groups * 1.34 * (x->job.nkeys + 3 + 3 * x->job.naggs) * 8 > sift_gpu_conn_result_bytes(x->conn))
		return decline(run, "the groups would not fit the result area of the worker");
	return true;
}

static bool
setup(Exec *x, const char *why)
{
	SiftGpuRun *run = x->run;
	const SiftGpuQuery *q = x->q;

	if (!GPU_BUILT)
		return decline(run, "this build has no Vulkan support (make VULKAN=1)");
	if (why != NULL)
		return decline(run, "%s", why);
	if (sift_gpu_mode == SIFT_GPU_AUTO && sift_plan_cache_get(q->rel, CurrentMemoryContext)->total_rows < (uint64) sift_gpu_min_rows)
		return decline(run, "fewer rows than sift.gpu_min_rows (%d)", sift_gpu_min_rows);
	run->plan = sift_gpu_f2_plan(q, CurrentMemoryContext);
	x->plan = run->plan;
	run->parts = x->plan->ndevice;
	run->parts_all = x->plan->nparts;
	run->dead_rows = x->plan->dead_rows;
	run->granules = x->plan->ngran;
	if (x->plan->ndevice == 0)
		return decline(run, "no part can be read by the device: %s", x->plan->why[0] ? x->plan->why : "the relation has none");
	choose_kernel(x);
	return route(x);
}

static void
raise_program_error(const SiftVkResult *res)
{
	static const struct
	{
		uint32		code;
		int			sqlstate;
		const char *msg;
	}			errors[] = {
		{SIFT_VK_PE_INT4, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "integer out of range"},
		{SIFT_VK_PE_INT8, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "bigint out of range"},
		{SIFT_VK_PE_DIV0, ERRCODE_DIVISION_BY_ZERO, "division by zero"},
		{SIFT_VK_PE_FLOAT_OVF, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "value out of range: overflow"},
		{SIFT_VK_PE_FLOAT_UNF, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "value out of range: underflow"},
		{SIFT_VK_PE_TIME, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "timestamp out of range"}
	};

	for (size_t i = 0; i < lengthof(errors); i++)
		if (errors[i].code == res->err_code)
			ereport(ERROR, (errcode(errors[i].sqlstate), errmsg("%s", errors[i].msg)));
}

/* The end of the job that can be sent: the granules whose decoded chunks fit what one job may take of the cache */
static uint32
job_end(const Exec *x, uint32 g0)
{
	uint64		bytes = 0;
	uint32		g = g0;

	for (; g < x->plan->ngran && g - g0 < x->granules_per_job; g++)
	{
		bytes += x->plan->est_chunk[g];
		if (g > g0 && bytes > x->cache_limit)
			break;
	}
	return g;
}

static void
count_run(SiftGpuRun *run, const SiftVkResult *res)
{
	run->resident += res->chunks_resident;
	run->uploaded += res->chunks_uploaded;
	run->batches += res->batches;
	run->bytes_up += res->bytes_uploaded;
	run->bytes_lz4 += res->bytes_lz4;
	for (size_t i = 0; i < sizeof(SiftVkTimes) / sizeof(double); i++)
		(&run->times.stage_ms)[i] += (&res->times.stage_ms)[i];
}

/* Granules [g0, g1) loaded and run; a chunk the device no longer holds is sent, a table too small is made larger once. *why is set for what the CPU must take over */
static SiftVkStatus
run_range(Exec *x, uint32 g0, uint32 gmax, uint32 *g1, SiftVkResult *res, const char **why)
{
	SiftGpuPlan *p = x->plan;
	uint64		first = (uint64) g0 * p->ncols;
	bool		elide = true,
				grown = false;
	SiftVkStatus st;

	sift_gpu_conn_resident(x->conn, x->keys + first, (gmax - g0) * p->ncols, x->resident + first);
	for (;;)
	{
		if (!sift_gpu_f2_load(p, g0, gmax, sift_gpu_conn_batch_bytes(x->conn), elide ? x->resident : NULL, g1, why))
			return SIFT_VK_DECODE;
		x->job.gran = &p->gran[g0];
		x->job.ngran = *g1 - g0;
		x->job.tab = sift_gpu_f2_tab_key(p, g0, *g1);
		st = sift_gpu_conn_run(x->conn, &x->job, res);
		sift_gpu_f2_unload(p);
		if (st == SIFT_GPU_NEED_BYTES && elide)
			elide = false;
		else if (st == SIFT_VK_OVERFLOW && x->job.mode == SIFT_VK_HASH && !grown)
		{
			x->job.est_groups = Min(x->job.est_groups * 8, Max(p->rows, (uint64) 1024));
			grown = true;
		}
		else
			return st;
	}
}

static const char *
status_why(Exec *x, SiftVkStatus st)
{
	static const char *const why[] = {NULL, "the device was lost", "the device did not answer in time", NULL, NULL,
	"the table of groups or candidates of the device was too small"};

	return (uint32) st < lengthof(why) && why[st] != NULL ? why[st] : sift_gpu_conn_error(x->conn);
}

/* Runs every granule of the plan; NULL when done, else why the CPU must take over (an error of the program is raised) */
static const char *
run_all(Exec *x, void (*take) (Exec *, const SiftVkResult *, void *), void *arg)
{
	uint32		g0 = 0,
				g1;

	while (g0 < x->plan->ngran)
	{
		SiftVkResult res;
		const char *why = NULL;
		SiftVkStatus st = run_range(x, g0, job_end(x, g0), &g1, &res, &why);

		if (st == SIFT_VK_PROG_ERROR && res.has_err)
			raise_program_error(&res);
		if (st != SIFT_VK_OK)
			return why != NULL ? why : status_why(x, st);
		count_run(x->run, &res);
		take(x, &res, arg);
		g0 = g1;
	}
	return NULL;
}

static SiftV6AccIn
acc_in(uint32 kind, const int64 *w)
{
	SiftV6AccIn a = {.n = w[2]};

	if (kind == SIFT_VK_AK_COUNT_STAR || kind == SIFT_VK_AK_COUNT)
		a.n = w[0];
	else if (kind == SIFT_VK_AK_SUM_I)
	{
		__int128	sum = (__int128) w[1] * 4294967296 + (uint64) w[0];

		a.v0 = (uint64) sum;
		a.v1 = (int64) (sum >> 64);
	}
	else if (a.n > 0)
		a.v0 = kind == SIFT_VK_AK_MIN_F || kind == SIFT_VK_AK_MAX_F ? (uint64) w[0] ^ ((uint64) (w[0] >> 63) >> 1) : (uint64) w[0];
	return a;
}

static void
fold_group(const Exec *x, SiftV6Group *g, const int64 *key, uint64 nulls, const int64 *w)
{
	SiftV6AccIn in[SIFT_VK_MAXAGGS];

	for (uint32 a = 0; a < x->job.naggs; a++)
		in[a] = acc_in(x->job.agg[a].kind, w + 3 * a);
	sift_v6_group_fold_rec(g, key, nulls, in);
}

static void
fold_dense(const Exec *x, SiftV6Group *g, const SiftVkResult *res)
{
	const SiftVkJob *j = &x->job;
	uint32		stride = 1 + 3 * j->naggs;
	uint32		digit[2] = {0, 0};	/* the slot of each key of gid: the digits of gid in the spans of the keys, which count up a slot at a time, so that no slot costs a division */

	for (uint32 gid = 0; gid * stride < res->acc_words; gid++)
	{
		const int64 *w = res->acc + (uint64) gid * stride;

		if (w[0] != 0)
		{
			int64		key[2];
			uint64		nulls = 0;

			for (uint32 i = 0; i < j->nkeys; i++)
			{
				const bool	null_slot = digit[i] == j->dense_span[i] - 1;

				key[i] = null_slot ? 0 : j->dense_kmin[i] + (int64) digit[i];
				nulls |= (uint64) null_slot << i;
			}
			fold_group(x, g, key, nulls, w + 1);
		}
		for (int i = (int) j->nkeys - 1; i >= 0 && ++digit[i] == j->dense_span[i]; i--)
			digit[i] = 0;
		if ((gid & 0xFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

typedef struct AggTake
{
	SiftV6Group *g;
	MemoryContext cx;
	TupleDesc	desc;
} AggTake;

static void
take_groups(Exec *x, const SiftVkResult *res, void *arg)
{
	AggTake    *t = arg;
	SiftV6Group *g = t->g != NULL ? t->g : (t->g = sift_v6_group_new(x->q->spec, t->cx, t->desc));

	if (x->job.mode == SIFT_VK_UNGROUPED)
		fold_group(x, g, NULL, 0, res->acc + 1);
	else if (x->job.mode == SIFT_VK_DENSE)
		fold_dense(x, g, res);
	else
		for (uint32 i = 0; i < res->ngroups; i++)
		{
			const int64 *rec = res->acc + (uint64) i * res->rec_words;

			fold_group(x, g, rec, (uint64) rec[x->job.nkeys], rec + x->job.nkeys + 2);
			if ((i & 0xFFF) == 0)
				CHECK_FOR_INTERRUPTS();
		}
}

static void
begin(Exec *x, const SiftGpuQuery *q, SiftGpuRun **runp, MemoryContext cx)
{
	*x = (Exec) {.q = q,.run = MemoryContextAllocZero(cx, sizeof(SiftGpuRun)),.em = MemoryContextAllocZero(cx, sizeof(SiftGpuEmit))};
	sift_gpu_emit_init(x->em);
	*runp = x->run;
}

/* True when the device ran every granule of the plan into take; else the run says why the CPU must scan it all */
static bool
drive(Exec *x, const char *translated, void (*take) (Exec *, const SiftVkResult *, void *), void *arg)
{
	const char *why;
	bool		ok = setup(x, GPU_BUILT ? translated : NULL);

	if (ok && (why = run_all(x, take, arg)) != NULL)
	{
		ok = decline(x->run, "%s", why);
		if (sift_gpu_mode == SIFT_GPU_ON)
			ereport(WARNING, (errmsg("sift.gpu: %s; the scan runs on the CPU", why)));
	}
	sift_gpu_conn_close(x->conn);
	x->run->used = ok;
	return ok;
}

SiftV6Group *
sift_gpu_v6_agg(const SiftGpuQuery *q, MemoryContext gcx, TupleDesc desc, SiftGpuRun **runp)
{
	MemoryContext old = MemoryContextSwitchTo(gcx);
	Exec		x;
	AggTake		t = {.cx = gcx,.desc = desc};

	begin(&x, q, runp, gcx);
	if (sift_gpu_mode != SIFT_GPU_OFF && !drive(&x, sift_gpu_emit_agg(x.em, q, &x.job), take_groups, &t) && t.g != NULL)
	{
		sift_v6_group_free(t.g);
		t.g = NULL;
	}
	MemoryContextSwitchTo(old);
	return t.g;
}

typedef struct TopnTake
{
	SiftGpuCand *cand;
	uint32		n;
	uint32		cap;
	bool		desc;
	bool		nulls_first;
} TopnTake;

static void
take_cand(Exec *x, const SiftVkResult *res, void *arg)
{
	TopnTake   *t = arg;

	for (uint32 i = 0; i < res->ncand; i++)
	{
		int64		meta = res->cand[2 * i + 1];
		SiftGpuCand *c;

		if (t->n == t->cap)
			t->cand = repalloc(t->cand, (t->cap *= 2) * sizeof(SiftGpuCand));
		c = &t->cand[t->n++];
		c->key = res->cand[2 * i];
		c->isnull = (meta >> 62) & 1;
		c->row = (uint32) meta & 0x3FFFFFFF;
		sift_gpu_f2_locate(x->plan, (uint32) ((uint64) meta >> 32) & 0x3FFFFFFF, &c->part_id, &c->part_gen, &c->granule);
	}
}

static int
cand_cmp(const void *a, const void *b, void *arg)
{
	const SiftGpuCand *x = a,
			   *y = b;
	const TopnTake *t = arg;

	if (x->isnull != y->isnull)
		return x->isnull == t->nulls_first ? -1 : 1;
	if (x->key == y->key)
		return 0;
	return (x->key < y->key) == !t->desc ? -1 : 1;
}

bool
sift_gpu_v6_topn(const SiftGpuQuery *q, uint32 k, bool desc, bool nulls_first, bool prunable, MemoryContext cx, SiftGpuRun **runp, SiftGpuCand **cand, uint32 *ncand)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	Exec		x;
	TopnTake	t = {.cap = 1024,.desc = desc,.nulls_first = nulls_first};
	bool		ok;

	begin(&x, q, runp, cx);
	x.job.mode = SIFT_VK_TOPN;
	t.cand = palloc(t.cap * sizeof(SiftGpuCand));
	if (sift_gpu_mode == SIFT_GPU_OFF)
		ok = false;
	else if (prunable)
		ok = decline(x.run, "the zones prune this top-N on the CPU");
	else if ((ok = drive(&x, sift_gpu_emit_topn(x.em, q, &x.job, k, desc, nulls_first), take_cand, &t)) && t.n < k && !nulls_first && x.plan->rows > t.n)
		ok = decline(x.run, "fewer rows than the LIMIT have a sort key, and the rows without one sort last");
	if (ok)
	{
		if (t.n > k && k > 0)
			sift_nth(t.cand, t.n, sizeof(SiftGpuCand), k - 1, cand_cmp, &t);
		sift_nth_sort(t.cand, Min(t.n, k), sizeof(SiftGpuCand), cand_cmp, &t);
	}
	*cand = ok ? t.cand : NULL;
	*ncand = ok ? Min(t.n, k) : 0;
	x.run->used = ok;
	MemoryContextSwitchTo(old);
	return ok;
}

bool
sift_gpu_v6_covers(const SiftGpuRun *run, uint64 part_gen)
{
	return run != NULL && run->used && run->plan != NULL && sift_gpu_f2_covers(run->plan, part_gen);
}

static const char *
kernel_name(SiftVkMode m)
{
	return m == SIFT_VK_UNGROUPED ? "ungrouped" : m == SIFT_VK_DENSE ? "dense" : m == SIFT_VK_HASH ? "hash" : "top-N";
}

void
sift_gpu_v6_explain(const SiftGpuRun *run, ExplainState *es)
{
	if (run == NULL || (!run->used && run->why[0] == '\0'))
		return;
	if (!run->used)
	{
		ExplainPropertyText("GPU", psprintf("not used (%s)", run->why), es);
		return;
	}
	ExplainPropertyText("GPU", psprintf("%s (%s), %s kernel; the CPU would take %.2f ms, the device %.2f ms", run->device, run->shared ? "shared worker" : "backend's own device",
										kernel_name(run->kernel), run->cpu_ms, run->gpu_ms), es);
	ExplainPropertyText("GPU Parts", psprintf("%u of %u parts, %u granules, %llu deleted rows masked", run->parts, run->parts_all, run->granules, (unsigned long long) run->dead_rows), es);
	ExplainPropertyText("GPU Chunks", psprintf("%u on the device, %u sent (%.1f MB, %.1f MB of them LZ4), %u batches", run->resident, run->uploaded, run->bytes_up / 1048576.0,
											   run->bytes_lz4 / 1048576.0, run->batches), es);
	if (es->analyze)
		ExplainPropertyText("GPU Time", psprintf("stage %.3f ms, decode %.3f ms, kernel %.3f ms, readback %.3f ms (wall %.3f ms, of it %.3f ms making kernels)", run->times.stage_ms, run->times.decode_ms,
												 run->times.kernel_ms, run->times.readback_ms, run->times.wall_ms, run->times.compile_ms), es);
}

void
sift_gpu_v6_end(SiftGpuRun *run)
{
	if (run == NULL || run->plan == NULL)
		return;
	sift_gpu_f2_free(run->plan);
	run->plan = NULL;
}
