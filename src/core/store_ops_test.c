/*
 * core/store_ops_test.c - test-only SQL functions sift._test_* for the store module: allocation, recycling, checks, truncation, space,
 * the free list, pages and churn.
 */
#include "store_test_int.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "storage/buffile.h"
#include "storage/lmgr.h"
#include "utils/catcache.h"
#include "utils/timestamp.h"

#include "core/damage.h"
#include "core/errctx.h"
#include "core/extlock.h"
#include "core/live.h"
#include "core/lane.h"
#include "core/inspect.h"

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
PG_FUNCTION_INFO_V1(sift_test_meta_stamp);
PG_FUNCTION_INFO_V1(sift_test_meta_size);
PG_FUNCTION_INFO_V1(sift_test_maint_wait_us);
PG_FUNCTION_INFO_V1(sift_test_freelist);
PG_FUNCTION_INFO_V1(sift_test_page);
PG_FUNCTION_INFO_V1(sift_test_poke);
PG_FUNCTION_INFO_V1(sift_test_raise);
PG_FUNCTION_INFO_V1(sift_test_raise_extending);
PG_FUNCTION_INFO_V1(sift_test_live_clear);
PG_FUNCTION_INFO_V1(sift_test_live2_clear);
PG_FUNCTION_INFO_V1(sift_test_write);
PG_FUNCTION_INFO_V1(sift_test_seghdr);
PG_FUNCTION_INFO_V1(sift_test_churn);
PG_FUNCTION_INFO_V1(sift_test_dir_updates_count);
PG_FUNCTION_INFO_V1(sift_test_flush_wal);
PG_FUNCTION_INFO_V1(sift_test_relcache_burn);
Datum
sift_test_alloc(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_store(PG_GETARG_OID(0), RowExclusiveLock);
	int32		nblocks = PG_GETARG_INT32(1);
	int32		count = PG_GETARG_INT32(2);
	BlockNumber last;

	if (nblocks <= 0 || count < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid allocation: %d blocks, %d times", nblocks, count)));
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
	Relation	index = sift_test_open_store(PG_GETARG_OID(0), mode);
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

/* sift._test_meta_size(): the bytes of the metapage struct, the most pd_lower of a metapage may cover */
Datum
sift_test_meta_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(sizeof(SiftMetaPageData));
}

/*
 * sift._test_meta_note(index, mode): the metapage as a build without the failure note and the long words mark wrote it ('old': pd_lower stops short of the note) or
 * with a note of bytes no build wrote ('junk': every byte 0xFF, one failure in a row), for the readers of sift.workers
 */
Datum
sift_test_meta_note(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Buffer		buf = ReadBuffer(index, SIFT_METAPAGE_BLKNO);
	SiftXlog	state;
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = sift_xlog_start(index);
	page = sift_xlog_register(&state, buf, 0);
	if (strcmp(mode, "old") == 0)
	{
		SiftMetaPageData *meta = (SiftMetaPageData *) SiftPageGetData(page);

		memset(SiftPageGetData(page) + SIFT_META_BASE_SIZE, 0, sizeof(SiftMetaPageData) - SIFT_META_BASE_SIZE);
		SiftPageSetUsed(page, SIFT_META_BASE_SIZE);
		/* one failure counted by such a build, which kept no note */
		meta->flags = (meta->flags & ~(SIFT_META_FAILURES_MASK | SIFT_META_LONG_WORDS)) | 1;
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
	sift_xlog_finish(&state);
	UnlockReleaseBuffer(buf);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

static bool
put_floor_xmin(SiftMetaPageData *meta, void *arg)
{
	meta->scope_floor[0].xmin = 7;
	return true;
}

/* sift._test_meta_stamp(index): a stamp of no lineage but a horizon, through the update every writer of the metapage uses (sift_meta_update) */
Datum
sift_test_meta_stamp(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);

	sift_meta_update(index, put_floor_xmin, NULL);
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

/* sift._test_flush_wal(): the flush of everything inserted, as the maintenance worker does before a VACUUM and the injected crash of a space step does */
Datum
sift_test_flush_wal(PG_FUNCTION_ARGS)
{
	sift_wal_flush_inserted();
	PG_RETURN_VOID();
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
	Relation	index = sift_test_open_store(PG_GETARG_OID(0), AccessShareLock);
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
	Relation	index = sift_test_open_store(PG_GETARG_OID(0), RowExclusiveLock);
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

/* sift._test_raise(index, sqlstate): raises the SQLSTATE under the error context of the index, as a sift path does when it fails for a reason that is no damage */
Datum
sift_test_raise(PG_FUNCTION_ARGS)
{
	char	   *state = text_to_cstring(PG_GETARG_TEXT_PP(1));
	SiftErrCtx	ctx;

	if (strlen(state) != 5)
		elog(ERROR, "invalid SQLSTATE");
	sift_errctx_push(&ctx, PG_GETARG_OID(0));
	ereport(ERROR, (errcode(MAKE_SQLSTATE(state[0], state[1], state[2], state[3], state[4])), errmsg("raised by sift._test_raise")));
	PG_RETURN_VOID();
}

/*
 * sift._test_raise_extending(index, kind, extend_first): reports `kind` (index, data, scope, other_data, other_index: the classes of core/damage.h; error, warning, log: what is no damage)
 * under the error context of the index while the relation extension lock of the index is held and the catalog cache of pg_class and pg_namespace is cold. PostgreSQL
 * forbids a heavyweight lock under that lock (an assertion with cassert), and every report runs the error context and the damage record. With extend_first the relation is extended
 * by one block under the lock before the report: the extension of a write that runs under the lock.
 */
Datum
sift_test_raise_extending(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(1));
	SiftErrCtx	ctx;

	sift_errctx_push(&ctx, RelationGetRelid(index));
	CatalogCacheFlushCatalog(RelationRelationId);
	CatalogCacheFlushCatalog(NamespaceRelationId);
	sift_extension_lock(index);
	if (PG_GETARG_BOOL(2))
		UnlockReleaseBuffer(sift_extend_zeroed_to(index, RelationGetNumberOfBlocks(index) + 1));
	if (strcmp(kind, "index") == 0)
		SIFT_DAMAGE_INDEX(index, errmsg("raised under the extension lock"));
	else if (strcmp(kind, "data") == 0)
		SIFT_DAMAGE_DATA(index, errmsg("raised under the extension lock"));
	else if (strcmp(kind, "scope") == 0)
		SIFT_DAMAGE_DATA(NULL, errmsg("raised under the extension lock"));
	else if (strcmp(kind, "other_data") == 0)
		SIFT_DAMAGE_OTHER_DATA(errmsg("raised under the extension lock"));
	else if (strcmp(kind, "other_index") == 0)
		SIFT_DAMAGE_OTHER_INDEX(errmsg("raised under the extension lock"));
	else if (strcmp(kind, "error") == 0)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("raised under the extension lock")));
	else if (strcmp(kind, "warning") == 0)
		ereport(WARNING, (errmsg("raised under the extension lock")));
	else if (strcmp(kind, "log") == 0)
		ereport(LOG, (errmsg("raised under the extension lock")));
	else
		elog(ERROR, "invalid kind");
	sift_extension_unlock(index);
	sift_errctx_pop(&ctx);
	relation_close(index, AccessShareLock);
	PG_RETURN_VOID();
}

/*
 * sift._test_live_clear(index, nsb, sbidx): writes the LIVE run of a segment of nsb superblocks (64 rows, none in the directory),
 * kills row 5 of superblock sbidx and returns the pages whose special space changed and whether the dirty bit reads back
 */
Datum
sift_test_live_clear(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint32		nsb = PG_GETARG_INT32(1),
				sbidx = PG_GETARG_INT32(2);
	uint64		dead = 5,
				gen;
	uint8	   *dirty = palloc0((nsb + 7) / 8);
	SiftLive	l = {.index = index, .nsb = nsb, .nbits = 64};
	SiftWriteTarget *wt;
	int32		damaged = 0;
	Datum		values[2];
	bool		nulls[2] = {0};

	MAINT_LOCK(index, ShareLock);
	l.gen = gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	sift_live_write(wt, gen, nsb, l.nbits, &l.first, &l.npages);
	sift_wt_finish(wt);
	sift_live_clear(&l, sbidx, &dead, 1);
	for (uint32 i = 0; i < l.npages; i++)
	{
		Buffer		buf = ReadBuffer(index, l.first + i);
		SiftPageOpaque o;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		o = SiftPageGetOpaque(BufferGetPage(buf));
		damaged += o->type != SIFT_PT_LIVE || o->flags != 0 || o->reserved != 0 || o->gen != gen;
		UnlockReleaseBuffer(buf);
	}
	sift_live_dirty(&l, dirty);
	MAINT_UNLOCK(index, ShareLock);
	values[0] = Int32GetDatum(damaged);
	values[1] = BoolGetDatum(dirty[sbidx / 8] >> (sbidx % 8) & 1);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/*
 * sift._test_live2_clear(index, nsb, sbidx): the same for the LIVE run of a segment of format 2 (empty regions, a table of 4200 dead counters): kills
 * the last bit of the run, so that the header, the dirty bitmap and the bit lie on three pages when the bitmap has more than one; returns the pages
 * whose special space changed, whether the dirty bit reads back, and the dead count and dl of the header
 */
Datum
sift_test_live2_clear(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint32		nsb = PG_GETARG_INT32(1),
				sbidx = PG_GETARG_INT32(2),
				nslots = 4200;
	uint64	   *reg;
	uint8	   *dirty;
	BufFile    *rows;
	SiftLive	l = {.index = index, .nsb = nsb, .nbits = 64};
	SiftLive2HeaderData h;
	SiftWriteTarget *wt;
	uint64		bit;
	int32		damaged = 0;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (nsb == 0 || nsb > 1000000 || sbidx >= nsb)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift._test_live2_clear: nsb must be 1 to 1000000 and sbidx below it")));
	reg = palloc0((nsb + 1) * sizeof(uint64));
	dirty = palloc0((nsb + 7) / 8);
	rows = BufFileCreateTemp(false);
	l.index = index;
	l.nsb = nsb;
	MAINT_LOCK(index, ShareLock);
	l.gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	sift_live2_write(wt, l.gen, nsb, nslots, reg, rows, &l.first, &l.npages);
	sift_wt_finish(wt);
	BufFileClose(rows);
	sift_live_copy(&l, 0, sizeof(h), (uint8 *) &h);
	bit = (h.tbl_off + (uint64) nslots * sizeof(uint32)) * 8 - 1;
	sift_live2_clear(&l, sbidx, &bit, 1, 7);
	for (uint32 i = 0; i < l.npages; i++)
	{
		Buffer		buf = ReadBuffer(index, l.first + i);
		SiftPageOpaque o;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		o = SiftPageGetOpaque(BufferGetPage(buf));
		damaged += o->type != SIFT_PT_LIVE || o->flags != 0 || o->reserved != 0 || o->gen != l.gen;
		UnlockReleaseBuffer(buf);
	}
	sift_live2_dirty(&l, &h, dirty);
	sift_live_copy(&l, 0, sizeof(h), (uint8 *) &h);
	MAINT_UNLOCK(index, ShareLock);
	values[0] = Int32GetDatum(damaged);
	values[1] = BoolGetDatum(dirty[sbidx / 8] >> (sbidx % 8) & 1);
	values[2] = Int32GetDatum(h.ndead);
	values[3] = Int64GetDatum(h.dead_dl);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/*
 * sift._test_write(index, blkno, offset, len, edit): sets len bytes of the page at offset to 0xFF as a writer of the index does, in a
 * generic WAL record (sift_xlog) or, with edit, as sift_page_edit_* logs an append: a change that breaks the rule of core/xlog.h is refused
 */
Datum
sift_test_write(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_store(PG_GETARG_OID(0), RowExclusiveLock);
	Buffer		buf = ReadBuffer(index, (BlockNumber) PG_GETARG_INT64(1));
	int32		off = PG_GETARG_INT32(2),
				len = PG_GETARG_INT32(3);

	if (off < 0 || len < 1 || off + len > BLCKSZ)
		elog(ERROR, "invalid write");
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (PG_GETARG_BOOL(4))
	{
		SiftPageEdit ed;

		sift_page_edit_begin(&ed, buf, SIFT_EDIT_HEAD);
		memset((char *) ed.page + off, 0xFF, len);
		sift_page_edit_end(index, &ed, 1);
	}
	else
	{
		SiftXlog	x = sift_xlog_start(index);

		memset((char *) sift_xlog_register(&x, buf, 0) + off, 0xFF, len);
		sift_xlog_finish(&x);
	}
	UnlockReleaseBuffer(buf);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * sift._test_seghdr(index, over, extralen): writes the header page of a segment with SIFT_MAX_EXTENTS + over extents and extralen bytes after them;
 * returns the bytes the page uses and SIFT_MAX_EXTENTS (an error when they do not fit the page)
 */
Datum
sift_test_seghdr(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint32		n = SIFT_MAX_EXTENTS + PG_GETARG_INT32(1),
				extralen = PG_GETARG_INT32(2);
	SiftExtent *ext = palloc0(Max(n, 1) * sizeof(SiftExtent));
	char	   *extra = palloc0(Max(extralen, 1));
	SiftSegHeaderData h = {0};
	SiftWriteTarget *wt;
	Buffer		buf;
	Datum		values[2];
	bool		nulls[2] = {0};

	MAINT_LOCK(index, ShareLock);
	h.gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	buf = ReadBuffer(index, sift_wt_write_seghdr(wt, &h, ext, n, extra, extralen));
	sift_wt_finish(wt);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	values[0] = Int32GetDatum(SiftPageGetUsed(BufferGetPage(buf)));
	UnlockReleaseBuffer(buf);
	values[1] = Int32GetDatum(SIFT_MAX_EXTENTS);
	MAINT_UNLOCK(index, ShareLock);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
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
