/*
 * bm25/docdir.c - the SB directory of a segment's docmap, kept by the backend for the next query (bm25/segcache.c).
 *
 * The directory is 24 bytes per superblock, and a query that reads docs of a segment reads all of it
 * and sums its ranks (more than 300 kB per segment of a billion-row index, per query).
 * Directories under sift_docdir_min_sb superblocks are read each time: that costs less than the lookup.
 * The SQLite port keeps none (stubs.c).
 */
#include "postgres.h"

#include "bm25/segment.h"

int			sift_docdir_min_sb = 128;

#ifdef SIFT_TESTING
SiftDocDirStats sift_docdir_stats;
#define DOCDIR_STAT(f)		(sift_docdir_stats.f++)
#else
#define DOCDIR_STAT(f)		((void) 0)
#endif

bool
sift_docdir_get(SiftSegment *seg, uint32 nsb, SiftDocSBRef **dir, uint64 **rank_base, void **handle)
{
	void	   *obj;
	bool		ready;

	*handle = NULL;
	if (nsb < sift_docdir_min_sb)
	{
		*dir = MemoryContextAllocHuge(seg->mcxt, nsb * sizeof(SiftDocSBRef));
		*rank_base = MemoryContextAllocHuge(seg->mcxt, (nsb + 1) * sizeof(uint64));
		return false;
	}
	ready = sift_segcache_get(&seg->sr, SIFT_SEGCACHE_DOCDIR, seg->entry.header_blk,
							  (Size) nsb * (sizeof(SiftDocSBRef) + sizeof(uint64)) + sizeof(uint64), seg->mcxt, &obj, handle);
	*rank_base = obj;
	*dir = (SiftDocSBRef *) (*rank_base + nsb + 1);
	if (*handle)
	{
		if (ready)
			DOCDIR_STAT(hits);
		else
			DOCDIR_STAT(loads);
	}
	return ready;
}

void
sift_docdir_ready(void *handle)
{
	sift_segcache_ready(handle);
}
