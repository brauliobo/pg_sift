/*
 * col/seg/zone.c - the zones and the leaf statistics of a segment: the min and max of each
 * unit, the kinds a leaf holds, the sampled statistics and their check
 * what the planner and the pruning read before a unit is opened
 */
#include "core/host.h"
#include "col/setsum.h"
#include "core/popcount.h"
#include "col/seg_int.h"

/* The smallest minimum and largest maximum of each word of 64 units, and the words with a unit whose zone does not describe its values (overflow, inline strings) */
static void
zone_words(MemoryContext cx, ZoneArrays *z, uint32 nsb)
{
	uint32		nw = (nsb + 63) / 64;
	int64	   *wmin = MemoryContextAlloc(cx, 2 * nw * sizeof(int64)),
			   *wmax = wmin + nw;
	uint64	   *wbad = MemoryContextAllocZero(cx, ((nw + 63) / 64) * sizeof(uint64));

	for (uint32 w = 0; w < nw; w++)
	{
		int64		mn = PG_INT64_MAX,
					mx = PG_INT64_MIN;

		for (uint32 u = w * 64; u < Min(nsb, (w + 1) * 64); u++)
		{
			mn = Min(mn, z->min[u]);
			mx = Max(mx, z->max[u]);
			if (z->flags[u] & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE))
				wbad[w / 64] |= UINT64CONST(1) << (w % 64);
		}
		wmin[w] = mn;
		wmax[w] = mx;
	}
	z->wmin = wmin;
	z->wmax = wmax;
	z->wbad = wbad;
}

const ZoneArrays *
zone_arrays(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICTIDX);

	if (pv->zones[col] == NULL && HEAD_CACHED())
		pv->zones[col] = sift_cseg_zones_find(seg, col);
	if (pv->zones[col] == NULL)
	{
		const SiftColDef *def = &seg->schema->cols[col];
		uint32		hdr[2];
		uint64		tab[SIFT_COL_MAX_COLS],
					off;
		Size		sz = zone_block_size(def, seg->nsb);
		char	   *blk;
		MemoryContext cx = AllocSetContextCreate(seg->mcxt, "sift cache segment zones", ALLOCSET_SMALL_SIZES);
		ZoneArrays *z = MemoryContextAllocZero(cx, sizeof(ZoneArrays));

		seg_read(seg, s->off, sizeof(hdr), hdr);
		if (hdr[0] != ZONES_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
			corrupt(seg, "ZONES section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
		seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
		off = tab[col];
		if (off % 8 != 0 || off + sz > s->len)
			corrupt(seg, "The zones of leaf %d (%llu, %zu bytes) lie outside the section.", col, ULL(off), sz);
		blk = MemoryContextAlloc(cx, sz);
		seg_read(seg, s->off + off, sz, blk);
		z->min = (const int64 *) blk;
		z->max = z->min + seg->nsb;
		z->sum = z->max + seg->nsb;
		z->nnull = (const uint32 *) (z->sum + seg->nsb);
		z->nvals = z->nnull + seg->nsb;
		z->flags = z->nvals + seg->nsb;
		if (def->store == SIFT_COLS_STR)
			z->set = (const uint64 (*)[4]) (blk + ALIGN8(seg->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));
		for (uint32 u = 0; u < seg->nsb; u++)
			z->any_ovf |= (z->flags[u] & SIFT_COLZ_OVERFLOW) != 0, z->any_inl |= (z->flags[u] & SIFT_COLZ_INLINE) != 0;
		if (def->store != SIFT_COLS_STR && seg->nsb > 64)
			zone_words(cx, z, seg->nsb);
		pv->zones[col] = z;
		if (HEAD_CACHED())
			sift_cseg_zones_keep(seg, col, cx, z);
	}
	return pv->zones[col];
}

void
sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *out)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	const SiftColDef *def = &seg->schema->cols[col];

	memset(out, 0, sizeof(SiftCZone));
	out->min = z->min[u];
	out->max = z->max[u];
	if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
		memcpy(&out->sum_f, &z->sum[u], sizeof(double));
	else
		out->sum_i = z->sum[u];
	out->nnull = z->nnull[u];
	out->nvals = z->nvals[u];
	out->flags = z->flags[u];
	if (z->set)
		memcpy(out->set, z->set[u], 4 * sizeof(uint64));
}

/* The entries of the segment's dictionary of leaf col: the count in its header (after the form), without parsing the dictionary */
static uint32
dict_count(SiftColSegment *seg, int col)
{
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2],
				n;
	uint64		tab[SIFT_COL_MAX_COLS];

	seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	if (tab[col] == 0 || tab[col] + 2 * sizeof(n) > s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	seg_read(seg, s->off + tab[col] + sizeof(n), sizeof(n), &n);
	return n;
}

void
sift_col_leaf_stats_init(SiftColLeafStats *acc)
{
	memset(acc, 0, sizeof(*acc));
	acc->min = PG_INT64_MAX;
	acc->max = PG_INT64_MIN;
	acc->has_range = acc->has_sum = true;
}

/* The kinds of a leaf in a unit of this segment that has nnull NULL slots among nvals: complete in a segment of format SIFT_COL_FORMAT_KINDS, and where nothing needs them */
static bool
kinds_exact_at(int format, const SiftColDef *def, uint32 nnull, uint32 nvals)
{
	if (format >= SIFT_COL_FORMAT_KINDS || !sift_col_def_has_kinds(def))
		return true;
	return nnull == 0 && !((def->flags & SIFT_COLD_ASTEXT) && nvals > 0);
}

static bool
leaf_kinds_exact(const SiftColSegment *seg, const SiftColDef *def, uint32 nnull, uint32 nvals)
{
	return kinds_exact_at(seg->entry.format, def, nnull, nvals);
}

bool
sift_cseg_kinds_exact(SiftColSegment *seg)
{
	if (seg->entry.format >= SIFT_COL_FORMAT_KINDS)
		return true;
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SiftColDef *def = &seg->schema->cols[c];
		const ZoneArrays *z;

		if (!sift_col_def_has_kinds(def))
			continue;
		z = zone_arrays(seg, c);
		for (uint32 u = 0; u < seg->nsb; u++)
			if (!leaf_kinds_exact(seg, def, z->nnull[u], z->nvals[u]))
				return false;
	}
	return true;
}

#define SET_STATS_UNITS	64				/* units of a segment the planner's statistics look at for a leaf's summaries */

#define OVF_STATS_UNITS	4				/* units flagged with an overflow value of a leaf, of a segment, whose overflow slots the statistics count */

/*
 * The overflow slots among those of an even sample of the units the zones flag (a dead row's value counts until the segment is rewritten): the rows
 * a scan that reads them again from the heap has to fetch. One chunk of each unit is read, once for the statistics the planner keeps.
 */
static void
sample_overflow(SiftColSegment *seg, int col, const ZoneArrays *z, SiftColLeafStats *acc)
{
	uint32		flagged = 0,
				stride,
				k = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
		flagged += (z->flags[u] & SIFT_COLZ_OVERFLOW) != 0;
	stride = Max(flagged / OVF_STATS_UNITS, 1);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCUnitView v;
		SiftCChunk	c;
		const uint8 *rec;
		uint32		len,
					avail;

		if (!(z->flags[u] & SIFT_COLZ_OVERFLOW) || k++ % stride != 0)
			continue;
		rec = sift_cseg_unit_head(seg, u, &len, &avail);
		if (!sift_cunit_parse_head(rec, avail, len, seg->schema, &v) || v.chunk_len[col] == 0)
			continue;
		sift_cseg_unit_fill(seg, u, v.chunk_off[col], v.chunk_len[col]);
		if (!sift_cunit_chunk(&v, col, &c))
			continue;
		acc->ovf_vals += c.n;
		for (uint32 i = 0; c.ovf != NULL && i < (c.n + 63) / 64; i++)
			acc->ovf_slots += sift_pc64(i == c.n / 64 ? c.ovf[i] & ((UINT64CONST(1) << (c.n % 64)) - 1) : c.ovf[i]);
	}
}

/* What the zones of the units of one leaf add to acc: the statistics the planner reads, one unit after the other (the writer keeps them per segment, LstLeaf) */
static void
zone_stats(const SiftColDef *def, int format, uint32 nsb, const ZoneArrays *z, SiftColLeafStats *acc)
{
	for (uint32 u = 0; u < nsb; u++)
	{
		bool		any = z->nnull[u] < z->nvals[u];

		acc->nunits++;
		acc->nvals += z->nvals[u];
		acc->nnull += z->nnull[u];
		acc->nokinds |= !kinds_exact_at(format, def, z->nnull[u], z->nvals[u]);
		acc->has_jnull |= (z->flags[u] & SIFT_COLZ_JNULL) != 0;
		if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		{
			acc->has_range = acc->has_sum = false;
			acc->has_ovf = true;
			acc->novf++;
		}
		if (def->kind == SIFT_COLK_GROUP)
			acc->has_sum &= !(z->flags[u] & SIFT_COLZ_NOSUM);
		else if (def->store == SIFT_COLS_INT && any)
		{
			acc->min = Min(acc->min, z->min[u]);
			acc->max = Max(acc->max, z->max[u]);
			acc->span += (double) z->max[u] - (double) z->min[u] + 1.0;
			acc->nzone++;
		}
		else if (def->store != SIFT_COLS_INT)
			acc->has_range = false;
		if (def->kind == SIFT_COLK_GROUP)
			acc->sum += (double) z->sum[u];
	}
}

/* The leaves whose zones the sample holds: the integer scalars (SiftColZones, zones_new) */
bool
sift_lst_sampled(const SiftColDef *def)
{
	return def->store == SIFT_COLS_INT && def->kind != SIFT_COLK_GROUP;
}

/* The zone of unit u of a sampled leaf as the sample holds it: an overflowed one is the whole range, one without a value the empty range */
static void
sample_zone(const ZoneArrays *z, uint32 u, int64 *lo, int64 *hi)
{
	if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		*lo = PG_INT64_MIN, *hi = PG_INT64_MAX;
	else if (z->nnull[u] >= z->nvals[u])
		*lo = PG_INT64_MAX, *hi = PG_INT64_MIN;
	else
		*lo = z->min[u], *hi = z->max[u];
}

/* A segment of up to SIFT_COL_ZONE_SAMPLE units samples all of them (what the zones gave before the block); a larger one every stride-th */
static uint32
lst_stride(uint32 nsb)
{
	return Max(1, (nsb + SIFT_COL_ZONE_SAMPLE - 1) / SIFT_COL_ZONE_SAMPLE);
}

uint32
sift_lst_nsample(uint32 nsb)
{
	return (nsb + lst_stride(nsb) - 1) / lst_stride(nsb);
}

void
sift_lst_leaf(const SiftColDef *def, int format, uint32 nsb, const ZoneArrays *z, LstLeaf *out, int64 *lo, int64 *hi)
{
	SiftColLeafStats a;

	sift_col_leaf_stats_init(&a);
	zone_stats(def, format, nsb, z, &a);
	memset(out, 0, sizeof(*out));
	out->nvals = a.nvals;
	out->nnull = a.nnull;
	out->sum = a.sum;
	out->span = a.span;
	out->nzone = a.nzone;
	out->novf = a.novf;
	out->min = a.min;
	out->max = a.max;
	out->flags = (a.has_range ? LST_HAS_RANGE : 0) | (a.has_sum ? LST_HAS_SUM : 0) | (a.has_ovf ? LST_HAS_OVF : 0) | (a.has_jnull ? LST_HAS_JNULL : 0) |
		(a.nokinds ? LST_NOKINDS : 0) | (z->any_inl ? LST_ANY_INL : 0) | (z->any_ovf ? LST_ANY_OVF : 0);
	if (lo != NULL)
		for (uint32 k = 0; k < sift_lst_nsample(nsb); k++)
			sample_zone(z, k * lst_stride(nsb), &lo[k], &hi[k]);
}

/*
 * The leaf statistics block of the segment, or NULL: a segment of 64 units or more, written by 3.5, has after its DOCMAP section what a plan
 * adds up over the zones of every unit of every leaf, so that the first plan of a backend reads a few pages for the segment, not
 * its zone arrays. An older segment, or a shorter one, has none (the flag of its directory entry) and is read as before.
 */
static const uint8 *
lst_block(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->lst_read)
	{
		const SiftSectionRef *d = seg_sec(seg, SIFT_SEC_DOCMAP);
		uint64		at = ALIGN8(d->off + d->len),
					size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;
		LstHeader	h;

		pv->lst_read = true;
		if (seg->entry.flags & SIFT_SEG_LSTATS)
		{
			uint8	   *blk;

			if (at + sizeof(h) > size)
				corrupt(seg, "The directory entry says the segment has leaf statistics, the stream ends at %llu.", ULL(size));
			seg_read(seg, at, sizeof(h), &h);
			if (h.magic != LST_MAGIC || h.ncols != (uint32) seg->schema->ncols || h.nsb != seg->nsb || h.nsample != sift_lst_nsample(seg->nsb) ||
				h.len < sizeof(h) + h.ncols * sizeof(LstLeaf) || at + h.len > size)
				corrupt(seg, "The leaf statistics (0x%08x, %u leaves, %u units, %llu bytes) do not fit the segment.", h.magic, h.ncols, h.nsb, ULL(h.len));
			blk = MemoryContextAlloc(seg->mcxt, h.len);
			seg_read(seg, at, h.len, blk);
			pv->lst = blk;
		}
	}
	return pv->lst;
}

static const LstLeaf *
lst_leaf(const uint8 *blk, int col)
{
	return (const LstLeaf *) (blk + sizeof(LstHeader)) + col;
}

/* The sample of the leaf: nsample lows, then nsample highs (NULL: the leaf has none, or the block is damaged) */
static const int64 *
lst_sample(const uint8 *blk, int col)
{
	const LstHeader *h = (const LstHeader *) blk;
	uint64		off = lst_leaf(blk, col)->sample_off;

	if (off == 0 || off % 8 != 0 || off + 2 * (uint64) h->nsample * sizeof(int64) > h->len)
		return NULL;
	return (const int64 *) (blk + off);
}

/* The statistics block of the segment is what its zone arrays add up to (sift.col_verify) */
void
sift_cseg_verify_lst(SiftColSegment *seg)
{
	const uint8 *blk = lst_block(seg);

	if (blk == NULL)
		return;
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SiftColDef *def = &seg->schema->cols[c];
		const LstLeaf *have = lst_leaf(blk, c);
		const int64 *smp = lst_sample(blk, c);
		uint32		ns = ((const LstHeader *) blk)->nsample;
		LstLeaf		want;
		int64	   *lo = NULL;

		if (sift_lst_sampled(def) != (have->sample_off != 0) || (have->sample_off != 0 && smp == NULL))
			corrupt(seg, "The leaf statistics of leaf %d have %s sample.", c, have->sample_off != 0 ? "a" : "no");
		if (smp != NULL)
			lo = palloc(2 * (Size) ns * sizeof(int64));
		sift_lst_leaf(def, seg->entry.format, seg->nsb, zone_arrays(seg, c), &want, lo, lo != NULL ? lo + ns : NULL);
		want.sample_off = have->sample_off;
		if (memcmp(&want, have, sizeof(want)) != 0 || (lo != NULL && memcmp(lo, smp, 2 * (Size) ns * sizeof(int64)) != 0))
			corrupt(seg, "The leaf statistics of leaf %d differ from the zones of its units.", c);
		if (lo != NULL)
			pfree(lo);
	}
}

void
sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc)
{
	const SiftColDef *def = &seg->schema->cols[col];
	const uint8 *blk = lst_block(seg);
	const LstLeaf *l = blk != NULL ? lst_leaf(blk, col) : NULL;
	const ZoneArrays *z = NULL;
	bool		inl;

	if (l != NULL && !(l->flags & LST_ANY_OVF))
	{
		/* the sum of the segments' sums is the sum of the units': counts are whole numbers, the spans the estimate they are */
		acc->nunits += seg->nsb;
		acc->nvals += l->nvals;
		acc->nnull += l->nnull;
		acc->nokinds |= (l->flags & LST_NOKINDS) != 0;
		acc->has_jnull |= (l->flags & LST_HAS_JNULL) != 0;
		acc->has_range &= (l->flags & LST_HAS_RANGE) != 0;
		acc->has_sum &= (l->flags & LST_HAS_SUM) != 0;
		acc->has_ovf |= (l->flags & LST_HAS_OVF) != 0;
		acc->novf += l->novf;
		acc->min = Min(acc->min, l->min);
		acc->max = Max(acc->max, l->max);
		acc->span += l->span;
		acc->nzone += l->nzone;
		acc->sum += l->sum;
		inl = (l->flags & LST_ANY_INL) != 0;
	}
	else
	{
		z = zone_arrays(seg, col);
		zone_stats(def, seg->entry.format, seg->nsb, z, acc);
		if (z->any_ovf)
			sample_overflow(seg, col, z, acc);
		inl = z->any_inl;
	}
	if (def->store == SIFT_COLS_STR)
	{
		acc->inl |= inl;
		acc->dict_n = Max(acc->dict_n, dict_count(seg, col));
	}
	{
		const PointLeaf *pl = seg_points_read(seg);

		if (pl != NULL && pl[col].nslots != 0)
		{
			acc->point_units += seg->nsb;
			if (pl[col].tids)
				acc->point_tid_units += seg->nsb;
			acc->point_keys += pl[col].nkeys;
			acc->point_rows += pl[col].count;
		}
	}
	if (seg->entry.format >= SIFT_COL_FORMAT_HEADS && sift_cseg_run_kinds(seg, col) != 0)
	{
		/* a unit has the head of its run or not (a key-cut segment's, a unit with a NULL or an overflowed row): the share of an even sample */
		uint32		stride = Max(seg->nsb / SET_STATS_UNITS, 1),
					seen = 0,
					with = 0;
		SiftCRun	r;
		uint64		at;

		for (uint32 u = 0; u < seg->nsb; u += stride, seen++)
			with += sift_cseg_run(seg, col, u, &r, &at) && r.heads > 0;
		acc->head_units += (double) seg->nsb * with / Max(seen, 1);
	}
	{
		for (int k = 0; k < 2; k++)
		{
			const GOrdLeaf *g = seg_gord_leaf(seg, col + k * seg->schema->ncols);

			if (g != NULL)
			{
				acc->gord_rows += g->hdr.entries;
				acc->gord_bytes += g->bytes;
				if (g->hdr.strbytes != 0)
					acc->gord_coll = acc->gord_coll != 0 && acc->gord_coll != g->hdr.collid ? UINT32_MAX : g->hdr.collid;
				if (g->hdr.gcol >= 0)
				{
					acc->gord_grp_rows += g->hdr.entries;
					acc->gord_gcol = g->hdr.gcol;
				}
			}
		}
	}
	if (seg_set_leaf(seg, col) != NULL)
	{
		uint32		stride = Max(seg->nsb / SET_STATS_UNITS, 1);

		for (uint32 u = 0; u < seg->nsb; u += stride)
		{
			uint32		len,
						n;
			const uint8 *blob = sift_cseg_set(seg, col, u, &len);

			acc->set_sampled++;
			if (blob != NULL && len >= SIFT_CSET_HDR)
			{
				memcpy(&n, blob + 4, sizeof(n));
				acc->set_units++;
				acc->set_keys += n;
			}
		}
	}
	else
		acc->set_sampled += Min(seg->nsb, SET_STATS_UNITS);
}

/* The next candidate of the sample: kept when it is the stride's turn, the sample thinned to every other entry (and the stride doubled) when it is full */
static bool
sample_offer(SiftColZones *zs)
{
	uint64		at = zs->seen++;

	if (at % zs->stride != 0)
		return false;
	if (zs->n == SIFT_COL_ZONE_SAMPLE)
	{
		/* the units kept are the even ones: every other sample goes, and one candidate in twice as many is sampled from here on */
		for (int c = 0; c < zs->ncols; c++)
			if (zs->lo[c] != NULL)
				for (uint32 k = 0; k < SIFT_COL_ZONE_SAMPLE / 2; k++)
				{
					zs->lo[c][k] = zs->lo[c][2 * k];
					zs->hi[c][k] = zs->hi[c][2 * k];
				}
		zs->n /= 2;
		zs->stride *= 2;
		if (at % zs->stride != 0)
			return false;
	}
	return true;
}

void
sift_cseg_zone_sample(SiftColSegment *seg, SiftColZones *zs)
{
	const ZoneArrays *z[SIFT_COL_MAX_COLS] = {NULL};
	const uint8 *blk = lst_block(seg);
	const int64 *smp[SIFT_COL_MAX_COLS] = {NULL};
	bool		fast = blk != NULL;

	for (int c = 0; c < seg->schema->ncols && c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
		{
			if (fast && (smp[c] = lst_sample(blk, c)) == NULL)
				fast = false;
		}
	if (fast)
	{
		/* the sampled units, from the block: the zone arrays of the leaves are not read */
		uint32		ns = ((const LstHeader *) blk)->nsample;

		for (uint32 k = 0; k < ns; k++)
		{
			if (!sample_offer(zs))
				continue;
			for (int c = 0; c < zs->ncols; c++)
				if (smp[c] != NULL)
				{
					zs->lo[c][zs->n] = smp[c][k];
					zs->hi[c][zs->n] = smp[c][ns + k];
				}
			zs->n++;
		}
		zs->units += seg->nsb;
		return;
	}
	for (int c = 0; c < seg->schema->ncols && c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
			z[c] = zone_arrays(seg, c);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		if (!sample_offer(zs))
			continue;
		for (int c = 0; c < zs->ncols; c++)
			if (z[c] != NULL)
				sample_zone(z[c], u, &zs->lo[c][zs->n], &zs->hi[c][zs->n]);
		zs->n++;
	}
	zs->units += seg->nsb;
}
