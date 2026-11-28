/*
 * table/f2/col_delta/watch.c - what a cache of the records of a column delta chain remembers of the chain it has read, and how it reads what the chain gained (f2_col_delta.h).
 *
 * Pages are only ever added at the newest end of a chain, and the newest page grows in place: a cache that read a chain once reads the pages after the last one it saw, and the new records of that
 * one, and the rest of the chain is the one it knows when the blocks of its pages are the blocks the directory names and its newest page is still that page (its generation, depth and link) with the
 * records it held. A chain that was folded or rewritten is made of pages of other generations.
 *
 * check-memory: allow-allocs a watch holds the block list of one chain, in the context of the cache that owns it, and the budget of core/cachebudget.h bounds the caches
 */
#include "postgres.h"

#include "port/pg_crc32c.h"
#include "utils/memutils.h"

#include "core/page_io.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"

void
sift_col_delta_watch_init(SiftColDeltaWatch *w, MemoryContext cx)
{
	*w = (SiftColDeltaWatch) {.cx = cx};
}

void
sift_col_delta_watch_reset(SiftColDeltaWatch *w)
{
	w->npages = 0;
}

void
sift_col_delta_watch_copy(SiftColDeltaWatch *w, const SiftColDeltaWatch *from)
{
	if (w->blks != NULL)
		pfree(w->blks);
	w->blks = MemoryContextAlloc(w->cx, sizeof(BlockNumber) * Max(from->npages, 1));
	memcpy(w->blks, from->blks, sizeof(BlockNumber) * from->npages);
	w->npages = from->npages;
	w->head = from->head;
}

/* The checksum of the first used bytes of the records of a page */
static uint32
records_sum(const char *page, uint32 used)
{
	const uint8 *first = sift_col_delta_page_first(page);
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, first, (const uint8 *) SiftPageGetData(page) + used - first);
	FIN_CRC32C(crc);
	return crc;
}

/* The page as seen to byte used of its data */
static SiftColDeltaHeadSeen
head_seen_to(const char *page, uint32 used)
{
	return (SiftColDeltaHeadSeen) {.gen = SiftPageGetOpaque((Page) page)->gen,.lsn = PageGetLSN((Page) page),.used = used,
								   .crc = *(const uint32 *) (SiftPageGetData((Page) page) + SIFT_PAGE_CRC_OFF),.sum = records_sum(page, used),
								   .chain_pages = sift_col_delta_page_hdr(page)->chain_pages,.prev_blk = sift_col_delta_page_hdr(page)->prev_blk};
}

SiftColDeltaHeadSeen
sift_col_delta_head_seen(const char *page)
{
	return head_seen_to(page, SiftPageGetUsed(page));
}

/* The newest page of the watch as the chain has it now: the same page (its generation, depth and link), and its records the ones it held */
static bool
same_head(const SiftColDeltaHeadSeen *held, const char *now)
{
	const SiftColDeltaPageData *b = sift_col_delta_page_hdr(now);

	return held->gen == SiftPageGetOpaque((Page) now)->gen && held->chain_pages == b->chain_pages && held->prev_blk == b->prev_blk && SiftPageGetUsed(now) >= held->used &&
		records_sum(now, held->used) == held->sum;
}

static bool
read_page(const SiftColDeltaPageReader *reader, BlockNumber blk, char *dst)
{
	return reader->read(reader->ctx, blk, dst) == NULL && sift_col_delta_page_check(dst) == NULL;
}

/* A page the prune leaves out because of the attributes it patches (not because it is of another part): what the watch will not have seen of the chain */
static bool
left_out(const SiftColDeltaPrune *prune, const SiftColDeltaDirEntry *e)
{
	if (prune == NULL || sift_cddir_entry_wanted(e, prune))
		return false;
	return prune->part_gen == 0 || e->part_gen == 0 || e->part_gen == prune->part_gen;
}

/* The pages of the chain, oldest first: those its directory lists, or those of a flat chain, found by their links from the newest */
static bool
chain_entries(const SiftColDeltaAnchor *a, const SiftColDeltaPageReader *reader, SiftColDeltaWatchRead *r, char *page)
{
	BlockNumber blk = a->delta_head_blkno;

	if ((a->flags & SIFT_CDLA_INDEXED_DIR) != 0)
		return sift_cddir_chain_entries(a, reader, &r->ent, &r->dir_blks, &r->ndir) == NULL;
	r->ent = palloc(sizeof(SiftColDeltaDirEntry) * a->npages);
	for (int i = a->npages - 1; i >= 0; i--)
	{
		if (!read_page(reader, blk, page))
			return false;
		r->ent[i] = sift_cddir_entry_of_page(blk, page);
		blk = sift_col_delta_page_hdr(page)->prev_blk;
	}
	return true;
}

bool
sift_col_delta_watch_read(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *a, const SiftColDeltaWatch *w, const SiftColDeltaPrune *prune, SiftColDeltaPageVisit visit, void *arg,
						  SiftColDeltaWatchRead *r)
{
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) entry->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	char	   *page;
	bool		ok;

	*r = (SiftColDeltaWatchRead) {0};
	if (!sift_col_delta_anchor_sane(a) || a->npages < w->npages)
		return false;
	page = palloc(BLCKSZ);
	r->last = palloc(BLCKSZ);
	ok = chain_entries(a, &reader, r, page);
	for (int i = 0; i < w->npages && ok; i++)
		ok = r->ent[i].blkno == w->blks[i];
	if (ok && w->npages > 0)
	{
		ok = read_page(&reader, w->blks[w->npages - 1], page) && same_head(&w->head, page);
		memcpy(r->last, page, BLCKSZ);
		ok = ok && visit(arg, page, w->head.used);
	}
	for (int i = w->npages; i < a->npages && ok; i++)
	{
		if (i < a->npages - 1 && prune != NULL && !sift_cddir_entry_wanted(&r->ent[i], prune))
		{
			if (left_out(prune, &r->ent[i]))
				r->skipped_bloom |= r->ent[i].attnum_bloom;
			continue;
		}
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
sift_col_delta_watch_cover(SiftColDeltaWatch *w, const SiftColDeltaWatchRead *r, int npages, const char *head, uint32 used)
{
	w->blks = w->blks == NULL ? MemoryContextAlloc(w->cx, sizeof(BlockNumber) * npages) : repalloc(w->blks, sizeof(BlockNumber) * npages);
	for (int i = 0; i < npages; i++)
		w->blks[i] = r->ent[i].blkno;
	w->head = head_seen_to(head, used);
	w->npages = npages;
}

void
sift_col_delta_watch_set(SiftColDeltaWatch *w, const SiftColDeltaAnchor *a, const SiftColDeltaWatchRead *r)
{
	sift_col_delta_watch_cover(w, r, a->npages, r->last, SiftPageGetUsed(r->last));
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
