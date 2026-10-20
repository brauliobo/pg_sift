/*
 * bm25/liveness.c - segment liveness: live rows and bits, counts, the dead postings of each term, VACUUM and deletes of single docs.
 */
#include "sift_host.h"

#include "bm25/kernels.h"
#include "sift_live.h"
#include "sift_popcount.h"
#include "bm25/segment.h"

#include "bm25/segment_int.h"

#define PAGE_RANK_WORDS	((MAXOFF + 63) / 64)	/* words of rank bits of a page of docs */

/* ---------- liveness, format 2 ---------- */

Live2 *
live2(SiftSegment *seg)
{
	Live2	   *l2 = seg->l2;
	SiftLive	l;

	if (l2)
		return l2;
	l = seg_live(seg);
	l2 = MemoryContextAllocZero(seg->mcxt, sizeof(Live2));
	sift_live2_header(&l, &l2->h);
	seg->l2 = l2;
	return l2;
}

/* The row of the page in a group of bitmaps, NULL when the page is not in it */
static const uint8 *
grp_row(const sift_grp *g, int page)
{
	int			wd = page / 64,
				idx = 0;

	if (!(g->pages[wd] >> (page % 64) & 1))
		return NULL;
	for (int j = 0; j < wd; j++)
		idx += sift_pc64(g->pages[j]);
	idx += sift_pc64(g->pages[wd] & ((UINT64CONST(1) << (page % 64)) - 1));
	return g->off + (Size) idx * g->w;
}

/*
 * The live offsets of the SB's pages, NULL for an SB without docs. They are kept until the reader asks for another SB, like the
 * dirty-SB bitmap: a doc vacuumed meanwhile is dead to every snapshot, so the heap check refuses it all the same. The group is
 * valid until the next call on seg.
 */
const sift_grp *
live2_rows(SiftSegment *seg, uint32 sb)
{
	Live2	   *l2 = live2(seg);

	if (!l2->rok || l2->rsb != sb)
	{
		SiftLive	l = seg_live(seg);
		SiftLive2RegionData *r;
		uint64		len;

		l2->rok = false;
		if (sb < seg->hdr->sb_lo || sb > seg->hdr->sb_hi)
			return NULL;
		l2->rstart = sift_live2_region(&l, &l2->h, sb - seg->hdr->sb_lo, &len);
		if (len == 0)
			return NULL;
		if (len < sizeof(*r) || len > sizeof(*r) + SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64))
			corrupt(seg, "Live rows of SB %u span %llu bytes.", sb, ULL(len));
		if (l2->rcap < len + 24)
		{
			l2->rcap = Max(len + 24, 4096);
			if (l2->rbuf)
				pfree(l2->rbuf);
			l2->rbuf = MemoryContextAllocAligned(seg->mcxt, l2->rcap, SIFT_SIMD_ALIGN, 0);
		}
		sift_live_copy(&l, l2->rstart, len, l2->rbuf + 24);
		r = (SiftLive2RegionData *) (l2->rbuf + 24);
		if (r->w == 0 || r->w % 8 != 0 || r->w > SIFT_OFFMASK_WORDS * 8 || r->np == 0 || r->np > SIFT_SB_PAGES ||
			len != sizeof(*r) + (uint64) r->np * r->w ||
			r->np != (uint32) (sift_pc64(r->pages[0]) + sift_pc64(r->pages[1]) + sift_pc64(r->pages[2]) + sift_pc64(r->pages[3])))
			corrupt(seg, "Live rows of SB %u: %u rows of %u bytes in %llu bytes.", sb, r->np, r->w, ULL(len));
		l2->grp = (sift_grp) {.gid = sb,.w = r->w,.off = l2->rbuf + 24 + sizeof(*r)};
		memcpy(l2->grp.pages, r->pages, sizeof(r->pages));
		l2->rsb = sb;
		l2->rok = true;
	}
	return &l2->grp;
}

static int
live2_mask(SiftSegment *seg, uint32 sb, int page, uint64 *mask)
{
	const sift_grp *g = live2_rows(seg, sb);
	const uint64 *row = g ? (const uint64 *) grp_row(g, page) : NULL;
	int			n = 0;

	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		n += sift_pc64(mask[k] &= row && k < (int) (g->w / 8) ? row[k] : 0);
	return n;
}

static uint64
live2_count(SiftSegment *seg, const sift_grp *g, const uint64 *pages)
{
	const sift_grp *rows = live2_rows(seg, g->gid);
	const uint8 *off = g->off;
	uint64		n = 0;

	if (rows == NULL)
		return 0;
	if (memcmp(pages, g->pages, sizeof(g->pages)) == 0)
		return sift_grp_and_count(g, rows);
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			bit = pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (pages[wd] >> bit & 1)
			{
				memcpy(mask, off, g->w);
				n += live2_mask(seg, g->gid, wd * 64 + bit, mask);
			}
		}
	return n;
}

static void
live2_cleared(SiftSegment *seg, int nd, uint64 dead_dl)
{
	Live2	   *l2 = live2(seg);

	l2->rok = false;
	l2->h.ndead += nd;
	l2->h.dead_dl += dead_dl;
}

/* ---------- liveness ---------- */

/*
 * The liveness bits of the docs of the SB, read once for all its pages: the bit of rank rank_base + i is bit
 * rank_base % 8 + i of the result (a word array), followed by zeros for the word loads past a last page. A reader
 * keeps the bits it first read until it asks for another SB, like it keeps the dirty-SB bitmap: a doc vacuumed meanwhile
 * is dead to every snapshot, so the heap check refuses it all the same. A writer that changes the bits drops them
 * (delete_ranks), and the rows made from them (live_om).
 */
static const uint8 *
live_sb_bits(SiftSegment *seg, DocMap *dm, const SiftDocChunk *dc)
{
	if (!dm->live_ok || dm->live_sb != dc->sb)
	{
		SiftLive	l = seg_live(seg);
		uint64		first = SIFT_LIVE_START(l.nsb) + dc->rank_base / 8,
					len = (dc->rank_base % 8 + dc->ndocs + 7) / 8;
		Size		pad = (PAGE_RANK_WORDS + 1) * sizeof(uint64);

		dm->live_ok = dm->live_om_ok = false;
		if (dm->live_cap < len + pad)
		{
			dm->live_cap = Max(len + pad, 1024);
			if (dm->live_bits)
				pfree(dm->live_bits);
			dm->live_bits = MemoryContextAlloc(seg->mcxt, dm->live_cap);
		}
		sift_live_copy(&l, first, len, dm->live_bits);
		memset(dm->live_bits + len, 0, pad);
		dm->live_sb = dc->sb;
		dm->live_ok = true;
	}
	return dm->live_bits;
}

/*
 * Keeps in mask, a subset of the present offsets of a page of a dirty SB, the live ones: the bits of its docs, taken in
 * rank order, spread over the present offsets. The words past the highest present offset are empty, so they are not
 * expanded. Returns the offsets left.
 */
int
sift_seg_page_mask_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *mask)
{
	DocMap	   *dm;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_mask(seg, dc->sb, page, mask);
	dm = indexed(seg, dc);
	const uint64 *bits = (const uint64 *) live_sb_bits(seg, dm, dc);
	int			nw = dm->rows[page] ? (dm->rowlen[page] + 63) / 64 : 0,
				left = sift_kern->expand(mask, mask, dm->om + dm->maskidx[page] * SIFT_OFFMASK_WORDS, bits,
										 dc->rank_base % 8 + dm->before[page], nw);

	memset(mask + nw, 0, (SIFT_OFFMASK_WORDS - nw) * sizeof(uint64));
	return left;
}

/*
 * The live offsets of every present page of the loaded SB, nwmax words each, in the order of om: the rows are packed
 * and spread over the present offsets by one expansion, which costs less than a page's own once the group has a
 * fraction of the SB's pages. They are a group of bitmaps, so a term's group meets them in the count kernel.
 */
static const uint64 *
live_sb_offsets(SiftSegment *seg, DocMap *dm, const SiftDocChunk *dc)
{
	const uint64 *bits = (const uint64 *) live_sb_bits(seg, dm, dc);
	int			nw = dm->nwmax;
	uint64	   *rows;

	if (dm->live_om_ok)
		return dm->live_om;
	if (dm->live_om == NULL)
		dm->live_om = MemoryContextAllocAligned(seg->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
												SIFT_SIMD_ALIGN, 0);
	rows = dm->live_om;
	for (int k = 0; k < dm->np; k++)
		for (int j = 0; j < nw; j++)
			rows[k * nw + j] = dm->om[k * SIFT_OFFMASK_WORDS + j];
	sift_kern->expand(rows, rows, rows, bits, dc->rank_base % 8, dm->np * nw);
	dm->live_om_ok = true;
	return rows;
}

static uint64
count_live1(SiftSegment *seg, const SiftDocChunk *dc, const sift_grp *g, const uint64 *pages)
{
	DocMap	   *dm = loaded(seg, dc);
	const uint8 *off = g->off;
	int			sel = 0;
	uint64		n = 0;

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		sel += sift_pc64(pages[wd]);
	if (memcmp(pages, g->pages, sizeof(g->pages)) == 0 && (dm->live_om_ok || sel * 12 >= dm->np * dm->nwmax))
	{
		sift_grp	rows = {.gid = dc->sb,.w = dm->nwmax * 8,.off = (const uint8 *) live_sb_offsets(seg, dm, dc)};

		memcpy(rows.pages, dm->pagemask, sizeof(rows.pages));
		return sift_grp_and_count(g, &rows);
	}
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			bit = pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (pages[wd] >> bit & 1)
			{
				memcpy(mask, off, g->w);
				n += sift_seg_page_mask_live(seg, dc, wd * 64 + bit, mask);
			}
		}
	return n;
}

uint64
sift_seg_live_count(SiftSegment *seg, const sift_grp *g, const uint64 *pages)
{
	SiftDocChunk dc;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_count(seg, g, pages);
	return sift_seg_docs(seg, g->gid, &dc) ? count_live1(seg, &dc, g, pages) : 0;
}

int
sift_seg_live_mask(SiftSegment *seg, uint32 sb, int page, uint64 *mask)
{
	SiftDocChunk dc;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_mask(seg, sb, page, mask);
	return sift_seg_docs(seg, sb, &dc) ? sift_seg_page_mask_live(seg, &dc, page, mask) : 0;
}

/* The live offsets of a page */
void
sift_seg_page_live(SiftSegment *seg, const SiftDocChunk *dc, int page, uint64 *live)
{
	if (sift_seg_page(seg, dc, page, live, NULL) && dc->dirty)
		sift_seg_page_mask_live(seg, dc, page, live);
}

bool
sift_seg_lookup(SiftSegment *seg, const char *term, int len, SiftTermInfo *info)
{
	if (!sift_dict_lookup(seg->dict, term, len, info))
		return false;
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		sift_seg_term_stats(seg, info);
	return true;
}

/* Does the group hold a posting of a live doc */
static bool
group_has_live(SiftSegment *seg, const sift_grp *g)
{
	SiftDocChunk dc;
	const uint8 *off = g->off;

	if (!sift_seg_docs(seg, g->gid, &dc) || !dc.dirty)
		return true;
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			uint64		mask[SIFT_OFFMASK_WORDS] = {0},
						live[SIFT_OFFMASK_WORDS];

			memcpy(mask, off, g->w);
			sift_seg_page_live(seg, &dc, wd * 64 + pg_rightmost_one_pos64(m), live);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				if (mask[k] & live[k])
					return true;
		}
	return false;
}

void
sift_seg_live_totals(SiftSegment *seg, uint64 *ndocs, uint64 *sum_dl)
{
	*ndocs = seg->hdr->ndocs;
	*sum_dl = seg->hdr->sum_dl;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS && (seg->entry.flags & SIFT_SEG_DIRTY))
	{
		const SiftLive2HeaderData *h = &live2(seg)->h;

		*ndocs -= h->ndead;
		*sum_dl -= h->dead_dl;
	}
}

/* Is the doc of the key live: its offset is in the rows of its SB, which are all the SB's docs while the SB is clean */
static bool
live2_key(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			off = (int) (key & 0xFFFF) - 1;
	const sift_grp *g;
	const uint64 *row;

	if (!sift_seg_sb_dirty(seg, sift_block_sb(blk)))
		return true;
	g = live2_rows(seg, sift_block_sb(blk));
	row = g ? (const uint64 *) grp_row(g, blk & (SIFT_SB_PAGES - 1)) : NULL;
	return row && off / 64 < (int) (g->w / 8) && (row[off / 64] >> (off % 64) & 1);
}

/* The header the segment kept when it read its first dfs (live2) is the one they count: its table was complete and no doc has been cleared since */
bool
sift_seg_live_stable(SiftSegment *seg)
{
	SiftLive	l;
	SiftLive2HeaderData h;
	const SiftLive2HeaderData *kept;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt < SIFT_SEGFMT_LIVEROWS)
		return false;
	l = seg_live(seg);
	sift_live2_header(&l, &h);
	kept = &live2(seg)->h;
	return kept->tbl_ndead == kept->ndead && h.ndead == kept->ndead;
}

bool
sift_seg_key_live(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	SiftDocChunk dc;
	uint64		live[SIFT_OFFMASK_WORDS];
	int			off = (int) (key & 0xFFFF) - 1;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return live2_key(seg, key);
	if (!sift_seg_sb_dirty(seg, sift_block_sb(blk)) || !sift_seg_docs(seg, sift_block_sb(blk), &dc))
		return true;
	sift_seg_page_live(seg, &dc, blk & (SIFT_SB_PAGES - 1), live);
	return off >= 0 && off < SIFT_OFFMASK_WORDS * 64 && (live[off / 64] >> (off % 64) & 1);
}

void
sift_seg_term_stats(SiftSegment *seg, SiftTermInfo *info)
{
	info->ldf = info->df;
	if (seg->fmt < SIFT_SEGFMT_LIVEROWS || !(seg->entry.flags & SIFT_SEG_DIRTY))
		return;
	if (info->slot >= 0)
	{
		SiftLive	l = seg_live(seg);

		info->ldf -= Min(info->df, sift_live2_dead(&l, &live2(seg)->h, info->slot));
	}
	else
		for (uint32 i = 0; i < info->df; i++)
			info->ldf -= !live2_key(seg, info->inline_keys[i]);
}

bool
sift_seg_term_live(SiftSegment *seg, const SiftTermInfo *info)
{
	MemoryContext cx;
	SiftPostingCursor *c;
	bool		live = false;

	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return true;
	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
	{
		SiftTermInfo t = *info;

		sift_seg_term_stats(seg, &t);
		return t.ldf > 0;
	}
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift term live", ALLOCSET_SMALL_SIZES);
	c = sift_post_open(&seg->sr, info, cx);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g && !live; g = sift_next(sift_post_cursor(c)))
		live = group_has_live(seg, g);
	sift_sr_release(&seg->sr);
	MemoryContextDelete(cx);
	return live;
}

/* ---------- VACUUM ---------- */

/* Deletes the docs of ranks dead[] (ascending) of one SB (format 1) */
static void
delete_ranks(SiftSegment *seg, uint32 sb, const uint64 *dead, int nd)
{
	SiftLive	l = seg_live(seg);

	sift_seg_mark_dirty(seg->index, &seg->entry);
	sift_live_clear(&l, sb - seg->hdr->sb_lo, dead, nd);
	docmap(seg)->live_ok = docmap(seg)->live_om_ok = false;
}

/* Deletes the docs with these bits of the run (format 2) in one SB, dead_dl the sum of their dl */
static void
delete_bits2(SiftSegment *seg, uint32 sb, const uint64 *bits, int nd, uint64 dead_dl)
{
	SiftLive	l = seg_live(seg);

	sift_seg_mark_dirty(seg->index, &seg->entry);
	sift_live2_clear(&l, sb - seg->hdr->sb_lo, bits, nd, dead_dl);
	live2_cleared(seg, nd, dead_dl);
}

int64
sift_seg_live_rank(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			page = blk & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	SiftDocChunk dc;
	uint64		present[SIFT_OFFMASK_WORDS],
				live[SIFT_OFFMASK_WORDS],
				rank;

	if (!sift_seg_docs(seg, sift_block_sb(blk), &dc) || !(dc.pagemask[page / 64] >> (page % 64) & 1))
		return -1;
	sift_seg_page(seg, &dc, page, present, NULL);
	sift_seg_page_live(seg, &dc, page, live);
	if (!(live[off / 64] >> (off % 64) & 1))
		return -1;
	rank = dc.rank_base + docmap(seg)->before[page];
	for (int w = 0; w < off / 64; w++)
		rank += pg_popcount64(present[w]);
	return rank + pg_popcount64(present[off / 64] & ((UINT64CONST(1) << (off % 64)) - 1));
}

/*
 * One doc of a segment of format 2. The table of dead postings is left behind (tbl_ndead < ndead) until a VACUUM of the index
 * counts it again: a host that deletes one doc at a time writes format 1
 */
static bool
delete_key2(SiftSegment *seg, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int			page = blk & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;
	SiftDocChunk dc;
	const sift_grp *rows;
	const uint64 *row;
	uint64		present[SIFT_OFFMASK_WORDS],
				bit;
	const uint16 *dl;

	if (!sift_seg_docs(seg, sift_block_sb(blk), &dc) || !(dc.pagemask[page / 64] >> (page % 64) & 1))
		return false;
	rows = live2_rows(seg, dc.sb);
	row = rows ? (const uint64 *) grp_row(rows, page) : NULL;
	if (row == NULL || off / 64 >= (int) (rows->w / 8) || !(row[off / 64] >> (off % 64) & 1))
		return false;
	sift_seg_page(seg, &dc, page, present, &dl);
	bit = (live2(seg)->rstart + sizeof(SiftLive2RegionData) + ((const uint8 *) row - rows->off)) * 8 + off;
	delete_bits2(seg, dc.sb, &bit, 1, dl[off]);
	return true;
}

bool
sift_seg_delete_key(SiftSegment *seg, SiftKey key)
{
	int64		rank;

	if (seg->fmt >= SIFT_SEGFMT_LIVEROWS)
		return delete_key2(seg, key);
	rank = sift_seg_live_rank(seg, key);
	if (rank < 0)
		return false;
	delete_ranks(seg, sift_block_sb(sift_key_block(key)), (uint64 *) &rank, 1);
	return true;
}

/* Asks cb about every live doc of the SB; returns the dead ranks in dead[] */
static int
sb_dead(SiftSegment *seg, const SiftDocChunk *dc, IndexBulkDeleteCallback cb, void *cbstate,
		IndexBulkDeleteResult *stats, uint64 *dead)
{
	DocMap	   *dm = docmap(seg);
	int			nd = 0;

	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS],
					live[SIFT_OFFMASK_WORDS],
					r;

		if (!(dc->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, dc, p, present, NULL);
		sift_seg_page_live(seg, dc, p, live);
		r = dc->rank_base + dm->before[p];
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = present[wd]; m; m &= m - 1, r++)
			{
				ItemPointerData tid;

				if (!(live[wd] & (m & -m)))
					continue;
				ItemPointerSet(&tid, dc->sb * SIFT_SB_PAGES + p, wd * 64 + __builtin_ctzll(m) + 1);
				if (cb(&tid, cbstate))
					dead[nd++] = r;
				else
					stats->num_index_tuples++;
			}
	}
	return nd;
}

/*
 * Asks cb about every live doc of the SB (format 2); returns the bits of the dead ones in the LIVE run in bits[] and the sum of
 * their dl in *dead_dl
 */
static int
sb_dead2(SiftSegment *seg, const SiftDocChunk *dc, IndexBulkDeleteCallback cb, void *cbstate,
		 IndexBulkDeleteResult *stats, uint64 *bits, uint64 *dead_dl)
{
	const sift_grp *rows = live2_rows(seg, dc->sb);
	uint64		first = (live2(seg)->rstart + sizeof(SiftLive2RegionData)) * 8;
	int			nd = 0,
				k = 0;

	if (rows == NULL || memcmp(rows->pages, dc->pagemask, sizeof(dc->pagemask)) != 0)
		corrupt(seg, "The live rows of SB %u are not those of its pages.", dc->sb);
	*dead_dl = 0;
	for (int p = 0; p < SIFT_SB_PAGES; p++)
	{
		uint64		present[SIFT_OFFMASK_WORDS];
		const uint64 *live = (const uint64 *) (rows->off + (Size) k * rows->w);
		const uint16 *dl;

		if (!(dc->pagemask[p / 64] & (1ULL << (p % 64))))
			continue;
		sift_seg_page(seg, dc, p, present, &dl);
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = present[wd] & (wd < (int) (rows->w / 8) ? live[wd] : 0); m; m &= m - 1)
			{
				ItemPointerData tid;
				int			o = wd * 64 + __builtin_ctzll(m);

				ItemPointerSet(&tid, dc->sb * SIFT_SB_PAGES + p, o + 1);
				if (cb(&tid, cbstate))
				{
					bits[nd++] = first + (uint64) k * rows->w * 8 + o;
					*dead_dl += dl[o];
				}
				else
					stats->num_index_tuples++;
			}
		k++;
	}
	return nd;
}

/* ---------- the dead postings of each term (format 2) ---------- */

typedef struct DeadScan
{
	SiftSegment *seg;
	MemoryContext tcxt;
	sift_grp  **grp;			/* by SB from sb_lo: the dead docs as rows, NULL when the SB has none */
	uint32	   *dead;			/* by slot */
	uint32		nslots;
} DeadScan;

/* The dead docs of the SB at position i of the SB directory (present and not live), as a group of the rows' width */
static sift_grp *
dead_rows(SiftSegment *seg, DocMap *dm, int64 i, MemoryContext cx)
{
	uint32		sb = dm->dir[i].sb;
	const sift_grp *rows;
	sift_grp   *g;
	uint64	   *out;
	bool		any = false;

	load_sb(seg, dm, i);
	rows = live2_rows(seg, sb);
	if (rows == NULL || memcmp(rows->pages, dm->pagemask, sizeof(dm->pagemask)) != 0)
		corrupt(seg, "The live rows of SB %u are not those of its pages.", sb);
	g = MemoryContextAlloc(cx, sizeof(sift_grp));
	out = MemoryContextAllocAligned(cx, (Size) dm->np * rows->w, SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	for (int k = 0; k < dm->np; k++)
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		{
			uint64		om = dm->om[k * SIFT_OFFMASK_WORDS + wd];

			if (wd >= (int) (rows->w / 8))
			{
				if (om)
					corrupt(seg, "The live rows of SB %u are narrower than its docs.", sb);
				continue;
			}
			out[(Size) k * (rows->w / 8) + wd] = om & ~((const uint64 *) rows->off)[(Size) k * (rows->w / 8) + wd];
			any |= out[(Size) k * (rows->w / 8) + wd] != 0;
		}
	*g = (sift_grp) {.gid = sb,.w = rows->w,.off = (const uint8 *) out};
	memcpy(g->pages, rows->pages, sizeof(g->pages));
	return any ? g : NULL;
}

static bool
dead_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DeadScan   *ds = arg;
	SiftSegment *seg = ds->seg;
	SiftPostingCursor *c;
	sift_cur   *cur;
	uint64		n = 0;

	if (info->slot < 0)
		return true;
	if (info->slot >= ds->nslots)
		corrupt(seg, "A term has slot %d in a table of %u.", info->slot, ds->nslots);
	c = sift_post_open(&seg->sr, info, ds->tcxt);
	sift_sr_unlock(&seg->sr);
	cur = sift_post_cursor(c);
	for (const sift_grp *g = sift_next(cur); g; g = sift_next(cur))
	{
		const sift_grp *d;

		if (g->gid < seg->hdr->sb_lo || g->gid > seg->hdr->sb_hi)
			corrupt(seg, "A posting lies in SB %u, outside the segment's %u to %u.", g->gid, seg->hdr->sb_lo, seg->hdr->sb_hi);
		d = ds->grp[g->gid - seg->hdr->sb_lo];
		if (d)
			n += sift_grp_and_count(g, d);
	}
	ds->dead[info->slot] = n;
	MemoryContextReset(ds->tcxt);
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* Counts again, for every term with a slot, the postings of the docs that are dead now: the whole table, so that a repeat is harmless */
void
count_dead(SiftSegment *seg, const SiftLive2HeaderData *h, uint32 *dead)
{
	SiftLive	l = seg_live(seg);
	DocMap	   *dm = docmap(seg);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift dead postings", ALLOCSET_DEFAULT_SIZES);
	DeadScan	ds = {.seg = seg,.dead = dead,.nslots = h->nslots};
	uint8	   *dirty = palloc((h->nsb + 7) / 8);

	ds.tcxt = AllocSetContextCreate(cx, "sift dead postings term", ALLOCSET_DEFAULT_SIZES);
	ds.grp = MemoryContextAllocZero(cx, (Size) h->nsb * sizeof(sift_grp *));
	memset(dead, 0, (Size) h->nslots * sizeof(uint32));
	sift_live2_dirty(&l, h, dirty);
	live2(seg)->rok = false;
	for (int64 i = 0; i < dm->nsb; i++)
	{
		uint32		idx = dm->dir[i].sb - seg->hdr->sb_lo;

		if (dirty[idx / 8] >> (idx % 8) & 1)
			ds.grp[idx] = dead_rows(seg, dm, i, cx);
		CHECK_FOR_INTERRUPTS();
	}
	live2(seg)->rok = false;
	sift_dict_scan(seg->dict, NULL, 0, NULL, 0, dead_term, &ds);
	sift_sr_release(&seg->sr);
	pfree(dirty);
	MemoryContextDelete(cx);
}

/* When the docs were cleared since the table was counted (VACUUM, or a crash that left it short): counts the table again and stores it */
void
sift_seg_refresh_dead(SiftSegment *seg)
{
	SiftLive	l = seg_live(seg);
	SiftLive2HeaderData h;
	uint32	   *dead;

	if (seg->fmt < SIFT_SEGFMT_LIVEROWS)
		return;
	sift_live2_header(&l, &h);
	if (h.tbl_ndead == h.ndead)
		return;
	dead = MemoryContextAllocHuge(CurrentMemoryContext, Max(h.nslots, 1) * sizeof(uint32));
	count_dead(seg, &h, dead);
	sift_live2_set_dead(&l, &h, dead);
	pfree(dead);
	sift_sr_release(&seg->sr);
}

int64
sift_seg_bulkdelete(SiftSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SiftDocChunk dc;
	uint64	   *dead = palloc(SIFT_SB_PAGES * MAXOFF * sizeof(uint64));
	int64		removed = 0;
	bool		rows = seg->fmt >= SIFT_SEGFMT_LIVEROWS;

	for (uint32 sb = seg->hdr->sb_lo; sb <= seg->hdr->sb_hi && sift_seg_docs_next(seg, sb, &dc); sb = dc.sb + 1)
	{
		uint64		dl = 0;
		int			nd = rows ? sb_dead2(seg, &dc, cb, cbstate, stats, dead, &dl) : sb_dead(seg, &dc, cb, cbstate, stats, dead);

		if (nd == 0)
			continue;
		if (rows)
			delete_bits2(seg, dc.sb, dead, nd, dl);
		else
			delete_ranks(seg, dc.sb, dead, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	pfree(dead);
	stats->tuples_removed += removed;
	return removed;
}

uint64
sift_seg_live_docs(SiftSegment *seg)
{
	SiftLive	l = seg_live(seg);

	return seg->entry.flags & SIFT_SEG_DIRTY ? seg->hdr->ndocs - sift_live_ndead(&l) : seg->hdr->ndocs;
}
