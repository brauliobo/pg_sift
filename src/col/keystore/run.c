/*
 * col/keystore/run.c - the keys of a scope key store: runs in the directory of its relation, the append that gives a new key its code,
 * and the view a backend keeps of them (docs/COL-SCOPE.md "Key types")
 *
 * The code space of a store is append-only: a key has exactly one code, the position at which it was appended, and a code never moves. A run
 * is an immutable segment of the directory (kind SIFT_SEG_VALRUN) that holds the keys of the codes [base, base + n) in code order, in the
 * serialized form of a string dictionary. A writer asks the store for the codes of a batch of keys: those it finds are answered, the others are
 * appended as one run, under the extension lock of the store (the one heavyweight lock that the members of a parallel build do not share: core/extlock.h) that is held for the append and no longer, and published by a directory update that is logged after the pages
 * of the run, so that a segment that names the codes (published later, by the writer) is never seen without them.
 * Runs are merged by the appender (adjacent runs, the codes unchanged), so that a lookup reads few of them, under a merge lock of their own that it only tries for: no append waits for a merge
 * (the members of a parallel build merge under the extension lock too, see merge_when_idle).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/inval.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/codec/codec.h"
#include "col/v1/enc/enc.h"
#include "col/v1/seg/seg.h"
#include "col/v1/seg/int.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"
#include "col/keystore/keystore.h"
#include "col/keystore/int.h"
#include "core/extlock.h"
#include "core/limits.h"
#include "core/page.h"
#include "core/sift.h"
#include "core/store/store.h"
#include "core/visibility/visibility.h"
#include "core/damage.h"
#include "core/catname.h"

#define VRUN_MAGIC			0x4e555256	/* 'VRUN' */
#define VRUN_MERGE_ABOVE	12			/* runs above which the appender merges the smallest adjacent ones */
#define VRUN_MERGE_WIDTH	4

typedef struct VRunHead
{
	uint32		magic;
	uint32		version;
	uint64		epoch;
	uint64		base;
	uint32		n;
	uint32		pad;
} VRunHead;

struct SiftKeyStore
{
	Relation	rel;
	KeyStoreView *view;
	ViewPin    *pin;
};

static void
ks_corrupt(Relation rel, const char *what)
{
	SIFT_DAMAGE_OTHER_DATA(errmsg("scope key store \"%s\": %s", RelationGetRelationName(rel), what));
}

SiftKeyStore *
sift_keystore_open(Oid store, LOCKMODE lock)
{
	return sift_keystore_open_epoch(store, lock, 0);
}

SiftKeyStore *
sift_keystore_open_epoch(Oid store, LOCKMODE lock, uint64 epoch)
{
	SiftKeyStore *ks = palloc(sizeof(SiftKeyStore));

	ks->rel = table_open(sift_rel_must_exist(store), lock);
	if (!sift_keystore_is_store(store))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a scope key store", RelationGetRelationName(ks->rel))));
	ks->view = kview_open(store, epoch, &ks->pin);
	return ks;
}

void
sift_keystore_close(SiftKeyStore *ks)
{
	kview_release(ks->pin);
	table_close(ks->rel, NoLock);
	pfree(ks);
}

static int
cmp_base(const void *a, const void *b)
{
	uint64		x = ((const SiftSegEntry *) a)->sum_dl,
				y = ((const SiftSegEntry *) b)->sum_dl;

	return x < y ? -1 : x > y;
}

/* The runs of the epoch of the store in code order */
static int
dir_runs(const SiftDirSnapshot *snap, uint64 epoch, SiftSegEntry *runs)
{
	int			n = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_VALRUN && snap->segs[i].reserved == epoch)
			runs[n++] = snap->segs[i];
	qsort(runs, n, sizeof(SiftSegEntry), cmp_base);
	return n;
}

/* The extents of the segment of an entry, from its header page */
static int
seg_extents(Relation rel, const SiftSegEntry *e, SiftExtent *ext)
{
	Buffer		buf = ReadBuffer(rel, e->header_blk);
	SiftSegHeaderData *hdr;
	int			n;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_SEGHEADER, e->gen);
	hdr = (SiftSegHeaderData *) SiftPageGetData(BufferGetPage(buf));
	n = Min(hdr->nextents, SIFT_MAX_EXTENTS);
	memcpy(ext, hdr->extents, n * sizeof(SiftExtent));
	UnlockReleaseBuffer(buf);
	return n;
}

/* Parses a run that the view does not have all of, and keeps it as a piece */
static void
load_run(SiftKeyStore *ks, const SiftSegEntry *e)
{
	KeyStoreView *v = ks->view;
	SiftExtent *ext = palloc(SIFT_MAX_EXTENTS * sizeof(SiftExtent));
	int			next = seg_extents(ks->rel, e, ext);
	SiftStreamReader sr;
	char	   *scratch = MemoryContextAllocHuge(CurrentMemoryContext, Max(e->bytes, 1));
	const VRunHead *head;
	SiftCDict  *run;
	Size		used;

	if (e->bytes < sizeof(VRunHead))
		ks_corrupt(ks->rel, "a run is shorter than its header");
	sift_cache_room(e->bytes);
	sift_sr_init(&sr, ks->rel, ext, next, e->gen);
	head = sift_sr_get(&sr, 0, e->bytes, scratch);
	if (head->magic != VRUN_MAGIC || head->epoch != v->key.epoch || head->base != e->sum_dl || head->n != e->ndocs)
		ks_corrupt(ks->rel, "a run does not match its directory entry");
	run = sift_cdict_parse(v->cx, (const char *) head + sizeof(VRunHead), e->bytes - sizeof(VRunHead), &used);
	if (run == NULL || run->n != head->n)
		ks_corrupt(ks->rel, "a run does not parse");
	kview_add(v, v->total, head->base, run);
	sift_sr_release(&sr);
	pfree(scratch);
	pfree(ext);
}

#define KEYSTORE_PREFETCH	32	/* hashes computed, and their slots prefetched, before they are probed */

/* The piece that holds a code */
static int
view_piece_at(const KeyStoreView *v, uint64 code)
{
	int lo = 0,
		hi = v->npiece - 1;

	Assert(code < v->total);
	while (lo < hi)
	{
		int mid = (lo + hi + 1) / 2;

		if (v->piece[mid].lo <= code)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static const char *
view_string(const KeyStoreView *v, uint64 code, uint32 *len)
{
	const ViewPiece *pc = &v->piece[view_piece_at(v, code)];

	return sift_cdict_str(pc->d, (uint32) (code - pc->dbase), len);
}

static void
slot_put(uint64 *slots, uint32 nslots, uint64 slot)
{
	uint32		i = (uint32) (slot >> 32) & (nslots - 1);

	while (slots[i])
		i = (i + 1) & (nslots - 1);
	slots[i] = slot;
}

/* The table of the view holds the hash of every string of the pieces (a code is found by its hash, the string is compared in its piece), at load 1/2 at the most */
static void
view_index(SiftKeyStore *ks)
{
	KeyStoreView *v = ks->view;
	int pi = v->indexed < v->total ? view_piece_at(v, v->indexed) : 0;

	if (v->slots == NULL || v->nslots < v->total * 2)
	{
		uint32		nslots = pg_nextpower2_32(Max(1024, v->total * 2));
		uint64	   *slots = MemoryContextAllocExtended(v->cx, (Size) nslots * sizeof(uint64), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

		for (uint32 i = 0; i < v->nslots; i++)
			if (v->slots[i])
				slot_put(slots, nslots, v->slots[i]);
		if (v->slots)
			pfree(v->slots);
		v->slots = slots;
		v->nslots = nslots;
	}
	for (uint64 c = v->indexed; c < v->total; c += KEYSTORE_PREFETCH)
	{
		uint32 n = Min(KEYSTORE_PREFETCH, v->total - c);
		uint64 slot[KEYSTORE_PREFETCH];

		for (uint32 j = 0; j < n; j++)
		{
			uint32 len;
			const char *s;

			while (pi + 1 < v->npiece && v->piece[pi + 1].lo <= c + j)
				pi++;
			s = sift_cdict_str(v->piece[pi].d, (uint32) (c + j - v->piece[pi].dbase), &len);

			slot[j] = (uint64) sift_cdict_mem_hash(s, len) << 32 | (c + j + 1);
			__builtin_prefetch(&v->slots[(uint32) (slot[j] >> 32) & (v->nslots - 1)], 1);
		}
		for (uint32 j = 0; j < n; j++)
			slot_put(v->slots, v->nslots, slot[j]);
	}
	v->indexed = v->total;
}

/* The code of a string among those indexed, or -1 */
static int64
view_find(const KeyStoreView *v, const char *s, uint32 len, uint32 hash)
{
	for (uint32 i = hash & (v->nslots - 1);; i = (i + 1) & (v->nslots - 1))
	{
		uint64		u = v->slots[i];
		uint32		l = 0;

		if (u == 0)
			return -1;
		if ((uint32) (u >> 32) == hash)
		{
			const char *cand = view_string(v, (uint32) u - 1, &l);

			if (l == len && memcmp(cand, s, len) == 0)
				return (uint32) u - 1;
		}
	}
}

/* The view up to the newest run published (a read of the directory and of the runs it has not read) */
static SiftDirSnapshot *
view_refresh(SiftKeyStore *ks)
{
	SiftDirSnapshot *snap;
	SiftSegEntry *runs;
	int			n;

	snap = sift_dir_read(ks->rel);
	runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	n = dir_runs(snap, ks->view->key.epoch, runs);
	if (!ks->view->loaded)
		kview_reset(ks->view);
	for (int i = 0; i < n; i++)
	{
		if (runs[i].sum_dl + runs[i].ndocs <= ks->view->total)
			continue;
		if (runs[i].sum_dl > ks->view->total)
			ks_corrupt(ks->rel, "the runs leave a gap in the codes");
		load_run(ks, &runs[i]);
	}
	pfree(runs);
	return snap;
}

uint64
sift_keystore_ncodes(SiftKeyStore *ks)
{
	return ks->view->total;
}

const char *
sift_keystore_string(SiftKeyStore *ks, uint32 code, uint32 *len)
{
	if (code >= ks->view->total)
		pfree(view_refresh(ks));
	if (code >= ks->view->total)
		ks_corrupt(ks->rel, "a code is past the codes of the store");
	return view_string(ks->view, code, len);
}

typedef struct DirEdit
{
	SiftSegEntry neu;
	uint64		old[VRUN_MERGE_WIDTH];
	int			nold;
} DirEdit;

static void
dir_edit(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	DirEdit    *ed = arg;
	int			k = 0;

	/* STOR[STOR-COL-10]: keystore run 16 KB min page pair per intern batch (until merge threshold) */
	for (int i = 0; i < dir->nsegs; i++)
	{
		bool		gone = false;

		for (int j = 0; j < ed->nold; j++)
			gone |= dir->segs[i].gen == ed->old[j];
		if (!gone)
			dir->segs[k++] = dir->segs[i];
	}
	dir->segs[k++] = ed->neu;
	dir->nsegs = k;
}

/*
 * Writes the strings [from, from + n) of src (of the view's pieces by code when src is NULL) as one run that starts at code lo, and publishes it,
 * replacing the entries in old (nold may be 0)
 */
static void
write_run(SiftKeyStore *ks, const SiftCDict *src, uint32 from, uint32 lo, uint32 n, const SiftCcPolicy *pol, const SiftSegEntry *old, int nold)
{
	Relation	rel = ks->rel;
	uint32		hi = lo + n;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift scope key store run");
	SiftCDict  *t = sift_cdict_create(cx);
	StringInfoData out;
	VRunHead	head = {VRUN_MAGIC, 1, 0, lo, hi - lo, 0};
	SiftWriteTarget *wt;
	SiftStreamWriter *sw;
	SiftExtent *ext;
	int			next;
	uint64		gen;
	DirEdit		ed = {0};
	SiftDirChange chg = {0};
	SiftExtent *rel_ext = NULL;
	int			nrel = 0;

	for (uint32 c = 0; c < n; c++)
	{
		uint32		len;
		const char *s = src ? sift_cdict_str(src, from + c, &len) : sift_keystore_string(ks, from + c, &len);

		(void) sift_cdict_intern(t, s, len);
	}
	initStringInfo(&out);
	appendBinaryStringInfo(&out, (const char *) &head, sizeof(head));
	t->packperm = true;
	sift_cdict_serialize(t, pol, DICT_LOOKUP, &out);
	wt = sift_wt_begin(rel, SIFT_WRITE_SHARED);
	sift_wt_expect(wt, out.len + BLCKSZ);
	gen = sift_store_new_gen(rel);
	sw = sift_sw_begin(wt, gen);
	sift_sw_write(sw, out.data, out.len);
	ext = sift_sw_finish(sw, &next);
	ed.neu = (SiftSegEntry) {.gen = gen, .kind = SIFT_SEG_VALRUN, .format = SIFT_COL_FORMAT_VSTORE, .reserved = 0,
							 .ndocs = hi - lo, .sum_dl = lo, .bytes = out.len};
	ed.neu.header_blk = sift_wt_write_seghdr(wt, &(SiftSegHeaderData) {.gen = gen, .ndocs = hi - lo}, ext, next, NULL, 0);
	sift_wt_finish(wt);
	ed.nold = nold;
	if (nold > 0)
		rel_ext = palloc(nold * (SIFT_MAX_EXTENTS + 1) * sizeof(SiftExtent));
	for (int i = 0; i < nold; i++)
	{
		ed.old[i] = old[i].gen;
		nrel += seg_extents(rel, &old[i], rel_ext + nrel);
		sift_extent_push(rel_ext, &nrel, old[i].header_blk, 1);
	}
	chg.fn = dir_edit;
	chg.nadd = 1;
	chg.arg = &ed;
	chg.release = rel_ext;
	chg.nrelease = nrel;
	sift_dir_update(rel, &chg);
	pfree(out.data);
	MemoryContextDelete(cx);
}

/*
 * The one session that appends codes: the codes it hands out start at the end of the store it read under this lock, and its run is published before the lock goes. The relation
 * extension lock, because it is the only heavyweight lock that conflicts between the members of a lock group too (src/backend/storage/lmgr/README, LockCheckConflicts: every other
 * lock of a group is shared): the members of a parallel build intern concurrently. Under it nothing may take a heavyweight lock: the xid is assigned first (the catalog the directory
 * update reads is read by the lock, core/extlock.h)
 */
static void
lock_extend(SiftKeyStore *ks)
{
	(void) GetCurrentTransactionId();
	sift_extension_lock(ks->rel);
}

/* The smallest window of adjacent runs, merged into one (the codes do not change) */
static void
merge_runs(SiftKeyStore *ks, const SiftCcPolicy *pol, const SiftDirSnapshot *snap)
{
	SiftSegEntry *runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	int			n = dir_runs(snap, ks->view->key.epoch, runs),
				best = 0;
	uint64		best_bytes = UINT64_MAX;

	if (n <= VRUN_MERGE_ABOVE)
	{
		pfree(runs);
		return;
	}
	for (int i = 0; i + VRUN_MERGE_WIDTH <= n; i++)
	{
		uint64		bytes = 0;

		for (int j = 0; j < VRUN_MERGE_WIDTH; j++)
			bytes += runs[i + j].bytes;
		if (bytes < best_bytes)
		{
			best_bytes = bytes;
			best = i;
		}
	}
	write_run(ks, NULL, runs[best].sum_dl, runs[best].sum_dl,
			  runs[best + VRUN_MERGE_WIDTH - 1].sum_dl + runs[best + VRUN_MERGE_WIDTH - 1].ndocs - runs[best].sum_dl, pol, &runs[best], VRUN_MERGE_WIDTH);
	pfree(runs);
}

/*
 * The merge of the runs after an append, by the one session that gets the merge lock: another that interns meanwhile appends (the append is under the extension lock, which the merge
 * does not hold) and leaves the merge to the one that holds it. The runs a skipped merge leaves are merged by the next append. The members of a lock group share the merge lock (page
 * locks do not conflict inside a group): they take the extension lock for the merge as well, one after the other as the appends of a parallel build are
 */
static void
merge_when_idle(SiftKeyStore *ks, const SiftCcPolicy *pol)
{
	SiftDirSnapshot *snap;
	bool		in_group = MyProc->lockGroupLeader != NULL;

	if (!ConditionalLockPage(ks->rel, SIFT_KEYSTORE_MERGE_LOCK_BLKNO, ExclusiveLock))
		return;
	SIFT_TEST_GATE(sift_test_intern_gate, 12);
	if (in_group)
		lock_extend(ks);
	snap = view_refresh(ks);
	merge_runs(ks, pol, snap);
	pfree(snap);
	if (in_group)
		sift_extension_unlock(ks->rel);
	UnlockPage(ks->rel, SIFT_KEYSTORE_MERGE_LOCK_BLKNO, ExclusiveLock);
}

/* map[i] = the code of the key i of the batch for the keys the table of the view has (the others are left as they are) */
static void
map_codes(SiftKeyStore *ks, SiftCDict *batch, uint32 *map)
{
	KeyStoreView *v = ks->view;
	uint32		hash[KEYSTORE_PREFETCH];

	view_index(ks);
	for (uint32 i = 0; i < batch->n; i += KEYSTORE_PREFETCH)
	{
		uint32		n = Min(KEYSTORE_PREFETCH, batch->n - i);

		for (uint32 j = 0; j < n; j++)
		{
			uint32		len;
			const char *s = sift_cdict_str(batch, i + j, &len);

			hash[j] = sift_cdict_mem_hash(s, len);
			__builtin_prefetch(&v->slots[hash[j] & (v->nslots - 1)]);
		}
		for (uint32 j = 0; j < n; j++)
		{
			uint32		len;
			const char *s;
			int64		code;

			if (map[i + j] != PG_UINT32_MAX)
				continue;
			s = sift_cdict_str(batch, i + j, &len);
			code = view_find(v, s, len, hash[j]);
			map[i + j] = code < 0 ? PG_UINT32_MAX : (uint32) code;
		}
	}
}

void
sift_keystore_refresh(SiftKeyStore *ks)
{
	pfree(view_refresh(ks));
	view_index(ks);
}

int64
sift_keystore_find_code(SiftKeyStore *ks, const char *s, uint32 len)
{
	return view_find(ks->view, s, len, sift_cdict_mem_hash(s, len));
}

/*
 * map[i]: the code of the key i of the batch (batch->n entries); the keys the store has not got are appended as one run. Returns how many were. The caller holds the store in
 * RowExclusiveLock to the end of its transaction
 */
uint32
sift_keystore_intern(SiftKeyStore *ks, const SiftCDict *batch_in, const SiftCcPolicy *pol, uint32 *map)
{
	SiftCDict  *batch = (SiftCDict *) batch_in;
	uint32		missing = 0,
				appended = 0;

	memset(map, 0xff, batch->n * sizeof(uint32));
	pfree(view_refresh(ks));
	map_codes(ks, batch, map);
	for (uint32 i = 0; i < batch->n; i++)
		missing += map[i] == PG_UINT32_MAX;
	if (missing == 0)
		return 0;
	lock_extend(ks);
	pfree(view_refresh(ks));
	map_codes(ks, batch, map);
	{
		MemoryContext batch_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift cache scope key batch");
		SiftCDict  *fresh = sift_cdict_create(batch_cx);
		uint32		lo = ks->view->total;

		for (uint32 i = 0; i < batch->n; i++)
		{
			uint32		len;
			const char *s;

			if (map[i] != PG_UINT32_MAX)
				continue;
			s = sift_cdict_str(batch, i, &len);
			if (len > SIFT_COL_DICT_MAX_ENTRY)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("a scope key of %u bytes is longer than the %d bytes a scope key store holds", len, SIFT_COL_DICT_MAX_ENTRY),
								errhint("Use a shorter key, or a key column of an integer type.")));
			map[i] = lo + sift_cdict_intern(fresh, s, len);
		}
		if ((uint64) lo + fresh->n > (uint64) sift_limit(key_store_codes, SIFT_KEYSTORE_MAX_CODES))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("scope key store \"%s\" cannot hold more than " UINT64_FORMAT " keys", RelationGetRelationName(ks->rel),
								   (uint64) sift_limit(key_store_codes, SIFT_KEYSTORE_MAX_CODES)),
							errdetail("It has %u and the batch needs %u more.", lo, fresh->n)));
		write_run(ks, fresh, 0, lo, fresh->n, pol, NULL, 0);
		MemoryContextSetParent(batch_cx, ks->view->cx);
		kview_add(ks->view, lo, lo, fresh);
		appended = fresh->n;
	}
	sift_extension_unlock(ks->rel);
	merge_when_idle(ks, pol);
	return appended;
}
