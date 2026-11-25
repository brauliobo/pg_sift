/*
 * col/v1/seg/verify_super.c - the superunit block of a segment against its units: the page masks, the merged sums of every INT leaf, and the cube of each group, a merge of the cubes of
 * its units or, for the key groups of a scope segment (docs/COL-SCOPE.md), the cube of their rows. Run by sift.col_verify, never by a scan.
 */
/* check-memory: allow-allocs a key group holds at most SIFT_SUPER_MAX_FAN units of the leaves its cube reads */
#include "col/v1/preagg/preagg.h"
#include "col/v1/seg/int.h"

/*
 * The cube of a key group is the merge of the cubes of its units, which the writer's own function recomputes, and apart the dims that are what the rows of the units give with the
 * dims and measures the blob says: the rows as the writer saw them
 */
static void
verify_key_cube(SiftColSegment *seg, uint32 k, uint32 first, uint32 n)
{
	const SiftColSchema *sc = seg->schema;
	const uint8 **blob = palloc(n * sizeof(uint8 *));
	uint32	   *len = palloc(n * sizeof(uint32));
	bool		anycube = false;
	StringInfoData want,
				x,
				e,
				dc,
				ds;
	uint32		glen;
	const uint8 *got = sift_cseg_super_cube(seg, k, &glen);

	for (uint32 i = 0; i < n; i++)
	{
		const uint8 *cube = sift_cseg_cube(seg, first + i, &len[i]);

		blob[i] = len[i] ? memcpy(palloc(len[i]), cube, len[i]) : NULL;
		anycube |= len[i] > 0;
	}
	initStringInfo(&want);
	if (!anycube || !sift_cpagg_super_cube(sc, blob, len, n, &want))
		want.len = 0;
	if (want.len != (int) glen || (glen > 0 && memcmp(want.data, got, glen) != 0))
		corrupt(seg, "The merged part of the cube of key group %u differs from the cubes of its units.", k);
	initStringInfo(&x);
	got = sift_cseg_super_extra(seg, k, &glen);
	if (glen > 0)
		appendBinaryStringInfo(&x, (const char *) got, glen);
	initStringInfo(&e);
	got = sift_cseg_super_rollup(seg, k, &glen);
	if (glen > 0)
		appendBinaryStringInfo(&e, (const char *) got, glen);
	if (sift_cseg_super_has_rollup(seg, k) != (e.len > 0))
		corrupt(seg, "The rollup of the rows of key group %u is flagged and absent, or present and not flagged.", k);
	initStringInfo(&dc);
	got = sift_cseg_super_days(seg, k, &glen);
	if (glen > 0)
		appendBinaryStringInfo(&dc, (const char *) got, glen);
	initStringInfo(&ds);
	got = sift_cseg_super_sets(seg, k, &glen);
	if (glen > 0)
		appendBinaryStringInfo(&ds, (const char *) got, glen);
	if (sift_cseg_super_has_sets(seg, k) != (ds.len > 0) || (ds.len > 0) != (dc.len > 0))
		corrupt(seg, "The id sets of key group %u are flagged and absent, or present and not flagged, or have no cells of days.", k);
	if (x.len + e.len + ds.len > 0)
	{
		bool		need[SIFT_COL_MAX_COLS] = {false};
		SiftCPaggCol cols[SIFT_COL_MAX_COLS] = {{0}};
		int			sleaf[SIFT_COL_MAX_COLS],
					scal = -1,
					ns = 0;
		uint32		want_mask = 0;
		uint32		rows = 0,
					at = 0;

		if (x.len + e.len > 0 && !sift_cpagg_leaves(sc, (const uint8 *) x.data, x.len, (const uint8 *) e.data, e.len, need))
			corrupt(seg, "The cube of key group %u does not decode.", k);
		if (ds.len > 0 && (ns = sift_cdsets_leaves(sc, (const uint8 *) ds.data, ds.len, &scal, sleaf)) < 0)
			corrupt(seg, "The id sets of key group %u do not decode.", k);
		if (!sift_cpagg_leaves(sc, (const uint8 *) dc.data, dc.len, NULL, 0, need))
			corrupt(seg, "The cells of days of key group %u do not decode.", k);
		for (int i = 0; i < ns; i++)
		{
			need[sleaf[i]] = true;
			want_mask |= 1 << sift_cdsets_ordinal(sc, sleaf[i]);
		}
		if (want_mask != sift_cseg_super_dset_mask(seg, k))
			corrupt(seg, "The leaves of the id sets of key group %u are not those its word says.", k);
		if (ns > 0)
			need[scal] = true;
		for (uint32 i = 0; i < n; i++)
			rows += seg->sbdir[first + i].nrows;
		for (int c = 0; c < sc->ncols; c++)
			if (need[c])
			{
				Size		words = rows / 64 + 2;

				cols[c] = (SiftCPaggCol) {palloc((Size) rows * sizeof(int64) + SIFT_COL_PAD), palloc0(words * sizeof(uint64)), palloc0(words * sizeof(uint64)), rows, true};
			}
		for (uint32 i = 0; i < n; i++)
		{
			SiftCUnitView v;
			uint32		ulen,
						nrows = seg->sbdir[first + i].nrows;
			const uint8 *rec = sift_cseg_unit(seg, first + i, &ulen);

			if (!sift_cunit_parse(rec, ulen, sc, &v))
				corrupt(seg, "Unit %u of key group %u does not parse.", first + i, k);
			for (int c = 0; c < sc->ncols; c++)
			{
				SiftCChunk	ch;
				SiftCPaggCol *pc = &cols[c];

				if (!need[c])
					continue;
				if (!sift_cunit_chunk(&v, c, &ch) || ch.n != nrows)
					corrupt(seg, "Unit %u of key group %u has no chunk of %u rows for leaf %d, which its cube reads.", first + i, k, nrows, c);
				sift_cchunk_decode(&ch, (int64 *) pc->key + at);
				if (ch.nulls)
					seg_bits_put((uint64 *) pc->nulls, at, ch.nulls, nrows);
				if (ch.ovf)
					seg_bits_put((uint64 *) pc->ovf, at, ch.ovf, nrows);
				pc->ready = pc->ready && sc->cols[c].store != SIFT_COLS_VAR && ch.enc != SIFT_COLENC_ISTR;
			}
			at += nrows;
		}
		if (x.len + e.len > 0 && !sift_cpagg_check(sc, cols, rows, (const uint8 *) x.data, x.len, (const uint8 *) e.data, e.len, seg->entry.format))
			corrupt(seg, "The dims and the rollup of the rows of key group %u differ from the rows of its units.", k);
		if (dc.len > 0 && !sift_cpagg_check(sc, cols, rows, (const uint8 *) dc.data, dc.len, NULL, 0, seg->entry.format))
			corrupt(seg, "The cells of days of key group %u differ from the rows of its units.", k);
		if (ns > 0)
		{
			StringInfoData built;

			initStringInfo(&built);
			sift_cdsets_build(sc, cols, rows, scal, sleaf, ns, &built);
			if (built.len != ds.len || memcmp(built.data, ds.data, ds.len) != 0)
				corrupt(seg, "The id sets of key group %u differ from the rows of its units.", k);
		}
		for (int c = 0; c < sc->ncols; c++)
			if (need[c])
			{
				pfree((void *) cols[c].key);
				pfree((void *) cols[c].nulls);
				pfree((void *) cols[c].ovf);
			}
	}
}

/* The units of a key group hold one scope key and no other, and no more rows than a cube is built from */
static void
verify_key_units(SiftColSegment *seg, uint32 k, uint32 first, uint32 n)
{
	int			key = -1;
	uint64		rows = 0;
	SiftCZone	z0;

	for (int c = 0; c < seg->schema->ncols; c++)
		if (seg->schema->cols[c].flags & SIFT_COLD_SCOPE)
			key = c;
	if (key < 0)
		corrupt(seg, "The segment has key groups and no derived scope key.");
	for (uint32 i = 0; i < n; i++)
	{
		SiftCZone	z;

		sift_cseg_zone(seg, key, first + i, &z);
		if (i == 0)
			z0 = z;
		if (z.nnull != 0 || z.min != z.max || z.min != z0.min)
			corrupt(seg, "Unit %u of key group %u does not hold the scope key of the group alone.", first + i, k);
		rows += seg->sbdir[first + i].nrows;
	}
	if (rows > SIFT_SUPER_KEY_ROWS)
		corrupt(seg, "The key group %u holds %llu rows.", k, ULL(rows));
}

/* The superunit block says what the units say: the page masks, the merged sums of every INT leaf and the cube of each group, recomputed by the writer's own functions */
void
sift_cseg_verify_super(SiftColSegment *seg)
{
	const SiftColSchema *sc = seg->schema;
	MemoryContext cx,
				old;
	uint64	   *pm;
	SiftCAgg   *ua;
	uint32		first,
				n,
				k;

	if (sift_cseg_super_fan(seg) == 0)
		return;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift col verify superunits", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	pm = palloc(SIFT_SUPER_MAX_FAN * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	/* the page mask of every unit, in a group or not */
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCUnitView v;
		uint32		ulen;
		const uint8 *rec = sift_cseg_unit(seg, u, &ulen);

		sift_cseg_unit_pagemasks(seg, u, 1, pm);
		if (!sift_cunit_parse(rec, ulen, sc, &v) ||
			(v.keyed ? (pm[0] != ((uint64) v.kblk1 << 32 | v.kblk0) || (pm[1] | pm[2] | pm[3]) != 0) : memcmp(v.pagemask, pm, sizeof(v.pagemask)) != 0))
			corrupt(seg, "The page mask of unit %u in the superunit block is not the unit's.", u);
		CHECK_FOR_INTERRUPTS();
	}
	pfree(pm);
	for (k = 0; k < sift_cseg_super_count(seg); k++)
	{
		const uint8 **blob;
		uint32	   *len;
		bool		anycube = false;
		StringInfoData want;
		const uint8 *got;
		uint32		glen;

		sift_cseg_super_range(seg, k, &first, &n);
		ua = palloc(SIFT_SUPER_MAX_FAN * sizeof(SiftCAgg));
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCSuperLeaf want_leaf;
			SiftCAgg	got_leaf;
			bool		want_ok;

			if (!super_leaf_ok(&sc->cols[c]))
				continue;
			for (uint32 i = 0; i < n; i++)
				sift_cseg_agg(seg, c, first + i, &ua[i]);
			want_ok = n >= 2 && sift_cpagg_super_leaf(ua, n, sc->cols[c].store == SIFT_COLS_INT, &want_leaf);
			if (want_ok != sift_cseg_super_agg(seg, c, k, &got_leaf) ||
				(want_ok && (got_leaf.nvals != want_leaf.nvals || got_leaf.nnull != want_leaf.nnull || got_leaf.min != want_leaf.min ||
							 got_leaf.max != want_leaf.max || got_leaf.sum != (SiftCI128) (((unsigned __int128) want_leaf.sum_hi << 64) | want_leaf.sum_lo) ||
							 got_leaf.sumsq != (SiftCI128) (((unsigned __int128) want_leaf.sumsq_hi << 64) | want_leaf.sumsq_lo) ||
							 got_leaf.flags != (want_leaf.flags & SIFT_CAGG_SUMSQ_OVF) || got_leaf.zflags != want_leaf.zflags)))
				corrupt(seg, "The summary of leaf %d in superunit %u differs from its units.", c, k);
		}
		if (sift_cseg_super_keyed(seg))
		{
			verify_key_units(seg, k, first, n);
			verify_key_cube(seg, k, first, n);
		}
		else
		{
			blob = palloc(n * sizeof(uint8 *));
			len = palloc(n * sizeof(uint32));
			for (uint32 i = 0; i < n; i++)
			{
				const uint8 *cube = sift_cseg_cube(seg, first + i, &len[i]);

				blob[i] = len[i] ? memcpy(palloc(len[i]), cube, len[i]) : NULL;
				anycube |= len[i] > 0;
			}
			initStringInfo(&want);
			if (n >= 2 && anycube && !sift_cpagg_super_cube(sc, blob, len, n, &want))
				want.len = 0;
			got = sift_cseg_super_cube(seg, k, &glen);
			if (want.len != (int) glen || (glen > 0 && memcmp(want.data, got, glen) != 0))
				corrupt(seg, "The cube of superunit %u differs from the cubes of its units.", k);
		}
		MemoryContextReset(cx);
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}
