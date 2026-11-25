/*
 * tests/main.c - Spec 39 on the real device: the selector (T0), the decoder against the codec's decoder value by value (T1), the
 * cache (T7) and the fallbacks (T8). The aggregation kernels are tests/agg.c (T2..T6). Exit status 0 when nothing failed.
 *   vk_v6_test [--device SEL] [--only T1,T7] [--seed N] [--nodriver]
 */
#include "postgres.h"

#include <sys/wait.h>
#include <unistd.h>

#include "agg_case.h"

extern uint64_t cov_root[16],
			cov_inner[16];

static char *dev_sel = "";
static uint64_t seed = 20260101;
static const char *only = NULL;

static bool
want(const char *t)
{
	return !only || strstr(only, t);
}

/* ---- T0: the devices and the selector ---- */
static void
t0_devices(void)
{
	SiftVkDevInfo di[8];
	char		err[256];
	int			chosen,
				n = sift_vk_list_devices(di, 8, &chosen, "", false, err, sizeof(err));

	printf("T0 devices: %d\n", n);
	CHECK(n > 0, "no device: %s", err);
	for (int i = 0; i < n; i++)
		printf("   [%d] %s  type %d  PCI %02x:%02x.%u  link %.1f GT/s x%u (max %.1f x%u)  %.2f GB/s  %.1f GiB  %s%s\n", i, di[i].name, di[i].type,
			   di[i].pci_bus, di[i].pci_dev, di[i].pci_fn, di[i].link_cur_gts, di[i].link_cur_width, di[i].link_max_gts, di[i].link_max_width,
			   di[i].pcie_gbps, di[i].vram_bytes / 1073741824.0, di[i].usable ? "usable" : "NOT usable: ", di[i].usable ? "" : di[i].why);
	if (n <= 0)
		return;
	printf("   auto-selected: [%d] %s (PCI %02x:%02x, %.2f GB/s)\n", chosen, di[chosen].name, di[chosen].pci_bus, di[chosen].pci_dev, di[chosen].pcie_gbps);
	for (int i = 0; i < n; i++)
		if (di[i].usable && di[i].type == di[chosen].type && di[i].vram_bytes == di[chosen].vram_bytes)
			CHECK(di[i].pcie_gbps <= di[chosen].pcie_gbps, "device %d has a faster link (%.2f GB/s) than the one chosen (%.2f)", i, di[i].pcie_gbps,
				  di[chosen].pcie_gbps);
	for (int i = 0; i < n; i++)
	{
		char		sel[32];
		int			c2;

		snprintf(sel, sizeof(sel), "%x:%x", di[i].pci_bus, di[i].pci_dev);
		sift_vk_list_devices(di, 8, &c2, sel, false, err, sizeof(err));
		CHECK(c2 >= 0 && di[c2].pci_bus == strtoul(sel, NULL, 16), "selector %s", sel);
		snprintf(sel, sizeof(sel), "%d", i);
		sift_vk_list_devices(di, 8, &c2, sel, false, err, sizeof(err));
		CHECK(c2 == i, "selector index %d gave %d", i, c2);
	}
	{
		int			c2;

		sift_vk_list_devices(di, 8, &c2, "no-such-device-name", false, err, sizeof(err));
		CHECK(c2 == -1, "an unknown name selects nothing");
	}
}

/* ---- T1: the decoder ---- */
typedef struct DecCase
{
	const char *name;
	const char *policy;
	uint32_t	enc;
	int			kind;			/* the data */
	int			nulls;
	bool		lz4;
	int			tz;
	bool		w0;
} DecCase;

enum
{
	K_CONST, K_SMALL, K_WIDE, K_SORTED, K_RUNS, K_FEW, K_NEG, K_BOOL, K_DEC2, K_RANDF, K_RDF, K_NUM
};

static void
fill_col(TGran *g, uint32_t c, int kind, int nulls, Rng *rng)
{
	uint32_t	n = g->nrows;
	int64_t		run = 0,
				acc = rng_range(rng, -1000, 1000),
				few[6];
	uint32_t	left = 0;

	for (int i = 0; i < 6; i++)
		few[i] = rng_range(rng, -4000000000000LL, 4000000000000LL);
	for (uint32_t r = 0; r < n; r++)
	{
		int64_t		v = 0;

		switch (kind)
		{
			case K_CONST: v = 424242; break;
			case K_SMALL: v = rng_range(rng, 100, 130); break;
			case K_WIDE: v = (int64_t) rng_next(rng); break;
			case K_SORTED: acc += rng_range(rng, 0, 9); v = acc; break;
			case K_RUNS:
				if (!left)
				{
					run = rng_range(rng, -50000, 50000);
					left = (uint32_t) rng_range(rng, 1, 400);
				}
				left--;
				v = run;
				break;
			case K_FEW: v = few[rng_next(rng) % 6]; break;
			case K_NEG: v = rng_range(rng, -70000, -3); break;
			case K_BOOL: v = rng_next(rng) & 1; break;
			case K_DEC2: v = f8_key((double) rng_range(rng, -99999, 99999) / 100.0); break;
			case K_RANDF: v = f8_key(f8_from_key(f8_key(rng_unit(rng) * 1e6 - 5e5))); break;
			case K_RDF: v = f8_key(1.0 + rng_unit(rng)); break;
			case K_NUM: v = rng_range(rng, -9999999, 9999999); break;
		}
		g->col[c].v[r] = v;
		g->col[c].ok[r] = 1;
	}
	if (nulls != NULLS_AUTO)
		for (uint32_t r = 0; r < n; r++)
		{
			bool		isnull = nulls == NULLS_BITMAP ? (rng_next(rng) % 5 == 0) : nulls == NULLS_SPARSE_NULL ? (r % 331 == 7) : (r % 331 != 7);

			if (isnull)
			{
				g->col[c].ok[r] = 0;
				g->col[c].v[r] = 0;
			}
		}
}

static SiftVkStatus
run_load_all(SiftVk *vk, TData *d, uint32_t ncols, SiftVkResult *res)
{
	Prog		p = {0};
	SiftVkJob	j = {.mode = SIFT_VK_UNGROUPED, .ncols = ncols, .ngran = d->ngran, .gran = d->vg, .where_slot = 255, .naggs = 1};

	for (uint32_t c = 0; c < ncols; c++)
		P_COL(&p, c, c, SIFT_VK_T_I64);
	j.nops = p.n;
	j.ops = p.w;
	j.agg[0] = (SiftVkAgg) {SIFT_VK_AK_COUNT, 0, 255};
	return sift_vk_run(vk, &j, NULL, res);
}

static uint64_t
dec_compare(SiftVk *vk, TGran *g, uint32_t c, const char *what)
{
	int64_t    *dv = calloc(SIFT_VK_GRANULE_ROWS, 8),
			   *cv = calloc(SIFT_VK_GRANULE_ROWS, 8);
	uint8_t    *dk = calloc(SIFT_VK_GRANULE_ROWS, 1),
			   *ck = calloc(SIFT_VK_GRANULE_ROWS, 1);
	uint64_t	bad = 0;
	bool		have = sift_vk_test_dcol(vk, &g->chunk[c].key, dv, dk);

	CHECK(have, "%s: the column is not in the cache", what);
	CHECK(cpu_decode(&g->chunk[c], cv, ck), "%s: the codec's decoder failed", what);
	for (uint32_t r = 0; have && r < g->nrows; r++)
	{
		bool		ok = g->col[c].ok[r];

		if (ck[r] != ok || (ok && cv[r] != g->col[c].v[r]))
		{
			bad++;
			if (bad < 3)
				fprintf(stderr, "  %s: codec oracle differs from the data at row %u (the test is wrong)\n", what, r);
		}
		if (dk[r] != ok || (ok && dv[r] != g->col[c].v[r]))
		{
			bad++;
			if (bad < 4)
				fprintf(stderr, "  %s: row %u device %s %lld, data %s %lld\n", what, r, dk[r] ? "value" : "null", (long long) dv[r], ok ? "value" : "null",
						(long long) g->col[c].v[r]);
		}
	}
	free(dv);
	free(cv);
	free(dk);
	free(ck);
	return bad;
}

static void
t1_decode(SiftVk *vk)
{
	static const DecCase cases[] = {
		{"const", "auto", SIFT_VK_ENC_INT_CONST, K_CONST, NULLS_AUTO, false, 0, false},
		{"const w0", "auto", SIFT_VK_ENC_INT_CONST, K_CONST, NULLS_AUTO, false, 0, true},
		{"const nulls", "auto", SIFT_VK_ENC_INT_CONST, K_CONST, NULLS_BITMAP, false, 0, true},
		{"for", "for", SIFT_VK_ENC_INT_FOR, K_SMALL, NULLS_AUTO, false, 0, false},
		{"for wide", "for", SIFT_VK_ENC_INT_FOR, K_WIDE, NULLS_AUTO, false, 0, false},
		{"for neg", "for", SIFT_VK_ENC_INT_FOR, K_NEG, NULLS_BITMAP, false, 0, false},
		{"for sparse null", "for", SIFT_VK_ENC_INT_FOR, K_SMALL, NULLS_SPARSE_NULL, false, 0, false},
		{"for sparse value", "for", SIFT_VK_ENC_INT_FOR, K_SMALL, NULLS_SPARSE_VALUE, false, 0, false},
		{"for lz4", "for", SIFT_VK_ENC_INT_FOR, K_SMALL, NULLS_BITMAP, true, 0, false},
		{"for+lz4 node", "for+lz4", SIFT_VK_ENC_INT_FOR, K_SMALL, NULLS_AUTO, false, 0, false},
		{"delta", "delta", SIFT_VK_ENC_INT_DELTA, K_SORTED, NULLS_AUTO, false, 0, false},
		{"delta nulls", "delta", SIFT_VK_ENC_INT_DELTA, K_SORTED, NULLS_BITMAP, false, 0, false},
		{"delta lz4", "delta+for", SIFT_VK_ENC_INT_DELTA, K_SORTED, NULLS_AUTO, true, 0, false},
		{"rle", "rle", SIFT_VK_ENC_RLE, K_RUNS, NULLS_AUTO, false, 0, false},
		{"rle nulls", "rle", SIFT_VK_ENC_RLE, K_RUNS, NULLS_BITMAP, false, 0, false},
		{"rle+for lz4", "rle+for", SIFT_VK_ENC_RLE, K_RUNS, NULLS_AUTO, true, 0, false},
		{"dict", "dict", SIFT_VK_ENC_DICT_CODE, K_FEW, NULLS_AUTO, false, 0, false},
		{"dict nulls lz4", "dict", SIFT_VK_ENC_DICT_CODE, K_FEW, NULLS_BITMAP, true, 0, false},
		{"bool", "auto", SIFT_VK_ENC_BOOL_BITS, K_BOOL, NULLS_AUTO, false, 0, false},
		{"bool nulls", "auto", SIFT_VK_ENC_BOOL_BITS, K_BOOL, NULLS_BITMAP, true, 0, false},
		{"num64", "for", SIFT_VK_ENC_NUM64, K_NUM, NULLS_AUTO, false, 0, false},
		{"num64 nulls lz4", "scale+for", SIFT_VK_ENC_NUM64, K_NUM, NULLS_BITMAP, true, 0, false},
		{"alp", "alp", SIFT_VK_ENC_ALP, K_DEC2, NULLS_AUTO, false, 0, false},
		{"alp nulls", "alp", SIFT_VK_ENC_ALP, K_DEC2, NULLS_BITMAP, false, 0, false},
		{"alp lz4", "alp", SIFT_VK_ENC_ALP, K_DEC2, NULLS_AUTO, true, 0, false},
		{"alp random", "alp", SIFT_VK_ENC_ALP, K_RANDF, NULLS_AUTO, false, 0, false},
		{"float raw", "auto", SIFT_VK_ENC_FLOAT_RAW, K_RANDF, NULLS_AUTO, false, 0, false},
		{"float raw nulls lz4", "auto", SIFT_VK_ENC_FLOAT_RAW, K_RANDF, NULLS_BITMAP, true, 0, false},
		{"alp_rd", "auto", SIFT_VK_ENC_ALP_RD, K_RDF, NULLS_AUTO, false, 0, false},
		{"alp_rd tz", "auto", SIFT_VK_ENC_ALP_RD, K_RDF, NULLS_BITMAP, false, 8, false},
		{"alp_rd lz4", "auto", SIFT_VK_ENC_ALP_RD, K_RDF, NULLS_SPARSE_NULL, true, 0, false},
	};
	static const uint32_t rowsets[] = {8192, 5000, 1000, 64, 37, 1};
	Rng			rng = {seed ^ 0x1111};
	uint64_t	chunks = 0,
				skipped = 0,
				bad = 0;
	uint32_t	enc_done[32] = {0};

	for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
		for (size_t ri = 0; ri < sizeof(rowsets) / sizeof(rowsets[0]); ri++)
		{
			const DecCase *dc = &cases[ci];
			TData		d;
			ChunkSpec	sp = {.policy = dc->policy, .enc = dc->enc, .nulls = dc->nulls, .lz4 = dc->lz4, .tz = dc->tz, .w0 = dc->w0};
			char		what[96];
			SiftVkResult res;
			SiftVkStatus st;
			bool		built = true;

			td_init(&d, 2, 1, rowsets[ri]);
			for (uint32_t gi = 0; gi < 2 && built; gi++)
			{
				fill_col(&d.g[gi], 0, dc->kind, dc->nulls, &rng);
				if (dc->tz)
					for (uint32_t r = 0; r < d.g[gi].nrows; r++)
						d.g[gi].col[0].v[r] = d.g[gi].col[0].ok[r] ? f8_key((double) (rng_range(&rng, 1, 1 << 20) * 256) / 1024.0) : 0;
				built = build_chunk(&d.g[gi], 0, &sp, (uint32_t) (ci * 16 + ri));
			}
			if (!built)
			{
				if (ri == 0)
					printf("   skip %-22s (%s)\n", dc->name, sp.why);
				skipped++;
				td_free(&d);
				continue;
			}
			td_publish(&d);
			snprintf(what, sizeof(what), "T1 %s rows=%u", dc->name, rowsets[ri]);
			st = run_load_all(vk, &d, 1, &res);
			CHECK(st == SIFT_VK_OK, "%s: run status %d (%s)", what, st, sift_vk_error(vk));
			if (st == SIFT_VK_OK)
				for (uint32_t gi = 0; gi < 2; gi++)
				{
					bad += dec_compare(vk, &d.g[gi], 0, what);
					chunks++;
					enc_done[d.g[gi].chunk[0].enc & 31]++;
				}
			td_free(&d);
		}
	printf("T1 decode: %llu chunks compared value by value with the data and the codec's decoder, %llu differences, %llu cases skipped\n",
		   (unsigned long long) chunks, (unsigned long long) bad, (unsigned long long) skipped);
	printf("   encodings covered:");
	for (int e = 0; e < 32; e++)
		if (enc_done[e])
			printf(" %d:%u", e, enc_done[e]);
	printf("\n   codec node tags (root):");
	for (int e = 0; e < 16; e++)
		if (cov_root[e])
			printf(" %d:%llu", e, (unsigned long long) cov_root[e]);
	printf("   (inner):");
	for (int e = 0; e < 16; e++)
		if (cov_inner[e])
			printf(" %d:%llu", e, (unsigned long long) cov_inner[e]);
	printf("\n");
	CHECK(bad == 0, "decode differences: %llu", (unsigned long long) bad);
}

/* ---- T7: the cache ---- */
static void
t7_cache(const char *dev)
{
	char		err[256];
	SiftVk	   *vk = sift_vk_open(dev, false, 16 << 20, err, sizeof(err));
	TData		d;
	ChunkSpec	sp = {.policy = "for", .enc = SIFT_VK_ENC_INT_FOR, .nulls = NULLS_AUTO};
	Rng			rng = {seed ^ 0x7777};
	SiftVkResult res;
	SiftVkKey	keys[160];
	uint32_t	ng = 160,
				ncols = 2;

	CHECK(vk != NULL, "T7 open: %s", err);
	if (!vk)
		return;
	td_init(&d, ng, ncols, 8192);
	for (uint32_t g = 0; g < ng; g++)
		for (uint32_t c = 0; c < ncols; c++)
		{
			fill_col(&d.g[g], c, K_WIDE, NULLS_AUTO, &rng);
			CHECK(build_chunk(&d.g[g], c, &sp, 1), "T7 chunk");
		}
	td_publish(&d);
	for (uint32_t g = 0; g < ng; g++)
		keys[g] = d.g[g].chunk[0].key;
	CHECK(sift_vk_resident_share(vk, keys, ng) == 0.0, "T7 a cold cache holds nothing");
	d.ngran = 4;
	CHECK(run_load_all(vk, &d, ncols, &res) == SIFT_VK_OK, "T7 first run");
	CHECK(res.chunks_uploaded == 8, "T7 first run uploaded %u chunks, expected 8", res.chunks_uploaded);
	CHECK(run_load_all(vk, &d, ncols, &res) == SIFT_VK_OK, "T7 second run");
	CHECK(res.chunks_uploaded == 0 && res.chunks_resident == 8, "T7 a warm run uploaded %u (resident %u)", res.chunks_uploaded, res.chunks_resident);
	CHECK(sift_vk_resident_share(vk, keys, 4) == 1.0, "T7 four granules are resident");
	printf("T7 cache: warm run uploaded %llu bytes, %u chunks resident\n", (unsigned long long) res.bytes_uploaded, res.chunks_resident);
	/* more than the 16 MiB of the cache in 64-bit columns: the oldest slabs are evicted and everything stays correct */
	d.ngran = ng;
	for (int round = 0; round < 3; round++)
	{
		SiftVkStatus st = run_load_all(vk, &d, ncols, &res);

		CHECK(st == SIFT_VK_OK, "T7 oversize run %d: %d (%s)", round, st, sift_vk_error(vk));
		printf("   oversize round %d: uploaded %u, resident %u, batches %u\n", round, res.chunks_uploaded, res.chunks_resident, res.batches);
	}
	{
		uint64_t	bad = 0;

		for (uint32_t g = ng - 2; g < ng; g++)
			bad += dec_compare(vk, &d.g[g], 0, "T7 after eviction");
		CHECK(bad == 0, "T7 values differ after eviction");
	}
	CHECK(sift_vk_set_cache(vk, 32 << 20), "T7 resize");
	CHECK(sift_vk_resident_share(vk, keys, ng) == 0.0, "T7 a resized cache is empty");
	td_free(&d);
	sift_vk_close(vk);
}

/* ---- T8: a device that is lost, a missing driver ---- */
static void
t8_faults(const char *dev, const char *self)
{
	char		err[256];
	SiftVk	   *vk = sift_vk_open(dev, false, 64 << 20, err, sizeof(err));
	TData		d;
	ChunkSpec	sp = {.policy = "for", .enc = SIFT_VK_ENC_INT_FOR, .nulls = NULLS_AUTO};
	Rng			rng = {seed};
	SiftVkResult res;
	SiftVkStatus st;
	int			rc;
	char		cmd[512];

	CHECK(vk != NULL, "T8 open: %s", err);
	if (!vk)
		return;
	td_init(&d, 2, 1, 4096);
	for (uint32_t g = 0; g < 2; g++)
	{
		fill_col(&d.g[g], 0, K_SMALL, NULLS_AUTO, &rng);
		CHECK(build_chunk(&d.g[g], 0, &sp, 1), "T8 chunk");
	}
	td_publish(&d);
	CHECK(run_load_all(vk, &d, 1, &res) == SIFT_VK_OK, "T8 healthy run");
	sift_vk_test_inject(vk, SIFT_VK_LOST);
	st = run_load_all(vk, &d, 1, &res);
	CHECK(st == SIFT_VK_LOST, "T8 injected loss gave %d", st);
	CHECK(!sift_vk_usable(vk), "T8 a lost device is not usable");
	st = run_load_all(vk, &d, 1, &res);
	CHECK(st == SIFT_VK_LOST, "T8 a lost device stays lost (%d)", st);
	sift_vk_close(vk);
	vk = sift_vk_open(dev, false, 64 << 20, err, sizeof(err));
	CHECK(vk && run_load_all(vk, &d, 1, &res) == SIFT_VK_OK, "T8 a device opened after the loss works");
	sift_vk_test_inject(vk, SIFT_VK_TIMEOUT);
	st = run_load_all(vk, &d, 1, &res);
	CHECK(st == SIFT_VK_TIMEOUT, "T8 injected timeout gave %d", st);
	sift_vk_close(vk);
	td_free(&d);
	snprintf(cmd, sizeof(cmd), "VK_DRIVER_FILES=/nonexistent VK_ICD_FILENAMES=/nonexistent '%s' --nodriver", self);
	rc = system(cmd);
	CHECK(rc == 0, "T8 the run without a driver failed (status %d)", rc);
	printf("T8 faults: lost and timeout fall back, a missing driver is reported without a crash (rc %d)\n", rc);
}

/* ---- T11: the descriptor tables kept on the device (agg/vk/tab.c) ---- */

/* WHERE v32 > thr, with the aggregates `variant` names, over the granules [0, ngran) of d, its table named by key */
static void
tab_job(Case *c, const TData *d, uint32_t ngran, int64_t thr, int variant, uint64_t rel)
{
	uint64_t	mask = 0;

	case_init(c, d, SIFT_VK_UNGROUPED);
	P_COL(&c->p, 0, C_V32, SIFT_VK_T_I64);
	P_COL(&c->p, 1, C_DBL, SIFT_VK_T_F64);
	P_CMPC(&c->p, SIFT_VK_T_I64, 1, 0, thr, SIFT_VK_CMP_GT);
	c->j.where_slot = 1;
	add_agg(c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(c, variant ? SIFT_VK_AK_MAX : SIFT_VK_AK_SUM_I, 0, 255);
	add_agg(c, variant ? SIFT_VK_AK_MIN_F : SIFT_VK_AK_SUM_F, 1, 255);
	c->j.nops = c->p.n;
	c->j.ops = c->p.w;
	c->j.ngran = ngran;
	for (int k = 0; k < C_NCOLS; k++)
		mask |= (uint64_t) (d->vg[0].col[k] != NULL) << k;
	c->j.tab = (SiftVkTabKey) {.rel = rel, .manifest_gen = 1, .col_mask = {mask}, .granules = ngran};
}

/* the job on the device: its status, whether its table was found, the chunks it had to send; the answer is compared with the oracle's */
static SiftVkStatus
tab_run(SiftVk *vk, const TData *d, Case *c, const char *what, int hit, int uploaded)
{
	SiftVkResult res;
	RefResult	ref;
	TData		view = *d;
	SiftVkStatus st = sift_vk_run(vk, &c->j, NULL, &res);

	view.ngran = c->j.ngran;
	CHECK(st == SIFT_VK_OK || hit < 0, "T11 %s: status %d (%s)", what, st, sift_vk_error(vk));
	if (st != SIFT_VK_OK)
		return st;
	ref_run(&view, &c->j, &ref);
	CHECK(ref_compare(&c->j, &res, &ref, what, 1e-9) == 0, "T11 %s: the answer differs from the oracle's", what);
	ref_free(&ref);
	CHECK(hit < 0 || res.tab_hit == (hit != 0), "T11 %s: table %s, expected %s", what, res.tab_hit ? "hit" : "miss", hit ? "a hit" : "a miss");
	CHECK(uploaded < 0 || (res.chunks_uploaded != 0) == (uploaded != 0), "T11 %s: %u chunks sent, expected %s", what, res.chunks_uploaded, uploaded ? "some" : "none");
	printf("   %-46s table %s  chunks sent %u  resident %u  wall %.2f ms\n", what, res.tab_hit ? "hit " : "miss", res.chunks_uploaded, res.chunks_resident, res.times.wall_ms);
	return st;
}

static void
t11_tabs(const char *dev)
{
	char		err[256];
	SiftVk	   *vk = sift_vk_open(dev, false, 16ull << 20, err, sizeof(err));
	Rng			rng = {seed ^ 0x7AB};
	TData		d,
				big;
	Case		c;
	SiftVkGranule *other;

	CHECK(vk != NULL, "T11 open: %s", err);
	if (!vk)
		return;
	printf("T11 descriptor tables (a 16 MiB cache)\n");
	sift_vk_test_verify_tabs(vk, true);
	build_base(&d, 6, &rng, 5001);
	tab_job(&c, &d, d.ngran, 0, 0, 77);
	tab_run(vk, &d, &c, "first job: chunks sent, table made", 0, 1);
	tab_run(vk, &d, &c, "the same job again", 1, 0);
	tab_job(&c, &d, d.ngran, 400, 1, 77);
	tab_run(vk, &d, &c, "other threshold and aggregates, same key", 1, 0);
	tab_job(&c, &d, d.ngran - 1, 0, 0, 77);
	tab_run(vk, &d, &c, "fewer granules under the same key", 0, 0);
	tab_job(&c, &d, d.ngran, 0, 0, 78);
	tab_run(vk, &d, &c, "another relation: a miss, chunks resident", 0, 0);
	tab_run(vk, &d, &c, "... and its table", 1, 0);
	other = malloc(sizeof(SiftVkGranule) * d.ngran);
	memcpy(other, d.vg, sizeof(SiftVkGranule) * d.ngran);
	for (uint32_t g = 0; g < d.ngran; g++)
		other[g].col[C_BIG] = NULL;
	tab_job(&c, &d, d.ngran, 0, 0, 78);
	c.j.gran = other;
	CHECK(tab_run(vk, &d, &c, "a key given to other chunks is caught", -1, -1) == SIFT_VK_FAIL, "T11 the table of other chunks was not caught");
	free(other);
	build_base(&big, 100, &rng, SIFT_VK_GRANULE_ROWS);
	tab_job(&c, &big, big.ngran, 0, 0, 79);
	tab_run(vk, &big, &c, "a job larger than the cache evicts", 0, 1);
	tab_job(&c, &d, d.ngran, 0, 0, 77);
	tab_run(vk, &d, &c, "after the eviction: chunks sent again", 0, 1);
	tab_run(vk, &d, &c, "... resident: its table, the slabs it names kept", 1, 0);
	tab_run(vk, &d, &c, "... and now its table", 1, 0);
	CHECK(sift_vk_set_cache(vk, 16ull << 20), "T11 set cache");
	tab_run(vk, &d, &c, "a new cache holds no table", 0, 1);
	tab_run(vk, &d, &c, "... and makes one", 1, 0);
	td_free(&big);
	td_free(&d);
	sift_vk_close(vk);
}

/* ---- T13: the cache gives way to the table of a job (agg/vk/cache.c fit_cache) ---- */

/* the pairs of a hash job of est groups: its table takes 64 MiB at 1 << 20 */
static void
hash_job(Case *c, const TData *d, uint64_t est)
{
	case_init(c, d, SIFT_VK_HASH);
	P_COL(&c->p, 0, C_V32, SIFT_VK_T_I64);
	c->j.nkeys = 1;
	c->j.key_reg[0] = 0;
	c->j.est_groups = est;
	c->j.finish = SIFT_VK_FIN_COUNT;
	c->j.nops = c->p.n;
	c->j.ops = c->p.w;
}

static void
t13_envelope(const char *dev)
{
	char		err[256];
	SiftVk	   *vk = sift_vk_open(dev, false, 64ull << 20, err, sizeof(err));
	Rng			rng = {seed ^ 0x13E};
	TData		d;
	Case		c,
				h;
	SiftVkResult res;
	uint64_t	distinct;

	CHECK(vk != NULL, "T13 open: %s", err);
	if (!vk)
		return;
	printf("T13 the cache gives way to the table of a job (a 64 MiB cache, an envelope that leaves it 36 MiB beside a 64 MiB table)\n");
	build_base(&d, 6, &rng, 5001);
	hash_job(&h, &d, 1 << 20);
	CHECK(sift_vk_run(vk, &h.j, NULL, &res) == SIFT_VK_OK, "T13 the job with no envelope: %s", sift_vk_error(vk));
	distinct = res.distinct;
	tab_job(&c, &d, d.ngran, 0, 0, 0);
	tab_run(vk, &d, &c, "ungrouped job: chunks resident", -1, 0);
	sift_vk_set_envelope(vk, 120ull << 20, 16ull << 20);
	CHECK(sift_vk_run(vk, &h.j, NULL, &res) == SIFT_VK_OK && res.distinct == distinct, "T13 the answer of a job whose table took room of the cache changed: %s", sift_vk_error(vk));
	CHECK(res.chunks_uploaded > 0, "T13 the cache the table made room in kept its chunks");
	CHECK(sift_vk_caps(vk)->cache_bytes == 64ull << 20, "T13 the ceiling of the cache changed: %llu MiB", (unsigned long long) (sift_vk_caps(vk)->cache_bytes >> 20));
	tab_run(vk, &d, &c, "after the table: the cache is whole again, chunks sent", -1, 1);
	tab_run(vk, &d, &c, "... and resident", -1, 0);
	td_free(&d);
	sift_vk_close(vk);
}

static int
nodriver_main(void)
{
	char		err[256] = "";
	SiftVkDevInfo di[4];
	int			chosen = 0,
				n = sift_vk_list_devices(di, 4, &chosen, "", false, err, sizeof(err));
	SiftVk	   *vk = sift_vk_open("", false, 1 << 20, err, sizeof(err));

	printf("   no driver: list gave %d, open gave %s, message \"%s\"\n", n, vk ? "a device" : "NULL", err);
	if (vk)
	{
		sift_vk_close(vk);
		return 1;
	}
	return n > 0 ? 1 : 0;
}

extern void test_agg(SiftVk *vk, uint64_t seed, const char *only);

int
main(int argc, char **argv)
{
	char		err[256];
	SiftVk	   *vk;
	const SiftVkCaps *cap;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--nodriver"))
			return nodriver_main();
		else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			dev_sel = argv[++i];
		else if (!strcmp(argv[i], "--only") && i + 1 < argc)
			only = argv[++i];
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			seed = strtoull(argv[++i], NULL, 0);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	codec_init();
	if (want("T0"))
		t0_devices();
	vk = sift_vk_open(dev_sel, false, 512ull << 20, err, sizeof(err));
	if (!vk)
	{
		fprintf(stderr, "no device: %s\n", err);
		return 2;
	}
	cap = sift_vk_caps(vk);
	printf("device %s  subgroup %u  ReBAR %d  UMA %d  transfer queue %d  budget %d  PCIe %.2f GB/s  VRAM %.1f GiB  cache %llu MiB  VRAM bw %.0f GB/s\n",
		   cap->name, cap->subgroup_size, cap->rebar, cap->uma, cap->dedicated_transfer, cap->ext_budget, cap->pcie_gbps, cap->vram_bytes / 1073741824.0,
		   (unsigned long long) (cap->cache_bytes >> 20), cap->vram_gbps);
	if (want("T1"))
		t1_decode(vk);
	test_agg(vk, seed, only);
	sift_vk_close(vk);
	if (want("T7"))
		t7_cache(dev_sel);
	if (want("T11"))
		t11_tabs(dev_sel);
	if (want("T13"))
		t13_envelope(dev_sel);
	if (want("T8"))
		t8_faults(dev_sel, argv[0]);
	printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
