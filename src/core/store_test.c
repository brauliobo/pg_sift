/*
 * core/store_test.c - test-only SQL functions sift._test_* for the store
 * module, compiled only with SIFT_TESTING (make SIFT_TESTING=1) and
 * registered by the separate pg_sift_test extension (pg_sift_test--1.0.sql), so a
 * release build exports none of them.
 *
 * A test index uses the sift_store_test access method (core/store_test_am.c): ambuild writes one
 * segment per heap row (v bytes, seed v + 1; |v| zero bytes, seed 0, when v
 * is negative) through a BULK target. Test
 * segments are real SEGHEADER + DATA streams whose content is a function of
 * (seed, payload offset); the header keeps seed, piece and align so readers
 * can replay the writer's piece sequence and alignment padding. Seed 0 writes
 * zeros without generating content, for throughput runs (read modes 2-3).
 *
 * Locks: writers and droppers take LockPage(index, 0, ShareLock), the sweep
 * and check take ExclusiveLock (the maintenance lock in release), drops
 * also serialize on LockPage(index, 1, ExclusiveLock).
 *
 * This file: the test index, the segments written and read back. The allocation, recycling, truncation, free list, page and churn functions are
 * in store_ops_test.c.
 */
#include "store_test_int.h"

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_seg_write);
PG_FUNCTION_INFO_V1(sift_test_seg_read);
PG_FUNCTION_INFO_V1(sift_test_seg_drop);
PG_FUNCTION_INFO_V1(sift_test_segments);
/* a new setting starts the count of the directory updates it applies to */
static void
crash_dir_assign(int newval, void *extra)
{
	sift_test_dir_updates = 0;
}

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
	DefineCustomBoolVariable("sift.test_no_heap_positions",
							 "Testing: promotions and merges take no positions from the heap, as 3.7.0 did.",
							 NULL, &sift_test_no_heap_positions, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_run_gap",
							 "Testing: every run a segment stream takes is followed by a page taken by another writer.",
							 NULL, &sift_test_run_gap, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_crash_dir",
							"Testing: SIGKILL before (n) or after (-n) the nth directory update since this setting (0 = never).",
							NULL, &sift_test_crash_dir, 0, INT_MIN, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, crash_dir_assign, NULL);
	DefineCustomBoolVariable("sift.test_crash_worker",
							 "Testing: sift.test_crash_dir applies to background workers (on) or to the other processes (off).",
							 NULL, &sift_test_crash_worker, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_cut_with_standby",
							 "Testing: VACUUM cuts the file even while a standby streams.",
							 NULL, &sift_test_cut_with_standby, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_max_extents",
							"Testing: extents a segment may use (0 = what its header page holds).",
							NULL, &sift_test_max_extents, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

/* Payload bytes [k, k + len) of a stream with this seed */
static void
fill(uint64 seed, uint64 k, char *dst, size_t len)
{
	while (len > 0)
	{
		uint64		w = sift_mix64(seed * UINT64CONST(0x100000001B3) + (k >> 3));
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
	uint64		r = sift_mix64((*rng)++);
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

Relation
sift_test_open_index(Oid relid, LOCKMODE mode)
{
	Relation	index = relation_open(relid, mode);

	if (index->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("\"%s\" is not an index", RelationGetRelationName(index))));
	return index;
}

void
sleep_ms(int ms)
{
	for (; ms > 0; ms -= 10)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(Min(ms, 10) * 1000L);
	}
}

void
sift_test_write_segment(SiftWriteTarget *wt, uint64 gen, uint64 payload, uint64 seed, uint32 piece,
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

bool
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
		uint64		off = sift_mix64(rng++) % seg->size;
		uint32		len = next_piece(&rng, 0, seg->size - off);

		fill(seg->seed, off, want, len);
		if (memcmp(sift_sr_get(sr, off, len, scratch), want, len) != 0)
			mismatch(index, seg, off);
		total += len;
	}
	return total;
}

#define AHEAD_PIECES	40

typedef struct AheadPieces
{
	int			next;
	uint64		off[AHEAD_PIECES];
	uint32		len[AHEAD_PIECES];
} AheadPieces;

static bool
ahead_next(void *arg, uint64 *off, uint64 *len)
{
	AheadPieces *p = arg;

	if (p->next == AHEAD_PIECES)
		return false;
	*off = p->off[p->next];
	*len = p->len[p->next++];
	return true;
}

static int
ahead_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* Pieces here and there, read in the order they are read ahead from: the stream starts with the first (a reader that has not missed yet starts none) and every byte read matches */
static uint64
read_ahead(SiftStreamReader *sr, const TestSeg *seg)
{
	char	   *want = palloc(MAX_RANDOM_PIECE);
	char	   *scratch = palloc(MAX_RANDOM_PIECE);
	AheadPieces *p = palloc0(sizeof(AheadPieces));
	uint64		rng = seg->seed ^ 0xa4ead;
	uint64		total = 0;

	if (seg->align)
		elog(ERROR, "reads ahead need an unaligned segment");
	if (seg->size == 0)
		return 0;
	for (int i = 0; i < AHEAD_PIECES; i++)
		p->off[i] = sift_mix64(rng++) % seg->size;
	qsort(p->off, AHEAD_PIECES, sizeof(uint64), ahead_cmp);
	for (int i = 0; i < AHEAD_PIECES; i++)
		p->len[i] = next_piece(&rng, 0, seg->size - p->off[i]);
	sift_sr_ahead(sr, ahead_next, p, seg->size);
	sr->cold = true;
	for (int i = 0; i < AHEAD_PIECES; i++)
	{
		fill(seg->seed, p->off[i], want, p->len[i]);
		if (memcmp(sift_sr_get(sr, p->off[i], p->len[i], scratch), want, p->len[i]) != 0)
			mismatch(sr->index, seg, p->off[i]);
		total += p->len[i];
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

uint64
read_segment(Relation index, const TestSeg *seg, int mode)
{
	SiftStreamReader sr;
	uint64		n;

	sift_sr_init(&sr, index, seg->extents, seg->nextents, seg->gen);
	if (mode == 0)
		n = read_replay(index, &sr, seg);
	else if (mode == 1)
		n = read_random(index, &sr, seg);
	else if (mode == 4)
		n = read_ahead(&sr, seg);
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

uint64
seg_write(Relation index, uint64 nbytes, uint64 seed, uint32 piece, uint32 align)
{
	SiftWriteTarget *wt;
	SiftSegEntry entry;
	uint64		gen;

	MAINT_LOCK(index, ShareLock);
	gen = sift_store_new_gen(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	sift_test_write_segment(wt, gen, nbytes, seed, piece, align, &entry);
	sift_wt_finish(wt);
	sift_dir_update(index, &(SiftDirChange) {.fn = add_seg, .arg = &entry, .extra = InvalidBuffer});
	MAINT_UNLOCK(index, ShareLock);
	return gen;
}

int64
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

int64
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
BlockNumber
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

Datum
sift_test_seg_write(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
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
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
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
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), RowExclusiveLock);
	int64		n = seg_drop(index, PG_GETARG_INT64(1));

	relation_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

Datum
sift_test_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index(PG_GETARG_OID(0), AccessShareLock);
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


#endif							/* SIFT_TESTING */

