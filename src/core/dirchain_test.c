/*
 * core/dirchain_test.c - test-only SQL functions sift._test_dir_* for the chained directory (SIFT_TESTING builds, registered by pg_sift_test).
 *
 * A test index (store_test_am.c) takes entries that name no storage: sift._test_dir_edit inserts, deletes, reverses and touches them through the
 * ordinary directory update, so that a model in SQL can follow every state of the directory chain. sift._test_dir_check says whether the blocks of
 * the chain and the free list are disjoint and inside the file.
 */
#include "store_test_int.h"
#include "col/order_bound.h"
#include "core/dirpart.h"
#include "core/store_int.h"
#include "nodes/bitmapset.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_dir_edit);
PG_FUNCTION_INFO_V1(sift_test_dir_list);
PG_FUNCTION_INFO_V1(sift_test_dir_info);
PG_FUNCTION_INFO_V1(sift_test_dir_check);
PG_FUNCTION_INFO_V1(sift_test_dir_read_stale);
PG_FUNCTION_INFO_V1(sift_test_dir_parts);
PG_FUNCTION_INFO_V1(sift_test_dir_part_bounds);

typedef struct DirEditArg
{
	const char *op;
	int			pos;
	int			n;
	uint64		first_gen;
} DirEditArg;

static const OrderCtx two_leaves = {.nleaf = 2},
			 one_leaf = {.nleaf = 1};

/* The lower bound of the partition made by the entry of generation gen: two leaves, ascending with gen, the second the least integer, the greatest or NULL */
static BoundKey
part_key(uint64 gen)
{
	static const int64 second[] = {PG_INT64_MIN, PG_INT64_MAX, 0};

	return (BoundKey) {.k = {(int64) (gen / 3) - 5, second[gen % 3], PG_INT64_MIN},.nul = {false, gen % 3 == 2}};
}

/* nold entries from pos become nnew new ones, each a partition of its own, with the bound part_key names when bounded (sift_dir_part_replace) */
static void
repartition(SiftMetaPageData *meta, SiftDirectoryData *dir, int pos, int nold, int nnew, bool bounded)
{
	SiftSegEntry *segs = dir->segs;
	uint64	   *old = palloc(nold * sizeof(uint64));
	uint8		(*lo)[SIFT_BOUND_BYTES] = palloc(Max(nnew, 1) * SIFT_BOUND_BYTES);

	for (int i = 0; i < nold; i++)
		old[i] = segs[pos + i].gen;
	memmove(&segs[pos + nnew], &segs[pos + nold], (dir->nsegs - pos - nold) * sizeof(SiftSegEntry));
	for (int i = 0; i < nnew; i++)
	{
		uint64		gen = meta->next_gen++;
		BoundKey	bk = part_key(gen);
		SiftBound	b;

		segs[pos + i] = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_VALRUN,.header_blk = InvalidBlockNumber,.ndocs = gen * 7};
		sift_colk_bound_pack(&two_leaves, &bk, &b);
		sift_bound_pack(&b, lo[i]);
	}
	dir->nsegs += nnew - nold;
	sift_dir_part_replace(old, nold, &segs[pos], nnew, bounded ? lo : NULL);
}

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
	else if (strcmp(a->op, "part") == 0)
		repartition(meta, dir, a->pos, a->n, a->n, false);
	else if (strcmp(a->op, "bound") == 0)
		repartition(meta, dir, a->pos, a->n, a->n, true);
	else if (strcmp(a->op, "split") == 0)
		repartition(meta, dir, a->pos, 1, a->n, false);
	else if (strcmp(a->op, "merge") == 0)
		repartition(meta, dir, a->pos, a->n, 1, false);
	else if (strcmp(a->op, "set") == 0)
	{
		for (int i = 0; i < a->n; i++)
			segs[a->pos + i].ndocs++;
	}
	else
		elog(ERROR, "unknown directory edit \"%s\"", a->op);
}

/* sift._test_dir_edit(index, op, pos, n): part makes the n entries from pos partitions of their own (new entries, one each), bound the same with a lower bound each (part_key), split cuts the partition at pos in n, merge makes the n from pos one; rechain writes the pages of a chain at or above block pos again below it; ins n new entries at pos, del n from pos, rev the n from pos, set (count a document in) the n from pos; the first generation inserted, else 0 */
Datum
sift_test_dir_edit(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	DirEditArg	arg = {.op = text_to_cstring(PG_GETARG_TEXT_PP(1)),.pos = PG_GETARG_INT32(2),.n = PG_GETARG_INT32(3)};
	SiftDirSnapshot *cur = sift_dir_read(index);

	if ((strcmp(arg.op, "rechain") != 0 && (arg.pos < 0 || arg.n < 0 || arg.pos + (strcmp(arg.op, "ins") == 0 || strcmp(arg.op, "split") == 0 ? 0 : arg.n) > cur->nsegs || (strcmp(arg.op, "split") == 0 && arg.pos >= cur->nsegs))) ||
		cur->nsegs + (strcmp(arg.op, "ins") == 0 || strcmp(arg.op, "split") == 0 ? arg.n : 0) > SIFT_DIR_WIDE_MAX)
		elog(ERROR, "directory edit %s %d %d does not fit %d entries", arg.op, arg.pos, arg.n, cur->nsegs);
	MAINT_LOCK(index, ShareLock);
	if (strcmp(arg.op, "rechain") == 0)
		sift_dir_update(index, &(SiftDirChange) {.chain_below = arg.pos,.extra = InvalidBuffer});
	else
		sift_dir_update(index, &(SiftDirChange) {.fn = edit_entries,.arg = &arg,.nadd = arg.n,.extra = InvalidBuffer});
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
	SiftDirSnapshot *list;

	InitMaterializedSRF(fcinfo, 0);
	list = sift_dir_read(index);
	for (int i = 0; i < list->nsegs; i++)
	{
		Datum		values[3] = {Int32GetDatum(i), Int64GetDatum(list->segs[i].gen), Int64GetDatum(list->segs[i].ndocs)};
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
	SiftDirSnapshot *list;
	BlockNumber top = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	list = sift_dir_read(index);
	values[0] = Int64GetDatum(list->version);
	values[1] = Int32GetDatum(list->nsegs);
	values[2] = BoolGetDatum(list->nchain > 0);
	values[3] = Int32GetDatum(list->nchain);
	values[4] = Int32GetDatum(list->meta.format_minor);
	for (int i = 0; i < list->nchain; i++)
		top = Max(top, list->chain[i]);
	values[5] = Int64GetDatum(top);
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* sift._test_dir_read_stale(index): reads the chain as a reader would that found a page of it recycled (the version its root was written under is not the one expected): the retry error on a standby, damage on a primary */
Datum
sift_test_dir_read_stale(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftSegEntry *segs;
	SiftDirChain chain;

	if (snap->nchain == 0)
		elog(ERROR, "sift test: the directory of \"%s\" is not chained", RelationGetRelationName(index));
	(void) sift_dirchain_read(index, snap->chain[0], snap->version + 1, 0, &segs, &chain);
	PG_RETURN_VOID();
}

/* sift._test_dir_parts(index): the partitions the chain of any sift index lists, in directory order: the position of the first entry, the entries, the lineage and the generation of the first entry */
Datum
sift_test_dir_parts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftDirPart *parts;
	SiftSegEntry *segs;
	int			np = sift_dir_parts_read(index, snap, &segs, &parts);

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < np; i++)
	{
		Datum		values[4] = {Int32GetDatum(parts[i].first), Int32GetDatum(parts[i].n), Int64GetDatum(parts[i].lineage), Int64GetDatum(segs[parts[i].first].gen)};
		bool		nulls[4] = {0};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_dir_part_bounds(index): of the partitions of a sift_store_test index, whether the lower bound the chain holds is the one the partition was made with, packed, unpacked and compared leaf by leaf */
Datum
sift_test_dir_part_bounds(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftDirPart *parts;
	SiftSegEntry *segs;
	int			np = sift_dir_parts_read(index, snap, &segs, &parts);

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < np; i++)
	{
		BoundKey	want = part_key(segs[parts[i].first].gen),
					got;
		SiftBound	b,
					packed;
		uint8		bytes[SIFT_BOUND_BYTES];
		bool		stored = sift_bound_unpack(parts[i].lo, &b);
		BoundKey	lowest;
		SiftBound	first_leaf;
		Datum		values[3];
		bool		nulls[3] = {0};

		if (stored)
			sift_colk_bound_unpack(&two_leaves, &b, &got);
		sift_colk_bound_pack(&two_leaves, &want, &packed);
		sift_bound_pack(&packed, bytes);
		sift_colk_bound_pack(&one_leaf, &want, &first_leaf);
		sift_colk_bound_unpack(&two_leaves, &first_leaf, &lowest);
		values[0] = Int32GetDatum(parts[i].first);
		values[1] = BoolGetDatum(stored && memcmp(&got.k, &want.k, sizeof(want.k)) == 0 && memcmp(&got.nul, &want.nul, sizeof(want.nul)) == 0 &&
								 memcmp(bytes, parts[i].lo, SIFT_BOUND_BYTES) == 0);
		values[2] = BoolGetDatum(lowest.k[0] == want.k[0] && lowest.k[1] == PG_INT64_MIN && !lowest.nul[0] && !lowest.nul[1]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
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
	SiftDirSnapshot *list;
	ChainBlocks c = {.index = index};
	Buffer		metabuf;

	list = sift_dir_read(index);
	c.hwm = list->meta.high_water;
	for (int i = 0; i < list->nchain; i++)
	{
		if (list->chain[i] < FIRST_DATA_BLKNO || list->chain[i] >= c.hwm)
			elog(ERROR, "directory chain block %u lies outside [%d, %u)", list->chain[i], FIRST_DATA_BLKNO, c.hwm);
		if (bms_is_member(list->chain[i], c.set))
			elog(ERROR, "directory chain block %u appears twice", list->chain[i]);
		c.set = bms_add_member(c.set, list->chain[i]);
	}
	metabuf = lock_meta(index, BUFFER_LOCK_SHARE);
	sift_freelist_walk(index, metabuf, free_page_blocks, &c);
	UnlockReleaseBuffer(metabuf);
	relation_close(index, AccessShareLock);
	PG_RETURN_INT32(list->nchain);
}

#endif							/* SIFT_TESTING */
