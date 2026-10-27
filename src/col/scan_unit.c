/*
 * col/scan_unit.c - the unit of the column source: chunks, dictionaries, keys, all-visible and liveness state, groups, zones, GPU view.
 */

#include "postgres.h"

#include "common/pg_prng.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/rls.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "core/cmp.h"
#include "core/popcount.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "bm25/estimate.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/scan_int.h"

/* The chunk of leaf col of the unit as stored; false when the unit has none. A stored unit reads its bytes on first use. */
bool
unit_chunk_raw(ColUnit *cu, int col, SiftCChunk *c)
{
	if (cu->loaded && !cu->loaded[col] && cu->v.chunk_len[col] > 0)
	{
		sift_cseg_unit_fill(cu->seg, cu->u, cu->v.chunk_off[col], cu->v.chunk_len[col]);
		cu->loaded[col] = 1;
	}
	return sift_cunit_chunk(&cu->v, col, c);
}

/* The same, an inline STR chunk as codes of a dictionary of its own (unit_dict, dict_id) */
bool
unit_chunk(ColUnit *cu, int col, SiftCChunk *c)
{
	if (!unit_chunk_raw(cu, col, c))
		return false;
	if (c->enc == SIFT_COLENC_ISTR)
	{
		ColInline  *in = &cu->istr[col];

		if (in->dict == NULL)
		{
			sift_cchunk_istr_codes(c, cu->src->arena, &in->dict, &in->chunk);
			in->id = INL_ID_FLAG | ++cu->src->inl_seq << 8 | col;
		}
		*c = in->chunk;
	}
	return true;
}

/* Is the STR leaf stored inline in this unit (known from the zones, without reading the chunk) */
bool
unit_inline(ColUnit *cu, int col)
{
	SiftCZone	z;

	if (cu->seg == NULL || cu->src->schema->cols[col].store != SIFT_COLS_STR)
		return false;
	sift_cseg_zone(cu->seg, col, cu->u, &z);
	return (z.flags & SIFT_COLZ_INLINE) != 0;
}

SiftCDict *
unit_dict(ColUnit *cu, int col)
{
	SiftCChunk	c;

	if (cu->src->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (cu->seg == NULL)
		return cu->batch->dict[col];
	if (!unit_inline(cu, col))
		return sift_cseg_dict(cu->seg, col);
	unit_chunk(cu, col, &c);
	return cu->istr[col].dict;
}

/*
 * Whether every immutable segment the source opened is still in the directory. A merge replaces its inputs with an output that has the liveness
 * of its moment; the VACUUM passes after it flag the output, never the inputs, which a scan holding the older directory copy still reads. Such a
 * scan must not take "live on an all-visible page" for "visible": the all-visible bit may have been set by a pass that never reached its
 * segments. While the segments are current, every pass that finished before the mask was read ran on them (a pass and a merge exclude each
 * other by the maintenance lock) and the liveness read after the mask shows it.
 */
static bool
segments_current(SiftColSource *src)
{
	uint32		v = sift_dir_version(src->index);

	if (v != src->dir_checked)
	{
		SiftDirSnapshot now;

		sift_dir_read(src->index, &now);
		src->dir_current = true;
		for (int i = 0; i < src->dir.nsegs && src->dir_current; i++)
		{
			bool		found = src->dir.segs[i].kind != SIFT_SEG_IMMUTABLE;

			for (int j = 0; j < now.nsegs && !found; j++)
				found = now.segs[j].gen == src->dir.segs[i].gen;
			src->dir_current = found;
		}
		src->dir_checked = v;
	}
	return src->dir_current;
}

/* A key-cut unit's pages are anywhere in the heap: the all-visible bit of each is read from the map by block, in the order of the unit's pages (same rules as unit_allvisible) */
static const uint64 *
unit_allvisible_keyed(ColUnit *cu)
{
	if (!cu->av_ready)
	{
		uint32		nw;

		/* no page between the first and the last of the unit is unclean (the list of those pages is made once per scan): the directory is not needed */
		if (!cu->lane && sift_vis_range_allvisible(&cu->src->vis, cu->v.kblk0, cu->v.kblk1) &&
			!(cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src)))
		{
			cu->avk = NULL;
			cu->av_ready = true;
			return NULL;
		}
		sift_cunit_keyed_load(&cu->v);
		nw = (cu->v.npages + 63) / 64;
		cu->avk = MemoryContextAllocZero(cu->src->arena, Max(nw, 1) * sizeof(uint64));
		if (!cu->lane && cu->src->vis.heap != NULL)
			sift_vis_blocks_allvisible(&cu->src->vis, cu->v.blocks, cu->v.npages, cu->avk);
		if (cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src))
			memset(cu->avk, 0, Max(nw, 1) * sizeof(uint64));
		cu->av_ready = true;
	}
	return cu->avk;
}

const uint64 *
unit_allvisible(ColUnit *cu)
{
	if (cu->v.keyed)
		return unit_allvisible_keyed(cu);
	if (!cu->av_ready)
	{
		/* A lane batch is a copy of the records as they were when the source began, and a VACUUM since then may have flagged records deleted
		 * and made their pages all-visible: no page of a lane batch is taken as all-visible, so its rows are checked against the heap */
		if (!cu->lane)
			cu->src->visibility.sb_mask(cu->src->visibility.arg, cu->sb, cu->av);
		/* the directory is checked after the mask, as the liveness is */
		if (cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src))
			memset(cu->av, 0, sizeof(cu->av));
		cu->av_ready = true;
	}
	return cu->av;
}

/* The element structure of a group leaf of the unit (counts decoded from the group's chunk) */
ColGroup *
unit_group(ColUnit *cu, int group)
{
	ColGroup   *g = &cu->grp[group];

	if (!g->ready)
	{
		MemoryContext arena = cu->src->arena;
		uint32		n = cu->v.nrows;
		SiftCChunk	c;
		const SiftCLens *kept = cu->seg ? sift_cseg_lens_find(cu->seg, cu->u, group, arena) : NULL;
		uint32	   *lead;
		bool		have;

		lead = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n) + 1) * sizeof(uint32));
		g->counts = lead + 1;		/* the slot before the counts lets the running ends be one prefix sum over `lead` */
		g->eoff = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n + 1)) * sizeof(uint32));
		/* the chunk is still looked at (its pages are checked, its row count too), what is decoded from it is the cache's */
		if (kept && unit_chunk(cu, group, &c) && c.n == n)
		{
			for (uint32 i = 0; i < n; i++)
				g->counts[i] = kept->counts[i];
			sift_cck->prefix32(lead, n + 1, g->eoff, 0, 0);
			g->nelems = kept->nelems;
			g->nulls = c.nulls;
			g->ovf = c.ovf;
			g->ready = true;
			return g;
		}
		have = unit_chunk(cu, group, &c);
		if (have)
		{
			if (c.n != n)
				corrupt("The counts of group %d hold %u values for %u rows.", group, c.n, n);
			sift_cchunk_decode_u32(&c, g->counts);
			g->nulls = c.nulls;
			g->ovf = c.ovf;
			cu->src->cnt.bytes_decoded += c.paylen;
		}
		sift_cck->prefix32(lead, n + 1, g->eoff, 0, 0);
		{
			uint64		total = 0;		/* the running ends are 32 bit: a sum that wrapped would size the child arrays below the counts */

			for (uint32 i = 0; i < n; i++)
				total += g->counts[i];
			if (total != g->eoff[n])
				corrupt("The counts of group %d add up to more than %u elements.", group, PG_UINT32_MAX);
		}
		g->nelems = g->eoff[n];
		if (have && cu->seg)
			sift_cseg_lens_keep(cu->seg, cu->u, group, n, g->counts, g->nelems);
		g->ready = true;
	}
	return g;
}

bool
sift_col_run_info(const SiftColUnit *unit, int col, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	ColUnit    *cu = unit->priv;

	if (cu->seg == NULL)
		return false;
	if (!cu->run.known || cu->run.col != col)
	{
		cu->run.col = col;
		cu->run.known = true;
		cu->run.found = sift_cseg_run(cu->seg, col, cu->u, &cu->run.r, &cu->run.at);
	}
	if (cu->run.found)
	{
		*nvals = cu->run.r.nvals;
		*nnull = cu->run.r.nnull;
		*id = cu->seg->entry.gen << 28 | cu->u;
	}
	return cu->run.found;
}

bool
sift_col_run_identity(const SiftColUnit *unit, int col)
{
	ColUnit    *cu = unit->priv;

	SIFT_INVARIANT(cu->run.known && cu->run.found && cu->run.col == col, "sift_col: the kind of a sorted run read without its info");
	return cu->run.r.kind == SIFT_CRUN_IDENT;
}

/*
 * The head of the sorted run of col in the unit of superblock sb, when the unit need not be opened to know that every row of it is live and visible to the
 * snapshot: the only unit of the superblock (a write buffer holding rows of it is the caller's to rule out), of a segment with superunit summaries (the page
 * masks are there), with its pages all-visible and no dead row, in the order of col_super_take. False otherwise, then the unit is read.
 */
bool
sift_col_sb_heads(SiftColSource *src, uint32 sb, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi)
{
	SiftColSegment *seg = NULL;
	uint32		u = 0;
	uint64		av[SIFT_PAGEMASK_WORDS];
	SiftCSbHead h;

	if (src->flags & COLO_TABLE)
		return false;
	for (int i = 0; i < src->nseg; i++)
	{
		SiftColSegment *s = src->seg[i].seg;
		uint32		k;

		/* a segment cut by key or TID range numbers its units by sequence: the blocks its units reach say whether it holds rows of the superblock, and then it is not the only unit */
		if (sift_cseg_keyed(&s->entry))
		{
			if (sift_cseg_sb_in_spans(s, sb))
				return false;
			continue;
		}
		k = sift_cseg_find_unit(s, sb);
		if (k >= s->nsb || s->sbdir[k].sb != sb)
			continue;
		if (seg != NULL)
			return false;
		seg = s;
		u = k;
	}
	if (seg == NULL || !sift_cseg_sbhead(seg, u, col, &h))
		return false;
	src->visibility.sb_mask(src->visibility.arg, sb, av);
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		if (h.pm[w] & ~av[w])
			return false;
	if (!segments_current(src))
		return false;
	SIFT_COL_JITTER();
	if (!h.has || sift_cseg_dirty(seg, u))
		return false;
	*nlo = h.nlo;
	*nhi = h.nhi;
	memcpy(out, h.ent, (h.nlo + h.nhi) * sizeof(SiftCHeadEnt));
	return true;
}

int
sift_col_run_heads(const SiftColUnit *unit, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi)
{
	ColUnit    *cu = unit->priv;

	SIFT_INVARIANT(cu->run.known && cu->run.found && cu->run.col == col, "sift_col: a sorted run head read without its info");
	*nlo = *nhi = 0;
	if (cu->run.r.heads == 0)
		return 0;
	sift_crun_head_counts(cu->run.r.heads, cu->run.r.n, nlo, nhi);
	sift_cseg_run_heads(cu->seg, cu->run.at, &cu->run.r, out);
	return cu->run.r.heads;
}

void
sift_col_run_read(const SiftColUnit *unit, int col, uint32 pos, uint32 cnt, uint32 *ords)
{
	ColUnit    *cu = unit->priv;

	SIFT_INVARIANT(cu->run.known && cu->run.found && cu->run.col == col, "sift_col: a sorted run read without its info");
	sift_cseg_run_read(cu->seg, cu->run.at, &cu->run.r, pos, cnt, ords);
}

void
sift_col_zone(const SiftColUnit *unit, int col, SiftColZone *zone)
{
	ColUnit    *cu = unit->priv;
	SiftCZone	z;

	if (cu->seg)
		sift_cseg_zone(cu->seg, col, cu->u, &z);
	else
		z = cu->mzones[col];
	zone->min = z.flags & SIFT_COLZ_INLINE ? 0 : z.min;	/* an inline unit has no codes to bound */
	zone->max = z.flags & SIFT_COLZ_INLINE ? PG_INT64_MAX : z.max;
	zone->sum_i = z.sum_i;
	zone->sum_f = z.sum_f;
	zone->nvals = z.nvals;
	zone->nnull = z.nnull;
	zone->nosum = (z.flags & SIFT_COLZ_NOSUM) != 0;
	zone->has_nan = (z.flags & SIFT_COLZ_NAN) != 0;
	zone->is_const = (z.flags & SIFT_COLZ_CONST) != 0;
	zone->has_ovf = (z.flags & SIFT_COLZ_OVERFLOW) != 0;
}

static uint32
zone_range(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone, bool wbuf)
{
	uint32		units = 0;

	memset(zone, 0, sizeof(*zone));
	zone->min = PG_INT64_MAX;
	zone->max = PG_INT64_MIN;
	for (int i = 0; i < src->nseg; i++)
	{
		SiftColSegment *seg = src->seg[i].seg;
		uint32		end = sb_hi == PG_UINT32_MAX ? seg->nsb : sift_cseg_find_unit(seg, sb_hi);

		for (uint32 u = sift_cseg_find_unit(seg, sb_lo); u < end; u++, units++)
		{
			SiftCZone	z;

			sift_cseg_zone(seg, col, u, &z);
			zone->nvals += z.nvals;
			zone->nnull += z.nnull;
			zone->has_ovf |= (z.flags & SIFT_COLZ_OVERFLOW) != 0;
			if (z.nnull == z.nvals)
				continue;
			zone->min = Min(zone->min, z.flags & SIFT_COLZ_INLINE ? 0 : z.min);
			zone->max = Max(zone->max, z.flags & SIFT_COLZ_INLINE ? PG_INT64_MAX : z.max);
		}
	}
	if (!wbuf)
		return units;
	/* the units of the write buffers are encoded when read, and not loaded before a begin: they are read */
	if (!src->mem_loaded && !(src->flags & SIFT_COLO_NOWBUF))
		zone->has_ovf = true;
	for (int i = 0; i < src->nmem; i++)
		if (src->mem[i].b->nrows > 0 && src->mem[i].b->sb >= sb_lo && src->mem[i].b->sb < sb_hi)
		{
			zone->has_ovf = true;
			units++;
		}
	return units;
}

uint32
sift_col_zone_range(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone)
{
	return zone_range(src, col, sb_lo, sb_hi, zone, true);
}

/* The same over the segments alone, the units of the write buffers left out (sift_col_walk_stamp lists their superblocks) */
uint32
sift_col_zone_range_segs(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone)
{
	return zone_range(src, col, sb_lo, sb_hi, zone, false);
}

/*
 * What the zones of the segments of an index source depend on: its file and the version of the directory it read. The superblocks that hold rows of the
 * write buffers are listed in mem (ascending, nmem of them); false when the source is not that of an index, its buffers are not loaded, or more than cap
 * superblocks hold such rows.
 */
bool
sift_col_walk_stamp(SiftColSource *src, SiftColWalkStamp *st, uint32 *mem, int cap, int *nmem)
{
	int			n = 0;

	if ((src->flags & COLO_TABLE) || !src->mem_loaded)
		return false;
	for (int i = 0; i < src->nmem; i++)
		if (src->mem[i].b->nrows > 0)
		{
			if (n == cap)
				return false;
			mem[n++] = src->mem[i].b->sb;
		}
	qsort(mem, n, sizeof(uint32), sift_cmp_u32);
	*nmem = 0;
	for (int i = 0; i < n; i++)
		if (i == 0 || mem[i] != mem[i - 1])
			mem[(*nmem)++] = mem[i];
	st->index = RelationGetRelid(src->index);
	st->file = src->index->rd_locator.relNumber;
	st->version = src->dir.version;
	return true;
}

/* Every page the unit has rows on is all-visible: the mask is read first, as unit_clean does, and the unit's liveness after it */
bool
unit_pages_allvisible(ColUnit *cu)
{
	const uint64 *av = unit_allvisible(cu);

	if (cu->v.keyed)
	{
		if (av == NULL)			/* every page of the table is all-visible */
			return true;
		for (uint32 p = 0; p < cu->v.npages; p++)
			if (!sift_col_bit_test(av, p))
				return false;
		return true;
	}
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		if (cu->v.pagemask[w] & ~av[w])
			return false;
	return true;
}

bool
sift_col_unit_allvisible(const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;

	return unit_clean(cu) && unit_pages_allvisible(cu);
}

bool
sift_col_unit_exact(const SiftColUnit *unit)
{
	return sift_col_unit_allvisible(unit);
}

/* Every page the units [first, first + n) have rows on is all-visible: the page masks of their summary (the block ranges of key-cut units) against the snapshot, no unit record read */
static bool
units_allvisible(SiftColSource *src, SiftColSegment *seg, uint32 first, uint32 n)
{
	uint64		pm[SIFT_SUPER_MAX_FAN * SIFT_PAGEMASK_WORDS];
	uint64		av[SIFT_PAGEMASK_WORDS];

	sift_cseg_super_pagemask(seg, first / sift_cseg_super_fan(seg), n, pm);
	for (uint32 i = 0; i < n; i++)
	{
		if (sift_cseg_keyed(&seg->entry))
		{
			/* key-cut units: the block range of each, in its slot of the summary; a unit whose range holds a page that is not all-visible is read on its own */
			uint64		r = pm[i * SIFT_PAGEMASK_WORDS];

			if (!sift_vis_range_allvisible(&src->vis, (uint32) r, (uint32) (r >> 32)))
				return false;
			continue;
		}
		src->visibility.sb_mask(src->visibility.arg, seg->sbdir[first + i].sb, av);
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
			if (pm[i * SIFT_PAGEMASK_WORDS + w] & ~av[w])
				return false;
	}
	return true;
}

/* Every superblock of the segment is plain for the table (tbl_sb_mask: all pages or none): a table's segment has no summary of page masks to test */
static bool
sbs_plain(SiftColSource *src, SiftColSegment *seg)
{
	uint64		av[SIFT_PAGEMASK_WORDS];

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		src->visibility.sb_mask(src->visibility.arg, seg->sbdir[u].sb, av);
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
			if (av[w] != ~UINT64CONST(0))
				return false;
	}
	return true;
}

/* The units [first, first + n) have no dead row or exception; *rows: their rows */
static bool
units_clean(SiftColSegment *seg, uint32 first, uint32 n, uint32 *rows)
{
	for (uint32 i = 0; i < n; i++)
	{
		if (sift_cseg_dirty(seg, first + i))
			return false;
		*rows += seg->sbdir[first + i].nrows;
	}
	return true;
}

static void
take_units(ColCursor *c, ColUnit *cu, uint32 first, uint32 n, uint32 rows)
{
	SiftColSegment *seg = c->seg;

	cu->seg = seg;
	cu->u = first;
	cu->nsu = n;
	cu->sb = seg->sbdir[first].sb;
	cu->v.nrows = rows;
	cu->clean = cu->clean_ready = cu->av_ready = true;
	memset(cu->av, 0xFF, sizeof(cu->av));
	c->cur = first + n;
}

/*
 * The segment as one unit, for a scan that asks for it (sift_col_super_whole) and a segment that has the key table of its leaf: every unit of it exact on its
 * own, as col_super_take wants of a group, whatever the zone tests kept of them (the table answers the comparisons on the leaf itself, and the consumer checks
 * that those are the only ones). Its rows are the units' rows.
 */
static bool
col_whole_take(SiftColSource *src, ColCursor *c, ColUnit *cu)
{
	SiftColSegment *seg = c->seg;
	uint32		fan = sift_cseg_super_fan(seg),
				rows = 0;
	const SiftCHistDim *kd = sift_cseg_ktab(seg, src->whole - 1);
	bool		table = src->flags & COLO_TABLE;

	if (!c->full || c->prow != NULL || c->ptidv != NULL || (fan == 0 && !table) || kd == NULL)
		return false;
	for (int i = 0; i < src->nwmeas; i++)
	{
		int			m = 0;

		while (m < kd->nmeas && kd->meas[m] != src->wmeas[i])
			m++;
		if (m == kd->nmeas)
			return false;
	}
	if (table)
	{
		if (!sbs_plain(src, seg))
			return false;
	}
	else
	{
		for (uint32 u = 0; u < seg->nsb; u += fan)
			if (!units_allvisible(src, seg, u, Min(fan, seg->nsb - u)))
				return false;
		if (!segments_current(src))
			return false;
	}
	SIFT_COL_JITTER();
	if (!units_clean(seg, 0, seg->nsb, &rows))
		return false;
	take_units(c, cu, 0, seg->nsb, rows);
	cu->whole = true;
	return true;
}

/*
 * The range of keys that the comparisons of an integer leaf (sift_col_count_cmp) select, all on that leaf: false when a test is of another leaf or is not a range
 */
static bool
count_keys(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, int64 *lo, int64 *hi)
{
	const SiftColDef *def = &seg->schema->cols[src->pcnt - 1];

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	if (npreds == 0 || def->store == SIFT_COLS_STR || sift_colw_is_wide_int(def))
		return false;
	for (int i = 0; i < npreds; i++)
	{
		SiftCTest	t;

		if (preds[i].col != src->pcnt - 1)
			return false;
		make_test(src, def, NULL, &preds[i], &t);
		if (t.kind != SIFT_CT_RANGE)
			return false;
		*lo = Max(*lo, t.lo);
		*hi = Min(*hi, t.hi);
	}
	return true;
}

/*
 * A segment whose dictionary of the leaf of sift_col_count_range holds the rows of each string (option count_columns), or whose global order holds the keys of the
 * leaf of sift_col_count_cmp (order_array_columns: the rows of the range are the entries between its bounds), answers the count for all its units
 * at once when every one is exact on its own, as col_whole_take wants of a segment: every page all-visible, no dead row or exception, the directory current.
 * The checks come in the order of col_super_take (all-visible bits, directory, liveness). Then col_super_take hands the segment over as one unit.
 */
bool
col_count_segment(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds)
{
	SiftColSegment *seg = c->seg;
	uint32		fan = sift_cseg_super_fan(seg),
				rows = 0,
				entries,
				values;
	uint64		n;
	int64		lo = 0,
				hi = 0;

	if (fan == 0 || seg->nsb == 0)
		return false;
	if (src->pcnt_int)
	{
		if (!sift_cseg_gorder(seg, src->pcnt - 1, &entries, &values) || sift_cseg_gorder_group(seg, src->pcnt - 1) >= 0 || !count_keys(src, seg, preds, npreds, &lo, &hi))
			return false;
	}
	else if (!sift_cseg_dict_counted(seg, src->pcnt - 1))
		return false;
	for (uint32 u = 0; u < seg->nsb; u += fan)
		if (!units_allvisible(src, seg, u, Min(fan, seg->nsb - u)))
			return false;
	if (!(src->flags & COLO_TABLE) && !segments_current(src))
		return false;
	SIFT_COL_JITTER();
	if (!units_clean(seg, 0, seg->nsb, &rows))
		return false;
	if (src->pcnt_int)
	{
		uint32		m = sift_cseg_gorder_count(seg, src->pcnt - 1, lo, hi);

		if (m == UINT32_MAX)
			return false;
		n = m;
	}
	else if (!sift_cseg_dict_count(seg, src->pcnt - 1, src->pcnt_op, &src->pcnt_val, &n))
		return false;
	c->count_ok = true;
	c->count_rows = n;
	c->count_unit_rows = rows;
	return true;
}

/*
 * The group of units at the cursor as one unit. Every unit of the group must have passed the zone tests and be exact on its own: its pages that hold rows
 * all-visible (the masks of the units are in the summary, so no unit record is read) and no dead row, read in the order of unit_allvisible and
 * unit_clean: the all-visible bits first, then the directory, then the liveness.
 */
bool
col_super_take(SiftColSource *src, ColCursor *c, ColUnit *cu)
{
	SiftColSegment *seg = c->seg;
	uint32		fan = sift_cseg_super_fan(seg),
				n,
				rows = 0;

	if (c->count_ok && src->pcnt)
	{
		c->count_ok = false;
		take_units(c, cu, 0, seg->nsb, c->count_unit_rows);
		cu->counted = true;
		cu->crows = c->count_rows;
		return true;
	}
	if (src->whole && col_whole_take(src, c, cu))
		return true;
	if (!src->super)
		return false;
	if (fan == 0 || c->cur % fan != 0 || c->cur < c->nosuper_to)
		return false;
	n = Min(fan, seg->nsb - c->cur);
	if (n < 2 || c->cur + n > c->end)
		return false;
	for (uint32 i = 0; i < n; i++)
		if (!sift_col_bit_test(c->keep, c->cur + i))
			return false;
	if (!units_allvisible(src, seg, c->cur, n))
		return false;
	if (!(src->flags & COLO_TABLE) && !segments_current(src))
		return false;
	SIFT_COL_JITTER();
	if (!units_clean(seg, c->cur, n, &rows))
		return false;
	take_units(c, cu, c->cur, n, rows);
	return true;
}

bool
sift_col_unit_super(const SiftColUnit *unit, SiftColSegment **seg, uint32 *k)
{
	ColUnit    *cu = unit->priv;

	if (cu->nsu == 0 || cu->whole || cu->counted)
		return false;
	*seg = cu->seg;
	*k = cu->u / sift_cseg_super_fan(cu->seg);
	return true;
}

bool
sift_col_unit_counted(const SiftColUnit *unit, uint64 *rows)
{
	ColUnit    *cu = unit->priv;

	*rows = cu->crows;
	return cu->counted;
}

bool
sift_col_unit_whole(const SiftColUnit *unit, SiftColSegment **seg)
{
	ColUnit    *cu = unit->priv;

	*seg = cu->seg;
	return cu->whole;
}

/* ---------- pre-aggregates (col/pagg.c, docs/COL-PREAGG.md) ---------- */

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar */
bool
sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u)
{
	ColUnit    *cu = unit->priv;

	if (cu->seg == NULL || cu->nsu > 0)
		return false;
	*seg = cu->seg;
	*u = cu->u;
	return true;
}

/* ---------- the Vulkan path (col/gpu.h, docs/GPU.md) ---------- */

bool
sift_col_unit_gpu(const SiftColUnit *unit, SiftColGpuUnit *g)
{
	ColUnit    *cu = unit->priv;

	if (cu->seg == NULL || !sift_col_unit_exact(unit))
		return false;
	g->store = (uint64) cu->seg->index->rd_locator.dbOid << 32 | cu->seg->index->rd_locator.relNumber;
	g->gen = cu->seg->entry.gen;
	g->unit = cu->u;
	g->nrows = cu->v.nrows;
	return true;
}

bool
sift_col_unit_gpu_dense(const SiftColUnit *unit, int col)
{
	SiftColZone z;

	sift_col_zone(unit, col, &z);
	return !z.has_ovf && z.nnull == 0 && z.nvals == unit->nrows;
}

bool
sift_col_unit_gpu_chunk(const SiftColUnit *unit, int col, SiftColGpuChunk *c)
{
	ColUnit    *cu = unit->priv;
	SiftCChunk	ch;

	if (cu->seg == NULL || !sift_col_unit_gpu_dense(unit, col) || !unit_chunk_raw(cu, col, &ch))
		return false;
	if (ch.nulls != NULL || ch.ovf != NULL || ch.n != cu->v.nrows)
		return false;
	if (ch.enc == SIFT_COLENC_CONST)
	{
		*c = (SiftColGpuChunk) {.n = ch.n, .is_const = true, .cval = ch.base};
		return true;
	}
	if (ch.enc != SIFT_COLENC_CC)
		return false;
	*c = (SiftColGpuChunk) {.blob = ch.payload, .len = ch.paylen, .n = ch.n};
	return true;
}

void
sift_col_unit_gpu_dict(const SiftColUnit *unit, int col, SiftColDict *dict)
{
	ColUnit    *cu = unit->priv;

	sift_cdict_view(sift_cseg_dict(cu->seg, col), dict_id(cu, col), dict);
}
