/*
 * col/vstore/vstore_run.c - the values of a shared value store: runs in the directory of its relation, the append that gives a new value its code,
 * and the view a backend keeps of them (docs/COL-VSTORE.md)
 *
 * The code space of a store is append-only: a value has exactly one code, the position at which it was appended, and a code never moves. A run
 * is an immutable segment of the directory (kind SIFT_SEG_VALRUN) that holds the values of the codes [base, base + n) in code order, in the
 * serialized form of a string dictionary. A writer asks the store for the codes of a batch of values: those it finds are answered, the others are
 * appended as one run, under the extension lock of the store (the one heavyweight lock that the members of a parallel build do not share: core/extlock.h) that is held for the append and no longer, and published by a directory update that is logged after the pages
 * of the run, so that a segment that names the codes (published later, by the writer) is never seen without them. An append that is not followed
 * by a segment leaves values nobody uses: the store keeps them until its epoch is retired (col/vstore/vstore_gc.c).
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

#include "col/codec.h"
#include "col/enc.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "core/cachebudget.h"
#include "col/vstore/vstore.h"
#include "col/vstore/vstore_int.h"
#include "core/extlock.h"
#include "core/limits.h"
#include "core/page.h"
#include "core/sift.h"
#include "core/store.h"
#include "core/visibility.h"
#include "core/damage.h"
#include "core/catname.h"

#define VRUN_MAGIC			0x4e555256	/* 'VRUN' */
#define VRUN_MERGE_ABOVE	12			/* runs of an epoch above which the appender merges the smallest adjacent ones */
#define VRUN_MERGE_WIDTH	4
#define VRUN_RETIRED		0x0100		/* flag of a run of an epoch no segment names: removed when the transaction id in sb_lo/sb_hi is below the horizon */

typedef struct VRunHead
{
	uint32		magic;
	uint32		version;
	uint64		epoch;
	uint64		base;
	uint32		n;
	uint32		pad;
} VRunHead;

struct SiftVStore
{
	Relation	rel;
	VStoreView *view;
	ViewPin    *pin;
};


static void
vs_corrupt(Relation rel, const char *what)
{
	SIFT_DAMAGE_OTHER_DATA(errmsg("shared value store \"%s\": %s", RelationGetRelationName(rel), what));
}

SiftVStore *
sift_vstore_open(Oid store, LOCKMODE lock)
{
	SiftVStore *vs = palloc(sizeof(SiftVStore));

	vs->rel = table_open(sift_rel_must_exist(store), lock);
	if (!sift_vstore_is_store(store))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a shared value store", RelationGetRelationName(vs->rel))));
	vs->view = vview_open(store, &vs->pin);
	return vs;
}

void
sift_vstore_close(SiftVStore *vs)
{
	vview_release(vs->pin);
	table_close(vs->rel, NoLock);
	pfree(vs);
}

static int
cmp_base(const void *a, const void *b)
{
	uint64		x = ((const SiftSegEntry *) a)->sum_dl,
				y = ((const SiftSegEntry *) b)->sum_dl;

	return x < y ? -1 : x > y;
}

/* The runs of an epoch in code order (UINT64_MAX in *epoch: the current epoch of the store, which is returned in it) */
static int
dir_runs(const SiftDirSnapshot *snap, SiftSegEntry *runs, uint64 *epoch)
{
	int			n = 0;

	if (*epoch == UINT64_MAX)
		*epoch = snap->meta.sum_dl;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_VALRUN && snap->segs[i].reserved == *epoch)
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
load_run(SiftVStore *vs, const SiftSegEntry *e)
{
	VStoreView *v = vs->view;
	SiftExtent *ext = palloc(SIFT_MAX_EXTENTS * sizeof(SiftExtent));
	int			next = seg_extents(vs->rel, e, ext);
	SiftStreamReader sr;
	char	   *scratch = MemoryContextAllocHuge(CurrentMemoryContext, Max(e->bytes, 1));
	const VRunHead *head;
	SiftCDict  *run;
	Size		used;

	sift_cache_room(e->bytes);
	sift_sr_init(&sr, vs->rel, ext, next, e->gen);
	head = sift_sr_get(&sr, 0, e->bytes, scratch);
	if (head->magic != VRUN_MAGIC || head->base != e->sum_dl || head->n != e->ndocs || head->epoch != v->epoch)
		vs_corrupt(vs->rel, "a run does not match its directory entry");
	run = sift_cdict_parse(v->cx, (const char *) head + sizeof(VRunHead), e->bytes - sizeof(VRunHead), &used);
	if (run == NULL || run->n != head->n)
		vs_corrupt(vs->rel, "a run does not parse");
	vview_add(v, v->total, head->base, run);
	sift_sr_release(&sr);
	pfree(scratch);
	pfree(ext);
}

#define VSTORE_PREFETCH	32	/* hashes computed, and their slots prefetched, before they are probed */

static const char *
view_string(const VStoreView *v, uint64 code, uint32 *len)
{
	int			lo = 0,
				hi = v->npiece - 1;

	Assert(code < v->total);
	while (lo < hi)
	{
		int			mid = (lo + hi + 1) / 2;

		if (v->piece[mid].lo <= code)
			lo = mid;
		else
			hi = mid - 1;
	}
	return sift_cdict_str(v->piece[lo].d, (uint32) (code - v->piece[lo].dbase), len);
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
view_index(SiftVStore *vs)
{
	VStoreView *v = vs->view;

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
	for (uint64 c = v->indexed; c < v->total; c += VSTORE_PREFETCH)
	{
		uint32		n = Min(VSTORE_PREFETCH, v->total - c);
		uint64		slot[VSTORE_PREFETCH];

		for (uint32 j = 0; j < n; j++)
		{
			uint32		len;
			const char *s = view_string(v, c + j, &len);

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
view_find(const VStoreView *v, const char *s, uint32 len, uint32 hash)
{
	for (uint32 i = hash & (v->nslots - 1);; i = (i + 1) & (v->nslots - 1))
	{
		uint64		u = v->slots[i];
		uint32		l = 0;

		if (u == 0)
			return -1;
		if ((uint32) (u >> 32) == hash && (view_string(v, (uint32) u - 1, &l), l == len) &&
			memcmp(view_string(v, (uint32) u - 1, &l), s, len) == 0)
			return (uint32) u - 1;
	}
}

/* The view up to the newest run published (a read of the directory and of the runs it has not read), the strings interned when index */
static SiftDirSnapshot *
view_refresh(SiftVStore *vs)
{
	SiftDirSnapshot *snap;
	SiftSegEntry *runs;
	uint64		epoch = UINT64_MAX;
	int			n;

	snap = sift_dir_read(vs->rel);
	runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	n = dir_runs(snap, runs, &epoch);
	if (!vs->view->loaded || vs->view->epoch != epoch)
		vview_reset(vs->view, epoch);
	for (int i = 0; i < n; i++)
	{
		if (runs[i].sum_dl + runs[i].ndocs <= vs->view->total)
			continue;
		if (runs[i].sum_dl > vs->view->total)
			vs_corrupt(vs->rel, "the runs leave a gap in the codes");
		load_run(vs, &runs[i]);
	}
	pfree(runs);
	return snap;
}


/* The current epoch of a store (the low 32 bits, the way a segment names it), from the metapage of its relation */
uint32
sift_vstore_current_epoch(uint32 store)
{
	Relation	rel = sift_relation_get(store);
	SiftDirSnapshot *snap;
	uint32		epoch;

	snap = sift_dir_read(rel);
	epoch = (uint32) snap->meta.sum_dl;
	RelationClose(rel);
	pfree(snap);
	return epoch;
}

/* The codes of the newest epoch of the store and the epoch, read from its directory (no run is read) */
uint64
sift_vstore_dir_codes(Relation rel, uint64 *epoch)
{
	SiftDirSnapshot *snap;
	SiftSegEntry *runs;
	int			n;
	uint64		codes = 0;

	*epoch = UINT64_MAX;
	snap = sift_dir_read(rel);
	runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	n = dir_runs(snap, runs, epoch);
	for (int i = 0; i < n; i++)
		codes = Max(codes, runs[i].sum_dl + runs[i].ndocs);
	pfree(runs);
	pfree(snap);
	return codes;
}

const char *
sift_vstore_string(SiftVStore *vs, uint32 code, uint32 *len)
{
	if (code >= vs->view->total)
		pfree(view_refresh(vs));
	if (code >= vs->view->total)
		vs_corrupt(vs->rel, "a code is past the codes of the store");
	return view_string(vs->view, code, len);
}

uint64
sift_vstore_epoch(SiftVStore *vs)
{
	pfree(view_refresh(vs));
	return vs->view->epoch;
}

uint64
sift_vstore_ncodes(SiftVStore *vs)
{
	pfree(view_refresh(vs));
	return vs->view->total;
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
 * Writes the strings [from, from + n) of src (of the view's pieces by code when src is NULL) as one run of the epoch that starts at code lo, and publishes it,
 * replacing the entries in old (nold may be 0)
 */
static void
write_run(SiftVStore *vs, const SiftCDict *src, uint32 from, uint32 lo, uint32 n, const SiftCcPolicy *pol, const SiftSegEntry *old, int nold)
{
	Relation	rel = vs->rel;
	uint32		hi = lo + n;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift value store run", ALLOCSET_DEFAULT_SIZES);
	SiftCDict  *t = sift_cdict_create(cx);
	StringInfoData out;
	VRunHead	head = {VRUN_MAGIC, 1, vs->view->epoch, lo, hi - lo, 0};
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
		const char *s = src ? sift_cdict_str(src, from + c, &len) : sift_vstore_string(vs, from + c, &len);

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
	ed.neu = (SiftSegEntry) {.gen = gen, .kind = SIFT_SEG_VALRUN, .format = SIFT_COL_FORMAT_VSTORE, .reserved = (uint32) vs->view->epoch,
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
lock_extend(SiftVStore *vs)
{
	(void) GetCurrentTransactionId();
	sift_extension_lock(vs->rel);
}

static void
epoch_edit(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->sum_dl++;
}

/*
 * A new epoch of the store: the codes begin again at 0 and the runs of the epochs before are kept for the segments that name them (until no segment does,
 * sift_vstore_retire). Nothing is written but the metapage; the strings a segment needs are appended to the new epoch when the segment is written
 */
void
sift_vstore_bump_epoch(SiftVStore *vs)
{
	SiftDirChange chg = {.fn = epoch_edit};

	lock_extend(vs);
	pfree(view_refresh(vs));
	if (vs->view->total > 0)
		sift_dir_update(vs->rel, &chg);
	sift_extension_unlock(vs->rel);
	pfree(view_refresh(vs));
}

/* Whether the store has runs of an epoch before the current one */
bool
sift_vstore_old_epochs(SiftVStore *vs)
{
	SiftDirSnapshot *snap;
	bool		old = false;

	snap = sift_dir_read(vs->rel);
	for (int i = 0; i < snap->nsegs; i++)
		old |= snap->segs[i].kind == SIFT_SEG_VALRUN && snap->segs[i].reserved < snap->meta.sum_dl;
	pfree(snap);
	return old;
}

typedef struct RetireEdit
{
	uint64	   *mark;			/* generations of the runs that are marked retired */
	int			nmark;
	uint64	   *gone;			/* generations of the runs that are removed */
	int			ngone;
	FullTransactionId xid;
} RetireEdit;

static bool
gen_in(const uint64 *gens, int n, uint64 gen)
{
	for (int i = 0; i < n; i++)
		if (gens[i] == gen)
			return true;
	return false;
}

static void
retire_edit(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	RetireEdit *ed = arg;
	int			k = 0;

	for (int i = 0; i < dir->nsegs; i++)
	{
		if (gen_in(ed->gone, ed->ngone, dir->segs[i].gen))
			continue;
		if (gen_in(ed->mark, ed->nmark, dir->segs[i].gen))
		{
			dir->segs[i].flags |= VRUN_RETIRED;
			dir->segs[i].sb_lo = (uint32) U64FromFullTransactionId(ed->xid);
			dir->segs[i].sb_hi = (uint32) (U64FromFullTransactionId(ed->xid) >> 32);
		}
		dir->segs[k++] = dir->segs[i];
	}
	dir->nsegs = k;
}

/*
 * The epochs before the current one that no segment names (in_use holds the others, by their low 32 bits): their runs are marked retired with the transaction
 * id after which no reader can hold a directory that has a segment naming them, and removed (their extents released) once that id is below the horizon.
 * The caller holds the store in ShareRowExclusiveLock (no writer is between its codes and its segment) and the members against maintenance. Returns the
 * runs removed
 */
int
sift_vstore_retire(SiftVStore *vs, const Bitmapset *in_use)
{
	SiftDirSnapshot *snap;
	RetireEdit	ed = {0};
	SiftExtent *rel_ext;
	int			nrel = 0;
	SiftDirChange chg = {0};

	snap = sift_dir_read(vs->rel);
	ed.mark = palloc(snap->nsegs * sizeof(uint64) + 1);
	ed.gone = palloc(snap->nsegs * sizeof(uint64) + 1);
	ed.xid = ReadNextFullTransactionId();
	rel_ext = palloc((Size) snap->nsegs * (SIFT_MAX_EXTENTS + 1) * sizeof(SiftExtent));
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind != SIFT_SEG_VALRUN || e->reserved >= snap->meta.sum_dl)
			continue;
		if (e->flags & VRUN_RETIRED)
		{
			FullTransactionId at = FullTransactionIdFromU64((uint64) e->sb_hi << 32 | e->sb_lo);

			if (sift_store_removable(vs->rel, at))
			{
				ed.gone[ed.ngone++] = e->gen;
				nrel += seg_extents(vs->rel, e, rel_ext + nrel);
				sift_extent_push(rel_ext, &nrel, e->header_blk, 1);
			}
		}
		else if (!bms_is_member((int) e->reserved, in_use))
			ed.mark[ed.nmark++] = e->gen;
	}
	if (ed.nmark + ed.ngone > 0)
	{
		chg.fn = retire_edit;
		chg.arg = &ed;
		chg.release = rel_ext;
		chg.nrelease = nrel;
		sift_dir_update(vs->rel, &chg);
	}
	pfree(snap);
	return ed.ngone;
}

/* The smallest window of adjacent runs, merged into one (the codes do not change) */
static void
merge_runs(SiftVStore *vs, const SiftCcPolicy *pol, const SiftDirSnapshot *snap)
{
	SiftSegEntry *runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	uint64		epoch = UINT64_MAX;
	int			n = dir_runs(snap, runs, &epoch),
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
	write_run(vs, NULL, runs[best].sum_dl, runs[best].sum_dl,
			  runs[best + VRUN_MERGE_WIDTH - 1].sum_dl + runs[best + VRUN_MERGE_WIDTH - 1].ndocs - runs[best].sum_dl, pol, &runs[best], VRUN_MERGE_WIDTH);
	pfree(runs);
}

/*
 * The merge of the runs after an append, by the one session that gets the merge lock: another that interns meanwhile appends (the append is under the extension lock, which the merge
 * does not hold) and leaves the merge to the one that holds it. The runs a skipped merge leaves are merged by the next append. The members of a lock group share the merge lock (page
 * locks do not conflict inside a group): they take the extension lock for the merge as well, one after the other as the appends of a parallel build are
 */
static void
merge_when_idle(SiftVStore *vs, const SiftCcPolicy *pol)
{
	SiftDirSnapshot *snap;
	bool		in_group = MyProc->lockGroupLeader != NULL;

	if (!ConditionalLockPage(vs->rel, SIFT_VSTORE_MERGE_LOCK_BLKNO, ExclusiveLock))
		return;
	SIFT_TEST_GATE(sift_test_intern_gate, 12);
	if (in_group)
		lock_extend(vs);
	snap = view_refresh(vs);
	merge_runs(vs, pol, snap);
	pfree(snap);
	if (in_group)
		sift_extension_unlock(vs->rel);
	UnlockPage(vs->rel, SIFT_VSTORE_MERGE_LOCK_BLKNO, ExclusiveLock);
}

/* map[i] = the code of the value i of the batch for the values the table of the view has (the others are left as they are) */
static void
map_codes(SiftVStore *vs, SiftCDict *batch, uint32 *map)
{
	VStoreView *v = vs->view;
	uint32		hash[VSTORE_PREFETCH];

	view_index(vs);
	for (uint32 i = 0; i < batch->n; i += VSTORE_PREFETCH)
	{
		uint32		n = Min(VSTORE_PREFETCH, batch->n - i);

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
sift_vstore_refresh(SiftVStore *vs)
{
	pfree(view_refresh(vs));
	view_index(vs);
}

int64
sift_vstore_find_code(SiftVStore *vs, const char *s, uint32 len)
{
	return view_find(vs->view, s, len, sift_cdict_mem_hash(s, len));
}

/*
 * map[i]: the code of the value i of the batch (batch->n entries) in the current epoch of the store, which is returned in *epoch with the codes it holds in *ncodes;
 * the values the store has not got are appended as one run. Returns how many were. The caller holds the store in RowExclusiveLock to the end of its transaction,
 * which keeps the epoch from being retired under the segment it writes
 */
uint32
sift_vstore_intern(SiftVStore *vs, const SiftCDict *batch_in, const SiftCcPolicy *pol, uint32 *map, uint64 *epoch, uint64 *ncodes)
{
	SiftCDict  *batch = (SiftCDict *) batch_in;
	uint32		missing,
				appended = 0;

retry:
	missing = 0;
	memset(map, 0xff, batch->n * sizeof(uint32));
	pfree(view_refresh(vs));
	*epoch = vs->view->epoch;
	map_codes(vs, batch, map);
	for (uint32 i = 0; i < batch->n; i++)
		missing += map[i] == PG_UINT32_MAX;
	if (missing > 0)
	{
		lock_extend(vs);
		pfree(view_refresh(vs));
		if (vs->view->epoch != *epoch)	/* a new epoch began since: the codes found are the old one's */
		{
			sift_extension_unlock(vs->rel);
			goto retry;
		}
		map_codes(vs, batch, map);
		{
			SiftCDict  *fresh = sift_cdict_create(vs->view->cx);
			uint32		lo = vs->view->total;

			for (uint32 i = 0; i < batch->n; i++)
			{
				uint32		len;
				const char *s;

				if (map[i] != PG_UINT32_MAX)
					continue;
				s = sift_cdict_str(batch, i, &len);
				map[i] = lo + sift_cdict_intern(fresh, s, len);
			}
			if ((uint64) lo + fresh->n > (uint64) sift_limit(value_store_codes, SIFT_VSTORE_MAX_CODES))
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("shared value store \"%s\" cannot hold more than " UINT64_FORMAT " codes in one epoch", RelationGetRelationName(vs->rel),
									   (uint64) sift_limit(value_store_codes, SIFT_VSTORE_MAX_CODES)),
								errdetail("It has %u and the batch needs %u more.", lo, fresh->n),
								errhint("sift.col_value_store_compact() begins a new epoch when codes are named by no segment.")));
			if (fresh->n > 0)
			{
				write_run(vs, fresh, 0, lo, fresh->n, pol, NULL, 0);
				/* published: the view has the run as it was made (the directory update is logged after the pages of the run) */
				vview_add(vs->view, lo, lo, fresh);
				appended = fresh->n;
			}
		}
		sift_extension_unlock(vs->rel);
		merge_when_idle(vs, pol);
	}
	*ncodes = vs->view->total;
	return appended;
}

/*
 * The strings of a store by code, read in place: the run that holds the code is a stream, and its dictionary is in the blocks form, so a string costs the read of
 * the table of its block and the block (the way a segment's dictionary is searched, col/seg/dict.c, over a pseudo segment that has only the stream)
 */
typedef struct RunRead
{
	SiftSegEntry e;
	SiftExtent *ext;
	int			next;
	SiftColSegment *seg;
	DictLook   *look;
} RunRead;

typedef struct StoreRead
{
	Oid			store;
	MemoryContext cx;
	int			nruns;
	RunRead    *run;
} StoreRead;

void *
sift_vstore_read_open(Relation index, uint32 store, uint32 epoch, MemoryContext mcxt)
{
	StoreRead  *r = MemoryContextAllocZero(mcxt, sizeof(StoreRead));
	Relation	rel;
	SiftDirSnapshot *snap;
	SiftSegEntry *runs;
	uint64		ep = epoch;

	if (!sift_vstore_is_store(store))
		SIFT_DAMAGE_DATA(NULL, errmsg("a segment names relation %s as its shared value store, which is none", sift_rel_label(store)));
	LockRelationOid(store, AccessShareLock);
	rel = sift_relation_get(store);
	snap = sift_dir_read(rel);
	runs = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	/* the epoch is the segment's: the store keeps those it names until no segment does (vstore_gc) */
	r->nruns = dir_runs(snap, runs, &ep);
	if (r->nruns == 0 && (uint32) snap->meta.sum_dl != epoch)
		vs_corrupt(rel, "a segment names an epoch of the store that is gone");
	r->store = store;
	r->cx = mcxt;
	r->run = MemoryContextAllocZero(mcxt, Max(r->nruns, 1) * sizeof(RunRead));
	for (int i = 0; i < r->nruns; i++)
		r->run[i].e = runs[i];
	RelationClose(rel);
	pfree(runs);
	pfree(snap);
	return r;
}

/* The run holding code, its stream open on rel (the caller releases it) */
static RunRead *
run_open(StoreRead *r, Relation rel, RunRead *run)
{
	MemoryContext old = MemoryContextSwitchTo(r->cx);

	if (run->ext == NULL)
	{
		run->ext = palloc(SIFT_MAX_EXTENTS * sizeof(SiftExtent));
		run->next = seg_extents(rel, &run->e, run->ext);
		run->seg = palloc0(sizeof(SiftColSegment));
		run->seg->entry = run->e;
		run->seg->mcxt = r->cx;
		run->seg->priv = palloc0(sizeof(SegPriv));
	}
	run->seg->index = rel;
	sift_sr_init(&run->seg->sr, rel, run->ext, run->next, run->e.gen);
	if (run->look == NULL)
	{
		run->look = sift_cseg_look_at(run->seg, 0, sizeof(VRunHead), run->e.bytes - sizeof(VRunHead), false);
		if (run->look == NULL || run->look->n != run->e.ndocs)
			vs_corrupt(rel, "a run does not parse in place");
	}
	MemoryContextSwitchTo(old);
	return run;
}

const char *
sift_vstore_read_str(void *handle, uint32 code, uint32 *len, char *buf)
{
	StoreRead  *r = handle;
	RunRead    *run = NULL;
	Relation	rel = sift_relation_get(r->store);
	const char *str;

	for (int i = 0; i < r->nruns; i++)
		if (code >= r->run[i].e.sum_dl && code - r->run[i].e.sum_dl < r->run[i].e.ndocs)
			run = &r->run[i];
	if (run == NULL)
		vs_corrupt(rel, "a segment names a code that no run holds");
	run_open(r, rel, run);
	str = sift_cseg_look_str(run->look, code - run->e.sum_dl, len, buf);
	sift_sr_release(&run->seg->sr);
	RelationClose(rel);
	return str;
}

/*
 * The strings of codes[0..n) appended to all, their offsets in off[0..n]: one open of each run for the whole pass. The codes of a segment climb (its index
 * meets the rows in the order of the store's first member), so each block of a run is decompressed once
 */
void
sift_vstore_read_strs(void *handle, const uint32 *codes, uint32 n, StringInfo all, uint32 *off)
{
	StoreRead  *r = handle;
	Relation	rel = sift_relation_get(r->store);
	RunRead    *run = NULL;

	for (uint32 i = 0; i < n; i++)
	{
		uint32		cnt;

		if (run == NULL || codes[i] < run->e.sum_dl || codes[i] - run->e.sum_dl >= run->e.ndocs)
		{
			if (run != NULL)
				sift_sr_release(&run->seg->sr);
			run = NULL;
			for (int j = 0; run == NULL && j < r->nruns; j++)
				if (codes[i] >= r->run[j].e.sum_dl && codes[i] - r->run[j].e.sum_dl < r->run[j].e.ndocs)
					run = run_open(r, rel, &r->run[j]);
			if (run == NULL)
				vs_corrupt(rel, "a segment names a code that no run holds");
		}
		cnt = 1;
		while (i + cnt < n && codes[i + cnt] == codes[i] + cnt && codes[i + cnt] - run->e.sum_dl < run->e.ndocs &&
			   (codes[i] - run->e.sum_dl + cnt) % DICT_BLOCK != 0)
			cnt++;
		seg_look_strs(run->look, codes[i] - run->e.sum_dl, cnt, all, off + i);
		i += cnt - 1;
	}
	off[n] = (uint32) all->len;
	if (run != NULL)
		sift_sr_release(&run->seg->sr);
	RelationClose(rel);
}

uint64
sift_vstore_read_ncodes(void *handle)
{
	StoreRead  *r = handle;

	return r->nruns == 0 ? 0 : r->run[r->nruns - 1].e.sum_dl + r->run[r->nruns - 1].e.ndocs;
}

void
sift_vstore_read_match(void *handle, SiftColOp op, const SiftColValue *c, int nvals, uint64 *bits)
{
	StoreRead  *r = handle;
	Relation	rel = sift_relation_get(r->store);

	for (int i = 0; i < r->nruns; i++)
	{
		RunRead    *run = run_open(r, rel, &r->run[i]);
		/* check-memory: allow one bit a string of one run: an eighth of a byte a string */
		uint64	   *lut = palloc0((run->e.ndocs / 64 + 1) * sizeof(uint64));

		run->look->look.match(&run->look->look, op, c, nvals, lut);
		for (uint32 w = 0; w <= run->e.ndocs / 64; w++)
			for (uint64 m = lut[w]; m != 0; m &= m - 1)
			{
				uint64		code = run->e.sum_dl + (uint64) w * 64 + pg_rightmost_one_pos64(m);

				bits[code >> 6] |= UINT64CONST(1) << (code & 63);
			}
		sift_sr_release(&run->seg->sr);
		pfree(lut);
	}
	RelationClose(rel);
}
