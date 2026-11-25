/*
 * col/scope_v6/test/rings.c - the scenarios of sift._test_scope_v6 (col/scope_v6/test/core.c) for the rings of several order leaves beside the time leaf, and the rows a deletion
 * vector and a sparse IS NULL predicate take out together. Each is compared with a brute force over the rows, for several seeds.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/test/world.h"

extern void scope_v6_scenario_rings(TCheck *t);
extern void scope_v6_scenario_union(TCheck *t);

/* ---------------------------------------------------------------------------
 * rings: the order leaves beside the time leaf
 * ------------------------------------------------------------------------- */

static const TWorldSpec spec_rings = {.nmembers = 60, .maxrows = 700, .long_pct = 25, .granule_rows = 128, .nparts = 2, .months = 10, .nkeys = 6,
	.keys_per_member = 3, .nrings = 3};

typedef struct RingAcc
{
	int64		queries;
	int64		ring_only;
	int64		spilled;
	int64		ties;
	int64		tie_reads;
	int64		cap_from_ring;
	int64		scans;
	int64		granules;
	int64		slots;
} RingAcc;

static uint32
truth_ring(const TWorld *w, SiftScopePairMap *pm, int64 key, uint32 ring, uint32 n, SiftScopeNewest *out)
{
	return ring == 0 ? tw_truth_newest(w, pm, key, n, out) : tw_truth_ring(w, pm, key, ring, n, out);
}

static void
rings_check(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label, bool exact_slots, RingAcc *a)
{
	SiftScopeEntity e = tw_entity(w);
	static const uint32 counts[] = {7, 20, 63, 64, 100, 300};

	w->cur_pm = pm;
	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);
		SiftScopeNewest truth[320],
					got[320],
					slot[SIFT_SCOPE_TOPK_CAP],
					*ties;

		if (r == NULL)
			continue;
		for (uint32 ring = 0; ring < m->ntopk_rings; ring++)
		{
			uint32		nr = sift_scope_topk_get_ring(m, r, ring, slot),
						nt;

			w->filter = false;
			nt = truth_ring(w, pm, key, ring, SIFT_SCOPE_TOPK_CAP, truth);
			if (exact_slots)
			{
				TCHK(t, nr <= nt && memcmp(slot, truth, sizeof(*slot) * nr) == 0, "%s: key %lld ring %u: %u entries, truth %u", label, (long long) key, ring, nr, nt);
				TCHK(t, nr == nt || sift_scope_ring_partial(r, ring), "%s: key %lld ring %u of %u entries is short of %u and not partial", label, (long long) key, ring, nr, nt);
			}
			a->slots += nr > 0;
			TCHK(t, sift_scope_ring_nulls_last(m, ring) && sift_scope_ring_serves(m, ring, false, false) && !sift_scope_ring_serves(m, ring, true, false) && sift_scope_ring_serves(m, ring, true, true) &&
				 !sift_scope_ring_serves(m, m->ntopk_rings, false, true), "%s: key %lld ring %u is DESC NULLS LAST", label, (long long) key, ring);
			{
				SiftScopeNewest none[1];
				uint32		from_none;
				bool		tie_none = true;

				TCHK(t, sift_scope_latest_ring(m, r, ring, 0, false, &e, none, &from_none, &tie_none) == 0 && from_none == 0 && !tie_none, "%s: key %lld ring %u: no rows asked, none returned", label, (long long) key, ring);
			}
			for (int filtered = 0; filtered < 2; filtered++)
				for (uint32 c = 0; c < lengthof(counts); c++)
				{
					uint32		n = counts[c],
								from_ring,
								ng,
								nt1;
					bool		tie;

					w->filter = filtered;
					nt1 = truth_ring(w, pm, key, ring, n + 1, truth);
					w->ring_scans = 0;
					ng = sift_scope_latest_ring(m, r, ring, n, filtered, &e, got, &from_ring, &tie);
					TCHK(t, ng == Min(nt1, n) && memcmp(got, truth, sizeof(*got) * ng) == 0, "%s: key %lld ring %u latest %u (filtered %d): %u rows, truth %u", label, (long long) key, ring, n, filtered, ng, Min(nt1, n));
					TCHK(t, tie == (nt1 > n && truth[n].time == truth[n - 1].time), "%s: key %lld ring %u latest %u: tie at the cut %d", label, (long long) key, ring, n, tie);
					a->queries++;
					a->ties += tie;
					if (exact_slots && !filtered && n == SIFT_SCOPE_TOPK_CAP && nr == SIFT_SCOPE_TOPK_CAP)
					{
						TCHK(t, from_ring == ng, "%s: key %lld ring %u latest %u comes from the full ring: %u of %u", label, (long long) key, ring, n, from_ring, ng);
						a->cap_from_ring++;
					}
					if (ring > 0)
					{
						a->ring_only += !filtered && n <= SIFT_SCOPE_TOPK_CAP && from_ring == ng;
						a->spilled += from_ring < ng;
						if (filtered || from_ring < ng)
						{
							SiftScopeGranRef *refs;
							uint32		nrefs = sift_scope_key_refs(m, r, NULL);

							refs = palloc(sizeof(*refs) * Max(nrefs, 1));
							sift_scope_key_refs(m, r, refs);
							a->scans += w->ring_scans;
							a->granules += nrefs;
							TCHK(t, w->ring_scans <= nrefs, "%s: key %lld ring %u scanned %llu granules of %u", label, (long long) key, ring, (unsigned long long) w->ring_scans, nrefs);
							pfree(refs);
						}
					}
					if (tie)
					{
						SiftScopeNewest *all = palloc(sizeof(*all) * 60000);
						uint32		nall = truth_ring(w, pm, key, ring, 60000, all),
									want = 0,
									nties = sift_scope_ring_ties(m, r, ring, got[ng - 1].time, &e, &ties);

						for (uint32 i = 0; i < nall; i++)
							if (all[i].time == got[ng - 1].time)
								all[want++] = all[i];
						qsort(ties, nties, sizeof(*ties), newest_desc);
						TCHK(t, nties == want && memcmp(ties, all, sizeof(*ties) * want) == 0, "%s: key %lld ring %u: %u rows of the value at the cut, truth %u", label, (long long) key, ring, nties, want);
						a->tie_reads++;
						pfree(all);
						pfree(ties);
					}
				}
		}
	}
	w->filter = false;
}

static void
rings_compact(TCheck *t, TWorld *w, SiftScopePairMap *pm, const SiftScopeMeta *m, uint32 part, uint32 new_rows, uint64 epoch, StringInfo obj, RingAcc *a)
{
	SiftScopeBuildSpec spec;
	SiftScopePartSwap swap;
	SiftScopeNewPart added;
	SiftScopeGranSource src;
	SiftScopeBuildStats st;
	SiftScopeMeta mn;
	uint64		old_gen;

	tw_kill_part_rows(w, t, part, 250);
	tw_compact(w, t, part, new_rows, &swap, &added, &old_gen);
	tw_spec(w, &spec, 0);
	spec.meta_epoch = epoch;
	src = tw_source(w);
	w->cur_pm = pm;
	resetStringInfo(obj);
	sift_scope_refresh_parts(m, &spec, pm, &swap, &src, obj, &st);
	tw_open(t, obj, &mn);
	tw_check_meta(t, w, &mn, pm, "rings-compacted");
	TCHK(t, mn.ntopk_rings == m->ntopk_rings && memcmp(&mn.hdr.reserved[0], &m->hdr.reserved[0], 8) == 0, "a refresh keeps the description of the rings");
	rings_check(t, w, &mn, pm, "rings-compacted", true, a);
}

static void
rings_damage(TCheck *t, const StringInfoData *obj, int64 *refused)
{
	SiftScopeTrailer tr;
	uint64		hdr_at = 0;
	static const struct
	{
		uint32		what;
		uint8		a;
		uint8		b;
		int16		leaf;
	} cases[] = {
		{0, 0, 0, 0}, {0, 1, 0, 0}, {0, 5, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, -1}, {4, 0, 0, -1}, {5, 0, 0, 0}
	};

	memcpy(&tr, obj->data + obj->len - sizeof(tr), sizeof(tr));
	for (uint32 i = 0; i < tr.nsec; i++)
	{
		SiftScopeSecEntry e;

		memcpy(&e, obj->data + tr.dir_off + (Size) i * sizeof(e), sizeof(e));
		if (e.tag == SEC_SCOPE_HDR)
			hdr_at = e.off;
	}
	for (uint32 c = 0; c < lengthof(cases); c++)
	{
		char	   *copy = memcpy(palloc(obj->len), obj->data, obj->len);
		SiftScopeMetaHeader h;
		SiftScopeTopKDesc d;
		SiftScopeMeta m;

		memcpy(&h, copy + hdr_at, sizeof(h));
		memcpy(&d, &h.reserved[0], sizeof(d));
		switch (cases[c].what)
		{
			case 0:
				d.ntopk_rings = cases[c].a;
				break;
			case 1:
				d.nulls_last_mask = 0;
				break;
			case 2:
				h.flags &= ~SIFT_SMETA_MULTI_TOPK;
				break;
			case 3:
				d.extra_leaf[0] = cases[c].leaf;
				break;
			case 4:
				d.extra_leaf[2] = cases[c].leaf;
				break;
			case 5:
				h.time_leaf = -1;
				break;
		}
		memcpy(&h.reserved[0], &d, sizeof(d));
		memcpy(copy + hdr_at, &h, sizeof(h));
		*refused += !sift_scope_meta_open(&m, copy, obj->len);
		TCHK(t, !sift_scope_meta_open(&m, copy, obj->len), "a damaged description of the rings (case %u) opens", c);
		pfree(copy);
	}
}

typedef struct PickCase
{
	SiftScopeRingCand cand[6];
	uint32		n;
	uint32		rings;
	int16		want[3];
} PickCase;

static void
rings_pick_cases(TCheck *t)
{
	static const PickCase cases[] = {
		{{{2, SIFT_RING_KIND_TIMESTAMP, 1000, 0}, {3, SIFT_RING_KIND_ORDINAL, 1000, 0}}, 2, 3, {2, 3, -1}},
		{{{2, SIFT_RING_KIND_ORDINAL, 1000, 0}, {3, SIFT_RING_KIND_DATE, 1000, 0}, {4, SIFT_RING_KIND_TIMESTAMP, 1000, 100}}, 3, 4, {4, 3, 2}},
		{{{2, SIFT_RING_KIND_TIMESTAMP, 1000, 999}, {3, SIFT_RING_KIND_TIMESTAMP, 1000, 501}}, 2, 1, {-1, -1, -1}},
		{{{2, SIFT_RING_KIND_TIMESTAMP, 1000, 500}, {3, SIFT_RING_KIND_TIMESTAMP, 1000, 100}}, 2, 3, {3, 2, -1}},
		{{{0, SIFT_RING_KIND_TIMESTAMP, 1000, 0}, {1, SIFT_RING_KIND_TIMESTAMP, 1000, 0}, {2, 9, 1000, 0}, {3, SIFT_RING_KIND_ORDINAL, 0, 0}}, 4, 1, {-1, -1, -1}},
		{{{2, SIFT_RING_KIND_ORDINAL, 10, 0}, {3, SIFT_RING_KIND_ORDINAL, 10, 0}, {4, SIFT_RING_KIND_ORDINAL, 10, 0}, {5, SIFT_RING_KIND_ORDINAL, 10, 0}, {6, SIFT_RING_KIND_ORDINAL, 10, 0}}, 5, 4, {2, 3, 4}},
		{{{2, SIFT_RING_KIND_TIMESTAMP, UINT64CONST(1) << 40, UINT64CONST(1) << 38}, {3, SIFT_RING_KIND_TIMESTAMP, UINT64CONST(1) << 40, 0}}, 2, 3, {3, 2, -1}},
	};

	for (uint32 c = 0; c < lengthof(cases); c++)
	{
		SiftScopeBuildSpec spec;
		uint32		rings;

		memset(&spec, 0, sizeof(spec));
		spec.member_leaf = 0;
		spec.time_leaf = 1;
		rings = sift_scope_rings_pick(&spec, cases[c].cand, cases[c].n);
		TCHK(t, rings == cases[c].rings && spec.ntopk_rings == rings, "rings pick case %u: %u rings, want %u", c, rings, cases[c].rings);
		for (uint32 q = 0; q < 3; q++)
			TCHK(t, spec.extra_leaf[q] == cases[c].want[q], "rings pick case %u: extra leaf %u is %d, want %d", c, q, spec.extra_leaf[q], cases[c].want[q]);
	}
}

/* SEC_SCOPE_TOPK holds the set entries of the rings of each key and no zero slot: the blocks follow one another and the bytes are the entries' */
static uint64
var_topk_check(TCheck *t, const SiftScopeMeta *m, uint32 keys_with_ring)
{
	uint64		entries = 0,
				at = 0;
	uint32		esize = (m->hdr.flags & SIFT_SMETA_TOPK_WIDE) ? 16 : 12;

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *r = &m->roots[k];

		if (!(r->flags & SIFT_L2_HAS_TOPK))
			continue;
		TCHK(t, r->topk_off == at, "key %u: its rings start at %llu, the blocks before it end at %llu", k, (unsigned long long) r->topk_off, (unsigned long long) at);
		for (uint32 ring = 0; ring < m->ntopk_rings; ring++)
			entries += sift_ring_count(r, ring);
		at = r->topk_off + sift_rings_bytes(r, m->ntopk_rings, (m->hdr.flags & SIFT_SMETA_TOPK_WIDE) != 0);
	}
	TCHK(t, (m->hdr.flags & SIFT_SMETA_VAR_TOPK) && m->topk.len == entries * esize && at == m->topk.len,
		 "the rings of %u keys take their %llu entries only: %llu bytes", keys_with_ring, (unsigned long long) entries, (unsigned long long) m->topk.len);
	TCHK(t, m->topk.len <= (uint64) keys_with_ring * m->ntopk_rings * SIFT_SCOPE_TOPK_CAP * esize, "the rings are no larger than the slots were");
	return entries;
}

static void
rings_round(TCheck *t, RingAcc *a, int64 *stats, int64 *budget_equal, int64 *refused)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData obj,
				cic,
				small,
				after;
	SiftScopeBuildStats st,
				ss;
	SiftScopeMeta m,
				ms,
				mc;
	SiftScopePairMap *tpm;
	SiftScopeTopKDesc d;
	uint32		with_ring = 0;

	tw_gen(&w, t, &spec_rings);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	initStringInfo(&cic);
	initStringInfo(&small);
	initStringInfo(&after);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, &st);
	tw_open(t, &obj, &m);
	memcpy(&d, &m.hdr.reserved[0], sizeof(d));
	TCHK(t, (m.hdr.flags & SIFT_SMETA_MULTI_TOPK) && m.ntopk_rings == 4 && d.ntopk_rings == 4 && d.nulls_last_mask == 0x0F &&
		 d.extra_leaf[0] == 2 && d.extra_leaf[1] == 3 && d.extra_leaf[2] == 4, "the header describes four rings");
	TCHK(t, sizeof(SiftScopeMetaHeader) == 160 && sizeof(SiftScopeL2Root) == 104 && sizeof(SiftScopeTopKDesc) == 8 && sizeof(SiftScopeL2ExtraTopK) == 8, "the structs keep their sizes");
	tw_check_meta(t, &w, &m, tpm, "rings");
	for (uint32 k = 0; k < m.hdr.nkeys; k++)
		with_ring += (m.roots[k].flags & SIFT_L2_HAS_TOPK) != 0;
	var_topk_check(t, &m, with_ring);
	TCHK(t, st.tail_reads < st.pure * 4, "the leaf of a ring is read for a granule that can enter it only: %llu reads, %llu pure granules", (unsigned long long) st.tail_reads, (unsigned long long) st.pure);
	stats[0] += st.tail_reads;
	stats[1] += st.pure * 4;

	tw_build(&w, &spec, tw_pm(&w), true, &cic, NULL);
	tw_open(t, &cic, &mc);
	TCHK(t, mc.topk.len == m.topk.len && memcmp(mc.topk.p, m.topk.p, m.topk.len) == 0, "plain and concurrent builds write the same rings");
	spec.mem_budget = 200 * 1024;
	tw_build(&w, &spec, tw_pm(&w), false, &small, &ss);
	tw_open(t, &small, &ms);
	TCHK(t, ss.evictions > 0 && ms.topk.len == m.topk.len && memcmp(ms.topk.p, m.topk.p, m.topk.len) == 0, "evictions (%llu) change no ring", (unsigned long long) ss.evictions);
	*budget_equal += ss.evictions > 0 && ms.topk.len == m.topk.len && memcmp(ms.topk.p, m.topk.p, m.topk.len) == 0;

	rings_check(t, &w, &m, tpm, "rings-fresh", true, a);
	tw_kill_rows(&w, t, 150);
	rings_check(t, &w, &m, tpm, "rings-15pct-dead", false, a);
	tw_kill_rows(&w, t, 400);
	rings_check(t, &w, &m, tpm, "rings-50pct-dead", false, a);
	rings_damage(t, &obj, refused);

	{
		TWorld		w2;
		SiftScopeMeta m2;
		StringInfoData o2;

		tw_gen(&w2, t, &spec_rings);
		tw_spec(&w2, &spec, 0);
		initStringInfo(&o2);
		tw_build(&w2, &spec, tw_pm(&w2), false, &o2, NULL);
		tw_open(t, &o2, &m2);
		rings_compact(t, &w2, tw_pm(&w2), &m2, 1, 48, 2, &after, a);
		tw_open(t, &after, &m2);
		rings_compact(t, &w2, tw_pm(&w2), &m2, 0, 96, 3, &o2, a);
	}
}

typedef struct ZipfBytes
{
	uint64		topk_raw;
	uint64		topk_stored;
	uint64		ring_slots;
	uint32		mm_omitted;
	uint32		mm_stored;
	uint64		mm_saved;
	uint64		mm_bytes;
} ZipfBytes;

static const TWorldSpec spec_zipf = {.nmembers = 900, .maxrows = 9, .long_pct = 0, .granule_rows = 128, .nparts = 2, .months = 6, .nkeys = 300,
	.keys_per_member = 2, .nrings = 3};

/*
 * A key of one granule stores no month map: its month cells answer as the map would (newest month first, the granule 0 in each month), and a key of
 * several granules has the map. The bytes the omitted maps would have taken are written for real and counted.
 */
static void
monthmap_check(TCheck *t, const SiftScopeMeta *m, ZipfBytes *z)
{
	SiftScopeMonthBucket bk[64];
	uint32		ords[2];

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *r = &m->roots[k];
		bool		one = r->l1_count == 1 && m->nodes[r->l1_first].nrefs == 1;
		uint32		nb = sift_scope_monthmap_buckets(m, r, bk, lengthof(bk));

		if (!(r->flags & SIFT_L2_HAS_MONTHS))
		{
			TCHK(t, !(r->flags & SIFT_L2_HAS_MONTHMAP) && nb == 0 && sift_scope_monthmap_ngranules(m, r) == 0, "key %u has no months and no map", k);
			continue;
		}
		TCHK(t, !!(r->flags & SIFT_L2_HAS_MONTHMAP) == !one, "key %u: the map is stored for several granules only (%u refs in %u nodes)", k, one, r->l1_count);
		TCHK(t, nb == sift_scope_monthmap_nmonths(m, r) && nb > 0 && sift_scope_monthmap_ngranules(m, r) >= 1, "key %u: %u months", k, nb);
		for (uint32 i = 1; i < nb; i++)
			TCHK(t, bk[i - 1].month_id > bk[i].month_id, "key %u: the months run newest first", k);
		if (!one)
		{
			z->mm_stored++;
			z->mm_bytes += MAXALIGN(sizeof(SiftScopeMonthMapHeader) + nb * sizeof(SiftScopeMonthBucket));
			continue;
		}
		TCHK(t, nb == r->nmonths && sift_scope_monthmap_ngranules(m, r) == 1, "key %u: its month cells are its months", k);
		for (uint32 i = 0; i < nb; i++)
			TCHK(t, sift_scope_monthmap_granules(m, r, bk[i].month_id, ords, lengthof(ords)) == 1 && ords[0] == 0, "key %u: granule 0 holds month %d", k, bk[i].month_id);
		TCHK(t, sift_scope_monthmap_granules(m, r, bk[0].month_id + 1, ords, lengthof(ords)) == 0 && sift_scope_monthmap_granules(m, r, bk[nb - 1].month_id - 1, ords, lengthof(ords)) == 0,
			 "key %u: no granule holds a month it has no cell for", k);
		{
			SiftMonthMapAcc acc = {0};
			StringInfoData as_stored;

			for (uint32 i = 0; i < nb; i++)
				sift_mmap_add(&acc, bk[i].month_id, 0);
			initStringInfo(&as_stored);
			sift_mmap_write(&acc, 1, NULL, &as_stored);
			z->mm_saved += as_stored.len;
			z->mm_omitted++;
			sift_mmap_free(&acc);
			pfree(as_stored.data);
		}
	}
	TCHK(t, z->mm_omitted > 0 && z->mm_stored > 0, "keys of one granule and of several: %u and %u", z->mm_omitted, z->mm_stored);
	TCHK(t, m->monthmap.len >= z->mm_bytes, "the stored maps take at least their headers and buckets");
}

static SiftScopeSecEntry *
section_entry(char *obj, uint64 len, uint16 tag)
{
	SiftScopeTrailer tr;
	SiftScopeSecEntry *dir;

	memcpy(&tr, obj + len - sizeof(tr), sizeof(tr));
	dir = (SiftScopeSecEntry *) (obj + tr.dir_off);
	for (uint32 i = 0; i < tr.nsec; i++)
		if (dir[i].tag == tag)
			return &dir[i];
	return NULL;
}

/* Reads every ring of an object that opened: a damaged section must not take a read out of its bounds */
static void
read_all_rings(const SiftScopeMeta *m)
{
	SiftScopeNewest slot[SIFT_SCOPE_TOPK_CAP];

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
		for (uint32 ring = 0; ring < m->ntopk_rings; ring++)
			(void) sift_scope_topk_get_ring(m, &m->roots[k], ring, slot);
}

/* The ring section stored LZ4-compressed: a damaged length, a flag where it does not belong, damaged bytes */
static int64
topk_lz4_damage(TCheck *t, const StringInfoData *obj)
{
	SiftScopeMeta m;
	SiftScopeSecEntry *e;
	char	   *copy = memcpy(palloc(obj->len), obj->data, obj->len);
	uint32		raw_len,
				bad[4];
	int64		refused = 0;
	uint64		clen;

	e = section_entry(copy, obj->len, SEC_SCOPE_TOPK);
	TCHK(t, e != NULL && ((e->flags & SIFT_SEC_LZ4) != 0) == sift_scope_inner_lz4() && sift_scope_meta_open(&m, copy, obj->len),
		 "the ring section of the Zipf object is stored compressed only where no outer layer packs the object, and opens");
	if (!sift_scope_inner_lz4())
	{
		sift_scope_meta_close(&m);
		e->flags = SIFT_SEC_LZ4;
		refused = !sift_scope_meta_open(&m, copy, obj->len);
		pfree(copy);
		return refused;
	}
	memcpy(&raw_len, copy + e->off, sizeof(raw_len));
	clen = e->len - sizeof(raw_len);
	TCHK(t, m.topk.len == raw_len && m.topk_stored == e->len && m.topk_stored * 100 <= (uint64) raw_len * (100 - SIFT_SCOPE_TOPK_LZ4_MIN_PCT),
		 "the section unpacks to the length it says, %u bytes from %llu, and saved at least %d%%", raw_len, (unsigned long long) e->len, SIFT_SCOPE_TOPK_LZ4_MIN_PCT);
	sift_scope_meta_close(&m);
	bad[0] = raw_len + 1;
	bad[1] = (uint32) clen;
	bad[2] = 0;
	bad[3] = UINT32_MAX;
	for (int i = 0; i < 4; i++)
	{
		memcpy(copy + e->off, &bad[i], sizeof(bad[i]));
		refused += !sift_scope_meta_open(&m, copy, obj->len);
	}
	memcpy(copy + e->off, &raw_len, sizeof(raw_len));
	e->flags = 2;
	refused += !sift_scope_meta_open(&m, copy, obj->len);
	e->flags = SIFT_SEC_LZ4;
	section_entry(copy, obj->len, SEC_SCOPE_KEYDIR)->flags = SIFT_SEC_LZ4;
	refused += !sift_scope_meta_open(&m, copy, obj->len);
	section_entry(copy, obj->len, SEC_SCOPE_KEYDIR)->flags = 0;
	TCHK(t, refused == 6, "a wrong raw length, an unknown flag and the flag on another section are refused: %lld of 6", (long long) refused);
	for (int i = 0; i < 400; i++)
	{
		uint32		at = e->off + TRAND(t, e->len);

		copy[at] ^= 1 << TRAND(t, 8);
		if (sift_scope_meta_open(&m, copy, obj->len))
		{
			read_all_rings(&m);
			sift_scope_meta_close(&m);
		}
		copy[at] = obj->data[at];
	}
	e->flags = 0;
	if (sift_scope_meta_open(&m, copy, obj->len))
	{
		read_all_rings(&m);
		sift_scope_meta_close(&m);
	}
	pfree(copy);
	return refused;
}

/* Keys of a few rows, as most keys of a skewed parent graph are: the rings hold the entries of the rows and the ring of every key is the brute force's; returns the share saved against 64-entry slots */
static void
rings_zipf(TCheck *t, int64 *saving, ZipfBytes *z)
{
	TWorld		w;
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	uint32		with_ring = 0;
	uint64		entries;

	tw_gen(&w, t, &spec_zipf);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tpm, false, &obj, NULL);
	tw_open(t, &obj, &m);
	tw_check_meta(t, &w, &m, tpm, "rings-zipf");
	w.cur_pm = tpm;
	for (uint32 k = 0; k < m.hdr.nkeys; k++)
	{
		const SiftScopeL2Root *r = &m.roots[k];
		SiftScopeNewest slot[SIFT_SCOPE_TOPK_CAP],
					truth[SIFT_SCOPE_TOPK_CAP];

		with_ring += (r->flags & SIFT_L2_HAS_TOPK) != 0;
		for (uint32 ring = 0; ring < m.ntopk_rings; ring++)
		{
			uint32		nr = sift_scope_topk_get_ring(&m, r, ring, slot),
						nt = truth_ring(&w, tpm, r->scope_key, ring, SIFT_SCOPE_TOPK_CAP, truth);

			TCHK(t, nr <= nt && memcmp(slot, truth, sizeof(*slot) * nr) == 0 && (nr == nt || sift_scope_ring_partial(r, ring)),
				 "zipf: key %lld ring %u: %u entries, truth %u", (long long) r->scope_key, ring, nr, nt);
		}
	}
	entries = var_topk_check(t, &m, with_ring);
	TCHK(t, with_ring > 100 && entries > 0, "zipf: %u keys with rings", with_ring);
	z->ring_slots = (uint64) with_ring * m.ntopk_rings * SIFT_SCOPE_TOPK_CAP * sizeof(SiftScopeTopKEntry);
	z->topk_raw = m.topk.len;
	z->topk_stored = m.topk_stored;
	*saving = 100 - (int64) (m.topk_stored * 100 / z->ring_slots);
	monthmap_check(t, &m, z);
	TCHK(t, topk_lz4_damage(t, &obj) == (sift_scope_inner_lz4() ? 6 : 1), "damage to the compressed ring section, or the flag on a section that is not");
	sift_scope_meta_close(&m);
}

void
scope_v6_scenario_rings(TCheck *t)
{
	uint64		base = pg_prng_uint64(&t->rng);
	RingAcc		a = {0};
	int64		stats[2] = {0},
				budget_equal = 0,
				refused = 0,
				saving = 0;
	ZipfBytes	z = {0};

	for (uint32 round = 0; round < 4; round++)
	{
		reseed(t, base, round);
		rings_round(t, &a, stats, &budget_equal, &refused);
	}
	rings_pick_cases(t);
	TCHK(t, a.cap_from_ring > 0, "a ring of %u entries answers LIMIT %u with the tie at the cut checked: %lld", SIFT_SCOPE_TOPK_CAP, SIFT_SCOPE_TOPK_CAP, (long long) a.cap_from_ring);
	rings_zipf(t, &saving, &z);
	TCHK(t, saving >= 75, "the rings of keys of few rows are %lld%% smaller than 64-entry slots", (long long) saving);
	TCHK(t, a.ring_only > 0 && a.spilled > 0 && a.ties > 0 && a.tie_reads > 0, "rings answer alone, spill after deletes and meet ties: %lld, %lld, %lld, %lld", (long long) a.ring_only, (long long) a.spilled, (long long) a.ties, (long long) a.tie_reads);
	TCHK(t, a.scans < a.granules, "a spill reads fewer granules than the key has: %lld of %lld", (long long) a.scans, (long long) a.granules);
	TCHK(t, stats[0] < stats[1], "the leaves of the rings are read for few granules: %lld of %lld", (long long) stats[0], (long long) stats[1]);
	TCHK(t, budget_equal == 4 && refused == 4 * 8, "evictions and damage: %lld and %lld", (long long) budget_equal, (long long) refused);
	OUTL(t, "rings rounds=4 queries_checked=%d ring_only_positive=%d spilled_after_deletes_positive=%d ties_at_cut_positive=%d tie_reads_positive=%d",
		 a.queries > 0, a.ring_only > 0, a.spilled > 0, a.ties > 0, a.tie_reads > 0);
	OUTL(t, "rings entries_only_no_zero_slots=1 zipf_saving_75pct=%d spill_reads_fewer_granules=%d tails_read_for_few_granules=%d evictions_change_no_ring=1 damaged_description_refused=1 default_pick=ok",
		 saving >= 75, a.scans < a.granules, stats[0] < stats[1]);
	OUTL(t, "rings limit_64_from_ring=%d nulls_last_api=ok", a.cap_from_ring > 0);
	OUTL(t, "rings zipf_slots_bytes=%llu entries_bytes=%llu lz4_bytes=%llu", (unsigned long long) z.ring_slots, (unsigned long long) z.topk_raw, (unsigned long long) z.topk_stored);
	OUTL(t, "rings zipf_monthmap_omitted_keys=%u omitted_bytes=%llu stored_keys=%u", z.mm_omitted, (unsigned long long) z.mm_saved, z.mm_stored);
}

/* ---------------------------------------------------------------------------
 * union: deletion-vector rows and sparse IS NULL rows leave a cube once
 * ------------------------------------------------------------------------- */

static uint32
random_list(TCheck *t, uint32 n, uint32 permille, uint16 *out, const uint16 *prefer, uint32 nprefer, uint32 overlap_pct)
{
	uint32		cnt = 0,
				p = 0;

	for (uint32 i = 0; i < n; i++)
	{
		bool		take = TRAND(t, 1000) < permille;

		while (p < nprefer && prefer[p] < i)
			p++;
		if (p < nprefer && prefer[p] == i)
			take = TRAND(t, 100) < overlap_pct;
		if (take)
			out[cnt++] = (uint16) i;
	}
	return cnt;
}

static void
union_unit(TCheck *t, int64 *overlaps, int64 *overflows)
{
	for (int trial = 0; trial < 300; trial++)
	{
		uint32		n = 1 + TRAND(t, 8192),
					na,
					nb,
					nu = 0,
					got;
		uint16	   *a = palloc(sizeof(uint16) * n),
				   *b = palloc(sizeof(uint16) * n),
				   *out = palloc(sizeof(uint16) * (n + 1));
		uint8	   *bits = palloc0(n);

		na = random_list(t, n, TRAND(t, 300), a, NULL, 0, 0);
		nb = random_list(t, n, TRAND(t, 300), b, a, na, 30);
		for (uint32 i = 0; i < na; i++)
			bits[a[i]] = 1;
		for (uint32 i = 0; i < nb; i++)
			bits[b[i]] |= 2;
		for (uint32 i = 0; i < n; i++)
			nu += bits[i] != 0;
		got = sift_scope_excl_union(a, na, b, nb, out, n);
		TCHK(t, got == nu, "union of %u and %u rows has %u rows, got %u", na, nb, nu, got);
		for (uint32 i = 0, q = 0; i < n && got == nu; i++)
			if (bits[i])
				TCHK(t, out[q++] == i, "union row %u", i);
		*overlaps += nu < na + nb;
		if (nu > 0)
		{
			TCHK(t, sift_scope_excl_union(a, na, b, nb, out, nu) == nu, "a cap of the exact length is enough");
			TCHK(t, sift_scope_excl_union(a, na, b, nb, out, nu - 1) == nu, "a cap one short answers cap + 1");
			*overflows += 1;
		}
		pfree(a);
		pfree(b);
		pfree(out);
		pfree(bits);
	}
}

static void
set_dead(TGran *g, const uint16 *rows, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		if (!g->dead[rows[i]])
		{
			g->dead[rows[i]] = 1;
			g->ndead++;
		}
}

static void
union_live(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label)
{
	SiftScopeEntity e = tw_entity(w);

	w->cur_pm = pm;
	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);

		for (int mm = 0; r != NULL && mm < 2; mm++)
		{
			SiftScopeQueryNeeds q = {mm == 1, SIFT_SCOPE_NO_MONTH, false};
			SiftScopeCubeAcc got,
						truth;
			SiftScopeDeltaStats st;
			bool		gh = sift_scope_cube_live(m, r, &q, &e, &got, &st),
						th = tw_truth_cube(w, pm, &key, SIFT_SCOPE_NO_MONTH, true, &truth);

			TCHK(t, gh == th, "%s: key %lld minmax %d: live presence %d, truth %d", label, (long long) key, mm, gh, th);
			if (gh && th)
				TCHK(t, sift_scube_same(&got, &truth, SIFT_SAME_TIME | SIFT_SAME_DSETS | (mm == 1 ? 0 : SIFT_SAME_EXTREMES)), "%s: key %lld minmax %d: cube differs", label, (long long) key, mm);
			if (gh)
				sift_scube_free(&got);
			if (th)
				sift_scube_free(&truth);
		}
	}
}

static void
union_round(TCheck *t, int64 *double_counted, int64 *granules_union)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 80, .maxrows = 200, .long_pct = 10, .granule_rows = 64, .nparts = 2, .months = 8, .nkeys = 8, .keys_per_member = 3};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *tpm;
	uint16	   *dv[2048],
			   *soft[2048],
			   *uni;
	uint32		ndv[2048],
				nsoft[2048];

	tw_gen(&w, t, &sp);
	tpm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	TCHK(t, w.ngran <= 2048, "the world has %u granules", w.ngran);
	uni = palloc(sizeof(uint16) * 128);
	for (uint32 gi = 0; gi < w.ngran; gi++)
	{
		dv[gi] = palloc(sizeof(uint16) * Max(w.g[gi].n, 1));
		soft[gi] = palloc(sizeof(uint16) * Max(w.g[gi].n, 1));
		ndv[gi] = random_list(t, w.g[gi].n, TRAND(t, 4) == 0 ? 0 : 1 + TRAND(t, 80), dv[gi], NULL, 0, 0);
		nsoft[gi] = random_list(t, w.g[gi].n, TRAND(t, 3) == 0 ? 0 : 1 + TRAND(t, 60), soft[gi], dv[gi], ndv[gi], 30);
	}
	for (uint32 gi = 0; gi < w.ngran; gi++)
		set_dead(&w.g[gi], dv[gi], ndv[gi]);
	union_live(t, &w, &m, tpm, "union-dv-only");
	for (uint32 gi = 0; gi < w.ngran; gi++)
	{
		uint32		nu = sift_scope_excl_union(dv[gi], ndv[gi], soft[gi], nsoft[gi], uni, w.g[gi].n);

		TCHK(t, nu <= w.g[gi].n, "the union of a granule is at most its rows");
		set_dead(&w.g[gi], soft[gi], nsoft[gi]);
		TCHK(t, w.g[gi].ndead == nu, "granule %u: %u dead rows, union %u", gi, w.g[gi].ndead, nu);
		*double_counted += ndv[gi] + nsoft[gi] > nu;
		*granules_union += nu > 0;
	}
	union_live(t, &w, &m, tpm, "union-dv-and-soft");
}

void
scope_v6_scenario_union(TCheck *t)
{
	uint64		base = pg_prng_uint64(&t->rng);
	int64		overlaps = 0,
				overflows = 0,
				double_counted = 0,
				granules = 0;

	reseed(t, base, 0);
	union_unit(t, &overlaps, &overflows);
	for (uint32 round = 1; round < 4; round++)
	{
		reseed(t, base, round);
		union_round(t, &double_counted, &granules);
	}
	TCHK(t, overlaps > 0 && double_counted > 0 && granules > 0, "overlapping lists: %lld unit cases, %lld granules", (long long) overlaps, (long long) double_counted);
	OUTL(t, "union unit_trials=300 overlapping_lists_positive=%d caps_exact=%d", overlaps > 0, overflows > 0);
	OUTL(t, "union dv_only_exact=1 dv_and_soft_union_exact=1 granules_where_a_sum_would_double_count_positive=%d", double_counted > 0);
}

#endif							/* SIFT_TESTING */
