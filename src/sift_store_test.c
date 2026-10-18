/*
 * sift_store_test.c - test-only SQL functions sift._test_* for the store
 * module, compiled only with SIFT_TESTING (make SIFT_TESTING=1) and
 * registered by the separate pg_sift_test extension (pg_sift_test--1.0.sql), so a
 * production build exports none of them.
 *
 * A test index uses the sift_store_test access method: ambuild writes one
 * segment per heap row (v bytes, seed v + 1; |v| zero bytes, seed 0, when v
 * is negative) through a BULK target. Test
 * segments are real SEGHEADER + DATA streams whose content is a function of
 * (seed, payload offset); the header keeps seed, piece and align so readers
 * can replay the writer's piece sequence and alignment padding. Seed 0 writes
 * zeros without generating content, for throughput runs (read modes 2-3).
 *
 * Locks: writers and droppers take LockPage(index, 0, ShareLock), the sweep
 * and check take ExclusiveLock (the maintenance lock in production), drops
 * also serialize on LockPage(index, 1, ExclusiveLock).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/amapi.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

#include "sift_maint.h"
#include "sift_store.h"

PG_FUNCTION_INFO_V1(sift_test_am_handler);
PG_FUNCTION_INFO_V1(sift_test_seg_write);
PG_FUNCTION_INFO_V1(sift_test_seg_read);
PG_FUNCTION_INFO_V1(sift_test_seg_drop);
PG_FUNCTION_INFO_V1(sift_test_segments);
PG_FUNCTION_INFO_V1(sift_test_alloc);
PG_FUNCTION_INFO_V1(sift_test_recycle);
PG_FUNCTION_INFO_V1(sift_test_check);
PG_FUNCTION_INFO_V1(sift_test_sweep);
PG_FUNCTION_INFO_V1(sift_test_truncate);
PG_FUNCTION_INFO_V1(sift_test_info);
PG_FUNCTION_INFO_V1(sift_test_maint_failures);
PG_FUNCTION_INFO_V1(sift_test_maint_wait_us);
PG_FUNCTION_INFO_V1(sift_test_freelist);
PG_FUNCTION_INFO_V1(sift_test_page);
PG_FUNCTION_INFO_V1(sift_test_poke);
PG_FUNCTION_INFO_V1(sift_test_churn);

void
sift_store_test_init(void)
{
	DefineCustomIntVariable("sift.test_fail_pages",
							"Testing: pages a promotion or merge writes before it fails (0 = never).",
							NULL, &sift_test_fail_pages, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_merge_max_kb",
							"Testing: kB of segments one merge may read (0 = 16 GB).",
							NULL, &sift_test_merge_max_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_merge_sleep_ms",
							"Testing: ms a merge of the maintenance worker sleeps, its segments claimed (0 = no).",
							NULL, &sift_test_merge_sleep_ms, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_room_segments",
							"Testing: directory entries from which inserters merge (0 = 112).",
							NULL, &sift_test_room_segments, 0, 0, SIFT_MAX_SEGMENTS, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_run_gap",
							 "Testing: every run a segment stream takes is followed by a page taken by another writer.",
							 NULL, &sift_test_run_gap, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_max_extents",
							"Testing: extents a segment may use (0 = what its header page holds).",
							NULL, &sift_test_max_extents, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

#define PAYLOAD			SIFT_PAGE_PAYLOAD
#define MAX_RANDOM_PIECE (3 * PAYLOAD)
#define MAINT_LOCK(index, mode)	LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode)
#define MAINT_UNLOCK(index, mode) UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, mode)
#define ULL(x)			((unsigned long long) (x))

typedef struct TestSeg
{
	uint64		gen;
	BlockNumber header;
	uint64		size;			/* logical bytes, padding included */
	uint64		payload;		/* bytes excluding padding */
	uint64		seed;
	uint32		piece;
	uint32		align;
	int			nextents;
	SiftExtent *extents;
} TestSeg;

static inline uint64
mix64(uint64 x)
{
	x += UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

/* Payload bytes [k, k + len) of a stream with this seed */
static void
fill(uint64 seed, uint64 k, char *dst, size_t len)
{
	while (len > 0)
	{
		uint64		w = mix64(seed * UINT64CONST(0x100000001B3) + (k >> 3));
		size_t		sh = k & 7;
		size_t		n = Min(8 - sh, len);

		memcpy(dst, (char *) &w + sh, n);
		dst += n;
		k += n;
		len -= n;
	}
}

/* Next piece length, capped at left */
static inline uint32
next_piece(uint64 *rng, uint32 piece, uint64 left)
{
	uint64		r = mix64((*rng)++);
	uint64		len = piece > 0 ? piece : 1 + (r >> 8) % ((r & 3) == 0 ? MAX_RANDOM_PIECE : 300);

	return Min(len, left);
}

/* Independent model of sift_sw_align: where the next byte lands */
static uint64
aligned_pos(uint64 pos, uint32 align)
{
	for (;;)
	{
		size_t		in = pos % PAYLOAD;
		size_t		room = in ? PAYLOAD - in : 0;
		size_t		pad = (size_t) -(SIFT_PAGE_DATA_OFFSET + in) & (align - 1);

		if (room == 0 || pad < room)
			return pos + pad;
		pos += room;
	}
}

static Relation
open_index(Oid relid, LOCKMODE mode)
{
	Relation	index = relation_open(relid, mode);

	if (index->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("\"%s\" is not an index", RelationGetRelationName(index))));
	return index;
}

static void
sleep_ms(int ms)
{
	for (; ms > 0; ms -= 10)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(Min(ms, 10) * 1000L);
	}
}

/* ---------- segments ---------- */

static void
write_segment(SiftWriteTarget *wt, uint64 gen, uint64 payload, uint64 seed, uint32 piece,
			  uint32 align, SiftSegEntry *entry)
{
	SiftStreamWriter *sw = sift_sw_begin(wt, gen);
	char	   *buf = palloc0(Max(piece, MAX_RANDOM_PIECE));
	uint64		rng = seed;
	uint64		k = 0;
	SiftExtent *ext;
	int			n;
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);

	while (k < payload)
	{
		uint32		len = next_piece(&rng, piece, payload - k);

		if (align)
		{
			uint64		want = aligned_pos(sift_sw_tell(sw), align);

			sift_sw_align(sw, align);
			if (sift_sw_tell(sw) != want)
				elog(ERROR, "alignment to %u put the stream at %llu, expected %llu",
					 align, ULL(sift_sw_tell(sw)), ULL(want));
		}
		if (seed != 0)
			fill(seed, k, buf, len);
		sift_sw_write(sw, buf, len);
		k += len;
	}
	ext = sift_sw_finish(sw, &n);
	sift_page_init(page.data, SIFT_PT_SEGHEADER, gen);
	hdr->gen = gen;
	hdr->ndocs = payload;
	hdr->nterms = seed;
	hdr->sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(sw);
	hdr->sections[SIFT_SEC_POSTINGS].len = piece;
	hdr->sections[SIFT_SEC_DICT].len = align;
	hdr->nextents = n;
	memcpy(hdr->extents, ext, n * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + n * sizeof(SiftExtent));
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->header_blk = sift_wt_write_page(wt, page.data);
	entry->bytes = sift_sw_tell(sw);
	entry->ndocs = payload;
	pfree(buf);
}

static bool
find_segment(Relation index, uint64 gen, SiftDirSnapshot *snap, TestSeg *seg)
{
	Buffer		buf;
	SiftSegHeaderData *hdr;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].gen != gen)
			continue;
		seg->gen = gen;
		seg->header = snap->segs[i].header_blk;
		buf = ReadBuffer(index, seg->header);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(index, buf, SIFT_PT_SEGHEADER, gen);
		hdr = (SiftSegHeaderData *) SiftPageGetData(BufferGetPage(buf));
		seg->size = hdr->sections[SIFT_SEC_DOCMAP].len;
		seg->payload = hdr->ndocs;
		seg->seed = hdr->nterms;
		seg->piece = hdr->sections[SIFT_SEC_POSTINGS].len;
		seg->align = hdr->sections[SIFT_SEC_DICT].len;
		seg->nextents = Min(hdr->nextents, SIFT_MAX_EXTENTS);
		seg->extents = palloc(Max(seg->nextents, 1) * sizeof(SiftExtent));
		memcpy(seg->extents, hdr->extents, seg->nextents * sizeof(SiftExtent));
		UnlockReleaseBuffer(buf);
		return true;
	}
	return false;
}

static void
mismatch(Relation index, const TestSeg *seg, uint64 off)
{
	ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
					errmsg("sift test segment %llu of \"%s\" differs at offset %llu",
						   ULL(seg->gen), RelationGetRelationName(index), ULL(off))));
}

/* Replays the writer: pieces, padding, content and pointer alignment */
static uint64
read_replay(Relation index, SiftStreamReader *sr, const TestSeg *seg)
{
	size_t		max = Max(seg->piece, MAX_RANDOM_PIECE);
	char	   *want = palloc(max);
	char	   *scratch = palloc_aligned(max, SIFT_SIMD_ALIGN, 0);
	uint64		rng = seg->seed;
	uint64		pos = 0;
	uint64		k = 0;

	sift_sr_prefetch(sr, 0, seg->size);
	while (k < seg->payload)
	{
		uint32		len = next_piece(&rng, seg->piece, seg->payload - k);
		const char *got;

		if (seg->align)
		{
			uint64		to = aligned_pos(pos, seg->align);

			for (; pos < to; pos++)
				if (*(const char *) sift_sr_get(sr, pos, 1, scratch) != 0)
					mismatch(index, seg, pos);
		}
		got = sift_sr_get(sr, pos, len, scratch);
		if (seg->align && got != scratch && (uintptr_t) got % seg->align != 0)
			elog(ERROR, "direct pointer for offset %llu is not %u-aligned", ULL(pos), seg->align);
		fill(seg->seed, k, want, len);
		if (memcmp(got, want, len) != 0)
			mismatch(index, seg, pos);
		pos += len;
		k += len;
	}
	if (pos != seg->size)
		elog(ERROR, "segment %llu holds %llu bytes, replay ended at %llu",
			 ULL(seg->gen), ULL(seg->size), ULL(pos));
	return k;
}

static uint64
read_random(Relation index, SiftStreamReader *sr, const TestSeg *seg)
{
	char	   *want = palloc(MAX_RANDOM_PIECE);
	char	   *scratch = palloc(MAX_RANDOM_PIECE);
	uint64		rng = seg->seed ^ 0x5eed;
	uint64		total = 0;

	if (seg->align)
		elog(ERROR, "random reads need an unaligned segment");
	for (int i = 0; i < 2000 && seg->size > 0; i++)
	{
		uint64		off = mix64(rng++) % seg->size;
		uint32		len = next_piece(&rng, 0, seg->size - off);

		fill(seg->seed, off, want, len);
		if (memcmp(sift_sr_get(sr, off, len, scratch), want, len) != 0)
			mismatch(index, seg, off);
		total += len;
	}
	return total;
}

/* Throughput: every byte through direct page pointers or 1 MB copies */
static uint64
read_bulk(SiftStreamReader *sr, const TestSeg *seg, size_t chunk)
{
	char	   *scratch = chunk > PAYLOAD ? palloc(chunk) : NULL;
	uint64		sum = 0;

	sift_sr_prefetch(sr, 0, seg->size);
	for (uint64 off = 0; off < seg->size; off += chunk)
	{
		size_t		len = Min(chunk, seg->size - off);
		const unsigned char *p = sift_sr_get(sr, off, len, scratch);

		sum += p[0] + p[len - 1];
	}
	return sum == UINT64CONST(0x123456789) ? 0 : seg->size;
}

static uint64
read_segment(Relation index, const TestSeg *seg, int mode)
{
	SiftStreamReader sr;
	uint64		n;

	sift_sr_init(&sr, index, seg->extents, seg->nextents, seg->gen);
	if (mode == 0)
		n = read_replay(index, &sr, seg);
	else if (mode == 1)
		n = read_random(index, &sr, seg);
	else
		n = read_bulk(&sr, seg, mode == 2 ? PAYLOAD : 1024 * 1024);
	sift_sr_release(&sr);
	return n;
}

static void
add_seg(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	dir->segs[dir->nsegs++] = *(SiftSegEntry *) arg;
}

static void
drop_seg(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	uint64		gen = *(uint64 *) arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == gen)
		{
			memmove(&dir->segs[i], &dir->segs[i + 1], (dir->nsegs - i - 1) * sizeof(SiftSegEntry));
			dir->nsegs--;
			return;
		}
}

static uint64
seg_write(Relation index, uint64 nbytes, uint64 seed, uint32 piece, uint32 align)
{
	SiftWriteTarget *wt;
	SiftSegEntry entry;
	uint64		gen;

	MAINT_LOCK(index, ShareLock);
	gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	write_segment(wt, gen, nbytes, seed, piece, align, &entry);
	sift_wt_finish(wt);
	sift_dir_update(index, &(SiftDirChange) {.fn = add_seg, .arg = &entry, .extra = InvalidBuffer});
	MAINT_UNLOCK(index, ShareLock);
	return gen;
}

static int64
seg_drop(Relation index, uint64 gen)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TestSeg		seg;
	SiftExtent *rel;
	int64		blocks = 1;

	MAINT_LOCK(index, ShareLock);
	LockPage(index, SIFT_DIRECTORY_BLKNO, ExclusiveLock);
	if (!find_segment(index, gen, snap, &seg))
		blocks = -1;
	else
	{
		rel = palloc((seg.nextents + 1) * sizeof(SiftExtent));
		rel[0] = (SiftExtent) {seg.header, 1};
		for (int i = 0; i < seg.nextents; i++)
		{
			rel[i + 1] = seg.extents[i];
			blocks += seg.extents[i].nblocks;
		}
		sift_dir_update(index, &(SiftDirChange) {.fn = drop_seg, .arg = &gen, .release = rel,
				.nrelease = seg.nextents + 1, .extra = InvalidBuffer});
	}
	UnlockPage(index, SIFT_DIRECTORY_BLKNO, ExclusiveLock);
	MAINT_UNLOCK(index, ShareLock);
	return blocks;
}

static int64
sweep(Relation index)
{
	Bitmapset  *orphans;
	SiftExtent *rel;
	int			n = 0;
	int64		blocks = 0;
	int			b = -1;

	MAINT_LOCK(index, ExclusiveLock);
	orphans = sift_store_orphans(index);
	rel = palloc(Max(bms_num_members(orphans), 1) * sizeof(SiftExtent));
	while ((b = bms_next_member(orphans, b)) >= 0)
	{
		if (n > 0 && rel[n - 1].start + rel[n - 1].nblocks == (BlockNumber) b)
			rel[n - 1].nblocks++;
		else
			rel[n++] = (SiftExtent) {b, 1};
		blocks++;
	}
	if (n > 0)
		sift_dir_update(index, &(SiftDirChange) {.release = rel, .nrelease = n, .extra = InvalidBuffer});
	MAINT_UNLOCK(index, ExclusiveLock);
	return blocks;
}

/* release: 0 none (orphans), 1 all, 2 every second extent */
static BlockNumber
alloc_many(Relation index, uint32 nblocks, int count, int release)
{
	SiftExtent *ext = palloc(Max(count, 1) * sizeof(SiftExtent));
	Buffer		bufs[SIFT_ALLOC_MAX_LOCKED];
	bool		locked = nblocks <= SIFT_ALLOC_MAX_LOCKED;
	BlockNumber last;
	int			n = 0;
	uint64		gen;

	MAINT_LOCK(index, ShareLock);
	gen = sift_store_new_gen(index);
	for (int i = 0; i < count; i++)
	{
		ext[i].start = sift_alloc_extent(index, nblocks, SIFT_PT_DATA, gen, locked ? bufs : NULL);
		ext[i].nblocks = nblocks;
		for (uint32 j = 0; locked && j < nblocks; j++)
			UnlockReleaseBuffer(bufs[j]);
	}
	last = count > 0 ? ext[count - 1].start : InvalidBlockNumber;
	for (int i = 0; i < count; i++)
		if (release == 1 || (release == 2 && i % 2 == 0))
			ext[n++] = ext[i];
	if (n > 0)
		sift_dir_update(index, &(SiftDirChange) {.release = ext, .nrelease = n, .extra = InvalidBuffer});
	MAINT_UNLOCK(index, ShareLock);
	return last;
}

/* ---------- test access method ---------- */

typedef struct BuildState
{
	int			n;
	int			max;
	int32	   *v;
} BuildState;

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *arg)
{
	BuildState *bs = arg;

	if (isnull[0])
		return;
	if (bs->n == bs->max)
	{
		bs->max = Max(16, bs->max * 2);
		bs->v = bs->v ? repalloc(bs->v, bs->max * sizeof(int32)) : palloc(bs->max * sizeof(int32));
	}
	bs->v[bs->n++] = DatumGetInt32(values[0]);
}

typedef struct Publish
{
	int			n;
	SiftSegEntry *entries;
} Publish;

static void
publish_all(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Publish    *p = arg;

	meta->next_gen = p->n + 1;
	memcpy(dir->segs, p->entries, p->n * sizeof(SiftSegEntry));
	dir->nsegs = p->n;
}

static IndexBuildResult *
test_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	BuildState	bs = {0};
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	Publish		pub;
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));

	res->heap_tuples = table_index_build_scan(heap, index, info, true, false, build_cb, &bs, NULL);
	pub.n = Min(bs.n, SIFT_MAX_SEGMENTS);
	pub.entries = palloc(Max(pub.n, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < pub.n; i++)
		write_segment(wt, i + 1, i64abs((int64) bs.v[i]), bs.v[i] < 0 ? 0 : (uint64) bs.v[i] + 1, 0, 0, &pub.entries[i]);
	sift_store_init_meta(wt, sift_index_options(index), 0);
	sift_wt_finish(wt);
	if (pub.n > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish_all, .arg = &pub, .extra = InvalidBuffer});
	res->index_tuples = pub.n;
	return res;
}

static void
test_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta(wt, sift_index_options(index), 0);
	sift_wt_finish(wt);
}

static bool
test_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer tid, Relation heap,
			  IndexUniqueCheck check, bool unchanged, IndexInfo *info)
{
	return false;
}

static IndexBulkDeleteResult *
test_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback cb, void *state)
{
	return stats ? stats : palloc0(sizeof(IndexBulkDeleteResult));
}

static IndexBulkDeleteResult *
test_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats;
}

static void
test_amcostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count, Cost *startup,
					Cost *total, Selectivity *sel, double *corr, double *pages)
{
	*startup = *total = 1.0e10;
	*sel = 1.0;
	*corr = 0.0;
	*pages = 1.0;
}

static bytea *
test_amoptions(Datum reloptions, bool validate)
{
	return NULL;
}

static bool
test_amvalidate(Oid opclassoid)
{
	return true;
}

static IndexScanDesc
test_ambeginscan(Relation index, int nkeys, int norderbys)
{
	elog(ERROR, "sift_store_test indexes cannot be scanned");
	pg_unreachable();
}

static void
test_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
}

static void
test_amendscan(IndexScanDesc scan)
{
}

Datum
sift_test_am_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 1;
	am->amkeytype = InvalidOid;
	am->ambuild = test_ambuild;
	am->ambuildempty = test_ambuildempty;
	am->aminsert = test_aminsert;
	am->ambulkdelete = test_ambulkdelete;
	am->amvacuumcleanup = test_amvacuumcleanup;
	am->amcostestimate = test_amcostestimate;
	am->amoptions = test_amoptions;
	am->amvalidate = test_amvalidate;
	am->ambeginscan = test_ambeginscan;
	am->amrescan = test_amrescan;
	am->amendscan = test_amendscan;
	PG_RETURN_POINTER(am);
}

/* ---------- SQL functions ---------- */

Datum
sift_test_seg_write(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		nbytes = PG_GETARG_INT64(1);
	int32		piece = PG_GETARG_INT32(3);
	int32		align = PG_GETARG_INT32(4);
	uint64		gen;

	if (nbytes < 0 || piece < 0 || align < 0)
		elog(ERROR, "negative size");
	gen = seg_write(index, nbytes, PG_GETARG_INT64(2), piece, align);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(gen);
}

Datum
sift_test_seg_read(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TestSeg		seg;
	int64		n = -1;

	if (find_segment(index, PG_GETARG_INT64(1), snap, &seg))
	{
		sleep_ms(PG_GETARG_INT32(3));
		n = read_segment(index, &seg, PG_GETARG_INT32(2));
	}
	relation_close(index, AccessShareLock);
	PG_RETURN_INT64(n);
}

Datum
sift_test_seg_drop(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		n = seg_drop(index, PG_GETARG_INT64(1));

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

Datum
sift_test_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftDirSnapshot *tmp = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		TestSeg		seg;
		Datum		values[5];
		bool		nulls[5] = {0};

		if (!find_segment(index, snap->segs[i].gen, tmp, &seg))
			continue;
		values[0] = Int64GetDatum(seg.gen);
		values[1] = Int64GetDatum(seg.header);
		values[2] = Int64GetDatum(seg.size);
		values[3] = Int64GetDatum(seg.payload);
		values[4] = Int32GetDatum(seg.nextents);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

Datum
sift_test_alloc(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
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
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
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
	Relation	index = open_index(PG_GETARG_OID(0), mode);
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
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
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
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		n = truncate_tail(index);

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

/* Failed maintenance steps in a row (the background retries wait longer after each) */
Datum
sift_test_maint_failures(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot snap;

	sift_dir_read(index, &snap);
	relation_close(index, AccessShareLock);
	PG_RETURN_INT32(snap.meta.flags & SIFT_META_FAILURES_MASK);
}

/* The longest time this backend waited for the maintenance of others since the last call, in microseconds */
Datum
sift_test_maint_wait_us(PG_FUNCTION_ARGS)
{
	int64		us = sift_maint_wait_max_us;

	sift_maint_wait_max_us = 0;
	PG_RETURN_INT64(us);
}

static void
put_info(ReturnSetInfo *rsinfo, const char *key, int64 value)
{
	Datum		values[2] = {CStringGetTextDatum(key), Int64GetDatum(value)};
	bool		nulls[2] = {0};

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

Datum
sift_test_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	SMgrRelation smgr = RelationGetSmgr(index);
	SiftDirSnapshot snap;
	int64		pages = 0;
	int64		extents = 0;
	int64		blocks = 0;
	int64		pending = 0;
	Buffer		metabuf;
	BlockNumber blk;

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, &snap);
	metabuf = ReadBuffer(index, SIFT_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	blk = ((SiftMetaPageData *) SiftPageGetData(BufferGetPage(metabuf)))->freelist_head;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		SiftFreeListData *fl;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(index, buf, SIFT_PT_FREELIST, 0);
		fl = (SiftFreeListData *) SiftPageGetData(BufferGetPage(buf));
		pages++;
		for (uint32 i = 0; i < fl->n; i++)
		{
			extents++;
			blocks += fl->ext[i].nblocks;
			if (FullTransactionIdIsValid(fl->ext[i].safexid) &&
				!sift_store_removable(index, fl->ext[i].safexid))
				pending += fl->ext[i].nblocks;
		}
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
	UnlockReleaseBuffer(metabuf);
	put_info(rsinfo, "high_water", snap.meta.high_water);
	put_info(rsinfo, "nblocks", RelationGetNumberOfBlocks(index));
	put_info(rsinfo, "init_blocks", smgrexists(smgr, INIT_FORKNUM) ? (int64) smgrnblocks(smgr, INIT_FORKNUM) : -1);
	put_info(rsinfo, "next_gen", snap.meta.next_gen);
	put_info(rsinfo, "dir_version", snap.version);
	put_info(rsinfo, "nsegs", snap.nsegs);
	put_info(rsinfo, "freelist_head", BlockNumberIsValid(snap.meta.freelist_head) ? (int64) snap.meta.freelist_head : -1);
	put_info(rsinfo, "freelist_pages", pages);
	put_info(rsinfo, "free_extents", extents);
	put_info(rsinfo, "free_blocks", blocks);
	put_info(rsinfo, "pending_blocks", pending);
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

/* Free-list records in chain order */
Datum
sift_test_freelist(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot snap;
	BlockNumber blk;

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, &snap);
	for (blk = snap.meta.freelist_head; BlockNumberIsValid(blk);)
	{
		Buffer		buf = ReadBuffer(index, blk);
		SiftFreeListData *fl;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		sift_store_check_page(index, buf, SIFT_PT_FREELIST, 0);
		fl = (SiftFreeListData *) SiftPageGetData(BufferGetPage(buf));
		for (uint32 i = 0; i < fl->n; i++)
		{
			Datum		values[4] = {Int64GetDatum(blk), Int64GetDatum(fl->ext[i].start),
			Int64GetDatum(fl->ext[i].nblocks), BoolGetDatum(FullTransactionIdIsValid(fl->ext[i].safexid))};
			bool		nulls[4] = {0};

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
		blk = fl->next;
		UnlockReleaseBuffer(buf);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

Datum
sift_test_page(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), AccessShareLock);
	Buffer		buf = ReadBuffer(index, (BlockNumber) PG_GETARG_INT64(1));
	Page		page;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	values[0] = Int32GetDatum(PageIsNew(page) ? 0 : SiftPageGetOpaque(page)->type);
	values[1] = Int64GetDatum(PageIsNew(page) ? 0 : SiftPageGetOpaque(page)->gen);
	values[2] = Int32GetDatum(((PageHeader) page)->pd_lower);
	values[3] = Int32GetDatum(((PageHeader) page)->pd_upper);
	UnlockReleaseBuffer(buf);
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

/* Overwrites width bytes at offset with a little-endian value, without WAL */
Datum
sift_test_poke(PG_FUNCTION_ARGS)
{
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
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
	Relation	index = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	uint64		rng = PG_GETARG_INT64(1);
	int64		maxbytes = Max(PG_GETARG_INT64(2), 1);
	uint64		op = mix64(rng++) % 100;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TestSeg		seg;
	uint64		pick;
	const char *what;

	sift_dir_read(index, snap);
	pick = snap->nsegs > 0 ? snap->segs[mix64(rng++) % snap->nsegs].gen : 0;
	if (op < 35 && snap->nsegs < 96)
	{
		uint64		r = mix64(rng++);

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
			sleep_ms(mix64(rng++) % 20);
			read_segment(index, &seg, seg.align ? 0 : mix64(rng++) % 2);
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
		uint64		r = mix64(rng++);

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
