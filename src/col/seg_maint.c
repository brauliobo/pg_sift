/*
 * col/seg_maint.c - column segments in maintenance: decoding for rewrites, overflow reaping, VACUUM and verification (col/seg.h).
 */

#include "core/host.h"

#include "col/hll.h"
#include "col/ktab.h"
#include "col/preagg.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/runs.h"
#include "col/setsum.h"
#include "col/ver.h"
#include "core/live.h"

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
	dst->keyed = v.keyed;
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

StaticAssertDecl(SIFT_COL_UNIT_MAX_ROWS / 8 + 1 <= 2 * SIFT_PAGE_PAYLOAD, "the liveness bits of a unit lie on at most three pages, which sift_live_clear clears with the header page in one GenericXLog record");

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

static bool
zone_equal(const SiftColDef *def, const SiftCZone *a, const SiftCZone *b)
{
	bool		fl = def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8;

	/* a segment written before the flag existed lacks SIFT_COLZ_JNULL: the stored zone may say less than the chunk, never more */
	if (a->nvals != b->nvals || a->nnull != b->nnull || ((a->flags ^ b->flags) & ~SIFT_COLZ_JNULL) || (a->flags & ~b->flags & SIFT_COLZ_JNULL))
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

/* The small-set summary of leaf c in unit u, when it has one, is what its keys give (an exact set holds them all and no other, a filter their bits and none else) */
static void
verify_set(SiftColSegment *seg, int c, uint32 u, const SiftCPaggCol *col)
{
	uint32		len,
				nd;
	const uint8 *blob = sift_cseg_set(seg, c, u, &len);
	SiftCSet	s;
	int64	   *keys;

	if (blob == NULL)
		return;
	if (!sift_cset_open(blob, len, &s))
		corrupt(seg, "The small-set summary of leaf %d in unit %u is malformed.", c, u);
	if (!col->ready)
		corrupt(seg, "Leaf %d has a small-set summary in unit %u, whose chunk has no codes.", c, u);
	keys = palloc(Max(col->n, 1) * sizeof(int64));
	nd = sift_cset_keys(col, keys);
	if (!sift_cset_matches(&s, keys, nd))
		corrupt(seg, "The small-set summary of leaf %d in unit %u differs from its chunk.", c, u);
	pfree(keys);
}

/* The sorted run of leaf c in unit u, when it has one, is what its chunk gives, entry for entry */
static void
verify_run(SiftColSegment *seg, int c, uint32 u, const SiftCPaggCol *col, const SiftKey *keys)
{
	SiftCRun	r;
	uint64		at;
	uint8	   *blob;

	if (!sift_cseg_run(seg, c, u, &r, &at))
		return;
	if (!col->ready)
		corrupt(seg, "Leaf %d has a sorted run in unit %u, whose chunk has no keys.", c, u);
	blob = palloc(sift_crun_total(&r));
	sift_cseg_run_blob(seg, at, &r, blob);
	if (!sift_crun_matches(&seg->schema->cols[c], col, keys, blob, sift_crun_total(&r)))
		corrupt(seg, "The sorted run of leaf %d in unit %u differs from its chunk.", c, u);
	pfree(blob);
}

/* The sketch of leaf c in unit u, when it has one, is what its chunk gives, byte for byte */
static void
verify_hll(SiftColSegment *seg, int c, uint32 u, const SiftCPaggCol *col)
{
	uint32		len;
	const uint8 *blob = sift_cseg_hll(seg, c, u, &len);
	StringInfoData want;

	if (blob == NULL)
		return;
	initStringInfo(&want);
	if (!sift_hll_build(&seg->schema->cols[c], col, seg->schema->cols[c].store == SIFT_COLS_STR ? sift_cseg_dict(seg, c) : NULL, &want) ||
		(uint32) want.len != len || memcmp(want.data, blob, len) != 0)
		corrupt(seg, "The sketch of leaf %d in unit %u differs from its chunk.", c, u);
	pfree(want.data);
}

/* The point directory of the leaf lists every row of the unit that holds a value, and none of a leaf with overflow values (listed counts them over the segment) */
static void
verify_point(SiftColSegment *seg, int c, uint32 u, const SiftCPaggCol *col, const SiftKey *keys, uint64 *listed)
{
	uint32		base = (uint32) seg->sbdir[u].rank_base;

	if (sift_cseg_point_count(seg, c) == 0)
		return;
	if (!col->ready)
		corrupt(seg, "Leaf %d has a point directory and no keys in the unit %u.", c, u);
	for (uint32 i = 0; col->ovf && i < SIFT_COL_NWORDS(col->n); i++)
		if (col->ovf[i])
			corrupt(seg, "Leaf %d has a point directory and an overflow value in the unit %u.", c, u);
	if (sift_cseg_point_is_tid(seg, c))
	{
		if (!sift_cseg_point_check_tids(seg, c, col->key, col->nulls, keys, col->n, listed))
			corrupt(seg, "The point directory of leaf %d does not list the address of a row of unit %u under its key.", c, u);
		return;
	}
	for (uint32 i = 0; i < col->n; i++)
	{
		if (col->nulls && (col->nulls[i / 64] >> (i % 64) & 1))
			continue;
		if (!sift_cseg_point_has(seg, c, col->key[i], base + i))
			corrupt(seg, "The point directory of leaf %d does not list row %u of unit %u under its key.", c, base + i, u);
		(*listed)++;
	}
}

/* The unit checks of verify: structure, chunk sizes, dictionary codes, ascending keys inside the SB, zones */
static void
verify_unit(SiftColSegment *seg, uint32 u, SiftKey *keys, uint64 *listed, SiftCKtab *kt)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	const SiftColSchema *sc = seg->schema;
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	uint64		group_elems[SIFT_COL_MAX_COLS] = {0};
	SiftCPaggCol pcol[SIFT_COL_MAX_COLS];
	int64	   *kept[SIFT_COL_MAX_COLS] = {NULL},
			   *late[SIFT_COL_MAX_COLS] = {NULL};
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
		if ((!v.keyed && sift_block_sb(sift_key_block(keys[i])) != r->sb) || (i > 0 && keys[i] <= keys[i - 1]) ||
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
		verify_set(seg, c, u, &pcol[c]);
		verify_run(seg, c, u, &pcol[c], keys);
		verify_hll(seg, c, u, &pcol[c]);
		verify_point(seg, c, u, &pcol[c], keys, &listed[c]);
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
		else if (kt)
			late[c] = vals;			/* the key table adds the leaves of the unit together, below */
		else
			pfree(vals);
	}
	if (kt)
	{
		sift_ktab_add(kt, pcol, r->nrows);
		for (int c = 0; c < sc->ncols; c++)
			if (late[c])
				pfree(late[c]);
	}
	if (!sift_cpagg_check(sc, pcol, r->nrows, cube, clen, ecube, elen, seg->entry.format))
		corrupt(seg, "The cube or the element cubes of the unit of SB %u differ from its chunks.", r->sb);
	if (seg->entry.format >= SIFT_COL_FORMAT_EXCEPT)
	{
		uint32		xlen,
					xelen;
		const uint8 *xcube = sift_cseg_xcube(seg, u, &xlen);
		const uint8 *xecube = sift_cseg_xecube(seg, u, &xelen);
		const uint32 **eoff = palloc0(sc->ncols * sizeof(uint32 *));
		uint32	  **own = palloc0(sc->ncols * sizeof(uint32 *));
		bool		same;

		for (int c = 0; c < sc->ncols; c++)
		{
			int			g = sc->cols[c].parent;

			if (g < 0 || !need[g])
				continue;
			if (own[g] == NULL)
			{
				own[g] = palloc((Size) (r->nrows + 1) * sizeof(uint32));
				own[g][0] = 0;
				for (uint32 i = 0; i < r->nrows; i++)
					own[g][i + 1] = own[g][i] + (uint32) kept[g][i];
			}
			eoff[c] = own[g];
		}
		same = sift_cpagg_check_excepts(sc, pcol, eoff, r->nrows, cube, clen, ecube, elen, seg->entry.format, xcube, xcube ? xlen : 0, xecube, xecube ? xelen : 0);
		if (!same)
			corrupt(seg, "The exceptions of the unit of SB %u differ from its chunks.", r->sb);
	}
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

/* The superunit block says what the units say: the page masks, the merged sums of every INT leaf and the merged cube, recomputed by the writer's own functions */
void
sift_cseg_verify_super(SiftColSegment *seg)
{
	const SiftColSchema *sc = seg->schema;
	uint32		fan = sift_cseg_super_fan(seg),
				nsu;
	MemoryContext cx,
				old;
	uint64	   *pm;
	SiftCAgg   *ua;

	if (fan == 0)
		return;
	nsu = (seg->nsb + fan - 1) / fan;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift col verify superunits", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	pm = palloc(fan * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	ua = palloc(fan * sizeof(SiftCAgg));
	for (uint32 k = 0; k < nsu; k++)
	{
		uint32		n = Min(fan, seg->nsb - k * fan);
		const uint8 **blob = palloc(n * sizeof(uint8 *));
		uint32	   *len = palloc(n * sizeof(uint32));
		bool		anycube = false;
		StringInfoData want;
		const uint8 *got;
		uint32		glen;

		sift_cseg_super_pagemask(seg, k, n, pm);
		for (uint32 i = 0; i < n; i++)
		{
			SiftCUnitView v;
			uint32		ulen;
			const uint8 *rec = sift_cseg_unit(seg, k * fan + i, &ulen);
			const uint8 *cube;

			if (!sift_cunit_parse(rec, ulen, sc, &v) ||
				(v.keyed ? (pm[i * SIFT_PAGEMASK_WORDS] != ((uint64) v.kblk1 << 32 | v.kblk0)) : memcmp(v.pagemask, pm + i * SIFT_PAGEMASK_WORDS, sizeof(v.pagemask)) != 0))
				corrupt(seg, "The page mask of unit %u in superunit %u is not the unit's.", k * fan + i, k);
			cube = sift_cseg_cube(seg, k * fan + i, &len[i]);
			blob[i] = len[i] ? memcpy(palloc(len[i]), cube, len[i]) : NULL;
			anycube |= len[i] > 0;
		}
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCSuperLeaf want_leaf;
			SiftCAgg	got_leaf;
			bool		want_ok;

			if (!super_leaf_ok(&sc->cols[c]))
				continue;
			for (uint32 i = 0; i < n; i++)
				sift_cseg_agg(seg, c, k * fan + i, &ua[i]);
			want_ok = n >= 2 && sift_cpagg_super_leaf(ua, n, sc->cols[c].store == SIFT_COLS_INT, &want_leaf);
			if (want_ok != sift_cseg_super_agg(seg, c, k, &got_leaf) ||
				(want_ok && (got_leaf.nvals != want_leaf.nvals || got_leaf.nnull != want_leaf.nnull || got_leaf.min != want_leaf.min ||
							 got_leaf.max != want_leaf.max || got_leaf.sum != (SiftCI128) (((unsigned __int128) want_leaf.sum_hi << 64) | want_leaf.sum_lo) ||
							 got_leaf.sumsq != (SiftCI128) (((unsigned __int128) want_leaf.sumsq_hi << 64) | want_leaf.sumsq_lo) ||
							 got_leaf.flags != (want_leaf.flags & SIFT_CAGG_SUMSQ_OVF) || got_leaf.zflags != want_leaf.zflags)))
				corrupt(seg, "The summary of leaf %d in superunit %u differs from its units.", c, k);
		}
		initStringInfo(&want);
		if (n >= 2 && anycube && !sift_cpagg_super_cube(sc, blob, len, n, &want))
			want.len = 0;
		got = sift_cseg_super_cube(seg, k, &glen);
		if (want.len != glen || (glen > 0 && memcmp(want.data, got, glen) != 0))
			corrupt(seg, "The cube of superunit %u differs from the cubes of its units.", k);
		MemoryContextReset(cx);
		pm = palloc(fan * SIFT_PAGEMASK_WORDS * sizeof(uint64));
		ua = palloc(fan * sizeof(SiftCAgg));
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* The kinds a leaf's record in the small-set summaries lists are those of its units' summaries */
static void
verify_set_kinds(SiftColSegment *seg)
{
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		uint32		want = sift_cseg_set_kinds(seg, c),
					have = 0;

		for (uint32 u = 0; want != 0 && u < seg->nsb; u++)
		{
			uint32		len;
			const uint8 *blob = sift_cseg_set(seg, c, u, &len);

			if (blob != NULL && len > 0)
				have |= blob[0] == SIFT_CSET_EXACT ? SETS_EXACT : blob[0] == SIFT_CSET_BLOOM ? SETS_BLOOM : SETS_PRES; /* PRES and PLIST */
		}
		if (have != want)
			corrupt(seg, "The record of the small-set summaries of leaf %d lists kinds %u, its units hold %u.", c, want, have);
	}
}

/* The kinds a leaf's record in the sorted runs lists are those of its units' runs */
static void
verify_run_kinds(SiftColSegment *seg)
{
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		uint32		want = sift_cseg_run_kinds(seg, c),
					have = 0;

		for (uint32 u = 0; want != 0 && u < seg->nsb; u++)
		{
			SiftCRun	r;
			uint64		at;

			if (sift_cseg_run(seg, c, u, &r, &at))
				have |= r.kind == SIFT_CRUN_ORD ? RUNS_ORD : RUNS_IDENT;
		}
		if (have != want)
			corrupt(seg, "The record of the sorted runs of leaf %d lists kinds %u, its units hold %u.", c, want, have);
	}
}

/* The forms a leaf's record in the sketches lists are those of its units' sketches */
static void
verify_hll_kinds(SiftColSegment *seg)
{
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		uint32		want = sift_cseg_hll_kinds(seg, c),
					have = 0;

		for (uint32 u = 0; want != 0 && u < seg->nsb; u++)
		{
			uint32		len;
			const uint8 *blob = sift_cseg_hll(seg, c, u, &len);

			if (blob != NULL)
				have |= blob[0];
		}
		if (have != want)
			corrupt(seg, "The record of the sketches of leaf %d lists forms %u, its units hold %u.", c, want, have);
	}
}

/* A dictionary stored in blocks is in a segment of minor 14; if it has an order, that names every code once and the strings ascend in it */
static void
verify_dicts(SiftColSegment *seg)
{
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		SiftCDict  *d;
		uint64	   *seen;

		if (seg->schema->cols[c].store != SIFT_COLS_STR)
			continue;
		d = sift_cseg_dict(seg, c);
		if (!d->blocked)
			continue;
		if (seg->entry.format < SIFT_COL_FORMAT_DICTIDX)
			corrupt(seg, "The dictionary of leaf %d is stored in blocks in a segment of column format 1.%d.", c, seg->entry.format);
		if (!d->stored_order)
			continue;
		seen = palloc0(SIFT_COL_NWORDS(d->n) * sizeof(uint64));
		for (uint32 i = 0; i < d->n; i++)
		{
			uint32		code = d->sorted[i],
						len,
						prev_len = 0;
			const char *s = sift_cdict_str(d, code, &len);

			if (seen[code >> 6] >> (code & 63) & 1)
				corrupt(seg, "The order of the dictionary of leaf %d lists code %u twice.", c, code);
			seen[code >> 6] |= UINT64CONST(1) << (code & 63);
			if (i > 0)
			{
				const char *prev = sift_cdict_str(d, d->sorted[i - 1], &prev_len);

				if (sift_cstr_cmp(prev, prev_len, s, len) >= 0)
					corrupt(seg, "The order of the dictionary of leaf %d is not the byte order of its strings (position %u).", c, i);
			}
		}
		pfree(seen);
		CHECK_FOR_INTERRUPTS();
	}
}

uint64
sift_cseg_verify(SiftColSegment *seg)
{
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64		live = seg->hdr->ndocs;
	uint64		listed[SIFT_COL_MAX_COLS] = {0};
	bool		haskt[SIFT_COL_MAX_COLS] = {false};
	SiftCKtab  *kt;

	for (int c = 0; c < seg->schema->ncols; c++)
		haskt[c] = sift_cseg_ktab(seg, c) != NULL;
	kt = sift_ktab_begin(seg->schema, haskt, CurrentMemoryContext);
	verify_vers(seg);
	verify_dicts(seg);
	sift_cseg_verify_points(seg);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		verify_unit(seg, u, keys, listed, kt);
		CHECK_FOR_INTERRUPTS();
	}
	for (int c = 0; c < seg->schema->ncols; c++)
		if (listed[c] != sift_cseg_point_count(seg, c))
			corrupt(seg, "The point directory of leaf %d lists %u rows, its units hold %llu values.", c, sift_cseg_point_count(seg, c), ULL(listed[c]));
	for (int c = 0; c < seg->schema->ncols; c++)
		if (haskt[c] && !sift_ktab_check(kt, sift_cseg_ktab(seg, c)))
			corrupt(seg, "The key table of leaf %d differs from the units.", c);
	sift_cseg_verify_super(seg);
	verify_set_kinds(seg);
	verify_run_kinds(seg);
	verify_hll_kinds(seg);
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		live -= sift_live_ndead(&PRIV(seg)->live);
	sift_sr_release(&seg->sr);
	pfree(keys);
	return live;
}
