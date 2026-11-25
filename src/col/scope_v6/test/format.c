/*
 * col/scope_v6/test/format.c - the scenarios of sift._test_scope_v6 (col/scope_v6/test/core.c) about the bytes of the summary: the section tags and the
 * sizes of the header and the roots, the wide ring of an object of more than 524,288 granules, the compressed cube with its rollups, distinct sets
 * and extremes, and the heap that picks the node to evict.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/compress.h"
#include "col/scope_v6/int.h"
#include "col/codec.h"
#include "col/scope_v6/test/world.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_format(TCheck *t);
extern void scope_v6_scenario_cubewire(TCheck *t);
extern void scope_v6_scenario_heap(TCheck *t);

/* ---------------------------------------------------------------------------
 * format: tags, sizes, the wide ring
 * ------------------------------------------------------------------------- */

static uint32
section_tags(TCheck *t, const StringInfoData *obj)
{
	SiftScopeTrailer tr;
	uint32		seen = 0;

	memcpy(&tr, obj->data + obj->len - sizeof(tr), sizeof(tr));
	for (uint32 i = 0; i < tr.nsec; i++)
	{
		SiftScopeSecEntry e;

		memcpy(&e, obj->data + tr.dir_off + (Size) i * sizeof(e), sizeof(e));
		TCHK(t, e.tag >= SEC_SCOPE_HDR && e.tag <= SEC_SCOPE_KEYDIR, "section tag 0x%04x is outside 0x0401..0x0408", e.tag);
		if (e.tag >= SEC_SCOPE_HDR && e.tag <= SEC_SCOPE_KEYDIR)
			seen |= 1U << (e.tag - SEC_SCOPE_HDR);
	}
	TCHK(t, tr.nsec == 8, "the object has %u sections, not 8", tr.nsec);
	return seen;
}

/* The ring section is stored as it is, or LZ4-compressed when that is at least SIFT_SCOPE_TOPK_LZ4_MIN_PCT smaller */
static bool
topk_stored_ok(const SiftScopeMeta *m)
{
	if (m->topk_raw == NULL)
		return m->topk_stored == m->topk.len;
	return m->topk_stored * 100 <= m->topk.len * (100 - SIFT_SCOPE_TOPK_LZ4_MIN_PCT);
}

/* A part of `total` granules of which only the first ones exist: the ring of a key only names granules that do */
static void
build_sparse(TWorld *w, SiftScopePairMap *pm, uint32 total, StringInfo out)
{
	SiftScopeBuildSpec spec;
	SiftScopeBuild *b;
	SiftScopeGranSource src = tw_source(w);
	uint32		base;

	tw_spec(w, &spec, 0);
	b = sift_scope_build_begin(&spec, pm);
	tw_dict_maps(w, b);
	w->cur_pm = pm;
	base = sift_scope_build_add_part(b, 1, total);
	for (uint32 gi = 0; gi < w->ngran; gi++)
		sift_scope_build_on_granule(b, &src, base + gi);
	sift_scope_build_end(b, out, NULL);
}

static uint32
check_rings(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label)
{
	SiftScopeEntity e = tw_entity(w);
	uint32		checked = 0;

	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);
		SiftScopeNewest ring[SIFT_SCOPE_TOPK_CAP],
					truth[SIFT_SCOPE_TOPK_CAP],
					got[20];
		uint32		nt = tw_truth_newest(w, pm, key, SIFT_SCOPE_TOPK_CAP, truth),
					nr,
					ng,
					from_ring;

		if (r == NULL)
			continue;
		nr = sift_scope_topk_get(m, r, ring);
		TCHK(t, nr == nt && memcmp(ring, truth, sizeof(*ring) * nt) == 0, "%s: key %lld ring of %u entries, truth %u", label, (long long) key, nr, nt);
		ng = sift_scope_latest(m, r, 20, false, &e, got, &from_ring);
		TCHK(t, ng == Min(nt, 20) && memcmp(got, truth, sizeof(*got) * ng) == 0 && from_ring == ng, "%s: key %lld latest 20 through the ring", label, (long long) key);
		checked += nr;
	}
	return checked;
}

/* The source of a world whose granules from `from` on are `shift` places lower than the object counts them; the granules [blank_lo, blank_hi) of the object hold no row */
typedef struct Shifted
{
	SiftScopeGranSource inner;
	uint32		from;
	uint32		shift;
	uint32		blank_lo;
	uint32		blank_hi;
} Shifted;

static uint32
shifted(const Shifted *s, uint32 gidx)
{
	return gidx >= s->from ? gidx - s->shift : gidx;
}

static void
sh_head(void *arg, uint32 gidx, SiftScopeGranHead *h)
{
	Shifted    *s = arg;

	memset(h, 0, sizeof(*h));
	if (gidx < s->blank_lo || gidx >= s->blank_hi)
		s->inner.head(s->inner.arg, shifted(s, gidx), h);
}

static void
sh_cube(void *arg, uint32 gidx, SiftScopeGranCube *c)
{
	Shifted    *s = arg;

	s->inner.cube(s->inner.arg, shifted(s, gidx), c);
}

static uint32
sh_tail(void *arg, uint32 gidx, uint32 ring, uint32 max, SiftScopeTail *out)
{
	Shifted    *s = arg;

	return s->inner.tail_ring(s->inner.arg, shifted(s, gidx), ring, max, out);
}

static void
sh_rows(void *arg, uint32 gidx, SiftScopeRows *r, const SiftScopeDictMap **map)
{
	Shifted    *s = arg;

	s->inner.rows(s->inner.arg, shifted(s, gidx), r, map);
}

/* The rings of the object name the granules of the world at the numbers the object gives them */
static void
check_shifted_rings(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, uint32 from_world, uint32 shift)
{
	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);
		SiftScopeNewest ring[SIFT_SCOPE_TOPK_CAP],
					truth[SIFT_SCOPE_TOPK_CAP];
		uint32		nt = tw_truth_newest(w, pm, key, SIFT_SCOPE_TOPK_CAP, truth),
					nr;

		if (r == NULL)
			continue;
		for (uint32 i = 0; i < nt; i++)
			truth[i].gidx += truth[i].gidx >= from_world ? shift : 0;
		nr = sift_scope_topk_get(m, r, ring);
		TCHK(t, nr <= nt && memcmp(ring, truth, sizeof(*ring) * nr) == 0 && (nr == nt || (r->flags & SIFT_L2_TOPK_PARTIAL)), "key %lld: ring of %u entries, truth %u", (long long) key, nr, nt);
	}
}

/* A part of 524,282 granules that no row is in, then the two parts of the world: 524,286 granules in all, the compact ring */
static void
build_with_blank_part(TWorld *w, SiftScopePairMap *pm, uint32 blank, StringInfo out)
{
	SiftScopeBuildSpec spec;
	SiftScopeBuild *b;
	SiftScopeGranSource src = tw_source(w);

	tw_spec(w, &spec, 0);
	b = sift_scope_build_begin(&spec, pm);
	tw_dict_maps(w, b);
	w->cur_pm = pm;
	for (uint32 p = 0; p < w->nparts; p++)
	{
		uint32		base = sift_scope_build_add_part(b, w->part_gen[p], w->part_n[p]);

		for (uint32 gi = 0; gi < w->part_n[p]; gi++)
			sift_scope_build_on_granule(b, &src, base + gi);
	}
	sift_scope_build_add_part(b, 99, blank);
	sift_scope_build_end(b, out, NULL);
}

static void
refresh_across_the_switch(TCheck *t, uint32 *rings_converted)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 60, .maxrows = 60, .long_pct = 10, .granule_rows = 32, .nparts = 2, .months = 6, .nkeys = 5, .keys_per_member = 2};
	SiftScopePairMap *pm;
	SiftScopeBuildSpec spec;
	StringInfoData before,
				after;
	SiftScopeMeta mb,
				ma;
	SiftScopePartSwap swap;
	SiftScopeNewPart added;
	SiftScopeBuildStats st;
	Shifted		sh;
	SiftScopeGranSource src;
	uint64		old_gen;
	uint32		blank,
				world_base,
				old_total;

	tw_gen(&w, t, &sp);
	pm = tw_pm(&w);
	blank = SIFT_TOPK_COMPACT_GRANS - 2 - w.ngran;
	initStringInfo(&before);
	build_with_blank_part(&w, pm, blank, &before);
	tw_open(t, &before, &mb);
	old_total = mb.hdr.total_granules;
	TCHK(t, old_total == SIFT_TOPK_COMPACT_GRANS - 2 && !(mb.hdr.flags & SIFT_SMETA_TOPK_WIDE), "%u granules: the compact ring", old_total);

	world_base = w.ngran;
	tw_compact(&w, t, 1, sp.granule_rows, &swap, &added, &old_gen);
	TCHK(t, added.ngranules >= 3, "the new part has enough granules to pass 524,288: %u", added.ngranules);
	sh = (Shifted) {tw_source(&w), old_total, old_total - world_base, world_base, old_total};
	src = (SiftScopeGranSource) {&sh, sh_head, sh_cube, sh_tail, sh_rows, NULL};
	tw_spec(&w, &spec, 0);
	spec.meta_epoch = 2;
	w.cur_pm = pm;
	initStringInfo(&after);
	sift_scope_refresh_parts(&mb, &spec, pm, &swap, &src, &after, &st);
	tw_open(t, &after, &ma);
	TCHK(t, ma.hdr.total_granules == old_total + added.ngranules && (ma.hdr.flags & SIFT_SMETA_TOPK_WIDE), "a refresh that takes the object past 524,288 granules writes the wide ring: %llu", (unsigned long long) ma.hdr.total_granules);
	tw_check_meta(t, &w, &ma, pm, "past the switch");
	check_shifted_rings(t, &w, &ma, pm, world_base, old_total - world_base);
	for (uint32 k = 0; k < ma.hdr.nkeys; k++)
		*rings_converted += ma.roots[k].topk_count;
}

static void
refresh_wide(TCheck *t, TWorld *w, SiftScopePairMap *pm, const StringInfoData *wide_obj, uint32 *entries)
{
	SiftScopeMeta mw,
				mr;
	SiftScopeMemberKey *keep = palloc(sizeof(*keep) * w->npairs);
	SiftScopePairMap *pm2;
	SiftScopeDirty *dd;
	SiftScopeBuildSpec spec;
	Shifted		sh = {tw_source(w), UINT32_MAX, 0, w->ngran, UINT32_MAX};
	SiftScopeGranSource src = {&sh, sh_head, sh_cube, sh_tail, sh_rows, NULL};
	StringInfoData out;
	uint32		nd;

	tw_open(t, wide_obj, &mw);
	memcpy(keep, w->pairs + 1, sizeof(*keep) * (w->npairs - 1));
	w->pairs = keep;
	w->npairs--;
	pm2 = tw_pm(w);
	nd = sift_spm_diff(pm, pm2, &dd);
	TCHK(t, nd == 1, "one pair out, one key changed");
	tw_spec(w, &spec, 0);
	spec.meta_epoch = 2;
	w->cur_pm = pm2;
	initStringInfo(&out);
	sift_scope_refresh_dirty(&mw, &spec, pm2, dd, nd, &src, &out, NULL);
	tw_open(t, &out, &mr);
	TCHK(t, mr.hdr.total_granules == mw.hdr.total_granules && (mr.hdr.flags & SIFT_SMETA_TOPK_WIDE), "a refresh of a wide object keeps the wide ring");
	tw_check_meta(t, w, &mr, pm2, "wide refreshed");
	check_shifted_rings(t, w, &mr, pm2, UINT32_MAX, 0);
	for (uint32 k = 0; k < mr.hdr.nkeys; k++)
		*entries += mr.roots[k].topk_count;
}

void
scope_v6_scenario_format(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 80, .maxrows = 120, .long_pct = 10, .granule_rows = 64, .nparts = 1, .months = 10, .nkeys = 6, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj,
				compact,
				edge,
				wide;
	SiftScopeMeta m,
				mc,
				me,
				mw;
	SiftScopePairMap *tpm;
	uint32		seen,
				entries,
				refreshed = 0,
				converted = 0;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	w.cur_pm = tpm;
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	seen = section_tags(t, &obj);
	TCHK(t, seen == 0xFF, "the tags 0x0401..0x0408 are not each in the directory once: mask 0x%02x", seen);
	TCHK(t, SEC_GRANULE_TSTRIDE == 0x0110, "the stride section is 0x0110");
	TCHK(t, sizeof(SiftScopeMetaHeader) == 160 && sizeof(SiftScopeL2Root) == 104, "header and root sizes");
	TCHK(t, m.hdr.reserved[0] == 0 && m.hdr.reserved[1] == 0 && m.hdr.reserved[2] == 0, "the reserved bytes of the header are zero");
	for (uint32 k = 0; k < m.hdr.nkeys; k++)
		TCHK(t, m.roots[k].reserved0 == 0 && m.roots[k].reserved1[0] == 0 && m.roots[k].reserved1[1] == 0, "the reserved bytes of root %u are zero", k);
	TCHK(t, !(m.hdr.flags & SIFT_SMETA_TOPK_WIDE), "a small object has the compact ring");

	initStringInfo(&compact);
	initStringInfo(&edge);
	initStringInfo(&wide);
	build_sparse(&w, tpm, SIFT_TOPK_COMPACT_GRANS, &compact);
	build_sparse(&w, tpm, SIFT_TOPK_COMPACT_GRANS + 1, &edge);
	build_sparse(&w, tpm, SIFT_TOPK_COMPACT_GRANS + 100000, &wide);
	tw_open(t, &compact, &mc);
	tw_open(t, &edge, &me);
	tw_open(t, &wide, &mw);
	TCHK(t, mc.hdr.total_granules == SIFT_TOPK_COMPACT_GRANS && !(mc.hdr.flags & SIFT_SMETA_TOPK_WIDE), "524,288 granules keep the compact ring");
	TCHK(t, me.hdr.total_granules == SIFT_TOPK_COMPACT_GRANS + 1 && (me.hdr.flags & SIFT_SMETA_TOPK_WIDE), "524,289 granules switch to the wide ring");
	TCHK(t, (mw.hdr.flags & SIFT_SMETA_TOPK_WIDE) != 0, "more granules keep the wide ring");
	TCHK(t, sizeof(SiftScopeTopKEntryWide) == 16 && sizeof(SiftScopeTopKEntry) == 12, "ring entry sizes");
	entries = check_rings(t, &w, &mc, tpm, "compact");
	TCHK(t, check_rings(t, &w, &me, tpm, "wide edge") == entries && check_rings(t, &w, &mw, tpm, "wide") == entries, "the wide rings hold the entries of the compact ones");
	TCHK(t, entries > 0, "rings with entries");
	TCHK(t, mc.topk.len == (uint64) entries * 12 && me.topk.len == (uint64) entries * 16 && mw.topk.len == (uint64) entries * 16,
		 "a ring takes its entries only, 12 bytes each in the compact ring and 16 in the wide one: %llu, %llu and %llu bytes for %u", (unsigned long long) mc.topk.len, (unsigned long long) me.topk.len,
		 (unsigned long long) mw.topk.len, entries);
	TCHK(t, (m.hdr.flags & SIFT_SMETA_VAR_TOPK) && (me.hdr.flags & SIFT_SMETA_VAR_TOPK), "the header says the rings are variable-length");
	TCHK(t, topk_stored_ok(&m) && topk_stored_ok(&mc) && topk_stored_ok(&me) && topk_stored_ok(&mw), "the ring section of an object is stored raw or at least %d%% smaller", SIFT_SCOPE_TOPK_LZ4_MIN_PCT);
	TCHK(t, ((m.topk_raw != NULL) + (mc.topk_raw != NULL) + (me.topk_raw != NULL) + (mw.topk_raw != NULL) > 0) == sift_scope_inner_lz4(),
		 "a ring section is stored compressed only where no outer layer packs the object");
	TCHK(t, sift_scope_ring_nulls_last(&m, 0) && !sift_scope_ring_nulls_last(&m, 1) && sift_scope_ring_serves(&m, 0, false, false) &&
		 !sift_scope_ring_serves(&m, 0, true, false) && sift_scope_ring_serves(&m, 0, true, true) && !sift_scope_ring_serves(&m, 1, false, true),
		 "the time ring is DESC NULLS LAST: NULLS FIRST needs a leaf that is never NULL");
	tw_check_meta(t, &w, &me, tpm, "wide edge");
	refresh_wide(t, &w, tpm, &edge, &refreshed);
	refresh_across_the_switch(t, &converted);
	TCHK(t, refreshed > 0 && converted > 0, "rings checked after the refreshes");
	OUTL(t, "format sections=8 tags=0x0401..0x0408 header=160 root=104 ring_entry_compact=12 ring_entry_wide=16");
	OUTL(t, "format compact_at=%u wide_at=%u ring_entries_checked=%u", (uint32) SIFT_TOPK_COMPACT_GRANS, (uint32) SIFT_TOPK_COMPACT_GRANS + 1, entries);
	OUTL(t, "format refresh_keeps_wide_ring=%d refresh_past_the_switch_converts_rings=%d", refreshed > 0, converted > 0);
}

/* ---------------------------------------------------------------------------
 * cubewire: LZ4, rollups, distinct sets, extremes
 * ------------------------------------------------------------------------- */

static bool
wire_flag(const StringInfoData *b, uint16 flag)
{
	SiftScopeCubeHeader h;

	memcpy(&h, b->data, sizeof(h));
	return (h.flags & flag) != 0;
}

static void
patch_header(StringInfo b, void (*fn) (SiftScopeCubeHeader *h))
{
	SiftScopeCubeHeader h;

	memcpy(&h, b->data, sizeof(h));
	fn(&h);
	memcpy(b->data, &h, sizeof(h));
}

/* The cube as a build without zstd writes it: its payload as one LZ4 block */
static void
as_lz4(StringInfo out, const StringInfoData *cube)
{
	SiftScopeCubeHeader h;
	Size		cap;
	char	   *packed;

	memcpy(&h, cube->data, sizeof(h));
	cap = sift_colz_bound(SIFT_CC_ALG_LZ4, h.raw_len);
	packed = palloc(cap);
	h.comp_len = (uint32) sift_colz_compress(SIFT_CC_ALG_LZ4, 0, cube->data + sizeof(h), h.raw_len, packed, cap);
	h.flags |= SIFT_SCUBE_LZ4;
	initStringInfo(out);
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(out, packed, h.comp_len);
	pfree(packed);
}

static void unknown_flag(SiftScopeCubeHeader *h) { h->flags |= 0x80; }
static void lz4_flag(SiftScopeCubeHeader *h) { h->flags ^= SIFT_SCUBE_LZ4; }
static void huge_raw(SiftScopeCubeHeader *h) { h->raw_len = UINT32_MAX; }
static void short_raw(SiftScopeCubeHeader *h) { h->raw_len = h->comp_len; }
static void zero_comp(SiftScopeCubeHeader *h) { h->comp_len = 0; }
static void drop_rollups(SiftScopeCubeHeader *h) { h->nrollups = 0; }
static void many_dims(SiftScopeCubeHeader *h) { h->ndims = SIFT_SCOPE_DIMS_CAP + 1; }

typedef void (*HeaderFn) (SiftScopeCubeHeader *h);

static bool
refused(const StringInfoData *good, HeaderFn fn)
{
	StringInfoData b;
	SiftScopeCubeAcc a;
	bool		ok;

	initStringInfo(&b);
	appendBinaryStringInfo(&b, good->data, good->len);
	patch_header(&b, fn);
	ok = sift_scube_read(b.data, b.len, &a);
	if (ok)
		sift_scube_free(&a);
	pfree(b.data);
	return !ok;
}

static uint32
canon(const TWorld *w, const TGran *g, uint32 i)
{
	return w->dmap[g->part][g->dim0[i]];
}

typedef struct Brute
{
	uint32		ncanon;
	uint32		canon[TW_NSTR];		/* the canonical codes of dim 0 that the rows hold */
	uint64		cell[TW_NSTR][12];
	uint64		by_dim1[12];
	int64		min1[12];
	int64		max1[12];
	uint64		nn1[12];
	bool		seen[300];
	uint32		distinct;
	uint32		cells;
} Brute;

static void
brute_of(const TWorld *w, Brute *b)
{
	memset(b, 0, sizeof(*b));
	for (uint32 gi = 0; gi < w->ngran; gi++)
	{
		const TGran *g = &w->g[gi];

		for (uint32 i = 0; i < g->n; i++)
		{
			uint32		code = canon(w, g, i),
						c = 0;
			int64		d1 = g->dim1[i];

			while (c < b->ncanon && b->canon[c] != code)
				c++;
			if (c == b->ncanon)
				b->canon[b->ncanon++] = code;
			b->cells += b->cell[c][d1] == 0;
			b->cell[c][d1]++;
			b->by_dim1[d1]++;
			if (!g->m0null[i])
			{
				b->min1[d1] = b->nn1[d1] == 0 ? g->m0[i] : Min(b->min1[d1], g->m0[i]);
				b->max1[d1] = b->nn1[d1] == 0 ? g->m0[i] : Max(b->max1[d1], g->m0[i]);
				b->nn1[d1]++;
			}
			if (!g->dset0null[i] && !b->seen[g->dset0[i]])
			{
				b->seen[g->dset0[i]] = true;
				b->distinct++;
			}
		}
	}
}

static void
check_rich(TCheck *t, const TWorld *w, const SiftScopeCubeAcc *c, const char *label)
{
	Brute		b;
	uint32		cells1 = 0;

	brute_of(w, &b);
	TCHK(t, sift_scube_distinct(c, 0) == b.distinct, "%s: %u distinct ids, truth %u", label, sift_scube_distinct(c, 0), b.distinct);
	TCHK(t, sift_scube_rollup_cells(c, 0) == b.cells, "%s: rollup 0 has %u cells, truth %u", label, sift_scube_rollup_cells(c, 0), b.cells);
	for (uint32 d = 0; d < 12; d++)
	{
		cells1 += b.by_dim1[d] > 0;
		TCHK(t, sift_scube_rollup_rows(c, 1, (int64[]) {(int64) d}) == b.by_dim1[d], "%s: rollup 1 cell %u", label, d);
		for (uint32 s = 0; s < b.ncanon; s++)
			TCHK(t, sift_scube_rollup_rows(c, 0, (int64[]) {(int64) b.canon[s], (int64) d}) == b.cell[s][d], "%s: rollup 0 cell (%u, %u)", label, s, d);
	}
	TCHK(t, sift_scube_rollup_rows(c, 0, (int64[]) {INT64CONST(1) << 40, 0}) == 0, "%s: a cell that no row has", label);
	TCHK(t, sift_scube_rollup_cells(c, 1) == cells1, "%s: rollup 1 has %u cells, truth %u", label, sift_scube_rollup_cells(c, 1), cells1);
	for (uint32 g = 0; g < c->dim[1].ngroups; g++)
	{
		char	   *rec = sift_dim_rec(&c->shape, &c->dim[1], g);
		int64		d1 = *sift_rec_key(rec);
		const SiftScopeGroupM *m = sift_rec_m(rec, 1);

		TCHK(t, m[0].nn == b.nn1[d1] && (b.nn1[d1] == 0 || (m[0].min == b.min1[d1] && m[0].max == b.max1[d1])), "%s: group %lld extremes", label, (long long) d1);
	}
}

void
scope_v6_scenario_cubewire(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 70, .maxrows = 90, .long_pct = 10, .granule_rows = 64, .nparts = 2, .months = 6, .nkeys = 4, .keys_per_member = 2, .rich = true};
	SiftScopeBuildSpec spec;
	StringInfoData obj,
				big,
				lz4big,
				small,
				tiny;
	SiftScopeCubeAcc all,
				back,
				one,
				rest,
				first;
	SiftScopePairMap *tpm;
	SiftScopeCubeHeader h;
	SiftScopeDictMap map;
	SiftScopeMeta mobj;
	uint32		mask,
				refused_prefixes = 0,
				refused_flips = 0;

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	w.cur_pm = tpm;
	tw_open(t, &obj, &mobj);
	tw_check_meta(t, &w, &mobj, tpm, "rich object");
	TCHK(t, tw_truth_cube(&w, tpm, NULL, SIFT_SCOPE_NO_MONTH, false, &all), "the table has rows");
	check_rich(t, &w, &all, "built");

	initStringInfo(&big);
	sift_scube_write(&big, &all);
	memcpy(&h, big.data, sizeof(h));
	TCHK(t, h.raw_len + sizeof(h) >= SIFT_SCUBE_LZ4_MIN, "the cube is larger than the compression threshold: %u", h.raw_len);
	TCHK(t, wire_flag(&big, SIFT_SCUBE_LZ4) == sift_scope_inner_lz4(), "a cube of %u bytes is compressed only where no outer layer packs the object", h.raw_len);
	TCHK(t, !wire_flag(&big, SIFT_SCUBE_LZ4) || h.comp_len < h.raw_len, "the compressed form is smaller");
	TCHK(t, wire_flag(&big, SIFT_SCUBE_HAS_DSETS) && wire_flag(&big, SIFT_SCUBE_HAS_ROLLUP) && wire_flag(&big, SIFT_SCUBE_MINMAX), "the flags name the dsets, rollups and extremes");
	TCHK(t, sift_scube_read(big.data, big.len, &back) && sift_scube_equal(&back, &all), "a compressed cube reads back as it was");
	check_rich(t, &w, &back, "read back");
	sift_scube_free(&back);

	initStringInfo(&small);
	initStringInfo(&tiny);
	for (int rich = 0; rich < 2; rich++)
	{
		SiftScopeRows r;
		SiftScopeDictMap m1 = {{w.dmap[w.g[0].part]}, {TW_NSTR}};
		SiftScopeShape plain = {.nmeas = 2, .ndims = 2, .float_meas = 2};
		StringInfo	to = rich ? &small : &tiny;

		memset(&r, 0, sizeof(r));
		r.n = 1;
		r.member = w.g[0].member;
		r.member_null = w.g[0].mnull;
		r.time = w.g[0].time;
		r.time_null = w.g[0].tnull;
		r.dim[0] = w.g[0].dim0;
		r.dim[1] = w.g[0].dim1;
		r.meas[0] = w.g[0].m0;
		r.meas_null[0] = w.g[0].m0null;
		r.meas[1] = w.g[0].m1;
		r.dset[0] = w.g[0].dset0;
		r.dset_null[0] = w.g[0].dset0null;
		sift_scube_init(&one, rich ? w.shape : plain);
		sift_scube_add_row(&one, &r, 0, &m1);
		sift_scube_write(to, &one);
		TCHK(t, sift_scube_read(to->data, to->len, &back) && sift_scube_equal(&back, &one), "a one-row cube reads back as it was");
		sift_scube_free(&back);
		sift_scube_free(&one);
	}
	memcpy(&h, small.data, sizeof(h));
	TCHK(t, tiny.len < SIFT_SCUBE_LZ4_MIN && !wire_flag(&tiny, SIFT_SCUBE_LZ4), "a cube under 256 bytes is stored as it is: %d bytes", tiny.len);
	TCHK(t, !wire_flag(&small, SIFT_SCUBE_LZ4) || (h.raw_len + sizeof(h) >= SIFT_SCUBE_LZ4_MIN && h.comp_len < h.raw_len),
		 "a cube is compressed only from 256 bytes and only when that is smaller: %u bytes, %d stored", h.raw_len, small.len);

	TCHK(t, refused(&tiny, unknown_flag) && refused(&big, unknown_flag), "an unknown flag is refused");
	TCHK(t, refused(&tiny, lz4_flag), "the compressed flag on a cube that is not is refused");
	TCHK(t, refused(&big, huge_raw) && refused(&tiny, huge_raw), "a length that no bytes could fill is refused");
	as_lz4(&lz4big, &big);
	TCHK(t, lz4big.len < big.len && sift_scube_read(lz4big.data, lz4big.len, &back) && sift_scube_equal(&back, &all), "the LZ4 form of a cube reads back as it was");
	sift_scube_free(&back);
	TCHK(t, refused(&lz4big, short_raw) && refused(&lz4big, zero_comp) && refused(&big, zero_comp), "lengths that do not fit each other are refused");
	TCHK(t, refused(&big, drop_rollups) && refused(&big, many_dims), "a header that disagrees with its flags or its caps is refused");
	for (Size n = 0; n < (Size) big.len; n++)
	{
		SiftScopeCubeAcc a;

		if (sift_scube_read(big.data, (uint32) n, &a))
			sift_scube_free(&a);
		else
			refused_prefixes++;
	}
	TCHK(t, refused_prefixes == (uint32) big.len, "every prefix of a cube is refused: %u of %d", refused_prefixes, big.len);
	for (Size at = sizeof(h); at < (Size) big.len; at += 7)
		for (int bit = 0; bit < 8; bit++)
		{
			SiftScopeCubeAcc a;

			big.data[at] ^= (char) (1 << bit);
			if (sift_scube_read(big.data, big.len, &a))
				sift_scube_free(&a);
			else
				refused_flips++;
			big.data[at] ^= (char) (1 << bit);
		}
	TCHK(t, refused_flips > 0, "a flipped bit of the body is caught");

	sift_scube_init(&first, w.shape);
	sift_scube_init(&rest, w.shape);
	for (uint32 gi = 0; gi < w.ngran; gi++)
	{
		map = (SiftScopeDictMap) {{w.dmap[w.g[gi].part]}, {TW_NSTR}};
		sift_scube_merge(gi == 0 ? &first : &rest, &w.g[gi].cube, &map);
	}
	mask = sift_scube_sub(&all, &first);
	TCHK(t, (mask & SIFT_SUB_DSETS) != 0, "taking rows out of a cube with a distinct set marks the set as unsure");
	TCHK(t, sift_scube_same(&all, &rest, SIFT_SAME_TIME | SIFT_SAME_EXTREMES | SIFT_SAME_DSETS), "the cube without a granule is the cube of the others");
	TCHK(t, sift_scube_distinct(&all, 0) >= sift_scube_distinct(&rest, 0), "a subtracted set never holds fewer ids than the exact one");
	sift_scube_free(&all);
	sift_scube_free(&first);
	sift_scube_free(&rest);
	OUTL(t, "cubewire lz4_smaller=%d small_stored_as_is=%d prefixes_refused=%u of=%d", wire_flag(&big, SIFT_SCUBE_LZ4), !wire_flag(&tiny, SIFT_SCUBE_LZ4), refused_prefixes, big.len);
	OUTL(t, "cubewire rollups=2 distinct_sets=1 group_extremes=1 sub_marks_distinct=%d", (mask & SIFT_SUB_DSETS) != 0);
}

/* ---------------------------------------------------------------------------
 * heap: the node to evict is the one with the most open bytes
 * ------------------------------------------------------------------------- */

static uint64
heap_ops(TCheck *t, uint32 n, uint32 ops)
{
	Size	   *size = palloc0(sizeof(Size) * n);
	bool	   *in = palloc0(n);
	SiftMaxHeap h;
	uint32		members = 0;
	Size		last = SIZE_MAX;

	sift_mheap_init(&h, n, size);
	for (uint32 i = 0; i < ops; i++)
	{
		uint32		k = TRAND(t, n);
		uint32		op = TRAND(t, 100);

		if (op < 65)
		{
			size[k] = 1 + TRAND(t, 1000000);
			members += !in[k];
			in[k] = true;
			sift_mheap_set(&h, k);
		}
		else if (op < 90)
		{
			members -= in[k];
			in[k] = false;
			sift_mheap_remove(&h, k);
		}
		else if (in[k])
		{
			size[k] = TRAND(t, 3) == 0 ? 0 : size[k] / 2;
			sift_mheap_set(&h, k);
		}
		if (n <= 2000 || i % 97 == 0)
		{
			Size		best = 0;
			uint32		top = sift_mheap_top(&h);

			for (uint32 j = 0; j < n; j++)
				if (in[j])
					best = Max(best, size[j]);
			TCHK(t, h.n == members && (members == 0 ? top == UINT32_MAX : (top < n && in[top] && size[top] == best)), "heap op %u: top %u of %u members", i, top, members);
		}
	}
	while (h.n > 0)
	{
		uint32		top = sift_mheap_top(&h);

		TCHK(t, size[top] <= last, "the heap hands out keys in descending size");
		last = size[top];
		sift_mheap_remove(&h, top);
	}
	sift_mheap_free(&h);
	return h.cmps;
}

void
scope_v6_scenario_heap(TCheck *t)
{
	uint64		small = heap_ops(t, 1000, 20000),
				large = heap_ops(t, 64000, 20000);

	TCHK(t, small < 20000 * 4 * 10 && large < 20000 * 4 * 16, "comparisons an operation: %llu and %llu over 20000 operations", (unsigned long long) small, (unsigned long long) large);
	TCHK(t, large < small * 16, "64 times the keys costs a few times the comparisons, not 64 times: %llu against %llu", (unsigned long long) large, (unsigned long long) small);
	OUTL(t, "heap top_is_largest=1 descending_drain=1 cost_grows_with_log_of_keys=%d", large < small * 16);
}

#endif							/* SIFT_TESTING */
