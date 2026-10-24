/*
 * core/store_int.h - shared between the files of the page and extent store (store.c, freelist.c, writer.c, reader.c,
 * relocate.c, verify.c).
 */
#ifndef SIFT_STORE_INT_H
#define SIFT_STORE_INT_H

#include "access/transam.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"

#include "core/store.h"

#define SPECIAL_SIZE		MAXALIGN(sizeof(SiftPageOpaqueData))
#define FIRST_DATA_BLKNO	(SIFT_DIRECTORY_BLKNO + 1)

#define META(page)	((SiftMetaPageData *) SiftPageGetData(page))
#define DIR(page)	((SiftDirectoryData *) SiftPageGetData(page))
#define FL(page)	((SiftFreeListData *) SiftPageGetData(page))
#define DIR_USED(n)	(offsetof(SiftDirectoryData, segs) + (n) * sizeof(SiftSegEntry))
#define FL_USED(n)	(offsetof(SiftFreeListData, ext) + (n) * sizeof(SiftFreeExtent))
#define ULL(x)		((unsigned long long) (x))

#define corrupt SIFT_CORRUPT

struct SiftWriteTarget
{
	Relation	index;
	SiftWriteMode mode;
	BulkWriteState *bulk;		/* BULK, INIT */
	BlockNumber next;			/* BULK, INIT: first unused block */
	bool		have_meta;
	SiftMetaPageData meta;		/* BULK, INIT: written at finish */
	SiftExtent *spare;			/* allocated, never written */
	int			nspare;
	int			maxspare;
	SiftExtent *taken;			/* SHARED: every block taken from the store and not given back yet */
	int			ntaken;
	int			maxtaken;
	uint64		expect;			/* SHARED: blocks the streams are expected to need, 0 = unknown */
	int			delay_ms;		/* sift_wt_throttle: sleep per THROTTLE_PAGES charged, 0 = none */
	int			wal_kbps;		/* sift_wt_wal_rate: kB of WAL this backend may write per second since wal_t0, 0 = no limit */
	uint64		wal_base;		/* the WAL bytes of the backend (pgWalUsage) at wal_t0 */
	TimestampTz wal_t0;
#ifdef SIFT_TESTING
	int			npages;			/* SHARED: pages written, for sift_test_fail_pages */
#endif
};

/* Records to add to the free list, and where they go */
typedef struct FreePlan
{
	SiftFreeExtent *ext;
	int			n;
	Buffer		flbuf;			/* existing page with room, locked */
	BlockNumber newhead;		/* or new pages, already logged */
} FreePlan;

/* The free list read into memory */
typedef struct FreeChain
{
	SiftFreeExtent *ext;		/* every record; those past the horizon are made reusable (safexid cleared) */
	int			n;
	int			maxext;
	BlockNumber *page;			/* the pages of the list itself */
	int			npages;
	int			maxpage;
	int			cleared;		/* records this read made reusable */
	FullTransactionId newest;	/* the newest safexid among them */
	Relation	index;
	FullTransactionId horizon;
} FreeChain;

static inline bool
layout_ok(Page page)
{
	PageHeader	ph = (PageHeader) page;

	return ph->pd_special == BLCKSZ - SPECIAL_SIZE && ph->pd_upper == ph->pd_special &&
		ph->pd_lower >= SIFT_PAGE_DATA_OFFSET && ph->pd_lower <= ph->pd_upper;
}

static inline Buffer
read_locked(Relation index, BlockNumber blkno, int mode)
{
	Buffer		buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);

	LockBuffer(buf, mode);
	return buf;
}

/* A block we own: old content is irrelevant, skip the read */
static inline Buffer
new_locked(Relation index, BlockNumber blkno)
{
	return ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK, NULL);
}

static inline bool
space_fits(const SiftFreeExtent *e, uint32 min, BlockNumber limit)
{
	return e->nblocks >= min && e->start < limit && min <= limit - e->start;
}

/* space mode (freelist.c) */
extern uint64 space_written;
extern bool space_exclusive;
extern BlockNumber space_limit;

/* store.c */
extern void log_pages(Relation index, Buffer *bufs, Page *srcs, int n);
extern Buffer lock_meta(Relation index, int mode);
extern void invalidate_mapping(Relation index);
extern void store_update(Relation index, const SiftDirChange *chg, bool publish, SiftWriteTarget *published);

/* freelist.c */
extern int	cmp_extent(const void *a, const void *b);
extern BlockNumber alloc_blocks(Relation index, Buffer metabuf, uint32 want, uint32 min, uint32 *got);
extern void read_chain(Relation index, Buffer metabuf, FreeChain *c, FullTransactionId horizon);
extern void prepare_free(Relation index, Buffer metabuf, const SiftExtent *release, int nrelease,
						 FullTransactionId safexid, FreePlan *plan);
extern void apply_free(SiftMetaPageData *meta, Page flpage, const FreePlan *plan);

#endif
