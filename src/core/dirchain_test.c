/*
 * core/dirchain_test.c - test-only SQL functions sift._test_dir_* for the chained directory (SIFT_TESTING builds, registered by pg_sift_test).
 *
 * A test index (store_test_am.c) takes entries that name no storage: sift._test_dir_edit inserts, deletes, reverses and touches them through the
 * ordinary directory update, so that a model in SQL can follow every state of the directory chain. sift._test_dir_check says whether the blocks of
 * the chain and the free list are disjoint and inside the file.
 */
#include "store_test_int.h"
#include "core/dirchain.h"
#include "core/store_int.h"
#include "nodes/bitmapset.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_dir_edit);
PG_FUNCTION_INFO_V1(sift_test_dir_list);
PG_FUNCTION_INFO_V1(sift_test_dir_info);
PG_FUNCTION_INFO_V1(sift_test_dir_check);

typedef struct DirEditArg
{
	const char *op;
	int			pos;
	int			n;
	uint64		first_gen;
} DirEditArg;

static void
edit_entries(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	DirEditArg *a = arg;
	SiftSegEntry *segs = dir->segs;

	if (strcmp(a->op, "ins") == 0)
	{
		memmove(&segs[a->pos + a->n], &segs[a->pos], (dir->nsegs - a->pos) * sizeof(SiftSegEntry));
		a->first_gen = meta->next_gen;
		for (int i = 0; i < a->n; i++)
		{
			uint64		gen = meta->next_gen++;

			segs[a->pos + i] = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_VALRUN,.header_blk = InvalidBlockNumber,.ndocs = gen * 7};
		}
		dir->nsegs += a->n;
	}
	else if (strcmp(a->op, "del") == 0)
	{
		memmove(&segs[a->pos], &segs[a->pos + a->n], (dir->nsegs - a->pos - a->n) * sizeof(SiftSegEntry));
		dir->nsegs -= a->n;
	}
	else if (strcmp(a->op, "rev") == 0)
	{
		for (int i = 0; i < a->n / 2; i++)
		{
			SiftSegEntry t = segs[a->pos + i];

			segs[a->pos + i] = segs[a->pos + a->n - 1 - i];
			segs[a->pos + a->n - 1 - i] = t;
		}
	}
	else if (strcmp(a->op, "set") == 0)
	{
		for (int i = 0; i < a->n; i++)
			segs[a->pos + i].ndocs++;
	}
	else
		elog(ERROR, "unknown directory edit \"%s\"", a->op);
}

/* sift._test_dir_edit(index, op, pos, n): rechain writes the pages of a chain at or above block pos again below it; ins n new entries at pos, del n from pos, rev the n from pos, set (count a document in) the n from pos; the first generation inserted, else 0 */
Datum
sift_test_dir_edit(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	DirEditArg	arg = {.op = text_to_cstring(PG_GETARG_TEXT_PP(1)),.pos = PG_GETARG_INT32(2),.n = PG_GETARG_INT32(3)};
	SiftDirList cur;

	sift_dir_read_list(index, &cur);
	if ((strcmp(arg.op, "rechain") != 0 && (arg.pos < 0 || arg.n < 0 || arg.pos + (strcmp(arg.op, "ins") == 0 ? 0 : arg.n) > cur.nsegs)) ||
		cur.nsegs + (strcmp(arg.op, "ins") == 0 ? arg.n : 0) > SIFT_DIR_WIDE_MAX)
		elog(ERROR, "directory edit %s %d %d does not fit %d entries", arg.op, arg.pos, arg.n, cur.nsegs);
	MAINT_LOCK(index, ShareLock);
	if (strcmp(arg.op, "rechain") == 0)
		sift_dir_update(index, &(SiftDirChange) {.chain_below = arg.pos,.extra = InvalidBuffer,.wide = true});
	else
		sift_dir_update(index, &(SiftDirChange) {.fn = edit_entries,.arg = &arg,.extra = InvalidBuffer,.wide = true});
	MAINT_UNLOCK(index, ShareLock);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(arg.first_gen);
}

/* sift._test_dir_list(index): the entries in directory order */
Datum
sift_test_dir_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirList list;

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read_list(index, &list);
	for (int i = 0; i < list.nsegs; i++)
	{
		Datum		values[3] = {Int32GetDatum(i), Int64GetDatum(list.segs[i].gen), Int64GetDatum(list.segs[i].ndocs)};
		bool		nulls[3] = {0};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_dir_info(index): version, entries, and the chain: its root, its leaves (nblocks - 1) */
Datum
sift_test_dir_info(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {0};
	SiftDirList list;
	BlockNumber top = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_dir_read_list(index, &list);
	values[0] = Int64GetDatum(list.version);
	values[1] = Int32GetDatum(list.nsegs);
	values[2] = BoolGetDatum(list.nchain > 0);
	values[3] = Int32GetDatum(list.nchain);
	values[4] = Int32GetDatum(list.meta.format_minor);
	for (int i = 0; i < list.nchain; i++)
		top = Max(top, list.chain[i]);
	values[5] = Int64GetDatum(top);
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

typedef struct ChainBlocks
{
	Bitmapset  *set;
	BlockNumber hwm;
	Relation	index;
} ChainBlocks;

static bool
free_page_blocks(BlockNumber blk, const SiftFreeListData *fl, void *arg)
{
	ChainBlocks *c = arg;

	if (bms_is_member(blk, c->set))
		elog(ERROR, "free list page %u is a block of the directory chain", blk);
	for (uint32 i = 0; i < fl->n; i++)
		for (BlockNumber b = fl->ext[i].start; b < fl->ext[i].start + fl->ext[i].nblocks; b++)
			if (bms_is_member(b, c->set))
				elog(ERROR, "block %u of the directory chain is free", b);
	return true;
}

/* sift._test_dir_check(index): the blocks of the chain are distinct, inside [2, high_water) and not on the free list; their number */
Datum
sift_test_dir_check(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirList list;
	ChainBlocks c = {.index = index};
	Buffer		metabuf;

	sift_dir_read_list(index, &list);
	c.hwm = list.meta.high_water;
	for (int i = 0; i < list.nchain; i++)
	{
		if (list.chain[i] < FIRST_DATA_BLKNO || list.chain[i] >= c.hwm)
			elog(ERROR, "directory chain block %u lies outside [%d, %u)", list.chain[i], FIRST_DATA_BLKNO, c.hwm);
		if (bms_is_member(list.chain[i], c.set))
			elog(ERROR, "directory chain block %u appears twice", list.chain[i]);
		c.set = bms_add_member(c.set, list.chain[i]);
	}
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	sift_freelist_walk(index, metabuf, free_page_blocks, &c);
	UnlockReleaseBuffer(metabuf);
	relation_close(index, AccessShareLock);
	PG_RETURN_INT32(list.nchain);
}

#endif							/* SIFT_TESTING */
