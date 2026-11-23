/*
 * tests/vk_v6_test_agg.c - the aggregation kernels on the real device, each job compared with the scalar oracle (vk_v6_oracle.c) over the
 * logical data: T2 no key, T3 dense keys, T4 hashed keys (1..8 keys, up to a million groups), T5 the expression machine (overflow, division
 * by zero, calendar, three-valued masks), T6 top N. The columns are chunks of the real codec in every encoding the decoder takes.
 */
#include <math.h>

#include "vk_v6_agg_case.h"

static SiftVk *vk;
static uint64_t cases_run,
			cases_bad,
			fast_cases;

int64_t
dbits(double d)
{
	int64_t		v;

	memcpy(&v, &d, 8);
	return v;
}

void
case_init(Case *c, const TData *d, SiftVkMode mode)
{
	memset(c, 0, sizeof(*c));
	c->j = (SiftVkJob) {.mode = mode, .ncols = d->ncols, .ngran = d->ngran, .gran = d->vg, .where_slot = 255};
}

void
add_agg(Case *c, uint8_t kind, uint8_t vreg, uint8_t freg)
{
	c->j.agg[c->j.naggs++] = (SiftVkAgg) {kind, vreg, freg};
}

static void
ld(Case *c, uint32_t reg, uint32_t col, uint32_t ty)
{
	P_COL(&c->p, reg, col, ty);
}

static void
slot_col(Case *c, uint32_t slot, uint32_t col)
{
	p_op(&c->p, SIFT_VK_OP_MASK_COL, 0, 0, 0, slot, col, 0, 0, 0, 0, 0, 0);
}

static void
diagnose_decode(const TData *d)
{
	for (uint32_t gi = 0; gi < d->ngran; gi++)
		for (uint32_t c = 0; c < d->ncols; c++)
		{
			Prog		p = {0};
			SiftVkGranule one = d->vg[gi];
			SiftVkJob	j = {.mode = SIFT_VK_UNGROUPED, .ncols = d->ncols, .ngran = 1, .gran = &one, .where_slot = 255, .naggs = 1};
			SiftVkResult res;

			for (uint32_t x = 0; x < SIFT_VK_MAXCOLS; x++)
				one.col[x] = x == c ? d->vg[gi].col[x] : NULL;
			P_COL(&p, 0, c, SIFT_VK_T_I64);
			j.nops = p.n;
			j.ops = p.w;
			j.agg[0] = (SiftVkAgg) {SIFT_VK_AK_COUNT, 0, 255};
			sift_vk_set_cache(vk, sift_vk_caps(vk)->cache_bytes);
			if (sift_vk_run(vk, &j, NULL, &res) != SIFT_VK_OK)
				fprintf(stderr, "  diagnose: granule %u column %u: %s [%s]\n", gi, c, sift_vk_error(vk), d->g[gi].desc[c]);
		}
}

/* one run of the job (the expression machine only when nofast) compared with the oracle; true when a fast kernel ran it */
static bool
case_pass(const char *what, Case *c, const TData *d, const RefResult *ref, double ftol, bool nofast)
{
	SiftVkResult res;
	SiftVkStatus st;
	uint64_t	bad = 0;

	sift_vk_test_nofast(vk, nofast);
	st = sift_vk_run(vk, &c->j, NULL, &res);
	cases_run++;
	if (ref->has_err)
	{
		bool		ok = st == SIFT_VK_PROG_ERROR && res.err_gran == ref->err_gran && res.err_row == ref->err_row && res.err_op == ref->err_op &&
			res.err_code == ref->err_code;

		CHECK(ok, "%s: expected the error (granule %u row %u op %u code %u), the device gave status %d error (granule %u row %u op %u code %u)", what,
			  ref->err_gran, ref->err_row, ref->err_op, ref->err_code, st, res.err_gran, res.err_row, res.err_op, res.err_code);
		bad = !ok;
		printf("   %-34s error as expected: granule %u row %u op %u code %u\n", what, ref->err_gran, ref->err_row, ref->err_op, ref->err_code);
	}
	else
	{
		CHECK(st == SIFT_VK_OK, "%s: status %d (%s)", what, st, sift_vk_error(vk));
		if (st == SIFT_VK_DECODE && getenv("VKT_DIAG"))
			diagnose_decode(d);
		if (st == SIFT_VK_OK)
		{
			if (c->j.mode == SIFT_VK_DENSE)
			{
				uint64_t	g = 1;

				for (uint32_t k = 0; k < c->j.nkeys; k++)
					g *= c->j.dense_span[k];
				CHECK(res.ngroups == g, "%s: %u dense groups, expected %llu", what, res.ngroups, (unsigned long long) g);
			}
			bad = ref_compare(&c->j, &res, ref, what, ftol);
			CHECK(bad == 0, "%s: %llu differences", what, (unsigned long long) bad);
			printf("   %-34s %s  groups %u  rows in %llu  kernel %.2f ms  wall %.2f ms%s\n", what, bad ? "DIFFERS" : "ok", c->j.mode == SIFT_VK_UNGROUPED ? 1 : res.ngroups,
				   (unsigned long long) ref->rows_in, res.times.kernel_ms, res.times.wall_ms, res.fast ? "  [fast kernel]" : "");
		}
		else
			bad = 1;
	}
	CHECK(!nofast || !res.fast, "%s: the fast kernel ran with the fast kernels off", what);
	cases_bad += bad != 0;
	return res.fast;
}

/* the job on the fast kernel when it has the shape for it, and always on the expression machine: both must give the oracle's answer; true when the fast kernel ran it */
bool
case_check(const char *name, Case *c, const TData *d, double ftol)
{
	RefResult	ref;
	char		what[128];
	bool		fast;

	c->j.nops = c->p.n;
	c->j.ops = c->p.w;
	c->j.npool = c->p.npool;
	c->j.pool = c->p.pool;
	snprintf(what, sizeof(what), "%s", name);
	ref_run(d, &c->j, &ref);
	fast = case_pass(what, c, d, &ref, ftol, false);
	if (fast)
	{
		fast_cases++;
		snprintf(what, sizeof(what), "%s (machine)", name);
		case_pass(what, c, d, &ref, ftol, true);
	}
	sift_vk_test_nofast(vk, false);
	ref_free(&ref);
	return fast;
}

typedef struct EncChoice
{
	const char *pol;
	uint32_t	enc;
} EncChoice;

static void
put_col(TGran *g, uint32_t c, int kind, Rng *rng, uint32_t part)
{
	static const EncChoice ints[] = {{"for", SIFT_VK_ENC_INT_FOR}, {"delta", SIFT_VK_ENC_INT_DELTA}, {"rle", SIFT_VK_ENC_RLE}, {"dict", SIFT_VK_ENC_DICT_CODE},
		{"rle+for", SIFT_VK_ENC_RLE}, {"for+lz4", SIFT_VK_ENC_INT_FOR}, {"delta+for", SIFT_VK_ENC_INT_DELTA}};
	static const EncChoice dbls[] = {{"alp", SIFT_VK_ENC_ALP}, {"auto", SIFT_VK_ENC_FLOAT_RAW}, {"alp", SIFT_VK_ENC_ALP}};
	static const EncChoice bools[] = {{"auto", SIFT_VK_ENC_BOOL_BITS}};
	static const EncChoice nums[] = {{"for", SIFT_VK_ENC_NUM64}, {"scale+for", SIFT_VK_ENC_NUM64}};
	const EncChoice *set = kind == 1 ? dbls : kind == 2 ? bools : kind == 3 ? nums : ints;
	uint32_t	n = kind == 1 ? 3 : kind == 2 ? 1 : kind == 3 ? 2 : 7;
	uint32_t	pick = (uint32_t) (rng_next(rng) % 12);
	ChunkSpec	sp = {.policy = set[rng_next(rng) % n].pol, .nulls = pick == 0 ? NULLS_SPARSE_NULL : pick == 1 ? NULLS_SPARSE_VALUE : NULLS_AUTO,
		.lz4 = rng_next(rng) % 4 == 0};
	const EncChoice *e = &set[0];

	for (uint32_t i = 0; i < n; i++)
		if (set[i].pol == sp.policy)
			e = &set[i];
	sp.enc = e->enc;
	if (!build_chunk(g, c, &sp, part))
	{
		ChunkSpec	fb = {.policy = kind == 1 ? "auto" : kind == 2 ? "auto" : "for",
			.enc = kind == 1 ? SIFT_VK_ENC_FLOAT_RAW : kind == 2 ? SIFT_VK_ENC_BOOL_BITS : kind == 3 ? SIFT_VK_ENC_NUM64 : SIFT_VK_ENC_INT_FOR, .nulls = NULLS_AUTO};

		CHECK(build_chunk(g, c, &fb, part), "fallback chunk: %s", fb.why);
	}
}

static void
set_val(TGran *g, uint32_t c, uint32_t r, int64_t v, bool ok)
{
	g->col[c].v[r] = ok ? v : 0;
	g->col[c].ok[r] = ok;
}

void
build_base(TData *d, uint32_t ngran, Rng *rng, uint32_t last_rows)
{
	td_init(d, ngran, C_NCOLS, SIFT_VK_GRANULE_ROWS);
	d->g[ngran - 1].nrows = last_rows;
	if (last_rows % 4 != 0)
		d->g[ngran - 2].nrows = 4099;
	for (uint32_t gi = 0; gi < ngran; gi++)
	{
		TGran	   *g = &d->g[gi];

		if (gi % 3 == 1)
		{
			g->has_dead = true;
			for (uint32_t r = 0; r < g->nrows; r++)
				if (rng_next(rng) % 33 == 0)
					g->dead[r >> 5] |= 1u << (r & 31);
		}
		for (uint32_t r = 0; r < g->nrows; r++)
		{
			uint64_t	big = rng_next(rng) % 20;
			int64_t		days = rng_range(rng, -36524, 36525);

			set_val(g, C_K0, r, rng_range(rng, 0, 9), rng_next(rng) % 10 != 0);
			set_val(g, C_K1, r, rng_range(rng, 100, 139), true);
			set_val(g, C_BIG, r, big == 0 ? INT64_MAX : big == 1 ? INT64_MIN + rng_range(rng, 0, 1000) : ((int64_t) 1 << 62) + rng_range(rng, -1000, 1000),
					rng_next(rng) % 20 != 0);
			set_val(g, C_V32, r, rng_range(rng, -1000, 1000), rng_next(rng) % 10 != 0);
			set_val(g, C_DBL, r, f8_key((double) rng_range(rng, 1, 99999) / 100.0), rng_next(rng) % 33 != 0);
			set_val(g, C_FLAG, r, rng_next(rng) & 1, true);
			set_val(g, C_TS, r, days * 86400000000LL + rng_range(rng, 0, 86399999999LL), rng_next(rng) % 50 != 0);
			set_val(g, C_DATE, r, days, rng_next(rng) % 50 != 0);
			set_val(g, C_NUM, r, rng_range(rng, -9999999, 9999999), rng_next(rng) % 25 != 0);
			set_val(g, C_DIV, r, rng_range(rng, -3, 3), rng_next(rng) % 10 != 0);
			set_val(g, C_KB, r, 2147483000LL + rng_range(rng, 0, 647), true);
			set_val(g, C_KL, r, rng_range(rng, 0, 299), rng_next(rng) % 50 != 0);
		}
		for (uint32_t c = 0; c < C_NCOLS; c++)
			put_col(g, c, c == C_DBL ? 1 : c == C_FLAG ? 2 : c == C_NUM ? 3 : 0, rng, gi * 16 + c);
	}
	td_publish(d);
}

/* ---- T2: no key ---- */
static void
t2_ungrouped(const TData *d)
{
	Case		c;
	int64_t		in1[] = {100, 105, 120, 139, 7},
				in2[] = {100, 120, 100000000000LL, -5, 6000000000LL};

	printf("T2 ungrouped\n");
	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	ld(&c, 1, C_V32, SIFT_VK_T_I64);
	ld(&c, 2, C_DBL, SIFT_VK_T_F64);
	ld(&c, 3, C_K0, SIFT_VK_T_I64);
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_COUNT, 3, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 0, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 1, 255);
	add_agg(&c, SIFT_VK_AK_MIN, 1, 255);
	add_agg(&c, SIFT_VK_AK_MAX, 0, 255);
	add_agg(&c, SIFT_VK_AK_SUM_F, 2, 255);
	add_agg(&c, SIFT_VK_AK_MIN_F, 2, 255);
	add_agg(&c, SIFT_VK_AK_MAX_F, 2, 255);
	add_agg(&c, SIFT_VK_AK_COUNT, 0, 255);
	case_check("all aggregates (sums past int64)", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	ld(&c, 1, C_V32, SIFT_VK_T_I64);
	ld(&c, 2, C_DBL, SIFT_VK_T_F64);
	ld(&c, 3, C_K0, SIFT_VK_T_I64);
	ld(&c, 4, C_K1, SIFT_VK_T_I64);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 1, 50, SIFT_VK_CMP_GT);
	slot_col(&c, 2, C_FLAG);
	p_inset(&c.p, 3, 4, in1, 5);
	P_MOR(&c.p, 4, 2, 3);
	P_MAND(&c.p, 5, 1, 4);
	P_CMPC(&c.p, SIFT_VK_T_I64, 6, 3, 5, SIFT_VK_CMP_LT);
	c.j.where_slot = 5;
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 0, 6);
	add_agg(&c, SIFT_VK_AK_SUM_F, 2, 255);
	add_agg(&c, SIFT_VK_AK_MAX, 1, 6);
	add_agg(&c, SIFT_VK_AK_COUNT, 3, 255);
	case_check("WHERE AND/OR/IN + FILTER", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_K1, SIFT_VK_T_I64);
	p_inset(&c.p, 1, 1, in2, 5);
	P_MNOT(&c.p, 2, 1);
	c.j.where_slot = 2;
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 0, 255);
	case_check("NOT IN (sorted set)", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_DBL, SIFT_VK_T_F64);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 0, 100000, SIFT_VK_CMP_GT);
	c.j.where_slot = 1;
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 0, 255);
	add_agg(&c, SIFT_VK_AK_MIN, 0, 255);
	add_agg(&c, SIFT_VK_AK_SUM_F, 1, 255);
	case_check("no row qualifies", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	add_agg(&c, SIFT_VK_AK_SUM_I, 0, 255);
	add_agg(&c, SIFT_VK_AK_MIN, 0, 255);
	add_agg(&c, SIFT_VK_AK_MAX, 0, 255);
	case_check("exact 128-bit sum near int64 limits", &c, d, 1e-9);
}

/* ---- T3: dense keys ---- */
static void
t3_dense(const TData *d)
{
	Case		c;

	printf("T3 dense\n");
	for (int variant = 0; variant < 4; variant++)
	{
		static const char *names[] = {"1 key (null slot)", "2 keys, 2 aggregates (shared memory)", "2 keys, 3 aggregates (global)",
			"2 keys, 12341 groups (global)"};

		case_init(&c, d, SIFT_VK_DENSE);
		ld(&c, 0, C_V32, SIFT_VK_T_I64);
		ld(&c, 1, C_DBL, SIFT_VK_T_F64);
		ld(&c, 2, C_K0, SIFT_VK_T_I64);
		ld(&c, 3, C_K1, SIFT_VK_T_I64);
		ld(&c, 4, C_KL, SIFT_VK_T_I64);
		ld(&c, 5, C_BIG, SIFT_VK_T_I64);
		c.j.nkeys = variant == 0 ? 1 : 2;
		c.j.key_reg[0] = variant == 3 ? 4 : 2;
		c.j.key_reg[1] = 3;
		c.j.dense_kmin[0] = 0;
		c.j.dense_span[0] = variant == 3 ? 301 : 11;
		c.j.dense_kmin[1] = 100;
		c.j.dense_span[1] = 41;
		add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
		add_agg(&c, SIFT_VK_AK_SUM_I, 0, 255);
		if (variant != 1)
			add_agg(&c, SIFT_VK_AK_SUM_F, 1, 255);
		if (variant == 0)
			add_agg(&c, SIFT_VK_AK_MAX, 5, 255);
		case_check(names[variant], &c, d, 1e-9);
	}
	case_init(&c, d, SIFT_VK_DENSE);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_K0, SIFT_VK_T_I64);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 0, 0, SIFT_VK_CMP_GE);
	c.j.where_slot = 1;
	c.j.nkeys = 1;
	c.j.key_reg[0] = 1;
	c.j.dense_span[0] = 11;
	add_agg(&c, SIFT_VK_AK_MIN, 0, 255);
	add_agg(&c, SIFT_VK_AK_MAX, 0, 255);
	add_agg(&c, SIFT_VK_AK_COUNT, 0, 255);
	case_check("1 key, WHERE, MIN/MAX", &c, d, 1e-9);
}

/* ---- T4: hashed keys ---- */
static void
build_hash_data(TData *d, uint32_t ngran, uint32_t nk, uint64_t groups, Rng *rng)
{
	td_init(d, ngran, 10, SIFT_VK_GRANULE_ROWS);
	for (uint32_t gi = 0; gi < ngran; gi++)
	{
		TGran	   *g = &d->g[gi];

		for (uint32_t r = 0; r < g->nrows; r++)
		{
			uint64_t	gid = ((uint64_t) gi * SIFT_VK_GRANULE_ROWS + r) % groups;

			for (uint32_t k = 0; k < 8; k++)
				set_val(g, k, r, k == 0 ? (int64_t) gid : (int64_t) ((gid * 2654435761u + k * 97) % (k * 1000 + 13)), k == 0 || gid % (k + 3) != 0);
			set_val(g, 8, r, rng_range(rng, -50000, 50000), rng_next(rng) % 17 != 0);
			set_val(g, 9, r, f8_key((double) rng_range(rng, 1, 99999) / 100.0), true);
		}
		for (uint32_t c = 0; c < 10; c++)
			put_col(g, c, c == 9 ? 1 : 0, rng, gi * 16 + c);
	}
	td_publish(d);
	(void) nk;
}

static void
hash_case(Case *c, const TData *d, uint32_t nk, uint64_t groups)
{
	case_init(c, d, SIFT_VK_HASH);
	for (uint32_t k = 0; k < 10; k++)
		ld(c, k, k, k == 9 ? SIFT_VK_T_F64 : SIFT_VK_T_I64);
	c->j.nkeys = nk;
	for (uint32_t k = 0; k < nk; k++)
		c->j.key_reg[k] = (uint8_t) k;
	c->j.est_groups = groups;
	add_agg(c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(c, SIFT_VK_AK_SUM_I, 8, 255);
	add_agg(c, SIFT_VK_AK_MIN, 8, 255);
	add_agg(c, SIFT_VK_AK_MAX, 8, 255);
	add_agg(c, SIFT_VK_AK_SUM_F, 9, 255);
	add_agg(c, SIFT_VK_AK_COUNT, 8, 255);
}

static void
t4_hash(void)
{
	static const struct { uint32_t nk, ngran; uint64_t groups; } cfg[] = {
		{1, 3, 1}, {1, 3, 64}, {2, 4, 1000}, {3, 10, 65536}, {4, 4, 1000}, {8, 4, 1000}, {8, 10, 65536}, {2, 40, 300000}, {2, 130, 1000000},
	};
	Rng			rng = {0x4444};

	printf("T4 hash\n");
	for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
	{
		TData		d;
		Case		c;
		char		name[96];

		build_hash_data(&d, cfg[i].ngran, cfg[i].nk, cfg[i].groups, &rng);
		hash_case(&c, &d, cfg[i].nk, cfg[i].groups);
		snprintf(name, sizeof(name), "%u keys, %llu groups, %u rows", cfg[i].nk, (unsigned long long) cfg[i].groups, cfg[i].ngran * SIFT_VK_GRANULE_ROWS);
		case_check(name, &c, &d, 1e-9);
		if (cfg[i].groups == 300000)
		{
			SiftVkResult res;
			SiftVkStatus st;

			c.j.est_groups = 4;
			c.j.nops = c.p.n;
			c.j.ops = c.p.w;
			st = sift_vk_run(vk, &c.j, NULL, &res);
			printf("   estimate 4 for 300000 groups: status %d (%s)\n", st, st == SIFT_VK_OVERFLOW ? "overflow, the CPU runs the scan" : "unexpected");
			CHECK(st == SIFT_VK_OVERFLOW, "T4 a table too small must overflow: status %d", st);
		}
		td_free(&d);
	}
}

/* ---- T5: the expression machine ---- */
static void
agg3(Case *c, uint8_t reg, bool fl)
{
	add_agg(c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(c, fl ? SIFT_VK_AK_SUM_F : SIFT_VK_AK_SUM_I, reg, 255);
	add_agg(c, fl ? SIFT_VK_AK_MIN_F : SIFT_VK_AK_MIN, reg, 255);
	add_agg(c, fl ? SIFT_VK_AK_MAX_F : SIFT_VK_AK_MAX, reg, 255);
}

static void
t5_vm(const TData *d)
{
	Case		c;
	static const char *units[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
	static const char *fields[] = {"year", "quarter", "month", "day", "dow", "isodow", "doy", "hour", "minute", "epoch"};
	char		name[96];

	printf("T5 expression machine\n");
	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_DIV, SIFT_VK_T_I64);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 1, 0, SIFT_VK_CMP_NE);
	P_BIN(&c.p, SIFT_VK_OP_DIV, SIFT_VK_T_I64, 2, 0, 1, 1);
	P_BIN(&c.p, SIFT_VK_OP_MUL, SIFT_VK_T_I64, 3, 0, 0, 0);
	P_BIN(&c.p, SIFT_VK_OP_SUB, SIFT_VK_T_I64, 4, 3, 0, 0);
	agg3(&c, 2, false);
	agg3(&c, 4, false);
	case_check("int division masked by the divisor", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_DIV, SIFT_VK_T_I64);
	P_BIN(&c.p, SIFT_VK_OP_DIV, SIFT_VK_T_I64, 2, 0, 1, 0);
	agg3(&c, 2, false);
	case_check("division by zero on a live row", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_KB, SIFT_VK_T_I32);
	P_BINC(&c.p, SIFT_VK_OP_ADD, SIFT_VK_T_I32, 1, 0, 100, 0);
	agg3(&c, 1, false);
	case_check("int4 overflow", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_KB, SIFT_VK_T_I32);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 0, 2147483500LL, SIFT_VK_CMP_LT);
	P_BINC(&c.p, SIFT_VK_OP_ADD, SIFT_VK_T_I32, 2, 0, 100, 1);
	agg3(&c, 2, false);
	case_check("int4 overflow masked off", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	P_BIN(&c.p, SIFT_VK_OP_ADD, SIFT_VK_T_I64, 1, 0, 0, 0);
	agg3(&c, 1, false);
	case_check("int8 overflow", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	p_op(&c.p, SIFT_VK_OP_NEG, SIFT_VK_T_I64, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
	agg3(&c, 1, false);
	case_check("int8 negate of the least value", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_DBL, SIFT_VK_T_F64);
	P_BINC(&c.p, SIFT_VK_OP_MUL, SIFT_VK_T_F64, 1, 0, dbits(1.5), 0);
	P_BINC(&c.p, SIFT_VK_OP_ADD, SIFT_VK_T_F64, 2, 1, dbits(2.25), 0);
	P_BINC(&c.p, SIFT_VK_OP_DIV, SIFT_VK_T_F64, 3, 2, dbits(3.0), 0);
	p_op(&c.p, SIFT_VK_OP_ABS, SIFT_VK_T_F64, 0, 0, 4, 3, 0, 0, 0, 0, 0, 0);
	agg3(&c, 3, true);
	agg3(&c, 4, true);
	case_check("double arithmetic (IEEE exact)", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_DBL, SIFT_VK_T_F64);
	P_BINC(&c.p, SIFT_VK_OP_MUL, SIFT_VK_T_F64, 1, 0, dbits(1e306), 0);
	agg3(&c, 1, true);
	case_check("double overflow", &c, d, 1e-9);

	for (int ty = 0; ty < 2; ty++)
	{
		for (int u = 0; u < 8; u++)
		{
			case_init(&c, d, SIFT_VK_UNGROUPED);
			ld(&c, 0, ty ? C_DATE : C_TS, ty ? SIFT_VK_T_I32 : SIFT_VK_T_I64);
			p_op(&c.p, SIFT_VK_OP_DATE_TRUNC, ty ? SIFT_VK_T_I32 : SIFT_VK_T_I64, 0, 0, 1, 0, 0, 0, 0, 0, u, 0);
			agg3(&c, 1, false);
			snprintf(name, sizeof(name), "date_trunc(%s) of a %s", units[u], ty ? "date" : "timestamp");
			if (ty && u < 3)
				continue;
			case_check(name, &c, d, 1e-9);
		}
		for (int f = 0; f < 10; f++)
		{
			if (ty && (f == 7 || f == 8))
				continue;
			case_init(&c, d, SIFT_VK_UNGROUPED);
			ld(&c, 0, ty ? C_DATE : C_TS, ty ? SIFT_VK_T_I32 : SIFT_VK_T_I64);
			p_op(&c.p, SIFT_VK_OP_EXTRACT, ty ? SIFT_VK_T_I32 : SIFT_VK_T_I64, 0, 0, 1, 0, 0, 0, 0, 0, f, 0);
			agg3(&c, 1, false);
			snprintf(name, sizeof(name), "extract(%s) of a %s", fields[f], ty ? "date" : "timestamp");
			case_check(name, &c, d, 1e-9);
		}
	}
	{
		TData		sp;
		Rng			rng = {99};

		td_init(&sp, 1, 1, 100);
		for (uint32_t r = 0; r < 100; r++)
			set_val(&sp.g[0], 0, r, r == 37 ? INT64_MAX : (int64_t) r * 86400000000LL, true);
		put_col(&sp.g[0], 0, 0, &rng, 1);
		td_publish(&sp);
		case_init(&c, &sp, SIFT_VK_UNGROUPED);
		ld(&c, 0, 0, SIFT_VK_T_I64);
		p_op(&c.p, SIFT_VK_OP_EXTRACT, SIFT_VK_T_I64, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
		agg3(&c, 1, false);
		case_check("calendar: infinity is an error", &c, &sp, 1e-9);
		case_init(&c, &sp, SIFT_VK_UNGROUPED);
		ld(&c, 0, 0, SIFT_VK_T_I64);
		P_CMPC(&c.p, SIFT_VK_T_I64, 1, 0, INT64_MAX, SIFT_VK_CMP_NE);
		p_op(&c.p, SIFT_VK_OP_EXTRACT, SIFT_VK_T_I64, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0);
		c.j.where_slot = 1;
		agg3(&c, 2, false);
		case_check("calendar: infinity masked off", &c, &sp, 1e-9);
		td_free(&sp);
	}

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_NUM, SIFT_VK_T_NUM);
	p_op(&c.p, SIFT_VK_OP_CAST, SIFT_VK_T_NUM, SIFT_VK_T_F64, 0, 1, 0, 0, 0, 0, 2, 0, 0);
	p_op(&c.p, SIFT_VK_OP_RESCALE, SIFT_VK_T_NUM, 0, 0, 2, 0, 0, 0, 0, 0, 3, 0);
	agg3(&c, 1, true);
	agg3(&c, 2, false);
	case_check("numeric cast and rescale", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_BIG, SIFT_VK_T_I64);
	p_op(&c.p, SIFT_VK_OP_CAST, SIFT_VK_T_I64, SIFT_VK_T_NUM, 0, 1, 0, 0, 0, 0, 9, 0, 0);
	agg3(&c, 1, false);
	case_check("numeric cast overflow", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	ld(&c, 1, C_DBL, SIFT_VK_T_F64);
	ld(&c, 2, C_K1, SIFT_VK_T_I64);
	P_CMPC(&c.p, SIFT_VK_T_I64, 1, 0, 0, SIFT_VK_CMP_GT);
	P_CMPC(&c.p, SIFT_VK_T_F64, 2, 1, dbits(500.0), SIFT_VK_CMP_LT);
	P_MAND(&c.p, 3, 1, 2);
	P_MOR(&c.p, 4, 1, 2);
	P_MNOT(&c.p, 5, 1);
	P_MOR(&c.p, 6, 5, 3);
	p_op(&c.p, SIFT_VK_OP_IS_NULL, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0);
	P_MAND(&c.p, 8, 4, 6);
	c.j.where_slot = 8;
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 2, 7);
	add_agg(&c, SIFT_VK_AK_COUNT, 0, 255);
	case_check("three-valued WHERE", &c, d, 1e-9);

	case_init(&c, d, SIFT_VK_UNGROUPED);
	ld(&c, 0, C_V32, SIFT_VK_T_I64);
	P_CONST(&c.p, 1, 7);
	p_op(&c.p, SIFT_VK_OP_IS_NULL, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
	p_op(&c.p, SIFT_VK_OP_COPY, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0);
	p_op(&c.p, SIFT_VK_OP_BLEND, 0, 0, 0, 2, 1, 0, 1, 0, 0, 0, 0);
	agg3(&c, 2, false);
	add_agg(&c, SIFT_VK_AK_COUNT, 2, 255);
	case_check("COALESCE by blend", &c, d, 1e-9);
}

/* ---- T6: top N ---- */
typedef struct Cand
{
	int64_t		key;
	uint64_t	tag;
} Cand;

static void
topn_case(const char *name, Case *c, const TData *d, uint32_t k)
{
	RefCand    *exp = malloc(sizeof(RefCand) * (k + 1));
	uint32_t	nexp;
	SiftVkResult res;
	SiftVkStatus st;
	uint64_t	bad = 0;
	uint32_t	ncand;
	const Cand *cand;

	c->j.mode = SIFT_VK_TOPN;
	c->j.topn_k = k;
	c->j.nops = c->p.n;
	c->j.ops = c->p.w;
	c->j.npool = c->p.npool;
	c->j.pool = c->p.pool;
	nexp = ref_topn(d, &c->j, exp, k);
	st = sift_vk_run(vk, &c->j, NULL, &res);
	cases_run++;
	CHECK(st == SIFT_VK_OK, "%s: status %d (%s)", name, st, sift_vk_error(vk));
	if (st != SIFT_VK_OK)
	{
		cases_bad++;
		free(exp);
		return;
	}
	cand = (const Cand *) res.cand;
	ncand = res.ncand;
	if (ncand != nexp)
	{
		bad++;
		fprintf(stderr, "  %s: %u candidates, expected %u\n", name, ncand, nexp);
	}
	for (uint32_t i = 0; i < ncand && i < nexp; i++)
	{
		bool		isnull = (cand[i].tag >> 62) & 1;
		uint32_t	gran = (uint32_t) ((cand[i].tag >> 32) & 0x3FFFFFFFu),
					row = (uint32_t) cand[i].tag;
		int64_t		key = 0;
		bool		rn = false;
		bool		have = ref_row_key(d, &c->j, gran, row, &key, &rn);

		if (isnull != exp[i].isnull || (!isnull && cand[i].key != exp[i].key) || !have || rn != isnull || (!isnull && key != cand[i].key))
		{
			if (bad++ < 4)
				fprintf(stderr, "  %s: candidate %u (key %lld, null %d, granule %u row %u) vs oracle key %lld null %d; row's own key %lld null %d\n", name, i,
						(long long) cand[i].key, isnull, gran, row, (long long) exp[i].key, exp[i].isnull, (long long) key, rn);
		}
		for (uint32_t j = i + 1; j < ncand && j < i + 4; j++)
			if (cand[j].tag == cand[i].tag)
				bad++;
	}
	CHECK(bad == 0, "%s: %llu differences", name, (unsigned long long) bad);
	cases_bad += bad != 0;
	printf("   %-34s %s  K %u  candidates %u  kernel %.2f ms  wall %.2f ms\n", name, bad ? "DIFFERS" : "ok", k, ncand, res.times.kernel_ms, res.times.wall_ms);
	free(exp);
}

static void
t6_topn(const TData *d, const TData *big)
{
	static const uint32_t ks[] = {10, 100, 256, 300, 1000};
	Case		c;
	char		name[96];

	printf("T6 top N\n");
	for (int v = 0; v < 6; v++)
		for (int ki = 0; ki < 5; ki++)
		{
			static const char *names[] = {"int desc", "int asc", "double desc", "double asc, WHERE", "int desc nulls first", "expression desc"};

			case_init(&c, d, SIFT_VK_TOPN);
			ld(&c, 0, v == 2 || v == 3 ? C_DBL : v == 5 ? C_BIG : C_V32, v == 2 || v == 3 ? SIFT_VK_T_F64 : SIFT_VK_T_I64);
			ld(&c, 1, C_FLAG, SIFT_VK_T_I64);
			c.j.sort_reg = 0;
			c.j.sort_float = v == 2 || v == 3;
			c.j.topn_desc = v == 0 || v == 2 || v == 4 || v == 5;
			c.j.topn_nulls = v == 4;
			if (v == 3)
			{
				P_CMPC(&c.p, SIFT_VK_T_I64, 1, 1, 1, SIFT_VK_CMP_EQ);
				c.j.where_slot = 1;
			}
			if (v == 5)
			{
				ld(&c, 2, C_V32, SIFT_VK_T_I64);
				P_BINC(&c.p, SIFT_VK_OP_MUL, SIFT_VK_T_I64, 3, 2, 3, 0);
				P_BINC(&c.p, SIFT_VK_OP_ADD, SIFT_VK_T_I64, 3, 3, 1, 0);
				c.j.sort_reg = 3;
			}
			snprintf(name, sizeof(name), "%s", names[v]);
			if (v == 5)
				snprintf(name, sizeof(name), "%s (v32*3+1)", names[v]);
			topn_case(name, &c, d, ks[ki]);
		}
	for (int ki = 0; ki < 5; ki++)
	{
		case_init(&c, big, SIFT_VK_TOPN);
		ld(&c, 0, 8, SIFT_VK_T_I64);
		c.j.sort_reg = 0;
		c.j.topn_desc = true;
		snprintf(name, sizeof(name), "int desc, %u granules (theta)", big->ngran);
		topn_case(name, &c, big, ks[ki]);
	}
}

/* ---- T9: the fast kernels: random programs of columns and comparisons, granules that are not a multiple of four rows ---- */
typedef struct T9Col
{
	uint32_t	col;
	int64_t		lo,
				hi;
} T9Col;

static const T9Col t9_cols[] = {
	{C_K0, -2, 11}, {C_K1, 95, 145}, {C_BIG, -2000, 2000}, {C_V32, -1000, 1000}, {C_DBL, 0, 1000}, {C_FLAG, 0, 1}, {C_TS, -3200000000000000LL, 3200000000000000LL},
	{C_DATE, -36525, 36525}, {C_NUM, -10000000, 10000000}, {C_DIV, -3, 3}, {C_KB, 2147482990LL, 2147483660LL}, {C_KL, -5, 305},
};

typedef struct T9Job
{
	Case		c;
	int			reg_of[C_NCOLS];
	uint32_t	nreg;
} T9Job;

static uint32_t
t9_reg(T9Job *t, uint32_t col)
{
	if (t->reg_of[col] < 0)
	{
		ld(&t->c, t->nreg, col, col == C_DBL ? SIFT_VK_T_F64 : SIFT_VK_T_I64);
		t->reg_of[col] = (int) t->nreg++;
	}
	return (uint32_t) t->reg_of[col];
}

static uint32_t
t9_int_col(Rng *rng)
{
	uint32_t	col;

	do
		col = t9_cols[rng_next(rng) % (sizeof(t9_cols) / sizeof(t9_cols[0]))].col;
	while (col == C_DBL);
	return col;
}

/* WHERE: a conjunction of up to four comparisons of a column with a constant, the constant an immediate or a register, on either side */
static void
t9_where(T9Job *t, Rng *rng)
{
	uint32_t	np = (uint32_t) (rng_next(rng) % 5),
				acc = 0;

	for (uint32_t p = 0; p < np; p++)
	{
		const T9Col *cc = &t9_cols[rng_next(rng) % (sizeof(t9_cols) / sizeof(t9_cols[0]))];
		uint32_t	reg = t9_reg(t, cc->col);
		int64_t		v = cc->col == C_DBL ? dbits((double) rng_range(rng, 0, 100000) / 100.0) : cc->col == C_BIG ? (rng_next(rng) & 1 ? INT64_MAX : ((int64_t) 1 << 62)) :
			rng_range(rng, cc->lo, cc->hi);
		uint32_t	cmp = (uint32_t) (rng_next(rng) % 6);

		uint32_t	ty = cc->col == C_DBL ? SIFT_VK_T_F64 : SIFT_VK_T_I64,
					form = (uint32_t) (rng_next(rng) % 3),
					creg = t->nreg;

		if (form == 0)
			P_CMPC(&t->c.p, ty, 1 + p, reg, v, cmp);
		else
		{
			t->nreg++;
			P_CONST(&t->c.p, creg, v);
			if (form == 1)
				P_CMP(&t->c.p, ty, 1 + p, reg, creg, cmp);
			else
				P_CMP(&t->c.p, ty, 1 + p, creg, reg, cmp);
		}
		if (p == 0)
			acc = 1;
		else
		{
			P_MAND(&t->c.p, 8 + p, acc, 1 + p);
			acc = 8 + p;
		}
	}
	t->c.j.where_slot = np ? acc : 255;
}

static void
t9_aggs(T9Job *t, Rng *rng, uint32_t n)
{
	static const uint8_t kinds[] = {SIFT_VK_AK_COUNT_STAR, SIFT_VK_AK_COUNT, SIFT_VK_AK_SUM_I, SIFT_VK_AK_SUM_F, SIFT_VK_AK_MIN, SIFT_VK_AK_MAX,
		SIFT_VK_AK_MIN_F, SIFT_VK_AK_MAX_F};

	for (uint32_t a = 0; a < n; a++)
	{
		uint8_t		k = kinds[rng_next(rng) % sizeof(kinds)];
		bool		dbl = k == SIFT_VK_AK_SUM_F || k == SIFT_VK_AK_MIN_F || k == SIFT_VK_AK_MAX_F;

		add_agg(&t->c, k, k == SIFT_VK_AK_COUNT_STAR ? 255 : (uint8_t) t9_reg(t, dbl ? C_DBL : t9_int_col(rng)), 255);
	}
}

/* dense variants beyond the random keys: 1 one key of 256 groups, 2 two keys of 33, 3 a key outside its range (an error) */
static void
t9_run(const TData *d, Rng *rng, SiftVkMode mode, int i, int variant)
{
	T9Job		t;
	char		name[96];
	uint64_t	before = fast_cases;
	uint32_t	n = 1 + (uint32_t) (rng_next(rng) % 8),
				nk = 1 + (uint32_t) (rng_next(rng) % (mode == SIFT_VK_HASH ? 3 : 2));
	static const uint32_t hash_keys[] = {C_K0, C_K1, C_KL, C_KB};

	memset(&t, 0, sizeof(t));
	for (int c = 0; c < C_NCOLS; c++)
		t.reg_of[c] = -1;
	case_init(&t.c, d, mode);
	t9_where(&t, rng);
	if (mode == SIFT_VK_DENSE)
	{
		n = nk == 2 && n > 2 ? 2 : n;
		t.c.j.nkeys = nk;
		t.c.j.key_reg[0] = (uint8_t) t9_reg(&t, nk == 1 && (rng_next(rng) & 1) ? C_K1 : C_K0);
		t.c.j.key_reg[1] = (uint8_t) t9_reg(&t, C_K1);
		t.c.j.dense_kmin[0] = t.reg_of[C_K1] == t.c.j.key_reg[0] ? 100 : 0;
		t.c.j.dense_span[0] = t.reg_of[C_K1] == t.c.j.key_reg[0] ? 41 : 11;
		t.c.j.dense_kmin[1] = 100;
		t.c.j.dense_span[1] = 41;
		if (variant == 1)
		{
			t.c.j.nkeys = 1;
			t.c.j.dense_span[0] = 256;
			t.c.j.dense_kmin[0] = 0;
			t.c.j.key_reg[0] = (uint8_t) t9_reg(&t, C_K0);
			n = n > 5 ? 5 : n;
		}
		else if (variant == 2)
		{
			t.c.j.nkeys = 2;
			t.c.j.key_reg[0] = (uint8_t) t9_reg(&t, C_K0);
			t.c.j.key_reg[1] = (uint8_t) t9_reg(&t, C_FLAG);
			t.c.j.dense_kmin[0] = t.c.j.dense_kmin[1] = 0;
			t.c.j.dense_span[0] = 11;
			t.c.j.dense_span[1] = 3;
			n = n > 4 ? 4 : n;
		}
		else if (variant == 3)
		{
			t.c.j.nkeys = 1;
			t.c.j.key_reg[0] = (uint8_t) t9_reg(&t, C_K1);
			t.c.j.dense_kmin[0] = 100;
			t.c.j.dense_span[0] = 20;
		}
	}
	else if (mode == SIFT_VK_HASH)
	{
		t.c.j.nkeys = nk;
		uint32_t	first = (uint32_t) (rng_next(rng) % 4);

		t.c.j.est_groups = 65536;
		for (uint32_t k = 0; k < nk; k++)
			t.c.j.key_reg[k] = (uint8_t) t9_reg(&t, hash_keys[(first + k) % 4]);
	}
	t9_aggs(&t, rng, n);
	snprintf(name, sizeof(name), "T9 %s #%d%s (%u aggregates)", mode == SIFT_VK_UNGROUPED ? "ungrouped" : mode == SIFT_VK_DENSE ? "dense" : "hash", i,
			 variant == 1 ? " 256 groups" : variant == 2 ? " 2 keys" : variant == 3 ? " key out of range" : "", n);
	case_check(name, &t.c, d, 1e-9);
	CHECK(fast_cases == before + 1, "%s: the job did not take the fast kernel", name);
}

static void
t9_fast(Rng *rng)
{
	TData		d;
	static const struct { SiftVkMode mode; int n; } plan[] = {{SIFT_VK_UNGROUPED, 24}, {SIFT_VK_DENSE, 16}, {SIFT_VK_HASH, 16}};

	build_base(&d, 6, rng, 5001);
	printf("T9 fast kernels (random programs, granules of 5001 and 4099 rows)\n");
	for (size_t m = 0; m < sizeof(plan) / sizeof(plan[0]); m++)
		for (int i = 0; i < plan[m].n + (plan[m].mode == SIFT_VK_DENSE ? 12 : 0); i++)
			t9_run(&d, rng, plan[m].mode, i, i < plan[m].n ? 0 : 1 + (i - plan[m].n) % 3);
	td_free(&d);
}

void
test_agg(SiftVk *dev, uint64_t seed, const char *only)
{
	TData		d,
				big;
	Rng			rng = {seed ^ 0xA6A6};
	bool		all = !only;

	vk = dev;
	if (all || strstr(only, "T2") || strstr(only, "T3") || strstr(only, "T5") || strstr(only, "T6"))
		build_base(&d, 7, &rng, 5000);
	if (all || strstr(only, "T2"))
		t2_ungrouped(&d);
	if (all || strstr(only, "T3"))
		t3_dense(&d);
	if (all || strstr(only, "T4"))
		t4_hash();
	if (all || strstr(only, "T5"))
		t5_vm(&d);
	if (all || strstr(only, "T6"))
	{
		build_hash_data(&big, 40, 1, 65536, &rng);
		t6_topn(&d, &big);
		td_free(&big);
	}
	if (all || strstr(only, "T2") || strstr(only, "T3") || strstr(only, "T5") || strstr(only, "T6"))
		td_free(&d);
	if (all || strstr(only, "T9"))
		t9_fast(&rng);
	if (all || strstr(only, "T10"))
		test_emit(dev, &rng);
	if (all || strstr(only, "T12"))
		test_finish(dev, &rng);
	printf("aggregate cases: %llu run (%llu on a fast kernel, each also on the machine), %llu with differences\n", (unsigned long long) cases_run,
		   (unsigned long long) fast_cases, (unsigned long long) cases_bad);
}
