/*
 * test/unit/col/f2/scaling.c - the algorithms of src/col/f2 that the O(N) work replaced, run on their own under ASan and UBSan: each is compared with a plain reference on
 * randomized input, and timed at N and 8N to show that the work does not grow with the square of N. The sources are included, so the functions tested are the ones of the library.
 *
 *   scripts/unit-scaling.sh   builds and runs it (and the rest of test/unit) under ASan
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#include <time.h>

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/f2/f2_pred.c"
#include "col/f2/f2_pack.c"
#include "col/f2/f2_col_delta_idx.c"
#include "col/f2/f2_vac_vm.c"
#include "col/f2/f2_part_idx.h"

/* ---- [ON-F2-02] the constants of an IN list in a table ---- */

enum { ROWS_PRED = 2 * UNIT_QUERIES };

static double
pred_run_kind(int nvals, int kind, bool verify)
{
	SiftColValue *vals = palloc0(nvals * sizeof(SiftColValue));
	char	  **strs = palloc0(nvals * sizeof(char *));
	SiftColPred p = {.op = SIFT_COLOP_IN,.nvals = nvals,.vals = vals};
	SiftPredSet *ps = sift_f2_pred_sets_new(CurrentMemoryContext, 1);
	double		t0;
	uint64		hits = 0;

	for (int i = 0; i < nvals; i++)
	{
		if (kind == 0)
			vals[i].v.i = (int64) (rnd() % (4 * (uint64) nvals));
		else if (kind == 1)
			vals[i].v.f = i % 97 == 0 ? NAN : i % 89 == 0 ? -0.0 : (double) (rnd() % (4 * (uint64) nvals)) / 4.0;
		else
		{
			strs[i] = palloc(16);
			snprintf(strs[i], 16, "k%u", (unsigned) (rnd() % (4 * (uint64) nvals) % 1000));
			vals[i].v.s = strs[i];
			vals[i].len = strlen(strs[i]);
		}
	}
	t0 = now_s();
	for (int r = 0; r < ROWS_PRED; r++)
	{
		bool		got;
		bool		want = false;

		if (kind == 0)
		{
			int64		v = (int64) (rnd() % (4 * (uint64) nvals));

			got = sift_f2_in_i64(ps, &p, v);
			for (int j = 0; verify && j < nvals && !want; j++)
				want = vals[j].v.i == v;
		}
		else if (kind == 1)
		{
			double		v = r % 101 == 0 ? NAN : r % 83 == 0 ? 0.0 : (double) (rnd() % (4 * (uint64) nvals)) / 4.0;

			got = sift_f2_in_f8(ps, &p, v);
			for (int j = 0; verify && j < nvals && !want; j++)
				want = sift_f2_f8_cmp(v, vals[j].v.f) == 0;
		}
		else
		{
			char		buf[16];

			snprintf(buf, sizeof(buf), "k%u", (unsigned) (rnd() % 1000));
			got = sift_f2_in_str(ps, &p, buf, strlen(buf));
			for (int j = 0; verify && j < nvals && !want; j++)
				want = sift_f2_str_cmp(buf, strlen(buf), vals[j].v.s, vals[j].len) == 0;
		}
		if (verify)
			SIFT_CHECKF(got == want, "IN kind %d nvals %d row %d: set %d reference %d", kind, nvals, r, got, want);
		hits += got;
	}
	t0 = now_s() - t0;
	if (ps->slots != NULL)
		pfree(ps->slots);
	pfree(ps);
	for (int i = 0; kind == 2 && i < nvals; i++)
		pfree(strs[i]);
	pfree(strs);
	pfree(vals);
	return hits == UINT64_MAX ? 0 : t0;
}

static double pred_run_i64(int n) { return pred_run_kind(n, 0, false); }
static double pred_run_f8(int n) { return pred_run_kind(n, 1, false); }
static double pred_run_str(int n) { return pred_run_kind(n, 2, false); }

static void
test_pred_set(void)
{
	for (int kind = 0; kind < 3; kind++)
		for (int n = 1; n <= 64; n += (n < 12 ? 1 : 13))
			(void) pred_run_kind(n, kind, true);
	(void) pred_run_kind(1000, 0, true);
	(void) pred_run_kind(1000, 1, true);
	(void) pred_run_kind(1000, 2, true);
	report("pred_set int64", pred_run_i64, 1000, 3.0);
	report("pred_set float8 (NaN, -0.0)", pred_run_f8, 1000, 3.0);
	report("pred_set string", pred_run_str, 1000, 3.0);
}

/* ---- [ON-F2-21] the first fit of the parts into the pages of a packed part ---- */

static int
ref_first_fit(PackCand *cands, int n, PackBin *bins)
{
	int			nbins = 0;

	qsort(cands, n, sizeof(PackCand), cand_len_desc);
	for (int i = 0; i < n; i++)
	{
		int			b = 0;

		while (b < nbins && bin_room(&bins[b]) < cands[i].len)
			b++;
		if (b == nbins)
			bins[nbins++] = (PackBin) {.used = sizeof(SiftPackedPageData)};
		bins[b].used = MAXALIGN(bins[b].used) + cands[i].len;
		bins[b].n++;
		cands[i].bin = b;
	}
	return nbins;
}

static PackCand *
make_cands(int n)
{
	PackCand   *c = palloc0(n * sizeof(PackCand));

	for (int i = 0; i < n; i++)
	{
		c[i].len = 16 + (uint32) (rnd() % (i % 7 == 0 ? 6000 : 400));
		c[i].part_id = i;
	}
	return c;
}

static double
pack_run(int n)
{
	PackCand   *c = make_cands(n);
	PackBin    *bins = palloc0((n + 1) * sizeof(PackBin));
	double		t0 = now_s();
	int			nb = assign_bins(c, n, bins);

	t0 = now_s() - t0;
	pfree(c);
	pfree(bins);
	return nb < 0 ? 0 : t0;
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PACK_REF_N	8000
#else
#define PACK_REF_N	32000
#endif

static void
test_pack_ffd(void)
{
	for (int n = 1; n < 3000; n = n * 3 + 1)
	{
		PackCand   *a = make_cands(n);
		PackCand   *b = memcpy(palloc(n * sizeof(PackCand)), a, n * sizeof(PackCand));
		PackBin    *ba = palloc0((n + 1) * sizeof(PackBin));
		PackBin    *bb = palloc0((n + 1) * sizeof(PackBin));
		int			na = assign_bins(a, n, ba);
		int			nb = ref_first_fit(b, n, bb);

		SIFT_CHECKF(na == nb, "pack n=%d: %d bins, reference %d", n, na, nb);
		for (int i = 0; i < n; i++)
		{
			const PackCand *x = &a[i];
			const PackCand *y = NULL;

			for (int j = 0; j < n && y == NULL; j++)
				y = b[j].part_id == x->part_id ? &b[j] : NULL;
			SIFT_CHECKF(y != NULL && x->bin == y->bin, "pack n=%d part %d: bin %d reference %d", n, (int) x->part_id, x->bin, y ? y->bin : -1);
			if (n > 1000)
				break;
		}
		pfree(a);
		pfree(b);
		pfree(ba);
		pfree(bb);
	}
	report("pack first-fit-decreasing", pack_run, 4000, 32.0);
	{
		PackCand   *c = make_cands(PACK_REF_N);
		PackBin    *bins = palloc0((PACK_REF_N + 1) * sizeof(PackBin));
		double		t0 = now_s();

		(void) ref_first_fit(c, PACK_REF_N, bins);
		printf("%-28s N=%d  linear first fit (the code replaced) %8.3f ms\n", "pack reference", PACK_REF_N, (now_s() - t0) * 1e3);
		pfree(c);
		pfree(bins);
	}
}

/* ---- [ON-F2-10] [ON-F2-27] the address changes of a chain ---- */

typedef struct RemapFix
{
	SiftCdRemapMap map;
	SiftVmDirtySet ds;
	int			nparts;
} RemapFix;

static int
slot_order(const void *a, const void *b)
{
	return slot_cmp(a, b);
}

static void
remap_fix(RemapFix *f, int m, int nparts)
{
	SiftCdRemap *v = palloc0(m * sizeof(SiftCdRemap));
	int			k = 0;

	memset(f, 0, sizeof(*f));
	f->nparts = nparts;
	for (int i = 0; i < m; i++)
		v[i] = (SiftCdRemap) {.part_gen = 1 + rnd() % nparts,.granule_id = (uint32) (rnd() % 64),.row = (uint16) (rnd() % 8192),.new_tid = ((rnd() % 4000) << 16) | (1 + rnd() % 100)};
	qsort(v, m, sizeof(SiftCdRemap), slot_order);
	f->map.slot = palloc0(m * sizeof(SiftCdRemap));
	for (int i = 0; i < m; i++)
		if (i + 1 == m || slot_cmp(&v[i], &v[i + 1]) != 0)
			f->map.slot[k++] = v[i];
	f->map.n = k;
	f->map.by_new = memcpy(palloc0(k * sizeof(SiftCdRemap)), f->map.slot, k * sizeof(SiftCdRemap));
	qsort(f->map.by_new, k, sizeof(SiftCdRemap), new_cmp);
	remap_index(&f->map);
	f->ds.intervals = palloc0(1000 * sizeof(SiftBlkInterval));
	for (int i = 0; i < 1000; i++)
		f->ds.intervals[f->ds.nintervals++] = (SiftBlkInterval) {.lo = i * 4 + 1,.hi = i * 4 + 1};
	f->ds.nheap_blocks = 4001;
	pfree(v);
}

static void
remap_free(RemapFix *f)
{
	pfree(f->map.slot);
	pfree(f->map.by_new);
	pfree(f->map.parts);
	pfree(f->map.grans);
	pfree(f->map.slot_ht);
	pfree(f->map.new_ht);
	pfree(f->ds.intervals);
}

static bool
ref_dirty(const RemapFix *f, uint64 part_gen, int64 granule)
{
	for (int i = 0; i < f->map.n; i++)
		if (f->map.slot[i].part_gen == part_gen && (granule < 0 || f->map.slot[i].granule_id == (uint32) granule) &&
			sift_vm_dirty_overlaps(&f->ds, (BlockNumber) (f->map.slot[i].new_tid >> 16), (BlockNumber) (f->map.slot[i].new_tid >> 16)))
			return true;
	return false;
}

static double
remap_run_fix(int m, bool verify)
{
	RemapFix	f;
	double		t0;
	uint64		sink = 0;

	remap_fix(&f, m, 256);
	t0 = now_s();
	for (int q = 0; q < UNIT_QUERIES; q++)
	{
		const SiftCdRemap *r = &f.map.slot[rnd() % f.map.n];
		uint64		tid = rnd() % 3 == 0 ? r->new_tid + 1 : r->new_tid;
		uint32		g = (uint32) (rnd() % 64);
		const SiftCdRemap *by = sift_cd_remap_by_new(&f.map, tid);
		uint64		moved = sift_cd_remap_new_tid(&f.map, r->part_gen, r->granule_id, r->row);

		sink += by != NULL;
		sink += moved;
		sink += sift_cd_remap_part_in_dirty(&f.map, r->part_gen, &f.ds);
		sink += sift_cd_remap_granule_in_dirty(&f.map, r->part_gen, g, &f.ds);
		if (verify)
		{
			const SiftCdRemap *want = NULL;

			for (int i = 0; i < f.map.n && want == NULL; i++)
				want = f.map.by_new[i].new_tid == tid ? &f.map.by_new[i] : NULL;
			SIFT_CHECKF((by == NULL) == (want == NULL) && (by == NULL || by->new_tid == want->new_tid), "by_new %llu", (unsigned long long) tid);
			SIFT_CHECKF(moved == r->new_tid, "new_tid of the slot %u/%u", r->granule_id, r->row);
			SIFT_CHECKF(sift_cd_remap_supersedes(&f.map, r->part_gen, r->granule_id, r->row, r->new_tid + 1), "supersedes");
			SIFT_CHECKF(!sift_cd_remap_supersedes(&f.map, r->part_gen, r->granule_id, r->row, r->new_tid), "does not supersede its own");
			SIFT_CHECKF(sift_cd_remap_part_in_dirty(&f.map, r->part_gen, &f.ds) == ref_dirty(&f, r->part_gen, -1), "part dirty");
			SIFT_CHECKF(sift_cd_remap_granule_in_dirty(&f.map, r->part_gen, g, &f.ds) == ref_dirty(&f, r->part_gen, g), "granule dirty");
			if (q % 17 == 0)
			{
				SiftCdRemapCursor cur;

				sift_cd_remap_cursor_init(&cur, &f.map, r->part_gen, r->granule_id);
				for (int row = 0; row < 8192; row += 97)
					SIFT_CHECKF(sift_cd_remap_cursor_new_tid(&cur, (uint16) row) == sift_cd_remap_new_tid(&f.map, r->part_gen, r->granule_id, (uint16) row), "cursor row %d", row);
			}
		}
	}
	t0 = now_s() - t0;
	remap_free(&f);
	return sink == UINT64_MAX ? 0 : t0;
}

static double remap_run(int m) { return remap_run_fix(m, false); }

static void
test_cd_remap(void)
{
	(void) remap_run_fix(40, true);
	(void) remap_run_fix(2000, true);
	report("cd remap hash, bloom, cursor", remap_run, 2000, 3.0);
}

/* ---- [ON-F2-18] [ON-F2-23] sets of generations ---- */

static double
u64_set_run(int n)
{
	SiftU64Set	s;
	double		t0 = now_s();
	uint64		sink = 0;

	sift_u64_set_init(&s, 16);
	for (int i = 0; i < n; i++)
		sift_u64_set_add(&s, (uint64) i * 7919);
	for (int i = 0; i < n; i++)
		sink += sift_u64_set_has(&s, (uint64) i * 7919) + sift_u64_set_has(&s, (uint64) i * 7919 + 1);
	SIFT_CHECKF(sink == (uint64) n, "u64 set: %llu hits of %d", (unsigned long long) sink, n);
	SIFT_CHECKF(s.count == (uint32) n, "u64 set: %u members of %d", s.count, n);
	sift_u64_set_free(&s);
	return now_s() - t0;
}

static void
test_u64_set(void)
{
	SiftU64Set	s;

	sift_u64_set_init(&s, 4);
	sift_u64_set_add(&s, 0);
	sift_u64_set_add(&s, 0);
	sift_u64_set_add(&s, 5);
	SIFT_CHECKF(sift_u64_set_has(&s, 0) && sift_u64_set_has(&s, 5) && !sift_u64_set_has(&s, 6) && s.count == 2, "u64 set with zero");
	sift_u64_set_free(&s);
	report("u64 set of generations", u64_set_run, 20000, 20.0);
}

/* the set of the dirty blocks of a heap holds at most SIFT_VM_DIRTY_MAX_INTERVALS runs: past them it is the set that skips nothing and its runs are freed, whatever the size of the heap */
static void
test_vm_dirty_bound(void)
{
	SiftVmDirtySet ds;
	uint64		words[16];
	const int	nwords = 16 * ((SIFT_VM_DIRTY_MAX_INTERVALS / 16) * 2 + 1);

	for (int w = 0; w < 16; w++)
		words[w] = UINT64CONST(0x4444444444444444);
	sift_vm_dirty_init(&ds, (BlockNumber) nwords * SIFT_VM_BLOCKS_PER_WORD);
	for (int w = 0; w < nwords; w += 16)
		sift_vm_dirty_scan_words(&ds, (BlockNumber) w * SIFT_VM_BLOCKS_PER_WORD, words, 16);
	sift_vm_dirty_finish(&ds);
	SIFT_CHECKF(ds.all_dirty && ds.intervals == NULL && ds.nintervals == 0, "dirty set past its bound skips nothing and holds no runs");
	SIFT_CHECKF(sift_vm_dirty_overlaps(&ds, 1, 1), "dirty set that gave up says every block may be dirty");
	sift_vm_dirty_free(&ds);

	sift_vm_dirty_init(&ds, 1000 * SIFT_VM_BLOCKS_PER_WORD);
	for (int w = 0; w < 1000; w += 16)
		sift_vm_dirty_scan_words(&ds, (BlockNumber) w * SIFT_VM_BLOCKS_PER_WORD, words, Min(16, 1000 - w));
	sift_vm_dirty_finish(&ds);
	SIFT_CHECKF(!ds.all_dirty && ds.nintervals == 16 * 1000 && sift_vm_dirty_overlaps(&ds, 0, 0) && !sift_vm_dirty_overlaps(&ds, 1, 1), "dirty set under its bound keeps its runs");
	sift_vm_dirty_free(&ds);
}

int
main(void)
{
	test_vm_dirty_bound();
	test_pred_set();
	test_pack_ffd();
	test_cd_remap();
	test_u64_set();
	return sift_check_summary();
}
