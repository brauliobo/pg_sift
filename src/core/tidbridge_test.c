/*
 * core/tidbridge_test.c - the checks of core/tidbridge.h against an oracle that is a sorted array of keys, compiled only with SIFT_TESTING: the SQL function
 * sift._test_tidbridge_check of the pg_sift_test extension runs them in the server, sqlite/tests/tidbridge.c in a program of its own (the SQLite host, ASan and UBSan).
 *
 * A scenario builds a bridge from random keys in random order (with repeats) under an unlimited byte budget, a small one and none, and checks it against the oracle:
 * a TID that was added is never answered NO, an exact page answers NO for a TID that was not added, the page masks, blooms and key range are those of the oracle's pages,
 * the offset masks of a block equal those of core/filter.h (an all-ones row for a lossy page), the container of a page is the one its offsets call for, the bytes stop growing
 * once the byte budget is spent, and the scores and row locators come back. Each check is counted; a count of failures other than 0, or of checks 0, is a failure.
 */
#include "core/host.h"

#ifdef SIFT_TESTING

#include "core/filter.h"
#include "core/testrng.h"
#include "core/tidbridge.h"

#if !defined(SIFT_HOST_SQLITE) && !defined(SIFT_HOST_MARIADB)
#include "fmgr.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "core/memtrack.h"
#endif

enum
{
	T_LAYOUT, T_KINDS, T_NOMISS, T_EXACT, T_MASKS, T_FILTER, T_BUDGET, T_ORDER, T_SCORES, T_ROWLOC, T_DOCIDS, T_POINT, T_RESET, T_INTERSECT,
	T_N
};

#define MAX_KEYS	(1 << 17)

typedef struct Model
{
	uint64_t   *key;				/* ascending, distinct */
	int			n;
} Model;

static void
check(int64_t *c, int what, bool ok)
{
	c[2 * what]++;
	c[2 * what + 1] += !ok;
}

static uint32_t
below(SiftRng *r, uint32_t n)
{
	return sift_rng_below(r, n);
}

static int
cmp_key(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a,
				y = *(const uint64_t *) b;

	return x < y ? -1 : x > y;
}

static int
model_find(const Model *m, uint64_t key)
{
	const uint64_t *p = bsearch(&key, m->key, m->n, sizeof(uint64_t), cmp_key);

	return p ? (int) (p - m->key) : -1;
}

static Model
model_of(const uint64_t *keys, int n)
{
	Model		m = {palloc(Max(n, 1) * sizeof(uint64_t)), 0};

	memcpy(m.key, keys, n * sizeof(uint64_t));
	qsort(m.key, n, sizeof(uint64_t), cmp_key);
	for (int i = 0; i < n; i++)
		if (m.n == 0 || m.key[m.n - 1] != m.key[i])
			m.key[m.n++] = m.key[i];
	return m;
}

/* The pages of a superblock a scenario fills: the ends of the words of the page mask are over-represented */
static int
pick_page(SiftRng *r)
{
	static const int edge[] = {0, 1, 63, 64, 65, 127, 128, 191, 192, 254, 255};

	return (below(r, 3) == 0 ? edge[below(r, lengthof(edge))] : (int) below(r, SIFT_SB_PAGES)) % SIFT_SB_PAGES;
}

static uint32_t
pick_sb(SiftRng *r)
{
	return below(r, 4) == 0 ? (UINT32_MAX >> SIFT_SB_SHIFT) - below(r, 4) : below(r, 3000);
}

static int
pick_off(SiftRng *r)
{
	return below(r, 6) == 0 ? (below(r, 2) ? 1 : SIFT_TIDB_MAX_OFFSET) : 1 + below(r, SIFT_TIDB_MAX_OFFSET);
}

/* nsb superblocks of ppsb pages with one to maxfill offsets each (a page may be full): the keys in the order of generation, repeats included */
static int
generate(SiftRng *r, uint64_t *keys, int nsb, int ppsb, int maxfill)
{
	int			n = 0;

	for (int s = 0; s < nsb; s++)
	{
		uint32_t	sb = pick_sb(r);

		for (int p = 0; p < ppsb; p++)
		{
			uint64_t	blk = sb << SIFT_SB_SHIFT | pick_page(r);
			int			fill = below(r, 8) == 0 ? SIFT_TIDB_MAX_OFFSET : 1 + below(r, maxfill);

			for (int i = 0; i < fill; i++)
				keys[n++] = blk << 16 | pick_off(r);
		}
	}
	return n;
}

/* up to 256 keys in a few superblocks */
static int
generate_sparse(SiftRng *r, uint64_t *keys)
{
	uint32_t	sb[4];
	int			nsb = 1 + below(r, 4),
				n = 1 + below(r, SIFT_TIDB_POINT_MAX);

	for (int i = 0; i < nsb; i++)
		sb[i] = pick_sb(r);
	for (int i = 0; i < n; i++)
		keys[i] = ((uint64_t) sb[below(r, nsb)] << SIFT_SB_SHIFT | pick_page(r)) << 16 | pick_off(r);
	return n;
}

/* the keys in a random order, a tenth of them again */
static int
insertion_order(SiftRng *r, uint64_t *keys, int n)
{
	int			total = n + n / 10;

	for (int i = n; i < total; i++)
		keys[i] = keys[below(r, n)];
	for (int i = total - 1; i > 0; i--)
	{
		int			j = below(r, i + 1);
		uint64_t	t = keys[i];

		keys[i] = keys[j];
		keys[j] = t;
	}
	return total;
}

static MemoryContext
child(MemoryContext parent)
{
	return sift_mem_allocset_create(parent, "tidbridge check");
}

static SiftTidBridge *
build(MemoryContext cx, Size budget, uint32_t flags, const uint64_t *keys, int n)
{
	SiftTidBridge *br = sift_tidbridge_create(cx, budget, flags);

	for (int i = 0; i < n; i++)
		sift_tidbridge_add_tid(br, keys[i]);
	sift_tidbridge_finalize(br);
	return br;
}

static int
page_of(uint64_t key)
{
	return (key >> 16) & (SIFT_SB_PAGES - 1);
}

static bool
page_is_lossy(const SiftTidBridge *br, uint64_t key)
{
	const SiftTidBridgeSB *s = sift_tidbridge_probe_sb(br, sift_block_sb(key >> 16));

	return s != NULL && (s->lossy_mask[page_of(key) / 64] >> (page_of(key) & 63) & 1);
}

static SiftTidProbe
expected_probe(const SiftTidBridge *br, const Model *m, uint64_t key)
{
	if ((key & 0xFFFF) < 1 || (key & 0xFFFF) > SIFT_TIDB_MAX_OFFSET)
		return SIFT_TIDPROBE_NO;
	if (page_is_lossy(br, key))
		return SIFT_TIDPROBE_LOSSY;
	return model_find(m, key) >= 0 ? SIFT_TIDPROBE_EXACT : SIFT_TIDPROBE_NO;
}

static uint64_t
random_probe(SiftRng *r, const Model *m)
{
	uint64_t	k = m->n ? m->key[below(r, m->n)] : (uint64_t) pick_sb(r) << (SIFT_SB_SHIFT + 16);

	switch (below(r, 5))
	{
		case 0:
			return (k & ~UINT64_C(0xFFFF)) | pick_off(r);
		case 1:
			return ((uint64_t) (uint32_t) ((k >> 16) + below(r, 3) - 1) << 16) | pick_off(r);
		case 2:
			return ((uint64_t) pick_sb(r) << SIFT_SB_SHIFT | pick_page(r)) << 16 | pick_off(r);
		case 3:
			return (k & ~UINT64_C(0xFFFF)) | (below(r, 2) ? 0 : SIFT_TIDB_MAX_OFFSET + 1 + below(r, 100));
		default:
			return k ^ (UINT64_C(1) << below(r, 6));
	}
}

static void
check_probes(int64_t *c, const SiftTidBridge *br, const Model *m, SiftRng *r)
{
	uint64_t	exact = 0;

	for (int i = 0; i < m->n; i++)
	{
		SiftTidProbe p = sift_tidbridge_probe_tid(br, m->key[i], NULL);

		check(c, T_NOMISS, p != SIFT_TIDPROBE_NO);
		check(c, T_EXACT, p == expected_probe(br, m, m->key[i]));
		exact += p == SIFT_TIDPROBE_EXACT;
	}
	for (int i = 0; i < 3 * m->n + 64; i++)
	{
		uint64_t	k = random_probe(r, m);

		check(c, T_EXACT, sift_tidbridge_probe_tid(br, k, NULL) == expected_probe(br, m, k));
	}
	check(c, T_NOMISS, br->ntids == exact && ((br->flags & SIFT_TBR_HAS_LOSSY) || exact == (uint64_t) m->n));
}

static void
check_masks(int64_t *c, const SiftTidBridge *br, const Model *m)
{
	uint32_t	pages = 0,
				sbs = 0,
				active = 0,
				exact = 0;
	uint64_t	union_bloom = 0;

	for (int i = 0; i < m->n; i++)
		if (i == 0 || (m->key[i] >> 16) != (m->key[i - 1] >> 16))
		{
			pages++;
			sbs += i == 0 || sift_block_sb(m->key[i] >> 16) != sift_block_sb(m->key[i - 1] >> 16);
		}
	check(c, T_MASKS, br->nsbs == sbs);
	for (uint32_t i = 0; i < br->nsbs; i++)
	{
		const SiftTidBridgeSB *s = &br->sbs[i];
		uint32_t	pc = 0;
		uint64_t	bloom = 0;

		check(c, T_MASKS, i == 0 || br->sbs[i - 1].sb < s->sb);
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		{
			check(c, T_MASKS, (s->lossy_mask[w] & ~s->page_mask[w]) == 0);
			pc += pg_popcount64(s->page_mask[w]);
			exact += pg_popcount64(s->page_mask[w] & ~s->lossy_mask[w]);
			for (uint64_t mm = s->page_mask[w]; mm; mm &= mm - 1)
				bloom |= sift_tid_bloom_bit(s->sb << SIFT_SB_SHIFT | (w * 64 + pg_rightmost_one_pos64(mm)));
		}
		check(c, T_MASKS, s->active_pages == pc && s->sb_blk_bloom == bloom);
		active += pc;
		union_bloom |= bloom;
	}
	check(c, T_MASKS, active == pages && br->npages == exact && br->blk_bloom_union == union_bloom);
	for (int i = 0; i < m->n; i++)
	{
		const SiftTidBridgeSB *s = sift_tidbridge_probe_sb(br, sift_block_sb(m->key[i] >> 16));

		check(c, T_MASKS, s != NULL && (s->page_mask[page_of(m->key[i]) / 64] >> (page_of(m->key[i]) & 63) & 1));
	}
	if (m->n == 0)
		check(c, T_MASKS, br->min_tid == UINT64_MAX && br->max_tid == 0);
	else if (br->flags & SIFT_TBR_HAS_LOSSY)
		check(c, T_MASKS, br->min_tid <= m->key[0] && br->max_tid >= m->key[m->n - 1]);
	else
		check(c, T_MASKS, br->min_tid == m->key[0] && br->max_tid == m->key[m->n - 1]);
}

/* The container of a page: the record of the exact page that holds the key */
static const SiftTidBridgePage *
page_record(const SiftTidBridge *br, uint64_t key)
{
	const SiftTidBridgeSB *s = sift_tidbridge_probe_sb(br, sift_block_sb(key >> 16));
	uint32_t	rank = 0;

	for (int p = 0; p < page_of(key); p++)
		rank += (s->page_mask[p / 64] & ~s->lossy_mask[p / 64]) >> (p & 63) & 1;
	return &br->pages[s->first_page_idx + rank];
}

static void
check_kinds(int64_t *c, const SiftTidBridge *br, const Model *m)
{
	for (int i = 0, j; i < m->n; i = j)
	{
		for (j = i; j < m->n && (m->key[j] >> 16) == (m->key[i] >> 16);)
			j++;
		if (!page_is_lossy(br, m->key[i]))
		{
			const SiftTidBridgePage *p = page_record(br, m->key[i]);
			int			cnt = j - i;

			check(c, T_KINDS, p->page_in_sb == page_of(m->key[i]));
			check(c, T_KINDS, cnt <= SIFT_TIDP_ARRAY_MAX ? p->kind == SIFT_TIDP_ARRAY && p->n == cnt : p->kind == SIFT_TIDP_BITMAP && p->n == 0);
		}
	}
}

/* The offset masks of every block of every superblock against the filter of the exact pages (core/filter.c) and an all-ones row for a lossy page */
static void
check_blocks(int64_t *c, MemoryContext cx, const SiftTidBridge *br, const Model *m)
{
	SiftFilter *f = sift_filter_create(cx);
	uint64_t	om[SIFT_OFFMASK_WORDS];

	for (int i = 0, j; i < m->n; i = j)
	{
		memset(om, 0, sizeof(om));
		for (j = i; j < m->n && (m->key[j] >> 16) == (m->key[i] >> 16); j++)
			om[((m->key[j] & 0xFFFF) - 1) / 64] |= UINT64_C(1) << (((m->key[j] & 0xFFFF) - 1) & 63);
		if (!page_is_lossy(br, m->key[i]))
			sift_filter_add_page(f, m->key[i] >> 16, om);
	}
	for (uint32_t i = 0; i < br->nsbs; i++)
	{
		const SiftTidBridgeSB *s = &br->sbs[i];
		const SiftFilterSB *fs = sift_filter_sb(f, s->sb);

		for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
		{
			uint64_t	rows[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS],
						flt[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS],
						lossy,
						active,
						want = 0;

			for (int p = 0; p < SIFT_BLK_PAGES; p++)
				want |= (s->page_mask[(b * SIFT_BLK_PAGES + p) / 64] >> ((b * SIFT_BLK_PAGES + p) & 63) & 1) << p;
			active = sift_tidbridge_block_mask(br, s, b, rows, &lossy);
			check(c, T_FILTER, active == want && (lossy & ~active) == 0);
			if (fs != NULL)
				sift_filter_block(f, fs, b, flt);
			for (int p = 0; p < SIFT_BLK_PAGES; p++)
			{
				if (!(active >> p & 1))
					continue;
				if (lossy >> p & 1)
					for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
						check(c, T_FILTER, rows[p][w] == UINT64_MAX);
				else
					check(c, T_FILTER, fs != NULL && memcmp(rows[p], flt[p], sizeof(rows[p])) == 0);
			}
		}
	}
}

static void
check_intersect(int64_t *c, const SiftTidBridge *br, SiftRng *r)
{
	for (uint32_t i = 0; i < br->nsbs; i++)
	{
		const SiftTidBridgeSB *s = &br->sbs[i];
		uint64_t	cand[SIFT_PAGEMASK_WORDS],
					want[SIFT_PAGEMASK_WORDS],
					out[SIFT_PAGEMASK_WORDS],
					any = 0;

		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		{
			cand[w] = below(r, 4) == 0 ? 0 : below(r, 4) == 0 ? s->page_mask[w] : sift_rng_next(r) & sift_rng_next(r);
			any |= want[w] = cand[w] & s->page_mask[w];
		}
		check(c, T_INTERSECT, sift_tidbridge_intersect_pages(s, cand, out) == (any != 0) && memcmp(out, want, sizeof(out)) == 0);
		check(c, T_INTERSECT, sift_tidbridge_intersect_pages(s, cand, cand) == (any != 0) && memcmp(cand, want, sizeof(cand)) == 0);
	}
}

static void
check_bridge(int64_t *c, MemoryContext cx, const SiftTidBridge *br, const Model *m, SiftRng *r)
{
	check_probes(c, br, m, r);
	check_masks(c, br, m);
	check_kinds(c, br, m);
	check_blocks(c, cx, br, m);
	check_intersect(c, br, r);
	check(c, T_POINT, sift_tidbridge_is_point(br) == (!(br->flags & SIFT_TBR_HAS_LOSSY) && br->ntids <= SIFT_TIDB_POINT_MAX));
}

static bool
same_bytes(const void *a, const void *b, size_t n)
{
	return n == 0 || memcmp(a, b, n) == 0;
}

static bool
same_bridge(const SiftTidBridge *a, const SiftTidBridge *b)
{
	return a->nsbs == b->nsbs && a->npages == b->npages && a->array_len == b->array_len && a->bm_len == b->bm_len && a->ntids == b->ntids &&
		a->flags == b->flags && a->min_tid == b->min_tid && a->max_tid == b->max_tid && a->blk_bloom_union == b->blk_bloom_union &&
		same_bytes(a->sbs, b->sbs, a->nsbs * sizeof(SiftTidBridgeSB)) && same_bytes(a->pages, b->pages, a->npages * sizeof(SiftTidBridgePage)) &&
		same_bytes(a->array_pool, b->array_pool, a->array_len * sizeof(uint16)) &&
		same_bytes(a->bm_pool, b->bm_pool, a->bm_len * sizeof(a->bm_pool[0]));
}

/* One set of keys: unlimited byte budget (no lossy page, and the same bridge as the sorted keys give), a small one, none */
static void
scenario(int64_t *c, MemoryContext parent, SiftRng *r, uint64_t *keys, int n, bool point)
{
	Model		m = model_of(keys, n);
	int			total = insertion_order(r, keys, n);
	Size		budgets[] = {SIZE_MAX, 1 + below(r, 3000), 0};

	for (int b = 0; b < 3; b++)
	{
		MemoryContext cx = child(parent);
		SiftTidBridge *br = build(cx, budgets[b], 0, keys, total);

		check_bridge(c, cx, br, &m, r);
		if (b == 0)
		{
			SiftTidBridge *sorted = build(cx, SIZE_MAX, 0, m.key, m.n);

			check(c, T_BUDGET, !(br->flags & SIFT_TBR_HAS_LOSSY));
			check(c, T_ORDER, same_bridge(br, sorted));
			check(c, T_POINT, !point || (sift_tidbridge_is_point(br) && m.n <= SIFT_TIDB_POINT_MAX));
		}
		if (b == 2)
			check(c, T_BUDGET, br->npages == 0 && br->ntids == 0 && ((br->flags & SIFT_TBR_HAS_LOSSY) != 0) == (m.n > 0));
		MemoryContextDelete(cx);
	}
}

/* The byte budget: the first page is exact when there is room, a page that does not fit or outgrows an ARRAY is lossy, and bytes stop growing once it is spent */
static void
check_budget(int64_t *c, MemoryContext parent)
{
	MemoryContext cx = child(parent);
	uint64_t	keys[40];
	SiftTidBridge *br;
	const Size	budget = 16384;

	for (int i = 0; i < SIFT_TIDP_ARRAY_MAX; i++)
		keys[i] = UINT64_C(5) << 16 | (i + 1);
	keys[SIFT_TIDP_ARRAY_MAX] = UINT64_C(6) << 16 | 1;
	br = build(cx, 1, 0, keys, SIFT_TIDP_ARRAY_MAX + 1);
	check(c, T_BUDGET, br->npages == 1 && br->pages[0].kind == SIFT_TIDP_ARRAY && br->pages[0].n == SIFT_TIDP_ARRAY_MAX && br->ntids == SIFT_TIDP_ARRAY_MAX);
	check(c, T_BUDGET, !page_is_lossy(br, keys[0]) && page_is_lossy(br, keys[SIFT_TIDP_ARRAY_MAX]) && (br->flags & SIFT_TBR_HAS_LOSSY));
	check(c, T_BUDGET, sift_tidbridge_probe_tid(br, UINT64_C(6) << 16 | 300, NULL) == SIFT_TIDPROBE_LOSSY);
	check(c, T_BUDGET, sift_tidbridge_probe_tid(br, UINT64_C(5) << 16 | 17, NULL) == SIFT_TIDPROBE_NO);

	keys[SIFT_TIDP_ARRAY_MAX] = UINT64_C(5) << 16 | (SIFT_TIDP_ARRAY_MAX + 1);
	br = build(cx, 1, 0, keys, SIFT_TIDP_ARRAY_MAX + 1);
	check(c, T_BUDGET, br->npages == 0 && br->ntids == 0 && br->array_len == 0 && page_is_lossy(br, keys[0]));
	check(c, T_BUDGET, sift_tidbridge_probe_tid(br, UINT64_C(5) << 16 | 400, NULL) == SIFT_TIDPROBE_LOSSY);
	br = build(cx, SIZE_MAX, 0, keys, SIFT_TIDP_ARRAY_MAX + 1);
	check(c, T_BUDGET, br->npages == 1 && br->pages[0].kind == SIFT_TIDP_BITMAP && br->ntids == SIFT_TIDP_ARRAY_MAX + 1);

	br = sift_tidbridge_create(cx, budget, 0);
	{
		Size		spent = 0;

		for (uint64_t blk = 0; blk < 6000; blk++)
		{
			for (int o = 1; o <= 3; o++)
				sift_tidbridge_add_tid(br, (blk * 7) << 16 | o);
			if (spent == 0 && (br->flags & SIFT_TBR_HAS_LOSSY))
				spent = sift_tidbridge_bytes(br);
		}
		check(c, T_BUDGET, spent >= budget && spent <= 2 * budget + 4096);
		check(c, T_BUDGET, sift_tidbridge_bytes(br) == spent);
		sift_tidbridge_finalize(br);
		for (uint64_t blk = 0; blk < 6000; blk++)
			for (int o = 1; o <= 3; o++)
			{
				SiftTidProbe p = sift_tidbridge_probe_tid(br, (blk * 7) << 16 | o, NULL);

				check(c, T_BUDGET, p == SIFT_TIDPROBE_EXACT || p == SIFT_TIDPROBE_LOSSY);
			}
		check(c, T_BUDGET, sift_tidbridge_bytes(br) <= spent);
	}
	MemoryContextDelete(cx);
}

/* The keys of one round with a score or a row locator for some: every exact TID has the last one added, 0 for none; a lossy page has none */
static void
check_side(int64_t *c, MemoryContext parent, SiftRng *r, uint64_t *keys, int n)
{
	Model		m = model_of(keys, n);
	int			total = insertion_order(r, keys, n);
	Size		budgets[] = {SIZE_MAX, 1 + below(r, 6000)};

	for (int b = 0; b < 2; b++)
	{
		MemoryContext cx = child(parent);
		SiftTidBridge *br = sift_tidbridge_create(cx, budgets[b], SIFT_TBR_HAS_SCORES | SIFT_TBR_HAS_ROWLOC | SIFT_TBR_EXACT_QUALS);
		float4	   *score = palloc0(Max(m.n, 1) * sizeof(float4));
		SiftTidRowLoc *loc = palloc0(Max(m.n, 1) * sizeof(SiftTidRowLoc));

		for (int i = 0; i < total; i++)
		{
			int			k = model_find(&m, keys[i]);

			switch (below(r, 3))
			{
				case 0:
					sift_tidbridge_add_tid(br, keys[i]);
					break;
				case 1:
					score[k] = 1 + below(r, 100000) / 64.0f;
					sift_tidbridge_add_score(br, keys[i], score[k]);
					break;
				default:
					loc[k] = (SiftTidRowLoc) {below(r, 1000000), below(r, 60000), below(r, 65536)};
					sift_tidbridge_add_rowloc(br, keys[i], loc[k]);
			}
		}
		sift_tidbridge_finalize(br);
		check(c, T_SCORES, (br->flags & (SIFT_TBR_HAS_SCORES | SIFT_TBR_HAS_ROWLOC | SIFT_TBR_EXACT_QUALS)) == (SIFT_TBR_HAS_SCORES | SIFT_TBR_HAS_ROWLOC | SIFT_TBR_EXACT_QUALS));
		for (int i = 0; i < m.n; i++)
		{
			float4		got = -1;
			SiftTidRowLoc gotloc = {0};
			SiftTidProbe p = sift_tidbridge_probe_tid(br, m.key[i], &got);
			bool		have = sift_tidbridge_rowloc(br, m.key[i], &gotloc);

			check(c, T_SCORES, p != SIFT_TIDPROBE_NO);
			if (p == SIFT_TIDPROBE_EXACT)
			{
				check(c, T_SCORES, got == score[i]);
				check(c, T_ROWLOC, have && memcmp(&gotloc, &loc[i], sizeof(gotloc)) == 0);
			}
			else
			{
				check(c, T_SCORES, got == -1);
				check(c, T_ROWLOC, !have);
			}
		}
		check_probes(c, br, &m, r);
		MemoryContextDelete(cx);
	}
	{
		MemoryContext cx = child(parent);
		SiftTidBridge *br = build(cx, SIZE_MAX, 0, keys, total);
		float4		got = -1;

		check(c, T_SCORES, sift_tidbridge_probe_tid(br, m.key[0], &got) == SIFT_TIDPROBE_EXACT && got == -1);
		check(c, T_ROWLOC, !sift_tidbridge_rowloc(br, m.key[0], &(SiftTidRowLoc) {0}));
		MemoryContextDelete(cx);
	}
}

/* from_docids: the bridge of the documents is the bridge of their keys */
static void
check_docids(int64_t *c, MemoryContext parent, SiftRng *r)
{
	static const uint32_t dpps[] = {1, 7, 100, 291, SIFT_TIDB_MAX_OFFSET};
	uint32_t	dpp = dpps[below(r, lengthof(dpps))];
	int			n = 1 + below(r, 5000);
	uint64_t   *docs = palloc(n * sizeof(uint64_t)),
			   *keys = palloc(n * sizeof(uint64_t));
	MemoryContext cx = child(parent);
	SiftTidBridge *br;
	Model		m;

	for (int i = 0; i < n; i++)
	{
		docs[i] = below(r, 3) ? below(r, 1 << 14) : below(r, UINT32_MAX);
		keys[i] = (docs[i] / dpp) << 16 | (docs[i] % dpp + 1);
	}
	m = model_of(keys, n);
	br = sift_tidbridge_from_docids(cx, docs, n, dpp);
	check(c, T_DOCIDS, br->flags == 0 && br->finalized && same_bridge(br, build(cx, SIZE_MAX, 0, m.key, m.n)));
	for (int i = 0; i < n; i++)
		check(c, T_DOCIDS, sift_tidbridge_probe_tid(br, keys[i], NULL) == SIFT_TIDPROBE_EXACT);
	check_bridge(c, cx, br, &m, r);
	check(c, T_DOCIDS, sift_tidbridge_from_docids(cx, docs, 0, dpp)->ntids == 0);
	MemoryContextDelete(cx);
}

/* A bridge that is reset takes the next chunk as a new one, its producer's flags kept and its lossy flag gone */
static void
check_reset(int64_t *c, MemoryContext parent, SiftRng *r, uint64_t *keys)
{
	static const Size budgets[] = {SIZE_MAX, 0};

	for (int b = 0; b < 2; b++)
	{
		MemoryContext cx = child(parent);
		SiftTidBridge *br = sift_tidbridge_create(cx, budgets[b], SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS | SIFT_TBR_HAS_SCORES);

		for (int round = 0; round < 3; round++)
		{
			int			n = generate(r, keys, 1 + below(r, 3), 3 + below(r, 6), 30);
			Model		m = model_of(keys, n);

			for (int i = 0; i < n; i++)
				sift_tidbridge_add_score(br, keys[i], 1.0f + round);
			sift_tidbridge_finalize(br);
			check(c, T_RESET, (br->flags & ~SIFT_TBR_HAS_LOSSY) == (SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS | SIFT_TBR_HAS_SCORES));
			check_probes(c, br, &m, r);
			check_masks(c, br, &m);
			for (int i = 0; i < m.n; i++)
			{
				float4		s = 0;

				if (sift_tidbridge_probe_tid(br, m.key[i], &s) == SIFT_TIDPROBE_EXACT)
					check(c, T_RESET, s == 1.0f + round);
			}
			sift_tidbridge_reset(br);
			check(c, T_RESET, br->nsbs == 0 && br->npages == 0 && br->ntids == 0 && !br->finalized && !(br->flags & SIFT_TBR_HAS_LOSSY) && br->min_tid == UINT64_MAX);
		}
		MemoryContextDelete(cx);
	}
}

/* The point mode is on for at most SIFT_TIDB_POINT_MAX exact TIDs and never with a lossy page */
static void
check_point(int64_t *c, MemoryContext parent)
{
	static const int sizes[] = {0, 1, 2, SIFT_TIDB_POINT_MAX - 1, SIFT_TIDB_POINT_MAX, SIFT_TIDB_POINT_MAX + 1, 3 * SIFT_TIDB_POINT_MAX};
	uint64_t	keys[3 * SIFT_TIDB_POINT_MAX];

	for (int s = 0; s < (int) lengthof(sizes); s++)
	{
		MemoryContext cx = child(parent);
		SiftTidBridge *br;

		for (int i = 0; i < sizes[s]; i++)
			keys[i] = ((uint64_t) (i / 8) * 5 << 16) | (i % 8 + 1);
		br = build(cx, SIZE_MAX, 0, keys, sizes[s]);
		check(c, T_POINT, br->ntids == (uint64_t) sizes[s] && sift_tidbridge_is_point(br) == (sizes[s] <= SIFT_TIDB_POINT_MAX));
		br = build(cx, 0, 0, keys, sizes[s]);
		check(c, T_POINT, !sift_tidbridge_is_point(br) == (sizes[s] > 0));
		MemoryContextDelete(cx);
	}
}

static void
check_layout(int64_t *c, SiftRng *r)
{
	check(c, T_LAYOUT, sizeof(SiftTidBridgeSB) == 24 + 16 * SIFT_PAGEMASK_WORDS && sizeof(SiftTidRowLoc) == 8 && sizeof(SiftTidBridgePage) == 12);
#if SIFT_SB_SHIFT == 8
	check(c, T_LAYOUT, sizeof(SiftTidBridgeSB) == 88);
#endif
	for (int i = 0; i < 256; i++)
		check(c, T_LAYOUT, pg_popcount64(sift_tid_bloom_bit(i < 4 ? UINT32_MAX - i : sift_rng_next(r) >> 32)) == 1);
}

/* c: a pair of counts (checks, failures) for every T_*, in the order of the enum */
static void
sift_tidbridge_selftest(uint64_t seed, int rounds, int64_t *c)
{
	SiftRng		rng;
	MemoryContext rc = sift_mem_allocset_create(CurrentMemoryContext, "tidbridge checks"),
				old = MemoryContextSwitchTo(rc);
	uint64_t   *keys = palloc(2 * MAX_KEYS * sizeof(uint64_t));
	int			n;

	sift_rng_seed(&rng, seed);
	memset(c, 0, 2 * T_N * sizeof(int64_t));
	check_layout(c, &rng);
	check_point(c, rc);
	check_budget(c, rc);
	check_reset(c, rc, &rng, keys);
	for (int i = 0; i < rounds; i++)
	{
		n = generate_sparse(&rng, keys);
		scenario(c, rc, &rng, keys, n, true);
		n = generate(&rng, keys, 1 + below(&rng, 3), 4 + below(&rng, 10), 40);
		scenario(c, rc, &rng, keys, n, false);
		n = generate(&rng, keys, 5 + below(&rng, 4), 3 + below(&rng, 5), 20);
		scenario(c, rc, &rng, keys, n, false);
		n = generate(&rng, keys, 1 + below(&rng, 3), 3 + below(&rng, 8), 25);
		check_side(c, rc, &rng, keys, n);
		check_docids(c, rc, &rng);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(rc);
}

#if !defined(SIFT_HOST_SQLITE) && !defined(SIFT_HOST_MARIADB)

PG_FUNCTION_INFO_V1(sift_test_tidbridge_check);

/* (seed int8, rounds int4) returns int8[]: the checks and the failures of each T_* */
Datum
sift_test_tidbridge_check(PG_FUNCTION_ARGS)
{
	int64_t		c[2 * T_N];
	Datum		out[2 * T_N];

	sift_tidbridge_selftest(PG_GETARG_INT64(0), PG_GETARG_INT32(1), c);
	for (int i = 0; i < 2 * T_N; i++)
		out[i] = Int64GetDatum(c[i]);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(out, 2 * T_N, INT8OID));
}

#endif

#endif							/* SIFT_TESTING */
