/*
 * core/writer.c - write targets (where a BULK, INIT or SHARED writer takes its blocks from) and the stream writer.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/nbtxlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "catalog/pg_control.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bulk_write.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "col/col.h"
#include "bm25/estimate.h"
#include "core/lane.h"
#include "core/store.h"
#include "core/store_int.h"

#define RUN_MIN				16
#define RUN_MAX				1024
#define RUN_BIG				8192
#define EXTENT_TARGET		256

#ifdef SIFT_TESTING
#define TEST_KNOB(knob, normal)	((knob) > 0 ? (knob) : (normal))
#else
#define TEST_KNOB(knob, normal)	(normal)
#endif

struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		pos;
	Page		page;			/* page being filled, NULL between pages */
	PGIOAlignedBlock *local;	/* SHARED: page buffer */
	Size		used;			/* bytes on page */
	BlockNumber blk;
	BlockNumber run_next;		/* unused part of the current run */
	BlockNumber run_end;
	uint32		grow;
	uint64		blocks;			/* blocks taken for this stream */
	SiftExtentList ext;
};

SiftWriteTarget *
sift_wt_begin(Relation index, SiftWriteMode mode)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));
	ForkNumber	fork = mode == SIFT_WRITE_INIT ? INIT_FORKNUM : MAIN_FORKNUM;

	wt->index = index;
	wt->mode = mode;
	if (mode != SIFT_WRITE_SHARED)
	{
		SIFT_INVARIANT(smgrnblocks(RelationGetSmgr(index), fork) == 0, "sift: bulk write into non-empty index \"%s\"",
					   RelationGetRelationName(index));
		wt->bulk = smgr_bulk_start_rel(index, fork);
		wt->next = FIRST_DATA_BLKNO;
	}
	return wt;
}

void
sift_store_init_meta_ex(SiftWriteTarget *wt, float4 k1, float4 b, uint32 analyzer_hash, uint32 flags)
{
	SiftMetaPageData *meta = &wt->meta;

	SIFT_INVARIANT(wt->bulk != NULL, "sift: the metapage is written by BULK or INIT write targets only");
	memset(meta, 0, sizeof(SiftMetaPageData));
	meta->magic = SIFT_MAGIC;
	meta->format_major = SIFT_FORMAT_MAJOR;
	meta->format_minor = SIFT_FORMAT_MINOR_BASE;
	meta->k1 = k1;
	meta->b = b;
	meta->analyzer_hash = analyzer_hash;
	meta->flags = flags;
	meta->next_gen = 1;
	meta->freelist_head = InvalidBlockNumber;
	wt->have_meta = true;
}

/* Room for one more extent in a list that lives as long as wt, whatever context the stream writer ran in */
static SiftExtent *
wt_list_room(SiftWriteTarget *wt, SiftExtent *list, int n, int *max)
{
	if (n < *max)
		return list;
	*max = Max(8, 2 * *max);
	return list ? repalloc(list, *max * sizeof(SiftExtent)) :
		MemoryContextAlloc(GetMemoryChunkContext(wt), *max * sizeof(SiftExtent));
}

/* a minus b (disjoint extents each) in a list of wt's lifetime */
static SiftExtent *
extents_subtract(SiftWriteTarget *wt, SiftExtent *a, int *na, SiftExtent *b, int nb)
{
	SiftExtent *out = MemoryContextAlloc(GetMemoryChunkContext(wt), Max(1, *na + nb) * sizeof(SiftExtent));
	int			m = 0;

	qsort(a, *na, sizeof(SiftExtent), cmp_extent);
	qsort(b, nb, sizeof(SiftExtent), cmp_extent);
	for (int i = 0, j = 0; i < *na; i++)
	{
		uint64		s = a[i].start,
					e = (uint64) a[i].start + a[i].nblocks;

		while (j < nb && (uint64) b[j].start + b[j].nblocks <= s)
			j++;
		for (int k = j; k < nb && b[k].start < e; k++)
		{
			if (b[k].start > s)
				out[m++] = (SiftExtent) {s, b[k].start - s};
			s = Max(s, (uint64) b[k].start + b[k].nblocks);
		}
		if (s < e)
			out[m++] = (SiftExtent) {s, e - s};
	}
	pfree(a);
	*na = m;
	return out;
}

void
sift_store_init_meta(SiftWriteTarget *wt, const SiftOptions *opts, uint32 analyzer_hash)
{
	sift_store_init_meta_ex(wt, (float4) opts->k1, (float4) opts->b, analyzer_hash, 0);
}

/* Takes want blocks (at least min): spare runs first, then fresh ones */
static BlockNumber
wt_take(SiftWriteTarget *wt, uint32 want, uint32 min, uint32 *got)
{
	Buffer		metabuf;
	BlockNumber start;
	SiftExtent *last;

	for (int i = 0; i < wt->nspare; i++)
	{
		SiftExtent *s = &wt->spare[i];

		if (s->nblocks >= min)
		{
			start = s->start;
			*got = Min(want, s->nblocks);
			s->start += *got;
			s->nblocks -= *got;
			if (s->nblocks == 0)
				*s = wt->spare[--wt->nspare];
			return start;
		}
	}
	if (wt->bulk)
	{
		/* Continue a trailing spare run so BULK layouts stay hole-free */
		for (int i = 0; i < wt->nspare; i++)
			if (wt->spare[i].start + wt->spare[i].nblocks == wt->next)
			{
				wt->next = wt->spare[i].start;
				wt->spare[i] = wt->spare[--wt->nspare];
				break;
			}
		if ((uint64) wt->next + want > MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" cannot grow past %u blocks",
							RelationGetRelationName(wt->index), MaxBlockNumber),
					 errhint("A relation holds at most 32 TB with 8 kB blocks: partition the table so that each index stays below it.")));
		*got = want;
		wt->next += want;
		return wt->next - want;
	}
	/* the note cannot fail once the blocks are taken */
	wt->taken = wt_list_room(wt, wt->taken, wt->ntaken, &wt->maxtaken);
	metabuf = lock_meta(wt->index, BUFFER_LOCK_EXCLUSIVE);
	start = alloc_blocks(wt->index, metabuf, want, min, got);
	UnlockReleaseBuffer(metabuf);
	last = wt->ntaken > 0 ? &wt->taken[wt->ntaken - 1] : NULL;
	if (last && last->start + last->nblocks == start)
		last->nblocks += *got;
	else
		wt->taken[wt->ntaken++] = (SiftExtent) {start, *got};
#ifdef SIFT_TESTING
	/* another backend allocating in between: the next run is not adjacent to this one */
	if (sift_test_run_gap)
		UnlockReleaseBuffer(sift_alloc_page(wt->index, SIFT_PT_FREE, 0));
#endif
	return start;
}

static void
wt_spare(SiftWriteTarget *wt, BlockNumber start, uint32 n)
{
	if (n == 0)
		return;
	wt->spare = wt_list_room(wt, wt->spare, wt->nspare, &wt->maxspare);
	wt->spare[wt->nspare++] = (SiftExtent) {start, n};
}

#define THROTTLE_PAGES	16		/* pages (128 kB) of I/O that earn one delay */

void
sift_wt_throttle(SiftWriteTarget *wt, int delay_ms)
{
	wt->delay_ms = delay_ms;
}

void
sift_wt_wal_rate(SiftWriteTarget *wt, int kb_per_sec)
{
	wt->wal_kbps = kb_per_sec;
	wt->wal_base = pgWalUsage.wal_bytes;
	wt->wal_t0 = GetCurrentTimestamp();
}

/* Sleeps until the WAL the backend wrote since the target was limited is within what its rate allows for the time that went by */
static void
wt_wal_wait(SiftWriteTarget *wt)
{
	for (;;)
	{
		uint64		due_ms = (pgWalUsage.wal_bytes - wt->wal_base) * 1000 / ((uint64) wt->wal_kbps * 1024);
		int64		ahead = (int64) due_ms - (GetCurrentTimestamp() - wt->wal_t0) / 1000;

		if (ahead <= 0)
			return;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, Min(ahead, 1000), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* pages charged since the last sleep, whatever the target: a rewrite by partitions opens a target for each, and the pages under THROTTLE_PAGES of each would never sleep */
static uint64 charged;

/* Counts pages of I/O of a throttled target and sleeps a delay for every THROTTLE_PAGES, interruptibly; a WAL rate sleeps before the page that would exceed it */
void
sift_wt_charge(SiftWriteTarget *wt, uint64 pages)
{
	if (wt->wal_kbps > 0)
		wt_wal_wait(wt);
	if (wt->delay_ms == 0)
		return;
	charged += pages;
	for (; charged >= THROTTLE_PAGES; charged -= THROTTLE_PAGES)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, wt->delay_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* Writes a complete page; BULK takes ownership of a smgr_bulk_get_buf page */
static void
wt_put(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	Buffer		buf;

	sift_wt_charge(wt, 1);

	if (wt->bulk)
	{
		smgr_bulk_write(wt->bulk, blkno, (BulkWriteBuffer) page, true);
		return;
	}
#ifdef SIFT_TESTING
	if (sift_test_fail_pages > 0 && ++wt->npages > sift_test_fail_pages)
		elog(ERROR, "sift test: injected failure after %d pages", sift_test_fail_pages);	/* check-errors: allow */
#endif
	buf = new_locked(wt->index, blkno);
	log_pages(wt->index, &buf, &page, 1);
	UnlockReleaseBuffer(buf);
}

static Page
wt_page(SiftWriteTarget *wt, PGIOAlignedBlock *local)
{
	return wt->bulk ? smgr_bulk_get_buf(wt->bulk)->data : local->data;
}

BlockNumber
sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks)
{
	uint32		got;

	return wt_take(wt, nblocks, nblocks, &got);
}

void
sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	Page		dst = wt_page(wt, (PGIOAlignedBlock *) page);

	Assert(layout_ok(page));
	if (dst != page)
		memcpy(dst, page, BLCKSZ);
	wt_put(wt, blkno, dst);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	BlockNumber blkno = sift_wt_reserve(wt, 1);

	sift_wt_put_page(wt, blkno, page);
	return blkno;
}

static void
bulk_put_meta(SiftWriteTarget *wt)
{
	Page		page = wt_page(wt, NULL);

	sift_page_init(page, SIFT_PT_META, 0);
	*META(page) = wt->meta;
	SiftPageSetUsed(page, SIFT_META_BASE_SIZE);
	wt_put(wt, SIFT_METAPAGE_BLKNO, page);
	page = wt_page(wt, NULL);
	sift_page_init(page, SIFT_PT_DIRECTORY, 0);
	SiftPageSetUsed(page, DIR_USED(0));
	wt_put(wt, SIFT_DIRECTORY_BLKNO, page);
}

/* BULK spare: trailing runs shrink the relation, the rest go to a free list */
static void
bulk_release_spare(SiftWriteTarget *wt)
{
	SiftFreeExtent *ext;
	int			k;

	qsort(wt->spare, wt->nspare, sizeof(SiftExtent), cmp_extent);
	while (wt->nspare > 0 && wt->spare[wt->nspare - 1].start + wt->spare[wt->nspare - 1].nblocks == wt->next)
		wt->next = wt->spare[--wt->nspare].start;
	if (wt->nspare == 0)
		return;
	ext = palloc(wt->nspare * sizeof(SiftFreeExtent));
	for (int i = 0; i < wt->nspare; i++)
		ext[i] = (SiftFreeExtent) {wt->spare[i].start, wt->spare[i].nblocks};
	k = (wt->nspare + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
	wt->meta.freelist_head = wt->next;
	for (int i = 0; i < k; i++)
	{
		Page		page = wt_page(wt, NULL);
		SiftFreeListData *fl = FL(page);

		sift_page_init(page, SIFT_PT_FREELIST, 0);
		fl->next = i + 1 < k ? wt->next + 1 : InvalidBlockNumber;
		fl->n = Min(wt->nspare - i * (int) SIFT_FREELIST_CAP, (int) SIFT_FREELIST_CAP);
		memcpy(fl->ext, ext + i * SIFT_FREELIST_CAP, fl->n * sizeof(SiftFreeExtent));
		SiftPageSetUsed(page, FL_USED(fl->n));
		wt_put(wt, wt->next++, page);
	}
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	if (wt->bulk == NULL)
	{
		if (wt->nspare > 0)
		{
			store_update(wt->index, &(SiftDirChange) {.release = wt->spare, .nrelease = wt->nspare,
					.extra = InvalidBuffer}, false, NULL);
			wt->taken = extents_subtract(wt, wt->taken, &wt->ntaken, wt->spare, wt->nspare);
			wt->maxtaken = wt->ntaken;
		}
		wt->nspare = 0;
		return;
	}
	SIFT_INVARIANT(wt->have_meta, "sift: bulk write target finished without a metapage");
	bulk_release_spare(wt);
	wt->meta.high_water = wt->next;
	bulk_put_meta(wt);
	smgr_bulk_finish(wt->bulk);
	wt->bulk = NULL;
}

void
sift_wt_expect(SiftWriteTarget *wt, uint64 bytes)
{
	wt->expect = bytes / BLCKSZ;
}

/* Transfer completed allocations to the published directory before any subsequent error can abort the write target. */
void
sift_wt_publish(SiftWriteTarget *wt, const SiftDirChange *chg)
{
	Assert(wt->mode == SIFT_WRITE_SHARED);
	store_update(wt->index, chg, true, wt);
}

/* The step that wrote through a SHARED target failed (its transaction is rolled back): the blocks go back */
void
sift_wt_abort(SiftWriteTarget *wt)
{
	if (wt->ntaken > 0)
		store_update(wt->index, &(SiftDirChange) {.release = wt->taken, .nrelease = wt->ntaken,
				.extra = InvalidBuffer}, false, NULL);
	wt->ntaken = wt->nspare = 0;
}

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->grow = RUN_MIN;
	sift_extlist_init(&sw->ext);
	sw->ext.max = TEST_KNOB(sift_test_max_extents, (int) SIFT_MAX_EXTENTS);
	if (wt->bulk == NULL)
		sw->local = palloc_aligned(sizeof(PGIOAlignedBlock), PG_IO_ALIGN_SIZE, 0);
	return sw;
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	return sw->pos;
}

static void
sw_flush(SiftStreamWriter *sw)
{
	if (sw->page == NULL)
		return;
	SiftPageSetUsed(sw->page, sw->used);
	wt_put(sw->wt, sw->blk, sw->page);
	sw->page = NULL;
	CHECK_FOR_INTERRUPTS();
}

/*
 * Runs grow with the stream, so a segment of any size fits the extents its header page lists: by
 * RUN_MAX blocks as before, then a 16th of what is written, up to RUN_BIG (a metapage-locked
 * extension of 64 MB at most).
 */
static uint32
run_cap(const SiftStreamWriter *sw)
{
	uint32		share = sw->pos / SIFT_PAGE_PAYLOAD / 16;

	/*
	 * A shared stream of unknown size (a parallel build's) gives back the unused end of its last run as a hole in the file, and
	 * the other streams are written next to it: the run is a 16th of what the stream has written, so the hole is at most that.
	 */
	if (sw->wt->expect == 0 && sw->wt->bulk == NULL)
		return Min(RUN_BIG, Max(RUN_MIN, share));
	return Max(RUN_MAX, Min(RUN_BIG, share));
}

/*
 * The shortest run worth taking off the free list. With the size of the stream known, anything that
 * keeps its extents within EXTENT_TARGET, so fragmented space left by merged segments is reused
 * instead of extending the file; unknown, a quarter of the run wanted.
 */
static uint32
run_min(const SiftStreamWriter *sw)
{
	if (sw->wt->expect == 0)
		return Max(RUN_MIN, sw->grow / 4);
	return Max(RUN_MIN, Min(sw->grow, sw->wt->expect / EXTENT_TARGET));
}

/* The run wanted: the growing one, but not far past what the stream is expected to still need */
static uint32
run_want(const SiftStreamWriter *sw, uint32 min)
{
	uint64		expect = sw->wt->expect + sw->wt->expect / 16 + RUN_MIN;

	if (sw->wt->expect == 0)
		return sw->grow;
	return Max(min, Min(sw->grow, expect > sw->blocks ? expect - sw->blocks : 0));
}

static void
sw_start_page(SiftStreamWriter *sw)
{
	if (sw->run_next == sw->run_end)
	{
		uint32		got;
		uint32		min = run_min(sw);

		sw->run_next = wt_take(sw->wt, run_want(sw, min), min, &got);
		sw->run_end = sw->run_next + got;
		sw->blocks += got;
		sw->grow = Min(sw->grow * 2, run_cap(sw));
	}
	sw->blk = sw->run_next++;
	sift_extlist_add(&sw->ext, sw->blk);
	sw->page = wt_page(sw->wt, sw->local);
	sift_page_init(sw->page, SIFT_PT_DATA, sw->gen);
	sw->used = 0;
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0)
	{
		size_t		n;

		if (sw->page == NULL || sw->used == SIFT_PAGE_PAYLOAD)
		{
			sw_flush(sw);
			sw_start_page(sw);
		}
		n = Min(len, SIFT_PAGE_PAYLOAD - sw->used);
		memcpy(SiftPageGetData(sw->page) + sw->used, p, n);
		sw->used += n;
		sw->pos += n;
		p += n;
		len -= n;
	}
}

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	sw_flush(sw);
	wt_spare(sw->wt, sw->run_next, sw->run_end - sw->run_next);
	sw->run_next = sw->run_end;
	*nextents = sw->ext.n;
	return sw->ext.ext;
}
