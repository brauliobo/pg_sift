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

/* ---------- unit helpers ---------- */

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
 * other by the maintenance lock) and the liveness read after the mask shows it (docs/COL.md 3, "Order of reads").
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

const uint64 *
unit_allvisible(ColUnit *cu)
{
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
		uint32	   *lead = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n) + 1) * sizeof(uint32));

		g->counts = lead + 1;		/* the slot before the counts lets the running ends be one prefix sum over `lead` */
		g->eoff = MemoryContextAllocZero(arena, (SIFT_CC_CAP32(n + 1)) * sizeof(uint32));
		if (unit_chunk(cu, group, &c))
		{
			if (c.n != n)
				corrupt("The counts of group %d hold %u values for %u rows.", group, c.n, n);
			sift_cchunk_decode_u32(&c, g->counts);
			g->nulls = c.nulls;
			g->ovf = c.ovf;
			cu->src->cnt.bytes_decoded += c.paylen;
		}
		sift_cck->prefix32(lead, n + 1, g->eoff, 0, 0);
		g->nelems = g->eoff[n];
		g->ready = true;
	}
	return g;
}

/* ---------- zones ---------- */

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

uint32
sift_col_zone_range(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone)
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

bool
sift_col_unit_allvisible(const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;
	const uint64 *av = unit_allvisible(cu);

	if (!unit_clean(cu))
		return false;
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		if (cu->v.pagemask[w] & ~av[w])
			return false;
	return true;
}

bool
sift_col_unit_exact(const SiftColUnit *unit)
{
	return sift_col_unit_allvisible(unit);
}

/* ---------- pre-aggregates (col/pagg.c, docs/COL-PREAGG.md) ---------- */

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar */
bool
sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u)
{
	ColUnit    *cu = unit->priv;

	if (cu->seg == NULL)
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
