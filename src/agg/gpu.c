/*
 * agg/gpu.c - the Vulkan backend of the aggregate scan (agg/back.h, docs/GPU.md): the settings, the device of the backend and its cache
 * of column chunks, the plans of the chunks, and the queue of units that becomes one job for the device (agg/vk.h).
 *
 * A unit that is stored, clean and all-visible, whose chunks the kernel reads (col/gpu.h), is queued instead of scanned. The queue is one
 * job: the device decodes the chunks, filters, groups and sums them, and the counts and sums of every unit come back as exact integers, which
 * the scan folds into its group table the way the pre-aggregates of a unit are folded. Any other unit is scanned by the CPU, in the same query,
 * into the same table. Whatever goes wrong on the device (lost, timeout, no memory) stops the device for the session and the CPU runs the scan
 * again. A scan that uses the device runs in the leader alone.
 *
 * Settings: sift.gpu (off, auto, on), sift.gpu_min_rows, sift.gpu_device, sift.gpu_cache_mb. Without SIFT_VULKAN (the default build) only the
 * settings exist, and a scan that asks for the device says once that the build has none and runs on the CPU.
 */
#include "postgres.h"

#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

#include "agg/back.h"
#ifdef SIFT_VULKAN
#include "agg/vk.h"
#include "col/gpu.h"
#endif

typedef enum GpuMode
{
	GPU_OFF,
	GPU_AUTO,
	GPU_ON
} GpuMode;

static const struct config_enum_entry gpu_mode_options[] = {
	{"off", GPU_OFF, false},
	{"auto", GPU_AUTO, false},
	{"on", GPU_ON, false},
	{"true", GPU_ON, true},
	{"false", GPU_OFF, true},
	{"yes", GPU_ON, true},
	{"no", GPU_OFF, true},
	{"1", GPU_ON, true},
	{"0", GPU_OFF, true},
	{NULL, 0, false}
};

#define GPU_MIN_ROWS_DEFAULT	5000000		/* sift.gpu = auto: grouped aggregates above this many rows of the index */
#define GPU_CACHE_MB_DEFAULT	256
#define GPU_PLAN_MAX			65536		/* chunk plans kept per backend */
#define GPU_PLAN_BYTES			((Size) 128 << 20)	/* ... and the bytes of them (the words of a wrapped chunk are kept decompressed) */

static int	sift_gpu_mode = GPU_OFF;
static int	sift_gpu_min_rows = GPU_MIN_ROWS_DEFAULT;
static char *sift_gpu_device = NULL;
static int	sift_gpu_cache_mb = GPU_CACHE_MB_DEFAULT;

static void
gpu_init(void)
{
	DefineCustomEnumVariable("sift.gpu",
							 "Runs the integer aggregates of a SiftAgg scan on a Vulkan device (docs/GPU.md): off, auto (above sift.gpu_min_rows) or on.",
							 "A build without Vulkan support (make VULKAN=1) runs on the CPU whatever this says.",
							 &sift_gpu_mode, GPU_OFF, gpu_mode_options, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.gpu_min_rows",
							"Rows of the index above which sift.gpu = auto uses the device for a grouped aggregate; an ungrouped one runs there only with sift.gpu = on.",
							NULL, &sift_gpu_min_rows, GPU_MIN_ROWS_DEFAULT, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.gpu_device",
							   "The Vulkan device sift.gpu uses: its index, or a part of its name. Empty: the best one.",
							   NULL, &sift_gpu_device, "", PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.gpu_cache_mb",
							"Device memory, in MB, for the column chunks that stay on the Vulkan device between queries (per backend).",
							NULL, &sift_gpu_cache_mb, GPU_CACHE_MB_DEFAULT, 16, 1 << 20,
							PGC_SUSET, GUC_UNIT_MB, NULL, NULL, NULL);
}

#ifdef SIFT_VULKAN

/* the context of the backend: the device, opened by the first query that wants it (never in the postmaster, never before the fork), and what it holds */
static struct GpuBackend
{
	SiftVk	   *vk;
	char	   *vk_dev;			/* the setting it was opened for */
	bool		dead;			/* it failed while running: no more for the session */
	bool		told_open;		/* the NOTICEs, once per session */
	bool		told_dead;
	bool		exit_hook;
	HTAB	   *plans;			/* chunk plans by (store, gen, unit, leaf) */
	MemoryContext plan_cxt;
}			gpu;

/* what the query asks of the chunks */
typedef struct GpuShape
{
	int			npred;
	struct
	{
		int			leaf;
		SiftSrcFilterOp op;
		int64		val;
	}			pred[SIFT_VK_MAX_PRED];
	int			key;					/* the leaf of the group key, -1 none */
	bool		key_code;				/* the key is a text leaf: its values are codes of the segment's dictionary */
	int			nsum;
	int			sum[SIFT_VK_MAX_SUM];	/* the leaves summed */
	int			ndense;
	int			dense[8];				/* leaves counted: the zone must say no NULL */
	int		   *asum;					/* per aggregate: its sum slot, -1 none */
} GpuShape;

/* a unit queued in the job */
typedef struct GpuUnit
{
	int64		key[SIFT_VK_MAX_GROUPS];	/* the key of each group slot: the value, or the code */
	uint64		kvalid;						/* the slots that have one */
	SiftSrcDict dict;
	int64		sbase[SIFT_VK_MAX_SUM];		/* a sum is count * base + mul * (the sum of the fields) */
	int64		smul[SIFT_VK_MAX_SUM];
	uint32		ngroups;					/* slots of the key field */
	uint32		wg0;
	uint32		nwg;
} GpuUnit;

typedef struct GpuScan
{
	SiftAggScan sc;						/* the view of the scan, copied */
	GpuShape	shape;
	SiftVk	   *vk;
	bool		active;					/* queues units */
	bool		failed;					/* the device failed: the scan is run again on the CPU */
	bool		rerun;					/* ... and that run has begun */
	char		why[256];				/* EXPLAIN: what the scan did instead / what failed */
	int			nslots;
	GpuUnit    *unit;
	uint32		nunit;
	uint32		capunit;
	uint32	   *desc;
	uint32	   *wg;
	uint32		nwg;
	uint32		capwg;
	uint32		maxwg;
	uint32		groups;					/* of the job: the most slots of a unit's key */
	/* EXPLAIN ANALYZE */
	int64		n_gpu_units;
	int64		n_resident;				/* chunks found on the device */
	int64		n_uploaded;
	int64		bytes_uploaded;
	int64		n_jobs;
	SiftVkTimes t;
} GpuScan;


static void
gpu_interrupt(void)
{
	CHECK_FOR_INTERRUPTS();
}

static void
gpu_exit(int code, Datum arg)
{
	sift_vk_close(gpu.vk);
	gpu.vk = NULL;
	if (gpu.plan_cxt)
		MemoryContextDelete(gpu.plan_cxt);
	gpu.plan_cxt = NULL;
	gpu.plans = NULL;
}

/* the cache the query needs, in bytes: the chunks of the index, at most what sift.gpu_cache_mb allows */
static uint64
gpu_cache_want(const SiftSrcStats *st)
{
	uint64		cap = (uint64) sift_gpu_cache_mb << 20,
				want = (uint64) (st->bytes * 1.1) + ((uint64) 32 << 20);

	return Min(cap, want);
}

/* The device, opened for the first query that wants it; NULL (and a NOTICE once) when it cannot be */
static SiftVk *
gpu_device(const SiftSrcStats *st)
{
	char		err[300];
	uint64		want = gpu_cache_want(st);

	if (gpu.vk && strcmp(gpu.vk_dev, sift_gpu_device) != 0)
	{
		sift_vk_close(gpu.vk);
		gpu.vk = NULL;
		gpu.told_open = false;
	}
	if (gpu.vk == NULL)
	{
		gpu.vk = sift_vk_open(sift_gpu_device, sift_gpu_mode == GPU_ON, want, err, sizeof(err));
		if (gpu.vk == NULL)
		{
			if (!gpu.told_open)
				ereport(NOTICE, (errmsg("sift.gpu: no usable Vulkan device (%s); the aggregate runs on the CPU", err)));
			gpu.told_open = true;
			return NULL;
		}
		if (!gpu.exit_hook)
		{
			on_proc_exit(gpu_exit, 0);
			gpu.exit_hook = true;
		}
		if (gpu.vk_dev)
			pfree(gpu.vk_dev);
		gpu.vk_dev = MemoryContextStrdup(TopMemoryContext, sift_gpu_device);
	}
	/* a bigger index, or another setting of the cap: a cache of the right size (it starts cold) */
	{
		uint64		have = sift_vk_cache_bytes(gpu.vk),
					cap = (uint64) sift_gpu_cache_mb << 20;

		if ((have + SIFT_VK_CHUNK_MAX * 4 <= want && have < cap) || have > cap)	/* a cache is whole slabs: have may be a slab under want */
			sift_vk_set_cache(gpu.vk, want);
	}
	return gpu.vk;
}

static bool
gpu_plain_leaf(const SiftAggScan *sc, int leaf)
{
	const SiftSrcLeaf *lf;

	if (leaf < 0 || leaf >= sc->nbase)
		return false;
	lf = &sc->lay->leaf[leaf];
	return lf->kind == SCL_SCALAR && lf->parent < 0 && !lf->lossy && !lf->wide;
}

static int
gpu_sum_slot(GpuShape *sh, int leaf)
{
	for (int i = 0; i < sh->nsum; i++)
		if (sh->sum[i] == leaf)
			return i;
	if (sh->nsum == SIFT_VK_MAX_SUM)
		return -1;
	sh->sum[sh->nsum] = leaf;
	return sh->nsum++;
}

static bool
gpu_dense_leaf(GpuShape *sh, int leaf)
{
	for (int i = 0; i < sh->ndense; i++)
		if (sh->dense[i] == leaf)
			return true;
	if (sh->ndense == lengthof(sh->dense))
		return false;
	sh->dense[sh->ndense++] = leaf;
	return true;
}

/* one aggregate: count(*), count(leaf), sum / avg of an integer, numeric or int8 leaf */
static bool
gpu_agg_ok(const SiftAggScan *sc, GpuShape *sh, int ai)
{
	const SiftAggAgg *a = &sc->spec->agg[ai];
	int			leaf = -1;

	sh->asum[ai] = -1;
	if (a->filter || a->distinct)
		return false;
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			return true;
		case SAA_COUNT:
			leaf = a->num.n ? (a->num.n == 1 && a->num.step[0].op == SNX_LEAF ? a->num.step[0].leaf : -1) : a->leaf;
			return gpu_plain_leaf(sc, leaf) && gpu_dense_leaf(sh, leaf);
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			leaf = a->leaf;
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			leaf = a->num.n == 1 && a->num.step[0].op == SNX_LEAF ? a->num.step[0].leaf : -1;
			break;
		default:
			return false;
	}
	if (!gpu_plain_leaf(sc, leaf) || (sc->lay->leaf[leaf].vtype != SCV_I32 && sc->lay->leaf[leaf].vtype != SCV_I64))
		return false;
	sh->asum[ai] = gpu_sum_slot(sh, leaf);
	return sh->asum[ai] >= 0;
}

/*
 * Can the device run this scan? Row level, at most one key which is a leaf of at most 64 values per chunk, conjuncts that are all comparisons
 * the source applies on its integer chunks, and aggregates the device sums exactly. Float aggregates are not among them: a float sum depends
 * on the order of its additions, and the CPU's order is the contract. Fills sh; the reason in why when not.
 */
static bool
gpu_shape(const SiftAggScan *sc, GpuShape *sh, const char **why)
{
	const SiftAggSpec *s = sc->spec;

	memset(sh, 0, sizeof(*sh));
	sh->key = -1;
	if (!sc->row_scan || s->group >= 0 || s->narms || s->nsets || s->njoin || s->ncombo || s->npost ||
		s->nvleaf || s->nkeys > 1 || s->naggs == 0)
	{
		*why = "unsupported query shape";
		return false;
	}
	if (s->nkeys == 1)
	{
		const SiftAggKey *k = &s->key[0];

		if (k->kind != SAK_LEAF || !gpu_plain_leaf(sc, k->leaf) || (sc->lay->leaf[k->leaf].vtype != SCV_I32 && sc->lay->leaf[k->leaf].vtype != SCV_CODE))
		{
			*why = "unsupported group key";
			return false;
		}
		sh->key = k->leaf;
		sh->key_code = sc->lay->leaf[k->leaf].vtype == SCV_CODE;
	}
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		const SiftSrcFilter *f = &sc->pflt[i];

		if (p->kind != SAP_CMP || p->level != 0 || p->late || !sc->pushed[i] || !gpu_plain_leaf(sc, p->leaf) || f->op == SSF_IN || f->op == SSF_NOTIN)
		{
			*why = "unsupported filter";
			return false;
		}
		if (f->op == SSF_NOTNULL)
		{
			if (!gpu_dense_leaf(sh, f->leaf))
			{
				*why = "too many filters";
				return false;
			}
			continue;				/* a numeric comparison every value passes: a unit whose leaf has no NULL passes every row */
		}
		if (sh->npred == SIFT_VK_MAX_PRED)
		{
			*why = "too many filters";
			return false;
		}
		sh->pred[sh->npred].leaf = f->leaf;
		sh->pred[sh->npred].op = f->op;
		sh->pred[sh->npred].val = f->val;
		sh->npred++;
	}
	sh->asum = palloc(s->naggs * sizeof(int));
	for (int ai = 0; ai < s->naggs; ai++)
		if (!gpu_agg_ok(sc, sh, ai))
		{
			*why = "unsupported aggregate";
			return false;
		}
	return true;
}

/* Does this scan run on the device? Decided before any worker is launched: a scan that uses the device is the leader's alone */
static void *
gpu_begin(const SiftAggScan *sc, bool *exclusive)
{
	GpuScan    *g;
	GpuShape	sh;
	const char *why = NULL;
	SiftSrcStats st;
	SiftVk	   *vk;

	if (sift_gpu_mode == GPU_OFF)
		return NULL;
	g = MemoryContextAllocZero(sc->cxt, sizeof(GpuScan));
	g->sc = *sc;
	if (!gpu_shape(sc, &sh, &why))
	{
		strlcpy(g->why, why, sizeof(g->why));
		return g;
	}
	g->shape = sh;
	if (gpu.dead)
	{
		strlcpy(g->why, "the device failed earlier in the session", sizeof(g->why));
		return g;
	}
	if (sift_gpu_mode == GPU_AUTO && sh.key < 0)
	{
		strlcpy(g->why, "an ungrouped aggregate runs on the device only with sift.gpu = on", sizeof(g->why));
		return g;
	}
	sift_colsrc_stats(sc->index, &st);
	if (sift_gpu_mode == GPU_AUTO && st.nrows < (double) sift_gpu_min_rows)
	{
		snprintf(g->why, sizeof(g->why), "%.0f rows are below sift.gpu_min_rows", st.nrows);
		return g;
	}
	vk = gpu_device(&st);
	if (vk == NULL)
	{
		strlcpy(g->why, "no usable Vulkan device", sizeof(g->why));
		return g;
	}
	if (sift_gpu_mode == GPU_AUTO && sift_vk_is_software(vk))
	{
		strlcpy(g->why, "the device is a software renderer", sizeof(g->why));
		return g;
	}
	g->vk = vk;
	g->active = true;
	g->nslots = sh.npred + 1 + sh.nsum;
	g->maxwg = sift_vk_max_wg(vk, sh.key >= 0 ? SIFT_VK_MAX_GROUPS : 0, sh.nsum);
	sift_vk_abandon(vk);			/* what an earlier query that failed between staging and running left */
	*exclusive = true;
	return g;
}

static void
gpu_fail(GpuScan *g, const char *what)
{
	if (!gpu.told_dead)
		ereport(NOTICE, (errmsg("sift.gpu: %s; the aggregate runs on the CPU and the device is not used again in this session", what)));
	gpu.told_dead = true;
	gpu.dead = true;
	g->failed = true;
	g->active = false;
	snprintf(g->why, sizeof(g->why), "%s", what);
}

/* ---- the plan of a chunk, kept for the life of the backend ---- */

typedef struct GpuPlanEnt
{
	SiftVkKey	key;
	bool		ok;
	SiftCGpuPlan plan;
} GpuPlanEnt;

static void
gpu_plans_reset(void)
{
	HASHCTL		ctl = {.keysize = sizeof(SiftVkKey), .entrysize = sizeof(GpuPlanEnt)};

	if (gpu.plan_cxt == NULL)
		gpu.plan_cxt = AllocSetContextCreate(TopMemoryContext, "sift gpu plans", ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(gpu.plan_cxt);
	ctl.hcxt = gpu.plan_cxt;
	gpu.plans = hash_create("sift gpu plans", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/* The plan of the leaf's chunk in the unit; NULL when the device cannot read it. Reads the chunk the first time only. */
static const SiftCGpuPlan *
gpu_plan(GpuScan *g, const SiftSrcUnit *unit, const SiftSrcGpuUnit *gu, int leaf)
{
	SiftVkKey	key = {gu->store, gu->gen, gu->unit, (uint32) leaf};
	GpuPlanEnt *e;
	bool		found;
	SiftSrcGpuChunk c;
	SiftCGpuPlan plan;
	bool		ok;
	MemoryContext old;

	e = hash_search(gpu.plans, &key, HASH_FIND, NULL);
	if (e)
		return e->ok ? &e->plan : NULL;
	old = MemoryContextSwitchTo(gpu.plan_cxt);
	ok = sift_colsrc_gpu_chunk(g->sc.src, unit, leaf, &c) && c.n == gu->nrows;
	if (ok && c.is_const)
		sift_cgpu_plan_const(c.cval, c.n, &plan);
	else
		ok = ok && sift_cgpu_plan(c.blob, c.len, &plan) && plan.leaf.nwords <= SIFT_VK_CHUNK_MAX;
	MemoryContextSwitchTo(old);
	e = hash_search(gpu.plans, &key, HASH_ENTER, &found);
	e->ok = ok;
	if (ok)
		e->plan = plan;
	return ok ? &e->plan : NULL;
}

/*
 * The word offset of the chunk on the device: found there, or copied to the staging buffer now. 0: no room in this job (run it, ask again);
 * -1: the chunk cannot be read.
 */
static int
gpu_resident(GpuScan *g, const SiftSrcUnit *unit, const SiftSrcGpuUnit *gu, int leaf, const SiftCGpuPlan *p, uint32 *woff)
{
	SiftVkKey	key = {gu->store, gu->gen, gu->unit, (uint32) leaf};
	SiftSrcGpuChunk c;
	const uint8 *words;

	if (p->leaf.kind == SIFT_CGL_CONST)
	{
		*woff = 0;
		return 1;
	}
	if (sift_vk_lookup(g->vk, &key, woff))
	{
		g->n_resident++;
		return 1;
	}
	if (p->leaf.wbuf)
		words = p->leaf.wbuf + p->leaf.woff;
	else if (sift_colsrc_gpu_chunk(g->sc.src, unit, leaf, &c) && c.len >= p->leaf.woff + (uint64) p->leaf.nwords * 4)
		words = c.blob + p->leaf.woff;
	else
		return -1;
	if (!sift_vk_stage(g->vk, &key, words, p->leaf.nwords, woff))
		return 0;
	g->n_uploaded++;
	g->bytes_uploaded += (int64) p->leaf.nwords * 4;
	return 1;
}

/* ---- predicates as ranges of the field ---- */

/* the key range [lo, hi] a comparison keeps; false when it keeps none */
static bool
gpu_cmp_range(SiftSrcFilterOp op, int64 v, int64 *lo, int64 *hi)
{
	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	switch (op)
	{
		case SSF_EQ:
		case SSF_NE:
			*lo = *hi = v;
			return true;
		case SSF_LT:
			*hi = v;
			return v > PG_INT64_MIN && (*hi = v - 1, true);
		case SSF_LE:
			*hi = v;
			return true;
		case SSF_GT:
			*lo = v;
			return v < PG_INT64_MAX && (*lo = v + 1, true);
		case SSF_GE:
			*lo = v;
			return true;
		default:
			return false;
	}
}

#define GPU_NEUTRAL		{0, 0, 0, UINT32_MAX}

/*
 * The descriptor of predicate p on a chunk: the field range the comparison keeps. false: no row of the chunk passes (the unit adds nothing).
 * A comparison every row passes is the neutral descriptor, which tests nothing.
 */
static bool
gpu_pred_desc(const SiftCGpuPlan *pl, SiftSrcFilterOp op, int64 v, uint32 woff, uint32 *d)
{
	int64		lo,
				hi;
	uint32		fl = 0,
				fh = 0;
	SiftCGpuRange r;
	bool		ne = op == SSF_NE;

	if (!gpu_cmp_range(op, v, &lo, &hi))
		return false;
	r = sift_cgpu_range(pl, lo, hi, &fl, &fh);
	if (ne)
		r = r == SIFT_CGR_NONE ? SIFT_CGR_ALL : r == SIFT_CGR_ALL ? SIFT_CGR_NONE : r;
	if (r == SIFT_CGR_NONE)
		return false;
	if (r == SIFT_CGR_ALL)
	{
		d[0] = d[1] = d[2] = 0;
		d[3] = UINT32_MAX;
		return true;
	}
	d[0] = woff;
	d[1] = (pl->leaf.kind == SIFT_CGL_BYTE ? (uint32) pl->leaf.W * 8 | SIFT_VK_BYTES : pl->leaf.W) | (ne ? SIFT_VK_NEG : 0);
	d[2] = fl;
	d[3] = fh - fl;
	return true;
}

static void
gpu_field_desc(const SiftCGpuPlan *pl, uint32 woff, uint32 *d)
{
	d[0] = pl->leaf.kind == SIFT_CGL_CONST ? 0 : woff;
	d[1] = pl->leaf.kind == SIFT_CGL_BYTE ? (uint32) pl->leaf.W * 8 | SIFT_VK_BYTES : pl->leaf.kind == SIFT_CGL_BIT ? pl->leaf.W : 0;
	d[2] = 0;
	d[3] = 0;
}

/* ---- the job ---- */

static void
gpu_grow(GpuScan *g)
{
	if (g->capunit == 0)
	{
		g->capunit = 256;
		g->capwg = 1024;
		g->unit = MemoryContextAlloc(g->sc.cxt, g->capunit * sizeof(GpuUnit));
		g->desc = MemoryContextAlloc(g->sc.cxt, (Size) g->capunit * g->nslots * 4 * sizeof(uint32));
		g->wg = MemoryContextAlloc(g->sc.cxt, (Size) g->capwg * 4 * sizeof(uint32));
	}
	if (g->nunit == g->capunit)
	{
		g->capunit *= 2;
		g->unit = repalloc(g->unit, g->capunit * sizeof(GpuUnit));
		g->desc = repalloc(g->desc, (Size) g->capunit * g->nslots * 4 * sizeof(uint32));
	}
}

static void
gpu_add_wg(GpuScan *g, uint32 nrows)
{
	uint32		tiles = (nrows + SIFT_VK_TILE - 1) / SIFT_VK_TILE;
	GpuUnit    *u = &g->unit[g->nunit];

	u->wg0 = g->nwg;
	for (uint32 t0 = 0; t0 < tiles; t0 += SIFT_VK_WG_TILES)
	{
		if (g->nwg == g->capwg)
		{
			g->capwg *= 2;
			g->wg = repalloc(g->wg, (Size) g->capwg * 4 * sizeof(uint32));
		}
		g->wg[g->nwg * 4] = g->nunit;
		g->wg[g->nwg * 4 + 1] = t0;
		g->wg[g->nwg * 4 + 2] = Min(tiles - t0, SIFT_VK_WG_TILES);
		g->wg[g->nwg * 4 + 3] = nrows;
		g->nwg++;
	}
	u->nwg = g->nwg - u->wg0;
}

/* the counts and sums of a unit's workgroups, added in 64 bits */
typedef struct GpuTotals
{
	uint64		cnt[SIFT_VK_MAX_GROUPS];
	uint64		lo[SIFT_VK_MAX_SUM][SIFT_VK_MAX_GROUPS];
	uint64		hi[SIFT_VK_MAX_SUM][SIFT_VK_MAX_GROUPS];
} GpuTotals;

static void
gpu_totals(const GpuScan *g, const GpuUnit *u, const uint32 *res, uint32 groups, GpuTotals *t)
{
	uint32		rs = 1 + 2 * g->shape.nsum,
				ng = g->shape.key >= 0 ? groups : 1;

	memset(t, 0, sizeof(*t));
	for (uint32 w = u->wg0; w < u->wg0 + u->nwg; w++)
		for (uint32 k = 0; k < ng; k++)
		{
			const uint32 *r = res + ((size_t) w * ng + k) * rs;

			t->cnt[k] += r[0];
			for (int s = 0; s < g->shape.nsum; s++)
			{
				t->lo[s][k] += r[1 + 2 * s];
				t->hi[s][k] += r[2 + 2 * s];
			}
		}
}

/* a unit's groups into the group table: exact counts and sums, as the pre-aggregates of a unit give them */
static void
gpu_fold(GpuScan *g, const GpuUnit *u, const uint32 *res)
{
	const int	naggs = g->sc.spec->naggs;
	GpuTotals  *t = palloc(sizeof(GpuTotals));
	uint32	   *kept = palloc(SIFT_VK_MAX_GROUPS * sizeof(uint32));
	int64	   *rows = palloc(SIFT_VK_MAX_GROUPS * sizeof(int64));
	sift_i128  *sums = palloc(Max(naggs, 1) * SIFT_VK_MAX_GROUPS * sizeof(sift_i128));
	sift_i128 **sum = palloc(Max(naggs, 1) * sizeof(sift_i128 *));
	SiftSrcHist h = {.ngroups = u->ngroups, .key = u->key, .dict = g->shape.key_code ? &u->dict : NULL};
	SiftAggUnitSums r = {.keys = g->shape.key >= 0 ? &h : NULL, .kept = kept, .rows = rows, .sum = sum};

	gpu_totals(g, u, res, g->groups, t);
	for (uint32 k = 0; k < (g->shape.key >= 0 ? g->groups : 1); k++)
	{
		if (t->cnt[k] == 0)
			continue;
		if (g->shape.key >= 0 && (k >= u->ngroups || !(u->kvalid >> k & 1)))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("sift: the device counted rows of a group the chunk does not have")));
		for (int ai = 0; ai < naggs; ai++)
		{
			int			s = g->shape.asum[ai];

			if (s >= 0)
				sums[ai * SIFT_VK_MAX_GROUPS + r.nkept] = (sift_i128) t->cnt[k] * u->sbase[s] +
					(sift_i128) u->smul[s] * ((sift_i128) t->lo[s][k] + ((sift_i128) t->hi[s][k] << 16));
		}
		rows[r.nkept] = t->cnt[k];
		kept[r.nkept++] = k;
	}
	for (int ai = 0; ai < naggs; ai++)
		sum[ai] = g->shape.asum[ai] >= 0 ? &sums[ai * SIFT_VK_MAX_GROUPS] : NULL;
	sift_agg_fold_sums(g->sc.host, &r);
	pfree(t);
	pfree(kept);
	pfree(rows);
	pfree(sums);
	pfree(sum);
}

static void
gpu_flush(GpuScan *g)
{
	SiftVkJob	job;
	const uint32 *res;
	SiftVkTimes t;
	SiftVkStatus st;

	if (!g->active || g->failed)
		return;
	if (g->nunit == 0)
	{
		sift_vk_abandon(g->vk);
		return;
	}
	job = (SiftVkJob) {g->shape.npred, g->shape.nsum, g->shape.key >= 0 ? g->groups : 0, g->nunit, g->nwg, g->desc, g->wg};
	st = sift_vk_run(g->vk, &job, gpu_interrupt, &res, &t);
	if (st != SIFT_VK_OK)
	{
		gpu_fail(g, psprintf("the Vulkan device %s (%s)", st == SIFT_VK_LOST ? "was lost" : st == SIFT_VK_TIMEOUT ? "timed out" : "failed",
							 sift_vk_error(g->vk)));
		return;
	}
	g->n_jobs++;
	g->t.upload_ms += t.upload_ms;
	g->t.kernel_ms += t.kernel_ms;
	g->t.readback_ms += t.readback_ms;
	g->t.wall_ms += t.wall_ms;
	for (uint32 i = 0; i < g->nunit; i++)
		gpu_fold(g, &g->unit[i], res);
	g->n_gpu_units += g->nunit;
	g->nunit = g->nwg = 0;
	g->groups = 0;
}

/* does the unit fit the job in preparation? */
static bool
gpu_room(const GpuScan *g, uint32 nwg_unit)
{
	return g->nunit < SIFT_VK_MAX_UNITS && g->nwg + nwg_unit <= g->maxwg;
}

/*
 * Queues the unit just returned by the source for the device; false when it must be scanned: it is not stored, clean and all-visible, a
 * chunk it needs is not one the kernel reads, or a group key has more than 64 values in it. A unit no row of which passes a filter is taken
 * and adds nothing.
 */
static bool
gpu_run_unit(void *state, const SiftSrcUnit *unit)
{
	GpuScan    *g = state;
	const GpuShape *sh = &g->shape;
	SiftSrcGpuUnit gu;
	const SiftCGpuPlan *pl[SIFT_VK_MAX_SLOTS] = {NULL};
	int			leaf[SIFT_VK_MAX_SLOTS];
	uint32		d[SIFT_VK_MAX_SLOTS][4],
				woff[SIFT_VK_MAX_SLOTS];
	int			ns = g->nslots,
				key_slot = sh->npred;
	uint32		nrows = unit->nrow;
	GpuUnit    *u;
	uint32		tiles,
				nwg_unit;

	if (!g->active || !sift_colsrc_gpu_unit(g->sc.src, unit, &gu) || gu.nrows != nrows)
		return false;
	if (gpu.plans == NULL || hash_get_num_entries(gpu.plans) >= GPU_PLAN_MAX || MemoryContextMemAllocated(gpu.plan_cxt, true) > GPU_PLAN_BYTES)
		gpu_plans_reset();			/* between units: the plans of this one are used below */
	for (int i = 0; i < sh->ndense; i++)
		if (!sift_colsrc_gpu_dense(g->sc.src, unit, sh->dense[i]))
			return false;
	for (int s = 0; s < ns; s++)
	{
		leaf[s] = s < sh->npred ? sh->pred[s].leaf : s == key_slot ? sh->key : sh->sum[s - key_slot - 1];
		pl[s] = leaf[s] < 0 ? NULL : gpu_plan(g, unit, &gu, leaf[s]);
		if (leaf[s] >= 0 && pl[s] == NULL)
			return false;
	}
	if (sh->key >= 0 && sift_cgpu_fhi(pl[key_slot]) >= SIFT_VK_MAX_GROUPS)
		return false;
	memset(d, 0, sizeof(d));
	for (int s = 0; s < ns; s++)
	{
		int			r;

		if (leaf[s] < 0)
			continue;
		r = gpu_resident(g, unit, &gu, leaf[s], pl[s], &woff[s]);
		if (r == 0 && g->nunit > 0)
		{
			gpu_flush(g);
			if (g->failed)
				return false;
			s = -1;				/* the flush uploaded what this unit staged; ask again */
			continue;
		}
		if (r <= 0)
			return false;
	}
	for (int s = 0; s < sh->npred; s++)
		if (!gpu_pred_desc(pl[s], sh->pred[s].op, sh->pred[s].val, woff[s], d[s]))
			return true;			/* no row of this unit passes */
	tiles = (nrows + SIFT_VK_TILE - 1) / SIFT_VK_TILE;
	nwg_unit = (tiles + SIFT_VK_WG_TILES - 1) / SIFT_VK_WG_TILES;
	if (!gpu_room(g, nwg_unit))
	{
		gpu_flush(g);
		if (g->failed)
			return false;
	}
	gpu_grow(g);
	u = &g->unit[g->nunit];
	memset(u, 0, sizeof(*u));
	if (sh->key >= 0)
	{
		gpu_field_desc(pl[key_slot], woff[key_slot], d[key_slot]);
		u->ngroups = sift_cgpu_fhi(pl[key_slot]) + 1;
		for (uint32 k = 0; k < u->ngroups; k++)
			if (sift_cgpu_value(pl[key_slot], k, &u->key[k]))
				u->kvalid |= UINT64CONST(1) << k;
		if (sh->key_code)
			sift_colsrc_gpu_dict(g->sc.src, unit, sh->key, &u->dict);
		g->groups = Max(g->groups, u->ngroups);
	}
	for (int j = 0; j < sh->nsum; j++)
	{
		int			s = key_slot + 1 + j;

		gpu_field_desc(pl[s], woff[s], d[s]);
		u->sbase[j] = pl[s]->leaf.base;
		u->smul[j] = pl[s]->leaf.mul;
	}
	memcpy(&g->desc[(size_t) g->nunit * ns * 4], d, (size_t) ns * 4 * sizeof(uint32));
	gpu_add_wg(g, nrows);
	g->nunit++;
	return true;
}

/* the end of the scan: the units still queued run; true when the device failed and the scan must be run again on the CPU */
static bool
gpu_finish(void *state)
{
	GpuScan    *g = state;

	if (g->active)
		gpu_flush(g);
	if (!g->failed)
		return false;
	g->n_gpu_units = 0;
	g->nunit = g->nwg = 0;
	g->rerun = true;
	return true;
}

/* the scan stops where it is: the device failed and the scan is yet to be run again */
static bool
gpu_stopped(const void *state)
{
	const GpuScan *g = state;

	return g->failed && !g->rerun;
}

static void
gpu_explain(const void *state, ExplainState *es, int64 n_units)
{
	const GpuScan *g = state;

	if (g->vk == NULL || g->n_jobs == 0)
	{
		ExplainPropertyText("GPU", psprintf("not used (%s)", g->why[0] ? g->why : "no unit qualified"), es);
		return;
	}
	ExplainPropertyText("GPU", psprintf("units %lld / %lld%s", (long long) g->n_gpu_units, (long long) n_units,
										g->failed ? psprintf(" (%s, rerun on the CPU)", g->why) : ""), es);
	ExplainPropertyText("GPU Device", sift_vk_device_name(g->vk), es);
	ExplainPropertyText("GPU Chunks", psprintf("%lld resident, %lld uploaded (%.1f MB), %lld jobs", (long long) g->n_resident, (long long) g->n_uploaded,
											   g->bytes_uploaded / 1048576.0, (long long) g->n_jobs), es);
	ExplainPropertyText("GPU Time", psprintf("upload %.3f ms, kernel %.3f ms, readback %.3f ms (wall %.3f ms)", g->t.upload_ms, g->t.kernel_ms,
											 g->t.readback_ms, g->t.wall_ms), es);
}

const SiftAggBackend sift_agg_gpu_backend = {
	.name = "gpu",
	.init = gpu_init,
	.begin = gpu_begin,
	.run_unit = gpu_run_unit,
	.stopped = gpu_stopped,
	.finish = gpu_finish,
	.explain = gpu_explain
};

#else							/* SIFT_VULKAN */

/* this build has no device: a scan that asks for one is told so, once per session, and runs on the CPU */
static void *
gpu_begin(const SiftAggScan *sc, bool *exclusive)
{
	static bool told = false;

	if (sift_gpu_mode != GPU_OFF && !told)
		ereport(NOTICE, (errmsg("sift.gpu = %s: this build has no Vulkan support (build with VULKAN=1); the aggregate runs on the CPU",
								sift_gpu_mode == GPU_ON ? "on" : "auto")));
	told = told || sift_gpu_mode != GPU_OFF;
	return NULL;
}

const SiftAggBackend sift_agg_gpu_backend = {.name = "gpu (not built)", .init = gpu_init, .begin = gpu_begin};

#endif							/* SIFT_VULKAN */
