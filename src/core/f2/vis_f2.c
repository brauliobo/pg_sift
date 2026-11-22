/*
 * core/vis_f2.c - the heap's visibility map for the rows of a granule of format major 2 (core/vis_f2.h).
 */
/* check-memory: allow-allocs every array here follows the rows of one granule (at most 65,535), not the rows of the table */
#include "core/host.h"

#include "col/f2/col_meta.h"
#include "col/f2/delvec_delta.h"
#include "core/popcount.h"
#include "core/f2/vis_f2.h"

void
sift_vis_f2_begin(SiftVisF2Guard *g, Relation index, uint64 manifest_ver)
{
	memset(g, 0, sizeof(*g));
	g->index = index;
	g->manifest_ver = manifest_ver;
}

void
sift_vis_f2_part(SiftVisF2Guard *g, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	g->part_id = e->part_id;
	memcpy(g->key_lo, e->key_lo, SIFT_BOUND_BYTES);
	g->part_gen = d->part_gen;
	g->dv_gen = d->dv_gen;
	g->dv_blk = d->dv_blk;
	g->dv_len = sift_dvd_part_takes(d) ? sift_dv_stream_len(g->index, d->dv_blk) : 0;
	g->moved = false;
}

/* The part is still in the manifest with the deletion vector the scan read */
static bool
part_unchanged(const SiftVisF2Guard *g)
{
	SiftPartitionEntry e;
	int			n;
	SiftPartDesc *parts;
	bool		same = false;

	if (!sift_manifest_find_entry(g->index, g->part_id, g->key_lo, &e))
		return false;
	parts = sift_manifest_read_parts(g->index, &e, &n);
	for (int i = 0; i < n && !same; i++)
		same = parts[i].part_gen == g->part_gen && parts[i].dv_gen == g->dv_gen;
	pfree(parts);
	return same;
}

/* Called after the map was read: a change of the manifest that leaves the part as it was (an insert, the compaction of another part) does not void the map */
static bool
part_current(SiftVisF2Guard *g)
{
	uint64		now;

	if (g->moved)
		return false;
	g->moved = g->dv_len != 0 && sift_dv_stream_len(g->index, g->dv_blk) != g->dv_len;
	pg_read_barrier();
	now = sift_col_meta_read(g->index).manifest_version;
	if (!g->moved && now != g->manifest_ver)
	{
		g->moved = !part_unchanged(g);
		g->manifest_ver = now;
	}
	return !g->moved;
}

static uint16
rows_left(const uint64 *sel, uint16 nrows)
{
	uint16		n = 0;

	for (int w = 0; w < (nrows + 63) / 64; w++)
		n += sift_pc64(sel[w]);
	return n;
}

typedef struct Cand
{
	BlockNumber blk;
	uint16		row;
} Cand;

static int
cand_cmp(const void *a, const void *b)
{
	const Cand *x = a,
			   *y = b;

	if (x->blk != y->blk)
		return x->blk < y->blk ? -1 : 1;
	return x->row < y->row ? -1 : x->row > y->row;
}

typedef struct DirtyBlocks
{
	const BlockNumber *blks;
	int			n;
	int			i;
} DirtyBlocks;

static BlockNumber
dirty_next(void *arg)
{
	DirtyBlocks *d = arg;

	return d->i < d->n ? d->blks[d->i++] : InvalidBlockNumber;
}

/* The rows of the run [from, to) of cand, all on one page the map does not call all-visible, asked of the heap */
static void
filter_run(SiftVis *v, const uint64 *tids, const Cand *cand, int from, int to, uint64 *sel)
{
	uint64		offmask[SIFT_OFFMASK_WORDS] = {0};

	for (int i = from; i < to; i++)
	{
		uint32		off = tids[cand[i].row] & 0xFFFF;

		offmask[(off - 1) / 64] |= UINT64CONST(1) << ((off - 1) % 64);
	}
	(void) sift_vis_filter_page(v, cand[from].blk, offmask);
	for (int i = from; i < to; i++)
	{
		uint32		off = tids[cand[i].row] & 0xFFFF;

		if (!((offmask[(off - 1) / 64] >> ((off - 1) % 64)) & 1))
			sel[cand[i].row >> 6] &= ~(UINT64CONST(1) << (cand[i].row & 63));
	}
}

static void
filter_rows(SiftVis *v, SiftVisF2Guard *g, const uint64 *tids, uint16 nrows, uint64 *sel)
{
	Cand	   *cand = palloc(Max(nrows, 1) * sizeof(Cand));
	BlockNumber *blks = palloc(Max(nrows, 1) * sizeof(BlockNumber));
	BlockNumber *dirty = palloc(Max(nrows, 1) * sizeof(BlockNumber));
	uint64	   *bits;
	int			n = 0,
				ndirty = 0;
	DirtyBlocks db;
	bool		own_stream;

	for (int w = 0; w < (nrows + 63) / 64; w++)
		for (uint64 m = sel[w]; m; m &= m - 1)
		{
			int			r = w * 64 + __builtin_ctzll(m);

			cand[n++] = (Cand) {(BlockNumber) (tids[r] >> 16), r};
		}
	qsort(cand, n, sizeof(Cand), cand_cmp);
	for (int i = 0; i < n; i++)
		blks[i] = cand[i].blk;
	bits = palloc(((n + 63) / 64 + 1) * sizeof(uint64));
	sift_vis_blocks_allvisible(v, blks, n, bits);
	if (!part_current(g))
		memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
	for (int i = 0; i < n; i++)
		if (!((bits[i / 64] >> (i % 64)) & 1) && (i == 0 || cand[i].blk != cand[i - 1].blk))
			dirty[ndirty++] = cand[i].blk;
	db = (DirtyBlocks) {dirty, ndirty, 0};
	own_stream = v->stream == NULL && ndirty > 1;
	if (own_stream)
		sift_vis_stream_begin(v, dirty_next, &db, false);
	for (int i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && cand[j].blk == cand[i].blk; j++)
			;
		if (!((bits[i / 64] >> (i % 64)) & 1))
			filter_run(v, tids, cand, i, j, sel);
	}
	if (own_stream)
		sift_vis_stream_end(v);
}

uint16
sift_vis_f2_granule_filter(SiftVis *v, SiftVisF2Guard *g, uint64 tid_min, uint64 tid_max, const uint64 *tids, uint16 nrows, uint64 *sel)
{
	if (!(sift_vis_range_allvisible(v, (BlockNumber) (tid_min >> 16), (BlockNumber) (tid_max >> 16)) && part_current(g)))
		filter_rows(v, g, tids, nrows, sel);
	return rows_left(sel, nrows);
}
