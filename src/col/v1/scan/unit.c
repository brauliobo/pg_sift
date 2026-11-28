/*
 * col/v1/scan/unit.c - the unit of the column source: chunks, dictionaries, keys, all-visible and liveness state, groups, zones.
 */

#include "postgres.h"

#include "port/pg_bitutils.h"

#include "access/xact.h"
#include "utils/rel.h"

#include "col/am.h"
#include "core/cmp.h"
#include "col/v1/pagg.h"
#include "col/v1/seg/seg.h"
#include "core/v1_col/tomb/tomb.h"
#include "core/visibility/visibility.h"
#include "col/v1/scan/int.h"
#include "col/v1/wbuf.h"

/* The chunk of leaf col of the unit as stored; false when the unit has none. A stored unit reads its bytes on first use. */
bool
unit_chunk_raw(ColUnit *cu, int col, SiftCChunk *c)
{
	if (cu->loaded && !cu->loaded[col] && cu->v.chunk_len[col] > 0)
	{
		sift_cseg_unit_fill(cu->seg, cu->u, cu->v.chunk_off[col], cu->v.chunk_len[col]);
		cu->loaded[col] = 1;
		if (cu->src->chunk_units == NULL)
			cu->src->chunk_units = MemoryContextAllocZero(cu->src->mcxt, cu->seg->schema->ncols * sizeof(uint32));
		cu->src->chunk_units[col]++;
	}
	return sift_cunit_chunk(&cu->v, col, c);
}

/*
 * The same, an inline STR chunk as codes of a dictionary of its own (unit_dict, dict_id): its strings are interned on first use, for the positions
 * idx[0..m) of a selected read only (NULL: all). A chunk interned for fewer positions than a later read wants is interned again, under a new
 * dictionary id: the vectors read from the first form keep theirs.
 */
bool
unit_chunk_sel(ColUnit *cu, int col, const uint32 *idx, uint32 m, SiftCChunk *c)
{
	if (!unit_chunk_raw(cu, col, c))
		return false;
	if (c->enc == SIFT_COLENC_ISTR)
	{
		ColInline  *in;

		/* only a unit with an inline string chunk needs the table: it is not zeroed for every unit of every scan */
		if (cu->istr == NULL)
			cu->istr = MemoryContextAllocZero(cu->src->arena, cu->src->schema->ncols * sizeof(ColInline));
		in = &cu->istr[col];

		if (in->dict != NULL && in->have != NULL)
		{
			bool		all = idx != NULL;

			for (uint32 k = 0; all && k < m; k++)
				all = sift_col_bit_test(in->have, idx[k]);
			if (!all)
				in->dict = NULL;
		}
		if (in->dict == NULL)
		{
			sift_cchunk_istr_codes(c, cu->src->arena, idx, m, &in->dict, &in->chunk);
			in->id = INL_ID_FLAG | ++cu->src->inl_seq << 8 | col;
			in->have = NULL;
			if (idx != NULL)
			{
				in->have = MemoryContextAllocZero(cu->src->arena, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
				for (uint32 k = 0; k < m; k++)
					in->have[idx[k] >> 6] |= UINT64CONST(1) << (idx[k] & 63);
			}
		}
		*c = in->chunk;
	}
	return true;
}

bool
unit_chunk(ColUnit *cu, int col, SiftCChunk *c)
{
	return unit_chunk_sel(cu, col, NULL, 0, c);
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
	return cu->istr != NULL ? cu->istr[col].dict : NULL;
}

/*
 * Whether every immutable segment the source opened is still in the directory. A merge replaces its inputs with an output that has the liveness
 * of its moment; the VACUUM passes after it flag the output, never the inputs, which a scan holding the older directory copy still reads. Such a
 * scan must not take "live on an all-visible page" for "visible": the all-visible bit may have been set by a pass that never reached its
 * segments. While the segments are current, every pass that finished before the mask was read ran on them (a pass and a merge exclude each
 * other by the maintenance lock) and the liveness read after the mask shows it.
 */
bool
segments_current(SiftColSource *src)
{
	uint32		v = sift_dir_version_peek(src->dirbuf);

	if (v != src->dir_checked)
	{
		SiftDirSnapshot *now = sift_dir_read_segs(src->index);

		src->dir_current = true;
		for (int i = 0; i < src->dir->nsegs && src->dir_current; i++)
		{
			bool		found = src->dir->segs[i].kind != SIFT_SEG_IMMUTABLE;

			for (int j = 0; j < now->nsegs && !found; j++)
				found = now->segs[j].gen == src->dir->segs[i].gen;
			src->dir_current = found;
		}
		pfree(now);
		src->dir_checked = v;
	}
	return src->dir_current;
}

/* A source opens current at the directory version it read, and pins the directory page that segments_current peeks (a table source has none) */
void
segments_pin(SiftColSource *src, uint32 flags)
{
	src->dir_checked = src->dir->version;
	src->dir_current = true;
	src->dirbuf = (flags & COLO_TABLE) ? InvalidBuffer : sift_dir_pin(src->index);
}

#ifdef SIFT_TESTING
bool		sift_test_no_heapfree = false;
bool		sift_test_hf_ignore_tombs = false;
bool		sift_test_hf_ignore_stamp = false;
#endif

/* The masks, ranges and blocks the log made all-visible where the map said no (tests, sift._test_hf_taken) */
int64		sift_col_hf_taken;

/* The heap pages the scans asked the log about (tests, sift._test_hf_pages_asked) */
int64		sift_col_hf_asked;

/*
 * Opens the log before the first segment or liveness page is read: a tombstone is dropped by the VACUUM that cleared the entry it names, after it cleared it, so a
 * source that took the log first and the liveness after sees the tombstone or the clear bit. Only an index that tracks deletes and a snapshot of the usual kind: the
 * serializable ones record what they read from each heap tuple (CheckForSerializableConflictOut), which a read of the index alone does not.
 */
void
col_hf_open(SiftColSource *src)
{
	src->hflog = NULL;
	src->hf = 0;
	memset(&src->hfmemo, 0, sizeof(src->hfmemo));
#ifdef SIFT_TESTING
	if (sift_test_no_heapfree)
		return;
#endif
	if ((src->flags & COLO_TABLE) || IsolationIsSerializable() || src->snap->snapshot_type != SNAPSHOT_MVCC || !TransactionIdIsNormal(src->snap->xmin) ||
		!sift_col_track_on(src->index))
		return;
	src->hflog = sift_col_log(src->index);
	if (src->hflog != NULL)
		src->hfgen = src->hflog->gen;
}

/* Do tombstones delete rows of the blocks for the snapshot (a block of them to be checked in the heap) */
bool
col_hf_deleted(SiftColSource *src, BlockNumber first, uint32 nblocks, uint64 *mask)
{
#ifdef SIFT_TESTING
	if (sift_test_hf_ignore_tombs)
		return false;
#endif
	sift_col_hf_asked += nblocks;
	return sift_tomb_deleted_blocks(src->hflog, src->snap, first, nblocks, mask, &src->hfmemo);
}

/* May the log be taken for complete for the snapshot (decided on the first need, one catalog read), and is it the log read at open */
static bool
hf_on(SiftColSource *src)
{
	if (src->hflog == NULL)
		return false;
	if (src->hf == 0)
	{
		src->hf = sift_col_track_ok(src->index, src->heap, src->snap) ? 1 : -1;
		/* the heap is checked for a log that the worker may re-baseline (it asks only for such a log) */
		if (src->hf < 0)
			sift_col_track_ask(src->index, src->heap);
	}
	return src->hf > 0 && src->hflog->gen == src->hfgen;
}

/* Did every inserter of the rows (stamp: none when not known) commit before the snapshot's xmin, and may the log be taken for complete */
static bool
hf_stamped(SiftColSource *src, TransactionId stamp)
{
	if (src->hflog == NULL)
		return false;
#ifdef SIFT_TESTING
	if (sift_test_hf_ignore_stamp)
		return hf_on(src);
#endif
	return TransactionIdIsNormal(stamp) && TransactionIdPrecedesOrEquals(stamp, src->snap->xmin) && hf_on(src);
}

/* ... for the entries of a stored segment */
static bool
hf_seg(SiftColSource *src, const SiftColSegment *seg)
{
	TransactionId stamp = InvalidTransactionId;

	if (seg == NULL)
		return false;
	(void) sift_seg_xstamp(&seg->entry, &stamp);
	return hf_stamped(src, stamp);
}

bool
col_hf_unit(SiftColSource *src, const ColUnit *cu)
{
	return cu->lane ? hf_stamped(src, cu->xstamp) : hf_seg(src, cu->seg);
}

/*
 * The rows of block blk in offmask that the snapshot sees, for a stored segment that is stamped (hf_seg): all but those a tombstone deletes for the snapshot. No heap
 * page is read. false, offmask untouched, when the block has more tombstones than are worth reading (the caller asks the heap).
 */
bool
col_hf_filter_page(SiftColSource *src, BlockNumber blk, uint64 *offmask, int *left)
{
	uint64		gone[SIFT_OFFMASK_WORDS] = {0};
	int			n = 0;

	if (!col_hf_block_gone(src, blk, gone))
		return false;
	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
	{
		offmask[w] &= ~gone[w];
		n += pg_popcount64(offmask[w]);
	}
	*left = n;
	return true;
}

/* Adds to av, read from the map, the pages of the superblock that no tombstone deletes from (the log is read after the map, as the liveness is) */
static void
sb_add_log(SiftColSource *src, const SiftColSegment *seg, uint32 sb, uint64 *av)
{
	if (hf_seg(src, seg))
	{
		uint64		bad[SIFT_PAGEMASK_WORDS] = {0};

		col_hf_deleted(src, sb * SIFT_SB_PAGES, SIFT_SB_PAGES, bad);
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
			av[w] |= ~bad[w];
		sift_col_hf_taken++;
	}
}

/* Does no tombstone delete a row of page blk for the snapshot, in a stored segment that is stamped: the one page of sb_add_log, for a read that takes a few pages of many superblocks */
bool
col_hf_page_clear(SiftColSource *src, const SiftColSegment *seg, BlockNumber blk)
{
	if (!hf_seg(src, seg) || col_hf_block_any(src, blk))
		return false;
	sift_col_hf_taken++;
	return true;
}

/*
 * Is every page of pm all-visible for the snapshot: by the visibility map, or by the log for the pages it does not hold (none of them has a tombstone). The tombstones of the
 * superblock are walked once and the first on one of those pages ends it, where sb_add_log reads every tombstone of the superblock. vm: the map as an earlier read of the caller has it, NULL to
 * read it now.
 */
static bool
col_sb_covers(SiftColSource *src, const SiftColSegment *seg, uint32 sb, const uint64 *vm, const uint64 *pm)
{
	uint64		mine[SIFT_PAGEMASK_WORDS],
				lacking[SIFT_PAGEMASK_WORDS];
	const uint64 *av = vm;
	bool		missing = false;

	if (av == NULL)
	{
		src->visibility.sb_mask(src->visibility.arg, sb, mine);
		av = mine;
	}
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		missing |= (pm[w] & ~av[w]) != 0;
	if (!missing)
		return true;
	if (!hf_seg(src, seg))
		return false;
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		lacking[w] = pm[w] & ~av[w];
#ifdef SIFT_TESTING
	if (!sift_test_hf_ignore_tombs)
#endif
		if (sift_tomb_deleted_in(src->hflog, src->snap, sb * SIFT_SB_PAGES, SIFT_SB_PAGES, lacking, &src->hfmemo))
			return false;
	sift_col_hf_taken++;
	return true;
}

static bool
col_range_allvisible(SiftColSource *src, const SiftColSegment *seg, BlockNumber lo, BlockNumber hi)
{
	if (sift_vis_range_allvisible(&src->vis, lo, hi))
		return true;
	if (!hf_seg(src, seg) || col_hf_deleted(src, lo, hi - lo + 1, NULL))
		return false;
	sift_col_hf_taken++;
	return true;
}

void
col_blocks_allvisible(SiftColSource *src, const SiftColSegment *seg, const BlockNumber *blocks, uint32 n, uint64 *bits)
{
	sift_vis_blocks_allvisible(&src->vis, blocks, n, bits);
	if (hf_seg(src, seg))
		for (uint32 i = 0; i < n; i++)
			if (!(bits[i / 64] >> (i % 64) & 1) && !col_hf_block_any(src, blocks[i]))
			{
				bits[i / 64] |= UINT64CONST(1) << (i % 64);
				sift_col_hf_taken++;
			}
}

/* A key-cut unit's pages are anywhere in the heap: the all-visible bit of each is read from the map by block, in the order of the unit's pages (same rules as unit_allvisible) */
static const uint64 *
unit_allvisible_keyed(ColUnit *cu)
{
	if (!cu->av_ready)
	{
		uint32		nw;

		/* no page between the first and the last of the unit is unclean (the list of those pages is made once per scan): the directory is not needed */
		if (!cu->lane && col_range_allvisible(cu->src, cu->seg, cu->v.kblk0, cu->v.kblk1) &&
			!(cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src)))
		{
			cu->avk = NULL;
			cu->av_ready = true;
			return NULL;
		}
		cu->marked = !cu->lane && hf_seg(cu->src, cu->seg);
		sift_cunit_keyed_load(&cu->v);
		nw = (cu->v.npages + 63) / 64;
		cu->avk = MemoryContextAllocZero(cu->src->arena, Max(nw, 1) * sizeof(uint64));
		if (!cu->lane && cu->src->vis.heap != NULL)
			col_blocks_allvisible(cu->src, cu->seg, cu->v.blocks, cu->v.npages, cu->avk);
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
		{
			memcpy(cu->av, unit_allvisible_vm(cu), sizeof(cu->av));
			sb_add_log(cu->src, cu->seg, cu->sb, cu->av);
			for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
				cu->marked |= (cu->v.pagemask[w] & ~cu->av[w]) != 0 && hf_seg(cu->src, cu->seg);
		}
		/* the directory is checked after the mask, as the liveness is */
		if (cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src))
			memset(cu->av, 0, sizeof(cu->av));
		cu->av_ready = true;
	}
	return cu->av;
}

/*
 * The all-visible pages of the unit's superblock by the visibility map alone (a key-cut unit: its own mask). It is read before the liveness (unit_clean). A read of a few rows
 * asks the log about the pages of those rows (col_hf_filter_page, a bisection each), where unit_allvisible asks it about the 256 pages of the superblock and so reads every
 * tombstone of the range.
 */
const uint64 *
unit_allvisible_vm(ColUnit *cu)
{
	if (cu->v.keyed)
		return unit_allvisible(cu);
	if (!cu->avvm_ready)
	{
		if (!cu->lane)
			cu->src->visibility.sb_mask(cu->src->visibility.arg, cu->sb, cu->avvm);
		if (cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src))
			memset(cu->avvm, 0, sizeof(cu->avvm));
		cu->avvm_ready = true;
	}
	return cu->avvm;
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

/* Every page the unit has rows on is all-visible: the mask is read first, as unit_clean does, and the unit's liveness after it */
bool
unit_pages_allvisible(ColUnit *cu)
{
	const uint64 *av;

	/* the answer alone, with no mask kept: the log is asked about the pages the map lacks, not about the whole superblock (col_sb_covers) */
	if (!cu->v.keyed && !cu->av_ready && !cu->lane && !(cu->seg && !(cu->src->flags & COLO_TABLE) && !segments_current(cu->src)))
		return col_sb_covers(cu->src, cu->seg, cu->sb, unit_allvisible_vm(cu), cu->v.pagemask);
	av = unit_allvisible(cu);

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

/*
 * The rows of the stored segment of the unit whose string of leaf col passes op against c, from the row counts of its dictionary (option count_columns): every unit of the
 * segment exact (every page all-visible by the page masks of its summary, then the directory current, then no dead row or exception), as the pre-aggregates of a unit want. false: the
 * segment is read.
 */
bool
sift_col_unit_seg_count(const SiftColUnit *unit, int col, SiftColOp op, const SiftColValue *c, uint64 *rows)
{
	ColUnit    *cu = unit->priv;
	SiftColSource *src = cu->src;
	SiftColSegment *seg = cu->seg;
	uint64		pm[SIFT_PAGEMASK_WORDS];

	if (seg == NULL || src->sx_on || (src->flags & COLO_TABLE) || !sift_cseg_dict_counted(seg, col))
		return false;
	for (uint32 u = 0; u < seg->nsb; u++)
		if (!sift_cseg_unit_pagemask(seg, u, pm) || (sift_cseg_keyed(&seg->entry) ? !col_range_allvisible(src, seg, (uint32) pm[0], (uint32) (pm[0] >> 32)) :
													  !col_sb_covers(src, seg, sift_cseg_unit_sb(seg, u), NULL, pm)))
			return false;
	return segments_current(src) && !sift_cseg_dirty_any(seg, 0, seg->nsb) && sift_cseg_dict_count(seg, col, op, c, rows);
}

/* ---------- pre-aggregates (col/v1/pagg.c, docs/COL-PREAGG.md) ---------- */

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
