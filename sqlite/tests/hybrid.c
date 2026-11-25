/*
 * tests/hybrid.c - agg/hybrid/hybrid.c against the real core/tidbridge.c and an oracle, in a program of its own on the portable host (no database, so ASan and UBSan run it).
 *
 * A model of 20000 rows (8 per heap page, so about ten superblocks) has a structured predicate, a text match with a score and a visibility. Direction A and direction B run over
 * it on a sift_col-like store (the heap decides visibility, dead rows are still in the store) and a sift_table-like one (the store decides), under an unlimited, a small and an
 * empty byte budget, and both must return the oracle's rows: the matches that pass the predicate and are visible, by score and then key. The granule test and the probe test
 * check that no TID of the bridge is ever refused, and the chooser test that the cheaper direction wins.
 */
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/host.h"
#include "agg/hybrid/hybrid.h"

#define N			20000
#define PER_PAGE	8
#define GRAN		256

static int64_t checks,
			failed;

#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rng = 88172645463325252ULL;

static uint64_t
rnd(void)
{
	uint64_t	z = (rng += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static bool
chance(double p)
{
	return (double) (rnd() % 1000000) < p * 1000000.0;
}

static SiftKey
key_of(int i)
{
	return ((uint64) (i / PER_PAGE + 1) << 16) | (uint64) (i % PER_PAGE + 1);
}

static int
row_of(SiftKey k)
{
	return (int) ((k >> 16) - 1) * PER_PAGE + (int) (k & 0xFFFF) - 1;
}

typedef struct Hit
{
	SiftKey		key;
	float4		score;
} Hit;

typedef struct Model
{
	bool		pred[N],
				match[N],
				vis[N];
	float4		score[N];
} Model;

typedef struct Ctx
{
	const Model *m;
	SiftHybridHost host;
	uint64		heap_checks,
				ranker_calls;
} Ctx;

static int
hit_cmp(const void *a, const void *b)
{
	const Hit  *x = a,
			   *y = b;

	return x->score > y->score ? -1 : x->score < y->score ? 1 : x->key < y->key ? -1 : x->key > y->key;
}

static void
fill(Model *m, double sel_col, double sel_bm25, bool clustered, int dead_pct)
{
	for (int i = 0; i < N; i++)
	{
		m->pred[i] = clustered ? (i / 128) % 100 < (int) (sel_col * 100) : chance(sel_col);
		m->match[i] = chance(sel_bm25);
		m->score[i] = 1.0f + (float) (rnd() % 64) / 4.0f;
		m->vis[i] = (int) (rnd() % 100) >= dead_pct;
	}
}

static bool
heap_check(void *arg, SiftKey key, bool quals)
{
	Ctx		   *c = arg;
	int			i = row_of(key);

	c->heap_checks++;
	return c->m->vis[i] && (!quals || c->m->pred[i]);
}

static bool
row_in_store(const Ctx *c, int i)
{
	return c->m->pred[i] && (c->host == SIFT_HYB_HOST_COL || c->m->vis[i]);
}

static void
granule_batch(const Ctx *c, int g, uint64 *tid, uint64 *live, SiftHybridBatch *b)
{
	int			n = Min(GRAN, N - g * GRAN);

	memset(live, 0, (GRAN / 64) * sizeof(uint64));
	for (int r = 0; r < n; r++)
	{
		tid[r] = key_of(g * GRAN + r);
		if (row_in_store(c, g * GRAN + r))
			live[r / 64] |= UINT64CONST(1) << (r % 64);
	}
	*b = (SiftHybridBatch) {.tid = tid,.live = live,.n = n,.local_g = g,.exact_vis = c->host == SIFT_HYB_HOST_TABLE};
}

static void
store_scan_quals(void *arg, SiftHybridSink sink, void *sink_arg)
{
	uint64		tid[GRAN],
				live[GRAN / 64];

	for (int g = 0; g * GRAN < N; g++)
	{
		SiftHybridBatch b;

		granule_batch(arg, g, tid, live, &b);
		sink(sink_arg, &b);
	}
}

static void
store_scan_bridge(void *arg, const SiftTidBridge *br, SiftHybridSink sink, void *sink_arg, SiftHybridStats *st)
{
	uint64		tid[GRAN],
				live[GRAN / 64];

	for (int g = 0; g * GRAN < N; g++)
	{
		SiftHybridBatch b;
		uint64		bloom = 0;

		granule_batch(arg, g, tid, live, &b);
		for (int r = 0; r < b.n; r++)
			bloom |= sift_tid_bloom_bit((BlockNumber) (tid[r] >> 16));
		st->granules_seen++;
		if (!sift_tidbridge_granule_may_match(br, tid[0], tid[b.n - 1], bloom))
			st->granules_skipped++;
		else
			sink(sink_arg, &b);
	}
}

static SiftHybridStore
make_store(Ctx *c, bool exact_quals)
{
	return (SiftHybridStore) {.host = c->host,.hot_chains = false,.exact_quals = exact_quals,.arg = c,.scan_quals = store_scan_quals,.scan_bridge = store_scan_bridge};
}

/* the rows the query must return: oracle[0..n) */
static int
oracle(const Model *m, int k, Hit *out)
{
	int			n = 0;

	for (int i = 0; i < N; i++)
		if (m->match[i] && m->pred[i] && m->vis[i])
			out[n++] = (Hit) {key_of(i), m->score[i]};
	qsort(out, n, sizeof(Hit), hit_cmp);
	return k > 0 ? Min(n, k) : n;
}

static int
all_matches(const Model *m, Hit *out)
{
	int			n = 0;

	for (int i = 0; i < N; i++)
		if (m->match[i])
			out[n++] = (Hit) {key_of(i), m->score[i]};
	qsort(out, n, sizeof(Hit), hit_cmp);
	return n;
}

/* A: the ranked search over the hits in score order takes what the bridge accepts */
static int
run_a(Ctx *c, bool exact_quals, Size budget, int k, Hit *out, SiftHybridStats *st)
{
	SiftHybridStore store = make_store(c, exact_quals);
	SiftHybridRun *r = sift_hybrid_begin(CurrentMemoryContext, SIFT_HYB_COL_TO_BM25, &store, budget, heap_check, c);
	Hit		   *cand = palloc(N * sizeof(Hit));
	int			n = 0;

	sift_hybrid_a_bridge(r, false, false);
	if (!r->stats.empty)
	{
		int			nc = all_matches(c->m, cand);

		c->ranker_calls++;
		for (int i = 0; i < nc && (k == 0 || n < k); i++)
			if (sift_hybrid_a_accept(r, cand[i].key))
				out[n++] = cand[i];
	}
	*st = r->stats;
	pfree(cand);
	sift_hybrid_end(r);
	return n;
}

typedef struct HitStream
{
	const Hit  *h;
	int			n,
				pos;
} HitStream;

static bool
next_hit(void *arg, SiftKey *key, float4 *score)
{
	HitStream  *s = arg;

	if (s->pos >= s->n)
		return false;
	*key = s->h[s->pos].key;
	*score = s->h[s->pos++].score;
	return true;
}

typedef struct Collect
{
	Hit		   *out;
	int			n;
} Collect;

static void
collect_sink(void *arg, const SiftHybridBatch *b)
{
	Collect    *c = arg;

	for (int r = 0; r < b->n; r++)
		if (b->live[r / 64] >> (r % 64) & 1)
			c->out[c->n++] = (Hit) {b->tid[r], b->score[r]};
}

static int
run_b(Ctx *c, Size budget, int k, Hit *out, SiftHybridStats *st)
{
	SiftHybridStore store = make_store(c, true);
	SiftHybridRun *r = sift_hybrid_begin(CurrentMemoryContext, SIFT_HYB_BM25_TO_COL, &store, budget, heap_check, c);
	Hit		   *cand = palloc(N * sizeof(Hit));
	HitStream	hs = {cand, all_matches(c->m, cand), 0};
	Collect		col = {out, 0};

	c->ranker_calls++;
	while (sift_hybrid_b_chunk(r, next_hit, &hs, true))
		sift_hybrid_b_emit(r, collect_sink, &col);
	qsort(out, col.n, sizeof(Hit), hit_cmp);
	*st = r->stats;
	pfree(cand);
	sift_hybrid_end(r);
	return k > 0 ? Min(col.n, k) : col.n;
}

static bool
same_hits(const Hit *a, const Hit *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i].key != b[i].key || a[i].score != b[i].score)
			return false;
	return true;
}

static const struct
{
	const char *name;
	double		sel_col,
				sel_bm25;
	bool		clustered;
	int			dead_pct;
} scenarios[] = {
	{"point", 0.0005, 0.5, false, 3},
	{"selective_col", 0.01, 0.4, false, 3},
	{"selective_text", 0.4, 0.002, false, 3},
	{"clustered_col", 0.05, 0.3, true, 3},
	{"wide_both", 0.6, 0.6, false, 5},
	{"empty_col", 0.0, 0.5, false, 0},
	{"no_text_hits", 0.5, 0.0, false, 0},
};

static void
test_parity(void)
{
	static Model m;
	Hit		   *want = palloc(N * sizeof(Hit)),
			   *got = palloc(N * sizeof(Hit));
	const Size	budgets[] = {(Size) 8 << 20, 2048, 1};

	for (size_t s = 0; s < lengthof(scenarios); s++)
		for (int host = 0; host < 2; host++)
			for (size_t bi = 0; bi < lengthof(budgets); bi++)
				for (int k = 0; k < 2; k++)
					for (int eq = 0; eq < 2; eq++)
					{
						Ctx			c = {.m = &m, .host = host ? SIFT_HYB_HOST_TABLE : SIFT_HYB_HOST_COL};
						SiftHybridStats sa,
									sb;
						int			nw,
									na,
									nb;
						int			nstore = 0;
						uint64		calls_a;

						if (eq == 0 && host)
							continue;
						fill(&m, scenarios[s].sel_col, scenarios[s].sel_bm25, scenarios[s].clustered, scenarios[s].dead_pct);
						nw = oracle(&m, k ? 10 : 0, want);
						na = run_a(&c, eq == 1, budgets[bi], k ? 10 : 0, got, &sa);
						CHECK(na == nw && same_hits(want, got, nw));
						calls_a = c.ranker_calls;
						nb = run_b(&c, budgets[bi], k ? 10 : 0, got, &sb);
						CHECK(nb == nw && same_hits(want, got, nw));
						for (int i = 0; i < N; i++)
							nstore += row_in_store(&c, i);
						CHECK(sa.bridge_tids <= (uint64) nstore);
						CHECK(sa.empty == (nstore == 0) && calls_a == (sa.empty ? 0 : 1));
						if (budgets[bi] > 1 << 20)
							CHECK(sa.point == (nstore <= SIFT_TIDB_POINT_MAX) && sa.pages_lossy == 0);
						if (budgets[bi] == 1 && nstore > 0)
							CHECK(sa.pages_lossy > 0 && !sa.point && (na == 0 || sa.lossy_rechecks > 0));
						if (budgets[bi] > 1 << 20 && nstore > 0 && host && eq == 1)
							CHECK(sa.heap_verifies == 0 && c.heap_checks == 0 && sa.zero_heap_rows == (uint64) na);
						if (budgets[bi] > 1 << 20 && !host && na > 0)
							CHECK(sa.zero_heap_rows == 0 && sa.heap_verifies > 0);
						CHECK(sb.pages_lossy == 0 && sb.granules_seen == sb.chunks * ((N + GRAN - 1) / GRAN));
					}
	pfree(want);
	pfree(got);
}

static void
test_granule_skips(void)
{
	static Model m;
	Ctx			c = {.m = &m, .host = SIFT_HYB_HOST_TABLE};
	Hit		   *got = palloc(N * sizeof(Hit));
	SiftHybridStats st;

	fill(&m, 0.5, 0.002, false, 0);
	run_b(&c, (Size) 8 << 20, 0, got, &st);
	CHECK(st.granules_skipped > 0 && st.granules_skipped < st.granules_seen);
	CHECK(st.chunks == 1 && st.rows_probed < (uint64) N);
	pfree(got);
}

static void
test_chunks(void)
{
	SiftHybridStore store = {.host = SIFT_HYB_HOST_TABLE,.exact_quals = true};
	SiftHybridRun *r = sift_hybrid_begin(CurrentMemoryContext, SIFT_HYB_BM25_TO_COL, &store, 1, NULL, NULL);
	Hit		   *h = palloc(6000 * sizeof(Hit));
	HitStream	hs = {h, 6000, 0};
	uint64		total = 0;

	for (int i = 0; i < 6000; i++)
		h[i] = (Hit) {((uint64) (1 + (rnd() % 1000000)) << 16) | 1, (float4) i};
	while (sift_hybrid_b_chunk(r, next_hit, &hs, true))
		total += r->br->ntids;
	CHECK(r->stats.chunks > 10 && r->stats.pages_lossy == 0);
	CHECK(total == r->stats.bridge_tids && total <= 6000 && total > 5900);
	sift_hybrid_end(r);
	pfree(h);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static void
test_granule_and_probe(void)
{
	const Size	budgets[] = {(Size) 8 << 20, 1};
	int			pruned = 0,
				disjoint = 0;

	for (size_t bi = 0; bi < lengthof(budgets); bi++)
		for (int round = 0; round < 40; round++)
		{
			SiftTidBridge *br = sift_tidbridge_create(CurrentMemoryContext, budgets[bi], SIFT_TBR_HAS_SCORES);
			uint64		in[400];
			uint64		tid[300];
			uint64		live[5],
						out[5];
			float4		score[300];

			for (int i = 0; i < 400; i++)
			{
				in[i] = ((uint64) (1 + rnd() % 5000) << 16) | (1 + rnd() % 64);
				sift_tidbridge_add_score(br, in[i], (float4) i);
			}
			sift_tidbridge_finalize(br);
			for (int gi = 0; gi < 200; gi++)
			{
				uint64		g[40],
							bloom = 0;
				uint32		base = 1 + (uint32) (rnd() % 4900);
				bool		any = false;

				for (int i = 0; i < 40; i++)
				{
					g[i] = ((uint64) (base + rnd() % 4) << 16) | (1 + rnd() % 64);
					bloom |= sift_tid_bloom_bit((BlockNumber) (g[i] >> 16));
				}
				qsort(g, 40, sizeof(uint64), cmp_u64);
				for (int i = 0; i < 40; i++)
					any |= sift_tidbridge_probe_tid(br, g[i], NULL) != SIFT_TIDPROBE_NO;
				if (any)
					CHECK(sift_tidbridge_granule_may_match(br, g[0], g[39], bloom));
				else if (budgets[bi] > 1 << 20)
				{
					disjoint++;
					pruned += !sift_tidbridge_granule_may_match(br, g[0], g[39], bloom);
				}
			}
			if (budgets[bi] > 1 << 20)
			{
				int			want = 0;

				for (int i = 0; i < 300; i++)
					tid[i] = i % 3 == 0 ? in[rnd() % 400] : ((uint64) (1 + rnd() % 5000) << 16) | (1 + rnd() % 64);
				memset(live, 0xFF, sizeof(live));
				for (int i = 0; i < 300; i++)
					want += sift_tidbridge_probe_tid(br, tid[i], NULL) == SIFT_TIDPROBE_EXACT;
				CHECK(sift_tidbridge_probe_vec(br, tid, live, 300, out, score) == want);
				for (int i = 0; i < 300; i++)
					if (out[i / 64] >> (i % 64) & 1)
					{
						float4		s = -1;

						sift_tidbridge_probe_tid(br, tid[i], &s);
						CHECK(s == score[i]);
					}
			}
		}
	CHECK(disjoint > 0 && pruned * 10 > disjoint * 5);
}

static SiftHybridEst
est(double sel_col, double sel_bm25, double col_granules, double limit, bool col_quals, bool covered, double heap_fetch)
{
	return (SiftHybridEst) {.host = SIFT_HYB_HOST_COL,.rows = 1e7,.granules = 1220,.col_granules = col_granules,.heap_fetch = heap_fetch,.sel_col = sel_col,
		.sel_bm25 = sel_bm25,.limit = limit,.ranked = limit > 0,.has_col_quals = col_quals,.covered = covered,.sel_acl = 1.0};
}

static void
test_chooser(void)
{
	SiftHybridCosts c;
	SiftHybridEst e = est(1e-4, 0.3, 12, 10, true, true, 1.0);

	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25 && c.col_to_bm25 < c.bm25_to_col && c.col_to_bm25 < c.standard);
	e = est(0.2, 1e-4, 1220, 0, true, true, 16.0);
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_BM25_TO_COL && c.bm25_to_col < c.col_to_bm25 && c.bm25_to_col < c.standard);
	e = est(1.0, 1e-5, 1220, 0, false, true, 0.01);
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.col_to_bm25 == DBL_MAX && c.bm25_to_col >= c.standard);
	e = est(1.0, 1e-3, 1220, 0, false, false, 1.0);
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.col_to_bm25 == DBL_MAX && c.bm25_to_col == DBL_MAX);
	e = est(1.0, 1e-3, 1220, 0, false, true, 16.0);
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.col_to_bm25 == DBL_MAX && c.bm25_to_col == DBL_MAX);
	e.agg_mode = true;
	e.allvisfrac = 1.0;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_BM25_TO_COL && c.col_to_bm25 == DBL_MAX);
	e.hit_budget = 1e4 * 64 - 1;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.bm25_to_col == DBL_MAX);
	e = est(0.2, 1e-4, 1220, 0, true, true, 16.0);
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_BM25_TO_COL && c.col_to_bm25 < DBL_MAX);
	e.hit_budget = 1e3 * 64 - 1;
	CHECK(sift_hybrid_choose(&e, &c) != SIFT_HYB_BM25_TO_COL && c.bm25_to_col == DBL_MAX);
	e.limit = 10;
	e.ranked = true;
	CHECK(sift_hybrid_choose(&e, &c) != SIFT_HYB_NONE || c.bm25_to_col < DBL_MAX);
	e = est(0.1, 0.05, 25, 0, true, true, 0.01);
	e.rows = 2e5;
	e.granules = 25;
	e.hit_budget = 262144;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25 && c.bm25_to_col == DBL_MAX);
	e.hit_budget = 0;
	CHECK(c.bm25_to_col == DBL_MAX && sift_hybrid_choose(&e, &c) != SIFT_HYB_NONE && c.bm25_to_col < DBL_MAX);
	e = est(2e-5, 0.5, 2, 10, true, true, 1.0);
	e.host = SIFT_HYB_HOST_TABLE;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25 && c.col_to_bm25 < c.bm25_to_col);
	e.col_granules = 1220;
	e.sel_col = 0.5;
	e.sel_bm25 = 0.5;
	CHECK(sift_hybrid_choose(&e, NULL) != SIFT_HYB_COL_TO_BM25);
	e = est(4e-6, 0.5, 3, 0, true, true, 0.01);
	e.agg_mode = true;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25 && c.standard > 5e4);
	e.probeable = true;
	e.probe_rows = 40;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.standard < 100 && c.standard < c.col_to_bm25 + 8.0);
	e.probe_rows = 1e6;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25 && c.standard > c.col_to_bm25);
	e.probeable = false;
	e.probe_rows = 40;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_COL_TO_BM25);
	e = est(0.5, 0.5, 1, 0, true, true, 0.01);
	e.rows = 200;
	e.granules = 1;
	e.agg_mode = true;
	CHECK(sift_hybrid_choose(&e, &c) == SIFT_HYB_NONE && c.col_to_bm25 < c.standard && !sift_hybrid_beats(c.col_to_bm25, &c));
	CHECK(sift_hybrid_beats(0.0, &(SiftHybridCosts) {DBL_MAX, DBL_MAX, 100.0}) && !sift_hybrid_beats(0.0, &(SiftHybridCosts) {DBL_MAX, DBL_MAX, 4.0}));
	CHECK(!strcmp(sift_hybrid_dir_name(SIFT_HYB_COL_TO_BM25), "A") && !strcmp(sift_hybrid_dir_name(SIFT_HYB_BM25_TO_COL), "B"));
}

static void
test_cleanup(void)
{
	static Model m;
	Hit		   *got = palloc(N * sizeof(Hit));
	SiftHybridStats st;

	fill(&m, 0.05, 0.05, false, 2);
	for (int i = 0; i < 60; i++)
	{
		Ctx			c = {.m = &m, .host = i % 2 ? SIFT_HYB_HOST_TABLE : SIFT_HYB_HOST_COL};

		run_a(&c, true, i % 3 == 0 ? 1 : (Size) 1 << 20, 10, got, &st);
		run_b(&c, 1 << 16, 10, got, &st);
	}
	pfree(got);
}

int
main(void)
{
	CurrentMemoryContext = AllocSetContextCreate(NULL, "hybrid-test", ALLOCSET_DEFAULT_SIZES);
	test_chooser();
	test_parity();
	test_granule_skips();
	test_chunks();
	test_granule_and_probe();
	test_cleanup();
	printf("hybrid-test: %lld checks, %lld failed\n", (long long) checks, (long long) failed);
	return failed != 0 || checks == 0;
}
