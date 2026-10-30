/*
 * core/store_ops_test.c - test-only SQL functions sift._test_* for the store module: allocation, recycling, checks, truncation, space,
 * the free list, pages and churn.
 */
#include "store_test_int.h"
#include "utils/timestamp.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_alloc);
PG_FUNCTION_INFO_V1(sift_test_recycle);
PG_FUNCTION_INFO_V1(sift_test_check);
PG_FUNCTION_INFO_V1(sift_test_sweep);
PG_FUNCTION_INFO_V1(sift_test_truncate);
PG_FUNCTION_INFO_V1(sift_test_space);
PG_FUNCTION_INFO_V1(sift_test_info);
PG_FUNCTION_INFO_V1(sift_test_maint_failures);
PG_FUNCTION_INFO_V1(sift_test_meta_note);
PG_FUNCTION_INFO_V1(sift_test_maint_wait_us);
PG_FUNCTION_INFO_V1(sift_test_freelist);
PG_FUNCTION_INFO_V1(sift_test_page);
PG_FUNCTION_INFO_V1(sift_test_poke);
PG_FUNCTION_INFO_V1(sift_test_churn);
PG_FUNCTION_INFO_V1(sift_test_dir_updates_count);
PG_FUNCTION_INFO_V1(sift_test_relcache_burn);
Datum
sift_test_alloc(PG_FUNCTION_ARGS)
{
	Relation	index = relation_open(PG_GETARG_OID(0), RowExclusiveLock);
	int32		nblocks = PG_GETARG_INT32(1);
	int32		count = PG_GETARG_INT32(2);
	BlockNumber last;

	if (nblocks <= 0 || count < 0)
		elog(ERROR, "invalid allocation");
	last = alloc_many(index, nblocks, count, PG_GETARG_INT32(3));
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(last);
}

Datum
sift_test_recycle(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int			n = sift_recycle(index);

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

/* Verifies the store invariants; returns the number of orphan blocks */
Datum
sift_test_check(PG_FUNCTION_ARGS)
{
	bool		standby = RecoveryInProgress();
	LOCKMODE	mode = standby ? AccessShareLock : RowExclusiveLock;
	Relation	index = relation_open(PG_GETARG_OID(0), mode);
	int64		n;

	if (!standby)
		MAINT_LOCK(index, ExclusiveLock);
	sift_store_verify(index);
	n = bms_num_members(sift_store_orphans(index));
	if (!standby)
		MAINT_UNLOCK(index, ExclusiveLock);
	relation_close(index, mode);
	PG_RETURN_INT64(n);
}

Datum
sift_test_sweep(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		n = sweep(index);

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

/* Blocks the end of the file gave back (sift_maint_truncate, reported at NOTICE); 0 when it gave up or there was nothing to give */
static int64
truncate_tail(Relation index)
{
	BlockNumber before = RelationGetNumberOfBlocks(index);

	MAINT_LOCK(index, ExclusiveLock);
	sift_maint_truncate(index, NOTICE);
	MAINT_UNLOCK(index, ExclusiveLock);
	return (int64) before - RelationGetNumberOfBlocks(index);
}

Datum
sift_test_truncate(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		n = truncate_tail(index);

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

/* Failed maintenance steps in a row (the background retries wait longer after each) */
Datum
sift_test_maint_failures(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot snap;

	sift_dir_read(index, &snap);
	relation_close(index, AccessShareLock);
	PG_RETURN_INT32(snap.meta.flags & SIFT_META_FAILURES_MASK);
}

/*
 * sift._test_meta_note(index, mode): the metapage as a build without the failure note wrote it ('old': pd_lower stops short of the note) or
 * with a note of bytes no build wrote ('junk': every byte 0xFF, one failure in a row), for the readers of sift.workers
 */
Datum
sift_test_meta_note(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Buffer		buf = ReadBuffer(index, SIFT_METAPAGE_BLKNO);
	GenericXLogState *state;
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	if (strcmp(mode, "old") == 0)
	{
		SiftMetaPageData *meta = (SiftMetaPageData *) SiftPageGetData(page);

		memset(SiftPageGetData(page) + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
		SiftPageSetUsed(page, SIFT_META_BASE_SIZE);
		/* one failure counted by such a build, which kept no note */
		meta->flags = (meta->flags & ~SIFT_META_FAILURES_MASK) | 1;
		meta->maint_requested_at = GetCurrentTimestamp();
	}
	else if (strcmp(mode, "junk") == 0)
	{
		memset(SiftPageGetData(page) + SIFT_META_BASE_SIZE, 0xFF, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
		((SiftMetaPageData *) SiftPageGetData(page))->flags = (((SiftMetaPageData *) SiftPageGetData(page))->flags & ~SIFT_META_FAILURES_MASK) | 1;
		SiftPageSetUsed(page, sizeof(SiftMetaPageData));
	}
	else
		elog(ERROR, "sift test: unknown mode \"%s\"", mode);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

/* The longest time this backend waited for the maintenance of others since the last call, in microseconds */
/* sift._test_dir_updates(): the directory updates of this process since sift.test_crash_dir was set */
Datum
sift_test_dir_updates_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(sift_test_dir_updates);
}

static void
burn_inval(Datum arg, Oid relid)
{
}

/* sift._test_relcache_burn(n): n relcache callback slots of this backend taken by a library that does nothing with them (test/t/relcache_slots.sh) */
Datum
sift_test_relcache_burn(PG_FUNCTION_ARGS)
{
	for (int i = PG_GETARG_INT32(0); i > 0; i--)
		CacheRegisterRelcacheCallback(burn_inval, (Datum) 0);
	PG_RETURN_VOID();
}

Datum
sift_test_maint_wait_us(PG_FUNCTION_ARGS)
{
	int64		us = sift_maint_wait_max_us;

	sift_maint_wait_max_us = 0;
	PG_RETURN_INT64(us);
}

Datum
sift_test_space(PG_FUNCTION_ARGS)
{
	Relation index = sift_test_open_index(PG_GETARG_OID(0), AccessExclusiveLock);
	uint64 written = sift_store_space_written();

	MAINT_LOCK(index, ExclusiveLock);
	sift_maint_space(index, NULL, DEBUG2);
	written = sift_store_space_written() - written;
	MAINT_UNLOCK(index, ExclusiveLock);
	relation_close(index, AccessExclusiveLock);
	PG_RETURN_INT64(written);
}

static void
put_info(ReturnSetInfo *rsinfo, const char *key, int64 value)
{
	Datum		values[2] = {CStringGetTextDatum(key), Int64GetDatum(value)};
	bool		nulls[2] = {0};

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* Runs visit on the free list with the metapage share-locked */
static void
walk_free(Relation index, SiftFreeListVisit visit, void *arg)
{
	Buffer		metabuf = ReadBuffer(index, SIFT_METAPAGE_BLKNO);

	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	sift_freelist_walk(index, metabuf, visit, arg);
	UnlockReleaseBuffer(metabuf);
}

Datum
sift_test_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SMgrRelation smgr = RelationGetSmgr(index);
	SiftDirSnapshot snap;
	SiftFreeStats fs;

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, &snap);
	fs = sift_store_free_stats(index);
	put_info(rsinfo, "high_water", snap.meta.high_water);
	put_info(rsinfo, "nblocks", RelationGetNumberOfBlocks(index));
	put_info(rsinfo, "init_blocks", smgrexists(smgr, INIT_FORKNUM) ? (int64) smgrnblocks(smgr, INIT_FORKNUM) : -1);
	put_info(rsinfo, "next_gen", snap.meta.next_gen);
	put_info(rsinfo, "dir_version", snap.version);
	put_info(rsinfo, "nsegs", snap.nsegs);
	put_info(rsinfo, "freelist_head", BlockNumberIsValid(snap.meta.freelist_head) ? (int64) snap.meta.freelist_head : -1);
	put_info(rsinfo, "freelist_pages", fs.pages);
	put_info(rsinfo, "free_extents", fs.records);
	put_info(rsinfo, "free_blocks", fs.blocks);
	put_info(rsinfo, "pending_blocks", fs.blocks - fs.reusable);
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

static bool
put_free_records(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	ReturnSetInfo *rsinfo = arg;

	for (uint32 i = 0; i < fl->n; i++)
	{
		Datum		values[4] = {Int64GetDatum(blk), Int64GetDatum(fl->ext[i].start),
		Int64GetDatum(fl->ext[i].nblocks), BoolGetDatum(FullTransactionIdIsValid(fl->ext[i].safexid))};
		bool		nulls[4] = {0};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return true;
}

/* Free-list records in chain order */
Datum
sift_test_freelist(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);

	InitMaterializedSRF(fcinfo, 0);
	walk_free(index, put_free_records, rsinfo);
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

Datum
sift_test_page(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	Buffer		buf = ReadBuffer(index, (BlockNumber) PG_GETARG_INT64(1));
	Page		page;
	Datum		values[4];
	bool		nulls[4] = {0};

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	values[0] = Int32GetDatum(PageIsNew(page) ? 0 : SiftPageGetOpaque(page)->type);
	values[1] = Int64GetDatum(PageIsNew(page) ? 0 : SiftPageGetOpaque(page)->gen);
	values[2] = Int32GetDatum(((PageHeader) page)->pd_lower);
	values[3] = Int32GetDatum(((PageHeader) page)->pd_upper);
	UnlockReleaseBuffer(buf);
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/* Overwrites width bytes at offset with a little-endian value, without WAL */
Datum
sift_test_poke(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	Buffer		buf = ReadBuffer(index, (BlockNumber) PG_GETARG_INT64(1));
	int32		off = PG_GETARG_INT32(2);
	int64		value = PG_GETARG_INT64(3);
	int32		width = PG_GETARG_INT32(4);

	if (off < 0 || width < 1 || width > 8 || off + width > BLCKSZ)
		elog(ERROR, "invalid poke");
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	memcpy(BufferGetPage(buf) + off, &value, width);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * One random operation for concurrency and crash tests: write, drop, read
 * (after a pause, so drops and reuse race with it), recycle, allocate +
 * release, truncate or sweep.
 */
Datum
sift_test_churn(PG_FUNCTION_ARGS)
{
	static const uint32 aligns[] = {0, 0, 8, 64};
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint64		rng = PG_GETARG_INT64(1);
	int64		maxbytes = Max(PG_GETARG_INT64(2), 1);
	uint64		op = sift_mix64(rng++) % 100;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TestSeg		seg;
	uint64		pick;
	const char *what;

	sift_dir_read(index, snap);
	pick = snap->nsegs > 0 ? snap->segs[sift_mix64(rng++) % snap->nsegs].gen : 0;
	if (op < 35 && snap->nsegs < 96)
	{
		uint64		r = sift_mix64(rng++);

		seg_write(index, r % maxbytes, r | 1, (r >> 40) % 3 == 0 ? 0 : 1 + (r >> 20) % 70000,
				  aligns[(r >> 50) & 3]);
		what = "write";
	}
	else if (op < 60 && snap->nsegs > 4)
	{
		seg_drop(index, pick);
		what = "drop";
	}
	else if (op < 92 && pick)
	{
		if (find_segment(index, pick, snap, &seg))
		{
			sleep_ms(sift_mix64(rng++) % 20);
			read_segment(index, &seg, seg.align ? 0 : sift_mix64(rng++) % 2);
		}
		what = "read";
	}
	else if (op < 94)
	{
		sift_recycle(index);
		what = "recycle";
	}
	else if (op < 95)
	{
		truncate_tail(index);
		what = "truncate";
	}
	else if (op < 99)
	{
		uint64		r = sift_mix64(rng++);

		alloc_many(index, 1 + r % 40, 1 + (r >> 32) % 8, 1);
		what = "alloc";
	}
	else
	{
		sweep(index);
		what = "sweep";
	}
	relation_close(index, RowExclusiveLock);
	PG_RETURN_TEXT_P(cstring_to_text(what));
}


#endif							/* SIFT_TESTING */
