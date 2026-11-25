/*
 * table/f2/f2_col_delta_watch.c - what a cache of the records of a column delta chain remembers of the chain it has read, and how it reads what the chain gained (f2_col_delta.h).
 *
 * Pages are only ever added at the newest end of a chain, and the newest page grows in place: a cache that read a chain once reads the pages after the last one it saw, and the new records of that
 * one, and the rest of the chain is the one it knows when the blocks of its pages are the blocks the directory names and its newest page is still that page (its generation, depth and link) with the
 * records it held. A chain that was folded or rewritten is made of pages of other generations.
 *
 * check-memory: allow-allocs a watch holds the block list of one chain and a copy of its newest page, in the context of the cache that owns it, and the budget of core/cachebudget.h bounds the caches
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "core/page_io.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_col_delta_dir.h"

void
sift_col_delta_watch_init(SiftColDeltaWatch *w, MemoryContext cx)
{
	*w = (SiftColDeltaWatch) {.cx = cx,.head = MemoryContextAlloc(cx, BLCKSZ)};
}

void
sift_col_delta_watch_reset(SiftColDeltaWatch *w)
{
	w->npages = 0;
}

/* The newest page of the watch as the chain has it now: the same page (its generation, depth and link), and its records the ones it held */
static bool
same_head(const char *held, const char *now)
{
	const SiftColDeltaPageData *a = sift_col_delta_page_hdr(held),
			   *b = sift_col_delta_page_hdr(now);

	return SiftPageGetOpaque((Page) held)->gen == SiftPageGetOpaque((Page) now)->gen && a->chain_pages == b->chain_pages && a->prev_blk == b->prev_blk &&
		SiftPageGetUsed(now) >= SiftPageGetUsed(held) &&
		memcmp(sift_col_delta_page_first(held), sift_col_delta_page_first(now), sift_col_delta_page_end(held) - sift_col_delta_page_first(held)) == 0;
}

static bool
read_page(const SiftColDeltaPageReader *reader, BlockNumber blk, char *dst)
{
	return reader->read(reader->ctx, blk, dst) == NULL && sift_col_delta_page_check(dst) == NULL;
}

bool
sift_col_delta_watch_read(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a, const SiftColDeltaWatch *w, SiftColDeltaPageVisit visit, void *arg, SiftColDeltaWatchRead *r)
{
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) entry->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	char	   *page;
	bool		ok = true;

	*r = (SiftColDeltaWatchRead) {0};
	if (!sift_col_delta_anchor_sane(a) || (a->flags & SIFT_CDLA_INDEXED_DIR) == 0 || a->npages < w->npages ||
		sift_cddir_chain_entries(a, &reader, &r->ent, &r->dir_blks, &r->ndir) != NULL)
		return false;
	page = palloc(BLCKSZ);
	r->last = palloc(BLCKSZ);
	for (int i = 0; i < w->npages && ok; i++)
		ok = r->ent[i].blkno == w->blks[i];
	if (ok && w->npages > 0)
	{
		ok = read_page(&reader, w->blks[w->npages - 1], page) && same_head(w->head, page);
		memcpy(r->last, page, BLCKSZ);
		ok = ok && visit(arg, page, SiftPageGetUsed(w->head));
	}
	for (int i = w->npages; i < a->npages && ok; i++)
	{
		ok = read_page(&reader, r->ent[i].blkno, page) && sift_col_delta_check_link(page, i + 1, i > 0 ? r->ent[i - 1].blkno : InvalidBlockNumber) == NULL;
		memcpy(r->last, page, BLCKSZ);
		ok = ok && visit(arg, page, 0);
	}
	pfree(page);
	if (!ok)
		sift_col_delta_watch_release(r);
	return ok;
}

void
sift_col_delta_watch_set(SiftColDeltaWatch *w, const SiftColDeltaAnchor *a, const SiftColDeltaWatchRead *r)
{
	w->blks = w->blks == NULL ? MemoryContextAlloc(w->cx, sizeof(BlockNumber) * a->npages) : repalloc(w->blks, sizeof(BlockNumber) * a->npages);
	for (int i = 0; i < a->npages; i++)
		w->blks[i] = r->ent[i].blkno;
	memcpy(w->head, r->last, BLCKSZ);
	w->npages = a->npages;
}

void
sift_col_delta_watch_release(SiftColDeltaWatchRead *r)
{
	if (r->ent != NULL)
		pfree(r->ent);
	if (r->dir_blks != NULL)
		pfree(r->dir_blks);
	if (r->last != NULL)
		pfree(r->last);
	*r = (SiftColDeltaWatchRead) {0};
}
