/*
 * col/vstore/vstore_run.c - the values of a shared value store: runs in the directory of its relation, the append that gives a new value its code,
 * and the view a backend keeps of them (docs/COL-VSTORE.md)
 *
 * The code space of a store is append-only: a value has exactly one code, the position at which it was appended, and a code never moves. A run
 * is an immutable segment of the directory (kind SIFT_SEG_VALRUN) that holds the values of the codes [base, base + n) in code order, in the
 * serialized form of a string dictionary. A writer asks the store for the codes of a batch of values: those it finds are answered, the others are
 * appended as one run, under the extension lock of the store (the one heavyweight lock that the members of a parallel build do not share) that is held for the append and no longer, and published by a directory update that is logged after the pages
 * of the run, so that a segment that names the codes (published later, by the writer) is never seen without them. An append that is not followed
 * by a segment leaves values nobody uses: the store keeps them until its epoch is retired (col/vstore/vstore_gc.c).
 * Runs are merged by the appender (adjacent runs, the codes unchanged), so that a lookup reads few of them.
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
#include "utils/inval.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/codec.h"
#include "col/enc.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/vstore/vstore.h"
#include "core/page.h"
#include "core/sift.h"
#include "core/store.h"

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

/*
 * What a backend keeps of a store, up to the last code it has read: the runs as they were parsed (a piece covers the codes from lo, its strings from index
 * lo - dbase of the dictionary of the run), which is all that a reader of strings by code needs, and, for an appender whose batch is small beside the
 * store, a table of the hash of the strings (slots: hash << 32 | code + 1) that finds the code of a string
 */
typedef struct ViewPiece
{
	uint64		lo;				/* the first code of the piece */
	uint64		dbase;			/* the code of string 0 of d */
	SiftCDict  *d;
} ViewPiece;

typedef struct VStoreView
{
	Oid			store;			/* first: the hash key */
	MemoryContext cx;
	bool		loaded;
	uint64	   *slots;			/* the table of the codes below indexed (NULL until an appender asks), nslots of them */
	uint32		nslots;
	uint64		indexed;
	ViewPiece  *piece;
	int			npiece;
	int			cap;
	uint64		total;			/* the codes the pieces cover */
	uint64		epoch;
} VStoreView;

struct SiftVStore
{
	Relation	rel;
	VStoreView *view;
};

static HTAB *views;

static void
vs_corrupt(Relation rel, const char *what)
{
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("shared value store \"%s\": %s", RelationGetRelationName(rel), what)));
}

static VStoreView *
view_get(Oid store)
{
	bool		found;
	VStoreView *v;

	if (views == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(VStoreView), .hcxt = TopMemoryContext};

		views = hash_create("sift value store views", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	v = hash_search(views, &store, HASH_ENTER, &found);
	if (!found)
	{
		v->cx = AllocSetContextCreate(TopMemoryContext, "sift cache value store view", ALLOCSET_DEFAULT_SIZES);
		v->slots = NULL;
		v->nslots = 0;
		v->indexed = 0;
			v->loaded = false;
		v->piece = NULL;
		v->npiece = v->cap = 0;
		v->total = 0;
		v->epoch = 0;
	}
	return v;
}

static void
view_reset(VStoreView *v, uint64 epoch)
{
	MemoryContextReset(v->cx);
	v->slots = NULL;
	v->nslots = 0;
	v->indexed = 0;
	v->piece = NULL;
	v->npiece = v->cap = 0;
	v->total = 0;
	v->loaded = true;
	v->epoch = epoch;
}

static void
view_add(VStoreView *v, uint64 lo, uint64 dbase, SiftCDict *d)
{
	if (v->npiece == v->cap)
	{
		v->cap = Max(8, 2 * v->cap);
		v->piece = v->piece ? repalloc(v->piece, v->cap * sizeof(ViewPiece)) : MemoryContextAlloc(v->cx, v->cap * sizeof(ViewPiece));
	}
	v->piece[v->npiece++] = (ViewPiece) {lo, dbase, d};
	v->total = lo + d->n - (lo - dbase);
}

SiftVStore *
sift_vstore_open(Oid store, LOCKMODE lock)
{
	SiftVStore *vs = palloc(sizeof(SiftVStore));

	vs->rel = table_open(store, lock);
	if (!sift_vstore_is_store(store))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a shared value store", RelationGetRelationName(vs->rel))));
	vs->view = view_get(store);
	return vs;
}

void
sift_vstore_close(SiftVStore *vs)
{
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

	sift_sr_init(&sr, vs->rel, ext, next, e->gen);
	head = sift_sr_get(&sr, 0, e->bytes, scratch);
	if (head->magic != VRUN_MAGIC || head->base != e->sum_dl || head->n != e->ndocs || head->epoch != v->epoch)
		vs_corrupt(vs->rel, "a run does not match its directory entry");
	run = sift_cdict_parse(v->cx, (const char *) head + sizeof(VRunHead), e->bytes - sizeof(VRunHead), &used);
	if (run == NULL || run->n != head->n)
		vs_corrupt(vs->rel, "a run does not parse");
	view_add(v, v->total, head->base, run);
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	uint64		epoch = UINT64_MAX;
	int			n;

	sift_dir_read(vs->rel, snap);
	n = dir_runs(snap, runs, &epoch);
	if (!vs->view->loaded || vs->view->epoch != epoch)
		view_reset(vs->view, epoch);
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

#ifdef SIFT_TESTING
void
sift_vstore_forget_views(void)
{
	HASH_SEQ_STATUS seq;
	VStoreView *v;

	if (views == NULL)
		return;
	hash_seq_init(&seq, views);
	while ((v = hash_seq_search(&seq)) != NULL)
		v->loaded = false;
}
#endif

/* The current epoch of a store (the low 32 bits, the way a segment names it), from the metapage of its relation */
uint32
sift_vstore_current_epoch(uint32 store)
{
	Relation	rel = RelationIdGetRelation(store);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint32		epoch;

	sift_dir_read(rel, snap);
	epoch = (uint32) snap->meta.sum_dl;
	RelationClose(rel);
	pfree(snap);
	return epoch;
}

/* The codes of the newest epoch of the store and the epoch, read from its directory (no run is read) */
uint64
sift_vstore_dir_codes(Relation rel, uint64 *epoch)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	int			n;
	uint64		codes = 0;

	*epoch = UINT64_MAX;
	sift_dir_read(rel, snap);
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
	SiftCDict  *t = sift_cdict_create(CurrentMemoryContext);
	StringInfoData out;
	VRunHead	head = {VRUN_MAGIC, 1, vs->view->epoch, lo, hi - lo, 0};
	SiftWriteTarget *wt;
	SiftStreamWriter *sw;
	SiftExtent *ext;
	int			next;
	uint64		gen;
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);
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
	sift_page_init(page.data, SIFT_PT_SEGHEADER, gen);
	hdr->gen = gen;
	hdr->ndocs = hi - lo;
	hdr->nextents = next;
	memcpy(hdr->extents, ext, next * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + next * sizeof(SiftExtent));
	ed.neu = (SiftSegEntry) {.gen = gen, .kind = SIFT_SEG_VALRUN, .format = SIFT_COL_FORMAT_VSTORE, .reserved = (uint32) vs->view->epoch,
							 .ndocs = hi - lo, .sum_dl = lo, .bytes = out.len};
	ed.neu.header_blk = sift_wt_write_page(wt, page.data);
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
	chg.arg = &ed;
	chg.release = rel_ext;
	chg.nrelease = nrel;
	sift_dir_update(rel, &chg);
	pfree(out.data);
}

/* Under the extension lock nothing may take a heavyweight lock: the catalog is warmed (the directory update reads the format minor of the segments) and the xid assigned first */
static void
lock_extend(SiftVStore *vs)
{
	(void) sift_catalog();
	(void) GetCurrentTransactionId();
	LockRelationForExtension(vs->rel, ExclusiveLock);
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
	UnlockRelationForExtension(vs->rel, ExclusiveLock);
	pfree(view_refresh(vs));
}

/* Whether the store has runs of an epoch before the current one */
bool
sift_vstore_old_epochs(SiftVStore *vs)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		old = false;

	sift_dir_read(vs->rel, snap);
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	RetireEdit	ed = {0};
	SiftExtent *rel_ext;
	int			nrel = 0;
	SiftDirChange chg = {0};

	sift_dir_read(vs->rel, snap);
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
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
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
	SiftDirSnapshot *snap;

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
		snap = view_refresh(vs);
		if (vs->view->epoch != *epoch)	/* a new epoch began since: the codes found are the old one's */
		{
			UnlockRelationForExtension(vs->rel, ExclusiveLock);
			pfree(snap);
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
			if (fresh->n > 0)
			{
				write_run(vs, fresh, 0, lo, fresh->n, pol, NULL, 0);
				/* published: the view has the run as it was made (the directory update is logged after the pages of the run) */
				view_add(vs->view, lo, lo, fresh);
				appended = fresh->n;
				pfree(snap);
				snap = view_refresh(vs);
			}
		}
		merge_runs(vs, pol, snap);
		UnlockRelationForExtension(vs->rel, ExclusiveLock);
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	uint64		ep = epoch;

	if (!sift_vstore_is_store(store))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("a segment names relation %u as its shared value store, which is none", store)));
	LockRelationOid(store, AccessShareLock);
	rel = RelationIdGetRelation(store);
	sift_dir_read(rel, snap);
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
	Relation	rel = RelationIdGetRelation(r->store);
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
	Relation	rel = RelationIdGetRelation(r->store);
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
	Relation	rel = RelationIdGetRelation(r->store);

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
