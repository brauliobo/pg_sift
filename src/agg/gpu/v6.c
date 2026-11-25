/*
 * agg/gpu/v6.c - the Vulkan path of SiftAggV6 (agg/gpu/v6.h, Spec 39): the settings, the choice between the device and the CPU, the program of the query as the device runs it, the batches of
 * parts sent to it and the groups it gives back folded into those of the CPU.
 *
 * The choice (sift.gpu = auto) is a cost (agg/gpu/cost.c, fitted to the measurements of BENCHMARK-RESULTS.md): what the device would take (a fixed start, the kernel over the rows, the readback of
 * the groups, and for chunks it does not hold the copy, the transfer and the decode) against what the CPU scan would take, and the device is used only when it wins by SIFT_GPU_MARGIN. Whatever the device does not finish (lost, timeout, a chunk it cannot decode, a table too small) discards what it folded and the scan runs on the CPU.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/gpu/cost.h"
#include "agg/gpu/dict_agg.h"
#include "agg/gpu/emit.h"
#include "agg/gpu/shm.h"
#include "agg/gpu/v6.h"
#include "agg/nth.h"
#include "agg/vk/layout.h"

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
							GPU_MIN_ROWS_DEFAULT, 0, INT_MAX, PGC_SUSET, 0, NULL, NULL, NULL);
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
#define TOPK_MAX					1024	/* the K of an ORDER BY .. LIMIT K the device keeps (agg/vk/run.c reads back about 4K groups) */
typedef struct Exec
{
	const SiftGpuQuery *q;
	SiftGpuRun *run;
	SiftGpuEmit *em;
	SiftVkJob	job;			/* the aggregates of the spec but COUNT(DISTINCT), by the keys */
	SiftVkJob	djob;			/* COUNT(DISTINCT): the pairs (the keys, the value) the job counts, run before job */
	SiftVkJob	prime;			/* counts the rows: what brings the chunks of a job that must run whole to the device */
	bool		dist;			/* the spec counts distinct values */
	bool		main_job;		/* job runs: not an ungrouped COUNT(DISTINCT) alone */
	int64	   *dcount;			/* dist: of each dense group of the keys, or of the one group: the pairs the finish of djob counts (SIFT_VK_COUNT_BITS) */
	uint32		dgroups;
	uint64		cache_bytes;	/* the ceiling of the cache of the device (sift.gpu_cache_mb as it came to) */
	uint64		tables;			/* the largest table of the jobs: the cache is what the envelope of the device leaves of its ceiling besides it */
	SiftGpuPlan *plan;
	SiftGpuUnits *units;		/* a query that reads a text: the scan in the pieces the dictionaries cut it in (NULL: one piece, the plan) */
	const SiftGpuUnit *unit;	/* the one that runs */
	uint32		glo;			/* the granules of the plan that run: [glo, ghi) */
	uint32		ghi;
	SiftGpuConn *conn;
	double		groups;
	double		pairs;			/* dist: the pairs (the keys, the value) there can be */
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
		run->plan->ops->free(run->plan);
		run->plan = NULL;
	}
	return false;
}

/* A binned instant that may be infinite raises on the device where PostgreSQL bins it to itself, and the first key of a query whose other keys are it plus a constant must keep them from overflowing: the zones of the plan say */
static const char *
keys_out_of_range(const Exec *x)
{
	const SiftV6Spec *s = x->q->spec;
	const SiftGpuPlan *p = x->plan;

	for (int b = 0; b < s->nbins; b++)
	{
		const int	c = sift_gpu_query_slot(x->q, s->key[s->bin[b].key].attno);

		if (c < 0 || !p->zrange[c] || p->zmin[c] == PG_INT64_MIN || p->zmax[c] == PG_INT64_MAX)
			return "a binned timestamp may be infinite";
	}
	if (s->affine)
	{
		const int	c = sift_gpu_query_slot(x->q, s->key[0].attno);

		if (c < 0 || !p->zrange[c] || p->zmin[c] < s->aff_lo || p->zmax[c] > s->aff_hi)
			return "a key whose offsets may overflow";
	}
	return NULL;
}

/* The end of the job that can be sent: the granules whose decoded chunks fit what one job may take of the cache */
static uint32
job_end(const Exec *x, uint32 g0)
{
	uint64		bytes = 0;
	uint32		g = g0;

	for (; g < x->ghi && g - g0 < x->granules_per_job; g++)
	{
		bytes += x->plan->est_chunk[g];
		if (g > g0 && bytes > x->cache_limit)
			break;
	}
	return g;
}

/* The group keys as the dense number of a DENSE job: the span of each (its values and the slot of NULL) from the zones of the plan; the groups there are, 0 when they are too many or a key has no zone */
static uint64
dense_groups(const Exec *x, uint64 *span)
{
	const SiftGpuPlan *p = x->plan;
	uint64		total = 1;

	if (x->job.nkeys < 1 || x->job.nkeys > 2)
		return 0;
	for (uint32 i = 0; i < x->job.nkeys; i++)
	{
		const int	c = x->em->key_slot[i];

		if (c < 0 || !p->zrange[c] || (uint64) p->zmax[c] - (uint64) p->zmin[c] >= DENSE_MAX_GROUPS)
			return 0;
		span[i] = (uint64) p->zmax[c] - (uint64) p->zmin[c] + 2;
		total *= span[i];
	}
	return total <= DENSE_MAX_GROUPS ? total : 0;
}

static void
choose_kernel(Exec *x)
{
	const SiftGpuPlan *p = x->plan;
	SiftVkJob  *j = &x->job;
	uint64		span[2],
				total;

	if (j->mode == SIFT_VK_TOPN)
		return;
	j->mode = j->nkeys == 0 ? SIFT_VK_UNGROUPED : SIFT_VK_HASH;
	x->groups = 1;
	if ((total = dense_groups(x, span)) > 0)
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
		j->est_groups = Min(p->rows, (uint64) (x->q->est_groups > 0 ? x->q->est_groups * HASH_EST_FACTOR : (double) (1 << 20)));
	}
}

/* The bytes of the table of a hash job on the device (agg/vk/job.c sizes it the same way) */
static uint64
table_bytes(const SiftVkJob *j)
{
	uint64		slots = 4096;

	while (slots < j->est_groups * 2)
		slots <<= 1;
	return slots * (j->nkeys + 3 + 3 * j->naggs) * 8;
}

static void
make_prime(Exec *x)
{
	SiftVkJob  *p = &x->prime;

	*p = x->job;
	p->mode = SIFT_VK_UNGROUPED;
	p->nkeys = 0;
	p->naggs = 1;
	p->agg[0] = (SiftVkAgg) {.kind = SIFT_VK_AK_COUNT_STAR,.vreg = SIFT_VK_NONE8,.freg = SIFT_VK_NONE8};
	p->finish = SIFT_VK_FIN_GROUPS;
}

/*
 * What the job ends with. COUNT(DISTINCT v) is a second job, djob, of the pairs (the group keys, v): a hash table of them that its finish counts (the number of pairs, or of each dense group of the keys), so
 * that the groups of the main job, when it has aggregates besides, are given the count. The best K of the groups of a hash job are all the node reads of its table when the order is of the keys and counts.
 * Whether one job can have both is known when the device is (confirm_finish).
 */
static const char *
choose_finish(Exec *x)
{
	const SiftGpuPlan *p = x->plan;
	SiftVkJob  *j = &x->job,
			   *d = &x->djob;
	uint64		span[2],
				pairs;
	int			c = x->em->dist_slot;

	x->dist = c >= 0;
	x->main_job = j->naggs > 0;
	if (!x->dist)
	{
		if (j->mode == SIFT_VK_HASH && x->q->topk.k > 0 && x->q->topk.k <= TOPK_MAX)
		{
			j->finish = SIFT_VK_FIN_TOPK;
			j->topk = x->q->topk;
			make_prime(x);
		}
		return NULL;
	}
	make_prime(x);
	x->dgroups = 1;
	if (j->nkeys > 0 && (x->dgroups = (uint32) dense_groups(x, span)) == 0)
		return "COUNT(DISTINCT) of the groups of keys that are not dense numbers";
	*d = *j;
	d->mode = SIFT_VK_HASH;
	d->naggs = 0;
	d->key_reg[d->nkeys++] = (uint8) x->em->dist_reg;
	d->finish = j->nkeys == 0 ? SIFT_VK_FIN_COUNT : SIFT_VK_FIN_DENSE_COUNT;
	pairs = p->rows;
	if (p->zrange[c] && (uint64) p->zmax[c] - (uint64) p->zmin[c] + 2 <= p->rows / x->dgroups)
		pairs = ((uint64) p->zmax[c] - (uint64) p->zmin[c] + 2) * x->dgroups;
	d->est_groups = pairs * 2 / 3 + 1024;
	x->pairs = (double) pairs;
	x->dcount = palloc0(x->dgroups * sizeof(int64));
	return NULL;
}

static SiftGpuCostIn
cost_in(const Exec *x, const SiftVkJob *j, double pcie_gbps, double cold_share)
{
	const bool	of_pairs = j == &x->djob;

	return (SiftGpuCostIn) {.mode = j->mode,.finish = j->finish,.fast = sift_vk_job_is_fast(j),.rows = x->plan->rows,.scan_rows = x->plan->scan_rows,.enc_bytes = x->plan->enc_bytes,
		.cold_share = cold_share,.pcie_gbps = pcie_gbps,.groups = of_pairs ? x->pairs : x->groups,.table_words = j->mode == SIFT_VK_HASH ? (double) table_bytes(j) / 8 : 0,
		.dist_pairs = x->dist && !of_pairs ? x->pairs : 0,.ncols = x->q->ncols,.nops = (int) j->nops,.nkeys = (int) j->nkeys,.naggs = (int) j->naggs,
		.njobs = sift_gpu_units_to_run(x->units),.parallel_workers = x->q->parallel_workers};
}

/* The time the jobs take on the device: the aggregates, and the pairs of COUNT(DISTINCT) after them, which find the chunks on it */
static double
device_ms(const Exec *x, double pcie_gbps, double cold_share)
{
	SiftGpuCostIn in = cost_in(x, &x->job, pcie_gbps, cold_share);
	double		ms = x->main_job || !x->dist ? sift_gpu_cost_gpu_ms(&in) : 0;

	if (x->dist)
	{
		in = cost_in(x, &x->djob, pcie_gbps, x->main_job ? 0 : cold_share);
		ms += sift_gpu_cost_gpu_ms(&in);
	}
	return ms;
}

static double
resident_share(Exec *x)
{
	uint64		n = (uint64) x->plan->ngran * x->plan->ncols,
				hit = 0;

	x->keys = palloc(Max(n, 1) * sizeof(SiftVkKey));
	x->resident = palloc0(Max(n, 1));
	x->plan->ops->keys(x->plan, x->keys);
	sift_gpu_conn_resident(x->conn, x->tables, x->keys, (uint32) n, x->resident);
	for (uint64 i = 0; i < n; i++)
		hit += x->resident[i];
	return n == 0 ? 0 : (double) hit / n;
}

/* The room the tables of the jobs have in the memory of the worker: its peak less the fixed part and the least cache a job runs with; the cache the sift.gpu_cache_mb asks for is a ceiling that gives way to a table */
static uint64
table_room(void)
{
	return SIFT_VK_VRAM_ENVELOPE - SIFT_VK_VRAM_FIXED - SIFT_VK_CACHE_MIN;
}

static bool
in_envelope(const SiftVkJob *j)
{
	return table_bytes(j) <= table_room();
}

/* The cache the device runs the jobs with: its ceiling, or what the envelope leaves of it besides the largest table (agg/vk/cache.c sift_vk_fit_cache) */
static uint64
job_cache_bytes(const Exec *x)
{
	const uint64 room = SIFT_VK_VRAM_ENVELOPE - SIFT_VK_VRAM_FIXED - Min(x->tables, table_room());

	return Min(x->cache_bytes, room / SIFT_VK_SLAB_BYTES * SIFT_VK_SLAB_BYTES);
}

/* The largest table of the jobs, which every job of the scan names so that the cache is sized once and the chunks the first of them brings stay for the last */
static void
set_tables(Exec *x)
{
	x->tables = sift_vk_job_tables(&x->job);
	if (x->dist)
		x->tables = Max(x->tables, sift_vk_job_tables(&x->djob));
	x->job.tables = x->djob.tables = x->prime.tables = x->tables;
}

/* False, and why in the run, when the CPU is to run the scan: auto asks the cost, on asks only whether the device can */
static bool
route(Exec *x)
{
	SiftGpuRun *run = x->run;
	SiftGpuCostIn in = cost_in(x, &x->job, SIFT_GPU_PCIE_DEFAULT_GBPS, 0);
	double		cpu = sift_gpu_cost_cpu_ms(&in),
				best = device_ms(x, SIFT_GPU_PCIE_DEFAULT_GBPS, 0),
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
	run->gpu_ms = device_ms(x, pcie, 1 - share);
	if (sift_gpu_mode == SIFT_GPU_AUTO && run->gpu_ms * SIFT_GPU_MARGIN >= cpu)
		return decline(run, "the CPU is cheaper: %.2f ms against %.2f ms on the device with %.0f%% of the chunks on it (it must win by %.2fx)", cpu, run->gpu_ms, share * 100, SIFT_GPU_MARGIN);
	x->cache_bytes = caps->cache_bytes > 0 ? caps->cache_bytes : (uint64) sift_gpu_cache_mb * ((uint64) 1 << 20);
	x->cache_limit = (uint64) (job_cache_bytes(x) * CACHE_USE);
	x->granules_per_job = sift_gpu_conn_shared(x->conn) ? GRANULES_PER_JOB_SHARED : GRANULES_PER_JOB_LOCAL;
	run->shared = sift_gpu_conn_shared(x->conn);
	run->pcie_gbps = caps->pcie_gbps;
	run->rebar = caps->rebar;
	run->kernel = x->job.mode;
	strlcpy(run->device, caps->name, sizeof(run->device));
	return true;
}

/* One job cannot hold the groups apart from the rows the CPU still scans, or the granules the device reads in more than one job: a count of distinct values and the best K of the groups need all of them */
static bool
confirm_finish(Exec *x)
{
	SiftVkJob  *j = &x->job;
	const bool	whole = !x->plan->cpu_rest && job_end(x, x->glo) >= x->ghi;

	if (x->dist)
	{
		if (!whole)
			return decline(x->run, "COUNT(DISTINCT) needs every granule in one job and none of the rows left to the CPU");
		if (!in_envelope(&x->djob))
			return decline(x->run, "the %llu MiB of pairs of COUNT(DISTINCT) would not fit the %llu MiB that the device memory leaves besides its least cache",
						   (unsigned long long) (table_bytes(&x->djob) >> 20), (unsigned long long) (table_room() >> 20));
		x->run->kernel = x->main_job ? j->mode : SIFT_VK_HASH;
		return true;
	}
	if (j->finish == SIFT_VK_FIN_TOPK && !(whole && in_envelope(j)))
	{
		j->finish = SIFT_VK_FIN_GROUPS;
		if (j->est_groups * 1.34 * (j->nkeys + 3 + 3 * j->naggs) * 8 > sift_gpu_conn_result_bytes(x->conn))
			return decline(x->run, "the groups would not fit the result area of the worker, and the best %u of them cannot be had on the device: %s (table %llu MiB of the %llu MiB the device memory leaves)",
						   x->q->topk.k, whole ? "the table would not fit its memory" : "the chunks do not all fit one job", (unsigned long long) (table_bytes(j) >> 20),
						   (unsigned long long) (table_room() >> 20));
	}
	if (j->mode == SIFT_VK_HASH && j->finish == SIFT_VK_FIN_GROUPS &&
		j->est_groups * 1.34 * (j->nkeys + 3 + 3 * j->naggs) * 8 > sift_gpu_conn_result_bytes(x->conn))
		return decline(x->run, "the groups would not fit the result area of the worker");
	x->run->kernel = j->mode;
	return true;
}

static bool
setup(Exec *x, const char *why)
{
	SiftGpuRun *run = x->run;
	const SiftGpuQuery *q = x->q;
	const char *range;

	if (!GPU_BUILT)
		return decline(run, "this build has no Vulkan support (make VULKAN=1)");
	if (why != NULL)
		return decline(run, "%s", why);
	if (sift_gpu_mode == SIFT_GPU_AUTO && q->live_rows < (double) sift_gpu_min_rows)
		return decline(run, "fewer rows than sift.gpu_min_rows (%d)", sift_gpu_min_rows);
	run->plan = q->ops->plan(q, CurrentMemoryContext);
	x->plan = run->plan;
	run->parts = x->plan->ndevice;
	run->parts_all = x->plan->nparts;
	run->dead_rows = x->plan->dead_rows;
	run->granules = x->plan->ngran;
	if (x->plan->ndevice == 0)
		return decline(run, "no part can be read by the device: %s", x->plan->why[0] ? x->plan->why : "the relation has none");
	x->glo = 0;
	x->ghi = x->plan->ngran;
	if ((x->units = sift_gpu_units(q, x->plan, CurrentMemoryContext, &range)) == NULL && range != NULL)
		return decline(run, "%s", range);
	if (q->spec != NULL && (range = keys_out_of_range(x)) != NULL)
		return decline(run, "%s", range);
	choose_kernel(x);
	if (q->spec != NULL && (range = choose_finish(x)) != NULL)
		return decline(run, "%s", range);
	if (x->units != NULL && x->units->n > 1 && x->dist)
		return decline(run, "COUNT(DISTINCT) over the pieces the dictionaries of the parts cut the scan in");
	if (x->units != NULL && x->units->n > 1 && x->job.finish == SIFT_VK_FIN_TOPK)
		x->job.finish = SIFT_VK_FIN_GROUPS;
	set_tables(x);
	if (!route(x))
		return false;
	return q->spec == NULL || confirm_finish(x);
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

/* Granules [g0, g1) loaded and run as job; a chunk the device no longer holds is sent, a table too small is made larger once (as far as the memory of the device allows it of a job that ends with less than the groups). *why is set for what the CPU must take over */
static SiftVkStatus
run_range(Exec *x, SiftVkJob *job, uint32 g0, uint32 gmax, uint32 *g1, SiftVkResult *res, const char **why)
{
	SiftGpuPlan *p = x->plan;
	uint64		first = (uint64) g0 * p->ncols;
	bool		elide = true,
				grown = false;
	SiftVkStatus st;

	sift_gpu_conn_resident(x->conn, job->tables, x->keys + first, (gmax - g0) * p->ncols, x->resident + first);
	for (;;)
	{
		SiftVkJob	larger = *job;

		if (!p->ops->load(p, g0, gmax, sift_gpu_conn_batch_bytes(x->conn), elide ? x->resident : NULL, g1, why))
			return SIFT_VK_DECODE;
		job->gran = &p->gran[g0];
		job->ngran = *g1 - g0;
		p->ops->tab_key(p, g0, *g1, &job->tab);
		st = sift_gpu_conn_run(x->conn, job, res);
		p->ops->unload(p);
		larger.est_groups = Min(job->est_groups * 8, Max(p->rows, (uint64) 1024));
		if (st == SIFT_GPU_NEED_BYTES && elide)
			elide = false;
		else if (st == SIFT_VK_OVERFLOW && job->mode == SIFT_VK_HASH && !grown && (job->finish == SIFT_VK_FIN_GROUPS || in_envelope(&larger)))
		{
			job->est_groups = larger.est_groups;
			job->tables = sift_vk_job_tables(job);
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

/* Runs every granule of the plan as job; NULL when done, else why the CPU must take over (an error of the program is raised) */
static const char *
run_all(Exec *x, SiftVkJob *job, void (*take) (Exec *, const SiftVkResult *, void *), void *arg)
{
	uint32		g0 = x->glo,
				g1;

	while (g0 < x->ghi)
	{
		SiftVkResult res;
		const char *why = NULL;
		SiftVkStatus st = run_range(x, job, g0, job_end(x, g0), &g1, &res, &why);

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

static void
take_none(Exec *x, const SiftVkResult *res, void *arg)
{
}

static bool
all_resident(const Exec *x)
{
	const uint32 first = x->glo * x->plan->ncols,
				n = (x->ghi - x->glo) * x->plan->ncols;

	sift_gpu_conn_resident(x->conn, x->tables, x->keys + first, n, x->resident + first);
	for (uint32 i = first; i < first + n; i++)
		if (!x->resident[i])
			return false;
	return true;
}

/*
 * The job over every granule of the plan in one run, for a job whose table is read once, at its end: a job takes only the chunks of its range that fit a staging buffer, and the table of a run of the first
 * of them would be that of a part of the rows. The chunks the device does not hold are brought to it first by a job that counts the rows.
 */
static const char *
run_whole(Exec *x, SiftVkJob *job, void (*take) (Exec *, const SiftVkResult *, void *), void *arg)
{
	SiftVkResult res;
	const char *why = NULL;
	SiftVkStatus st;
	uint32		g1;

	if (x->ghi == x->glo)
		return NULL;
	if (!all_resident(x) && (why = run_all(x, &x->prime, take_none, NULL)) != NULL)
		return why;
	st = run_range(x, job, x->glo, x->ghi, &g1, &res, &why);
	if (st == SIFT_VK_PROG_ERROR && res.has_err)
		raise_program_error(&res);
	if (st != SIFT_VK_OK)
		return why != NULL ? why : status_why(x, st);
	count_run(x->run, &res);
	if (g1 < x->ghi)
		return "the chunks of the scan did not all stay on the device";
	take(x, &res, arg);
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

/* One group of the device into the groups: w holds the states of the aggregates of the job (NULL when it ran none), distinct the pairs COUNT(DISTINCT) counts of the group */
static void
fold_group(const Exec *x, SiftV6Group *g, const int64 *key, uint64 nulls, const int64 *w, int64 distinct)
{
	SiftV6AccIn in[SIFT_VK_MAXAGGS];
	SiftV6StrIn str[SIFT_VK_MAXKEYS];

	for (int a = 0; a < x->q->spec->naggs; a++)
	{
		const int	m = x->em->amap[a];

		in[a] = m >= 0 ? acc_in(x->job.agg[m].kind, w + 3 * m) : (SiftV6AccIn) {.v0 = (uint64) distinct};
	}
	sift_v6_group_fold_rec(g, key, sift_gpu_key_strings(x->q, x->plan, x->units, x->unit, x->job.nkeys, x->em->key_slot, key, nulls, str), nulls, in);
}

static int64
distinct_of(const Exec *x, uint32 gid)
{
	return x->dist ? x->dcount[gid] & (((int64) 1 << SIFT_VK_COUNT_BITS) - 1) : 0;
}

/* The groups of a DENSE job (or, with no aggregate beside COUNT(DISTINCT), the groups that have a pair): gid counts the digits of the keys up, a slot at a time, so that no slot costs a division */
static void
fold_dense(const Exec *x, SiftV6Group *g, const SiftVkResult *res)
{
	const SiftVkJob *j = &x->job;
	const uint32 stride = 1 + 3 * j->naggs,
				ngid = x->main_job ? res->acc_words / stride : x->dgroups;
	uint32		digit[2] = {0, 0};

	for (uint32 gid = 0; gid < ngid; gid++)
	{
		const int64 *w = x->main_job ? res->acc + (uint64) gid * stride : NULL;

		if (x->main_job ? w[0] != 0 : x->dcount[gid] != 0)
		{
			int64		key[2];
			uint64		nulls = 0;

			for (uint32 i = 0; i < j->nkeys; i++)
			{
				const bool	null_slot = digit[i] == j->dense_span[i] - 1;

				key[i] = null_slot ? 0 : j->dense_kmin[i] + (int64) digit[i];
				nulls |= (uint64) null_slot << i;
			}
			fold_group(x, g, key, nulls, w != NULL ? w + 1 : NULL, distinct_of(x, gid));
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

static SiftV6Group *
take_group_set(Exec *x, AggTake *t)
{
	return t->g != NULL ? t->g : (t->g = sift_v6_group_new(x->q->spec, t->cx, t->desc));
}

static void
take_groups(Exec *x, const SiftVkResult *res, void *arg)
{
	SiftV6Group *g = take_group_set(x, arg);

	if (x->job.mode == SIFT_VK_UNGROUPED)
		fold_group(x, g, NULL, 0, res->acc + 1, distinct_of(x, 0));
	else if (x->job.mode == SIFT_VK_DENSE)
		fold_dense(x, g, res);
	else
		for (uint32 i = 0; i < res->ngroups; i++)
		{
			const int64 *rec = res->acc + (uint64) i * res->rec_words;

			fold_group(x, g, rec, (uint64) rec[x->job.nkeys], rec + x->job.nkeys + 2, 0);
			if ((i & 0xFFF) == 0)
				CHECK_FOR_INTERRUPTS();
		}
}

/* djob of the whole plan in one job: its words stay (the result is read back into a buffer the next job reuses) */
static void
take_distinct(Exec *x, const SiftVkResult *res, void *arg)
{
	if (x->job.nkeys == 0)
		x->dcount[0] = (int64) res->distinct;
	else
		memcpy(x->dcount, res->acc, x->dgroups * sizeof(int64));
}

/* The aggregates, and the counts of distinct values apart from them */
static const char *
run_aggs(Exec *x, void *arg)
{
	AggTake    *t = arg;
	const char *why;

	if (x->dist && (why = run_whole(x, &x->djob, take_distinct, NULL)) != NULL)
		return why;
	if (x->main_job)
		return x->job.finish == SIFT_VK_FIN_TOPK ? run_whole(x, &x->job, take_groups, t) : run_all(x, &x->job, take_groups, t);
	if (x->ghi > x->glo && x->job.nkeys == 0)
		fold_group(x, take_group_set(x, t), NULL, 0, NULL, distinct_of(x, 0));
	else if (x->ghi > x->glo)
		fold_dense(x, take_group_set(x, t), NULL);
	return NULL;
}

/* The pieces of the scan one after the other: each with the program of its WHERE, its codes meaning what its parts' dictionaries say */
static const char *
run_units(Exec *x, void *arg)
{
	for (uint32 i = 0; x->units != NULL && i < x->units->n; i++)
	{
		SiftGpuQuery qu = *x->q;
		const char *why;

		x->unit = &x->units->unit[i];
		if (x->unit->skip)
			continue;
		x->glo = x->unit->g0;
		x->ghi = x->unit->g1;
		if (x->q->dict != NULL && x->q->dict->where != NULL)
			qu.where = &x->unit->where;
		sift_gpu_emit_init(x->em);
		if ((why = sift_gpu_emit_agg(x->em, &qu, &x->job)) != NULL || (why = run_aggs(x, arg)) != NULL)
			return why;
	}
	return x->units != NULL ? NULL : run_aggs(x, arg);
}

static void
begin(Exec *x, const SiftGpuQuery *q, SiftGpuRun **runp, MemoryContext cx)
{
	*x = (Exec) {.q = q,.run = MemoryContextAllocZero(cx, sizeof(SiftGpuRun)),.em = MemoryContextAllocZero(cx, sizeof(SiftGpuEmit))};
	sift_gpu_emit_init(x->em);
	*runp = x->run;
}

/* True when the device ran every granule of the plan through run; else the run says why the CPU must scan it all */
static bool
drive(Exec *x, const char *translated, const char *(*run) (Exec *, void *), void *arg)
{
	const char *why;
	bool		ok = setup(x, GPU_BUILT ? translated : NULL);

	if (ok && (why = run(x, arg)) != NULL)
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
	if (sift_gpu_mode != SIFT_GPU_OFF)
	{
		if (!drive(&x, sift_gpu_emit_agg(x.em, q, &x.job), run_units, &t))
		{
			if (t.g != NULL)
				sift_v6_group_free(t.g);
			t.g = NULL;
		}
		else if (t.g == NULL)
			t.g = sift_v6_group_new(q->spec, gcx, desc);
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
		x->plan->ops->locate(x->plan, (uint32) ((uint64) meta >> 32) & 0x3FFFFFFF, &c->part_id, &c->part_gen, &c->granule);
	}
}

static const char *
run_cands(Exec *x, void *arg)
{
	return run_all(x, &x->job, take_cand, arg);
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
	else if ((ok = drive(&x, sift_gpu_emit_topn(x.em, q, &x.job, k, desc, nulls_first), run_cands, &t)) && t.n < k && !nulls_first && x.plan->rows > t.n)
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
	return run != NULL && run->used && run->plan != NULL && run->plan->ops->covers(run->plan, part_gen);
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
	run->plan->ops->free(run->plan);
	run->plan = NULL;
}
