/*
 * col/seg_maint.c - column segments in maintenance: decoding for rewrites, overflow reaping, VACUUM and verification (col/seg.h).
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/ver.h"
#include "core/live.h"

/* ---------- decoding for rewrites and verification ---------- */

void
sift_cseg_decode_unit_freeze(SiftColSegment *seg, uint32 u, SiftColBatch *dst, SiftColFreezeFn freeze, void *arg)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	SiftCDict **dicts = palloc0(seg->schema->ncols * sizeof(SiftCDict *));
	uint64	   *live = palloc(SIFT_COL_NWORDS(r->nrows) * sizeof(uint64));
	bool		dirty = unit_dirty(seg, u);

	if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
	for (int c = 0; c < seg->schema->ncols; c++)
		dicts[c] = sift_cseg_dict(seg, c);
	sift_cseg_live(seg, u, live);
	sift_cbatch_reset(dst, r->sb);
	sift_cunit_decode(&v, r->sb, dicts, dirty ? live : NULL, dst);
	pfree(dicts);
	if (sift_cseg_vers(seg))
	{
		SiftColVerSum sum;

		sift_cseg_ver_sum(seg, u, &sum);
		if (sum.any)
		{
			uint32	   *xm = palloc(r->nrows * sizeof(uint32)),
					   *cm = palloc(r->nrows * sizeof(uint32));
			uint32		k = 0;

			sift_cseg_ver_unit(seg, u, xm, cm);
			for (uint32 i = 0; i < r->nrows; i++)
			{
				if (dirty && !(live[i / 64] >> (i % 64) & 1))
					continue;
				if (xm[i] != 0 && freeze != NULL && freeze(arg, xm[i], cm[i]))
					xm[i] = 0;
				sift_cbatch_set_ver(dst, k++, xm[i], cm[i]);
			}
			Assert(k == dst->nrows);
			pfree(xm);
			pfree(cm);
		}
	}
	pfree(live);
}

void
sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst)
{
	sift_cseg_decode_unit_freeze(seg, u, dst, NULL, NULL);
}

static bool
vals_have_ovf(const SiftCVals *v)
{
	for (uint32 w = 0; v->ovf != NULL && w < SIFT_COL_NWORDS(v->n); w++)
		if (v->ovf[w])
			return true;
	return false;
}

bool
sift_cseg_stale_ovf(SiftColSegment *seg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col stale overflow", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColBatch *live = sift_cbatch_create(cx, seg->schema);
	bool		stale = false;

	for (uint32 u = 0; u < seg->nsb && !stale; u++)
	{
		bool		decoded = false;

		for (int c = 0; c < seg->schema->ncols && !stale; c++)
		{
			const ZoneArrays *z = zone_arrays(seg, c);

			if (!z->any_ovf || !(z->flags[u] & SIFT_COLZ_OVERFLOW) || !unit_dirty(seg, u))
				continue;
			if (!decoded)
				sift_cseg_decode_unit(seg, u, live);
			decoded = true;
			stale = !vals_have_ovf(&live->col[c]);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return stale;
}

/* Does row r of the batch hold an overflow value in a leaf, or in an element of a group (the eoff of the group says which are the row's)? */
static bool
batch_row_ovf(const SiftColBatch *b, uint32 r)
{
	for (int c = 0; c < b->schema->ncols; c++)
	{
		const SiftCVals *v = &b->col[c];
		int			parent = b->schema->cols[c].parent;
		uint32		lo = parent < 0 ? r : b->col[parent].eoff[r],
					hi = parent < 0 ? r + 1 : b->col[parent].eoff[r + 1];

		for (uint32 i = lo; v->ovf != NULL && i < hi; i++)
			if (sift_col_bit_test(v->ovf, i))
				return true;
	}
	return false;
}

int64
sift_cseg_reap_overflow(SiftColSegment *seg, SiftCsegDead dead, void *arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col reap overflow", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColBatch *b = sift_cbatch_create(cx, seg->schema);
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	uint64	   *gone = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint64));
	int64		removed = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *r = &seg->sbdir[u];
		uint32		j = 0;
		int			nd = 0;
		bool		flagged = false;

		for (int c = 0; c < seg->schema->ncols; c++)
			flagged |= zone_arrays(seg, c)->any_ovf && (zone_arrays(seg, c)->flags[u] & SIFT_COLZ_OVERFLOW);
		if (!flagged)
			continue;
		sift_cseg_decode_unit(seg, u, b);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < r->nrows; i++)
		{
			ItemPointerData tid;

			if (!sift_col_bit_test(live, i))
				continue;
			sift_key_to_tid(b->keys[j], &tid);
			if (batch_row_ovf(b, j) && dead(&tid, arg))
				gone[nd++] = r->rank_base + i;
			j++;
		}
		if (nd == 0)
			continue;
		sift_seg_mark_dirty(seg->index, &seg->entry);
		sift_live_clear(&PRIV(seg)->live, r->sb - seg->hdr->sb_lo, gone, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return removed;
}

/* ---------- VACUUM ---------- */

int64
sift_cseg_bulkdelete(SiftColSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SegPriv    *pv = PRIV(seg);
	uint64	   *dead = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint64));
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	int64		removed = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *r = &seg->sbdir[u];
		SiftCUnitView v;
		const uint8 *rec;
		uint32		len;
		int			nd = 0;

		rec = sift_cseg_unit(seg, u, &len);
		if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
			corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
		sift_cunit_keys(&v, r->sb, keys);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < r->nrows; i++)
		{
			ItemPointerData tid;

			if (!(live[i / 64] >> (i % 64) & 1))
				continue;
			sift_key_to_tid(keys[i], &tid);
			if (cb(&tid, cbstate))
				dead[nd++] = r->rank_base + i;
			else
				stats->num_index_tuples++;
		}
		if (nd == 0)
			continue;
		sift_seg_mark_dirty(seg->index, &seg->entry);
		sift_live_clear(&pv->live, r->sb - seg->hdr->sb_lo, dead, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	pfree(dead);
	pfree(keys);
	pfree(live);
	stats->tuples_removed += removed;
	return removed;
}

/* ---------- verification ---------- */

static bool
zone_equal(const SiftColDef *def, const SiftCZone *a, const SiftCZone *b)
{
	bool		fl = def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8;

	if (a->nvals != b->nvals || a->nnull != b->nnull || a->flags != b->flags)
		return false;
	if (a->nnull == a->nvals)
		return true;
	if (a->min != b->min || a->max != b->max)
		return false;
	if (!fl && !(a->flags & SIFT_COLZ_NOSUM) && a->sum_i != b->sum_i)
		return false;
	if (fl && memcmp(&a->sum_f, &b->sum_f, sizeof(double)) != 0)
		return false;
	return def->store != SIFT_COLS_STR || memcmp(a->set, b->set, sizeof(a->set)) == 0;
}

/* The unit checks of verify: structure, chunk sizes, dictionary codes, ascending keys inside the SB, zones */
static void
verify_unit(SiftColSegment *seg, uint32 u, SiftKey *keys)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	const SiftColSchema *sc = seg->schema;
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	uint64		group_elems[SIFT_COL_MAX_COLS] = {0};
	SiftCPaggCol pcol[SIFT_COL_MAX_COLS];
	int64	   *kept[SIFT_COL_MAX_COLS] = {NULL};
	bool		need[SIFT_COL_MAX_COLS] = {false};
	uint32		clen,
				elen;
	const uint8 *cube = sift_cseg_cube(seg, u, &clen);
	const uint8 *ecube = sift_cseg_ecube(seg, u, &elen);

	if (!sift_cunit_parse(rec, len, sc, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse or lists another row count.", u, r->sb);
	if (!sift_cpagg_leaves(sc, cube, clen, ecube, elen, need))
		corrupt(seg, "The cube or the element cubes of the unit of SB %u do not decode.", r->sb);
	sift_cunit_keys(&v, r->sb, keys);
	for (uint32 i = 0; i < r->nrows; i++)
		if (sift_block_sb(sift_key_block(keys[i])) != r->sb || (i > 0 && keys[i] <= keys[i - 1]) ||
			(keys[i] & 0xFFFF) == 0)
			corrupt(seg, "Row %u of the unit of SB %u has key %llx out of order or outside the SB.", i, r->sb, ULL(keys[i]));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		SiftCChunk	ch;
		SiftCZone	stored,
					computed;
		int64	   *vals;

		if (!sift_cunit_chunk(&v, c, &ch))
			corrupt(seg, "Unit %u has no chunk for leaf %d.", u, c);
		if (((ch.flags & SIFT_COLC_TAGS) != 0) != (seg->entry.format >= 4 && ch.ovf != NULL))
			corrupt(seg, "The chunk of leaf %d in the unit of SB %u %s the types of its overflow values (segment of column format 1.%d).", c, r->sb,
					ch.flags & SIFT_COLC_TAGS ? "has" : "lacks", seg->entry.format);
		if ((ch.flags & SIFT_COLC_KINDS) && seg->entry.format < 4)
			corrupt(seg, "The chunk of leaf %d in the unit of SB %u has kinds in a segment of column format 1.%d.", c, r->sb, seg->entry.format);
		if (ch.n !=(def->parent < 0 ? r->nrows : group_elems[def->parent]))
			corrupt(seg, "Chunk of leaf %d in the unit of SB %u holds %u values, expected %llu.", c, r->sb, ch.n,
					ULL(def->parent < 0 ? r->nrows : group_elems[def->parent]));
		vals = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
		sift_cchunk_decode(&ch, vals);
		sift_cchunk_zone(&ch, def->store, &computed);
		sift_cseg_zone(seg, c, u, &stored);
		if (!zone_equal(def, &stored, &computed))
			corrupt(seg, "The zone of leaf %d in the unit of SB %u differs from its chunk.", c, r->sb);
		if (def->kind == SIFT_COLK_GROUP)
			group_elems[c] = computed.sum_i;
		pcol[c] = (SiftCPaggCol) {vals, ch.nulls, ch.ovf, ch.n, def->store != SIFT_COLS_VAR && ch.enc != SIFT_COLENC_ISTR};
		if (def->store == SIFT_COLS_INT)
		{
			SiftCLeafAgg la;
			SiftCAgg	sa;

			sift_cpagg_leaf(&pcol[c], &la);
			sift_cseg_agg(seg, c, u, &sa);
			if (sa.sum != la.sum || sa.sumsq != la.sumsq || sa.flags != la.flags)
				corrupt(seg, "The exact sums of leaf %d in the unit of SB %u differ from its chunk.", c, r->sb);
		}
		if (def->store == SIFT_COLS_STR && ch.enc != SIFT_COLENC_ISTR)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			for (uint32 i = 0; i < ch.n; i++)
				if (!(ch.nulls && (ch.nulls[i / 64] >> (i % 64) & 1)) && (uint64) vals[i] >= d->n)
					corrupt(seg, "Leaf %d of the unit of SB %u holds code %lld of a dictionary of %u.", c, r->sb,
							(long long) vals[i], d->n);
		}
		if (need[c])
			kept[c] = vals;
		else
			pfree(vals);
	}
	if (!sift_cpagg_check(sc, pcol, r->nrows, cube, clen, ecube, elen))
		corrupt(seg, "The cube or the element cubes of the unit of SB %u differ from its chunks.", r->sb);
	for (int c = 0; c < sc->ncols; c++)
		if (kept[c])
			pfree(kept[c]);
}

/* The version section: every unit's vectors decode, say what the directory of the section says, and the range of the xmins is the header's and the entry's */
static void
verify_vers(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);
	const VersUnit *vu;
	uint32	   *xm,
			   *cm;
	uint32		lo = 0,
				hi = 0;
	bool		any = false;

	if (pv->vers_len == 0)
		return;
	vu = vers_units(seg);
	xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		n = seg->sbdir[u].nrows,
					ulo = 0,
					uhi = 0,
					cmax = 0,
					flags = 0;
		bool		first = true;

		if (!(vu[u].flags & VU_VEC))
		{
			if (vu[u].xmin_lo || vu[u].xmin_hi || vu[u].cmin_hi || (vu[u].flags & VU_FROZEN))
				corrupt(seg, "Unit %u has no versions but a summary of them.", u);
			continue;
		}
		sift_cseg_ver_unit(seg, u, xm, cm);
		for (uint32 i = 0; i < n; i++)
		{
			if (xm[i] == 0)
			{
				flags |= VU_FROZEN;
				if (cm[i] != 0)
					corrupt(seg, "Row %u of unit %u is frozen and has a command.", i, u);
				continue;
			}
			if (first || sift_cver_xid_precedes(xm[i], ulo))
				ulo = xm[i];
			if (first || sift_cver_xid_precedes(uhi, xm[i]))
				uhi = xm[i];
			cmax = Max(cmax, cm[i]);
			first = false;
		}
		if (first || ulo != vu[u].xmin_lo || uhi != vu[u].xmin_hi || cmax != vu[u].cmin_hi || flags != (vu[u].flags & VU_FROZEN))
			corrupt(seg, "The summary of the versions of unit %u (xmin %u..%u, cmin up to %u) is not what its vectors hold.", u, vu[u].xmin_lo,
					vu[u].xmin_hi, vu[u].cmin_hi);
		if (!any || sift_cver_xid_precedes(ulo, lo))
			lo = ulo;
		if (!any || sift_cver_xid_precedes(hi, uhi))
			hi = uhi;
		any = true;
		CHECK_FOR_INTERRUPTS();
	}
	if (!any || lo != pv->vmin || hi != pv->vmax || ((seg->entry.flags & SIFT_SEG_VERS) && seg->entry.reserved != lo))
		corrupt(seg, "The smallest xmin of the entry (%u) and the range of the version section (%u..%u) are not what the vectors (%u..%u) say.",
				seg->entry.reserved, pv->vmin, pv->vmax, lo, hi);
	pfree(xm);
	pfree(cm);
}

uint64
sift_cseg_verify(SiftColSegment *seg)
{
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64		live = seg->hdr->ndocs;

	verify_vers(seg);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		verify_unit(seg, u, keys);
		CHECK_FOR_INTERRUPTS();
	}
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		live -= sift_live_ndead(&PRIV(seg)->live);
	sift_sr_release(&seg->sr);
	pfree(keys);
	return live;
}
