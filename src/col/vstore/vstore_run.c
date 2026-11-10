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
#include "access/table.h"
#include "catalog/pg_class.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/codec.h"
#include "col/enc.h"
#include "col/vstore/vstore.h"
#include "core/page.h"
#include "core/store.h"

#define VRUN_MAGIC			0x4e555256	/* 'VRUN' */
#define VRUN_MERGE_ABOVE	12			/* runs of an epoch above which the appender merges the smallest adjacent ones */
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

/* What a backend keeps of a store: its values up to the last code it has read (the strings of the codes, interned in code order) */
typedef struct VStoreView
{
	Oid			store;			/* first: the hash key */
	MemoryContext cx;
	SiftCDict  *dict;
	uint64		epoch;
} VStoreView;

struct SiftVStore
{
	Relation	rel;
	VStoreView *view;
};

static HTAB *views;

static void
corrupt(Relation rel, const char *what)
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
		v->cx = AllocSetContextCreate(TopMemoryContext, "sift value store view", ALLOCSET_DEFAULT_SIZES);
		v->dict = NULL;
		v->epoch = 0;
	}
	return v;
}

static void
view_reset(VStoreView *v, uint64 epoch)
{
	MemoryContextReset(v->cx);
	v->dict = sift_cdict_create(v->cx);
	v->epoch = epoch;
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

/* The runs of the newest epoch in code order; their epoch */
static int
dir_runs(const SiftDirSnapshot *snap, SiftSegEntry *runs, uint64 *epoch)
{
	int			n = 0;

	*epoch = 0;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_VALRUN)
			*epoch = Max(*epoch, snap->segs[i].reserved);
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

/* Interns the strings of a run that the view does not have yet, in code order */
static void
load_run(SiftVStore *vs, const SiftSegEntry *e)
{
	VStoreView *v = vs->view;
	SiftExtent *ext = palloc(SIFT_MAX_EXTENTS * sizeof(SiftExtent));
	int			next = seg_extents(vs->rel, e, ext);
	SiftStreamReader sr;
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift value run", ALLOCSET_DEFAULT_SIZES),
				old = MemoryContextSwitchTo(tmp);
	char	   *scratch = MemoryContextAllocHuge(tmp, Max(e->bytes, 1));
	const VRunHead *head;
	SiftCDict  *run;
	Size		used;

	sift_sr_init(&sr, vs->rel, ext, next, e->gen);
	head = sift_sr_get(&sr, 0, e->bytes, scratch);
	if (head->magic != VRUN_MAGIC || head->base != e->sum_dl || head->n != e->ndocs || head->epoch != v->epoch)
		corrupt(vs->rel, "a run does not match its directory entry");
	run = sift_cdict_parse(tmp, (const char *) head + sizeof(VRunHead), e->bytes - sizeof(VRunHead), &used);
	if (run == NULL || run->n != head->n)
		corrupt(vs->rel, "a run does not parse");
	MemoryContextSwitchTo(v->cx);
	for (uint32 i = (uint32) (v->dict->n - head->base); i < run->n; i++)
	{
		uint32		len;
		const char *s = sift_cdict_str(run, i, &len);

		if (sift_cdict_intern(v->dict, s, len) != head->base + i)
			corrupt(vs->rel, "a run holds a value twice");
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	sift_sr_release(&sr);
	pfree(ext);
}

/* The view up to the newest run published (a read of the directory and of the runs it has not read) */
static SiftDirSnapshot *
view_refresh(SiftVStore *vs)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	uint64		epoch;
	int			n;

	sift_dir_read(vs->rel, snap);
	n = dir_runs(snap, runs, &epoch);
	if (vs->view->dict == NULL || vs->view->epoch != epoch)
		view_reset(vs->view, epoch);
	for (int i = 0; i < n; i++)
	{
		if (runs[i].sum_dl + runs[i].ndocs <= vs->view->dict->n)
			continue;
		if (runs[i].sum_dl > vs->view->dict->n)
			corrupt(vs->rel, "the runs leave a gap in the codes");
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
		v->dict = NULL;
}
#endif

SiftCDict *
sift_vstore_view(SiftVStore *vs)
{
	pfree(view_refresh(vs));
	return vs->view->dict;
}

const SiftCDict *
sift_vstore_view_to(SiftVStore *vs, uint64 ncodes, uint32 epoch)
{
	VStoreView *v = vs->view;

	if (v->dict == NULL || v->dict->n < ncodes || (uint32) v->epoch != epoch)
		pfree(view_refresh(vs));
	return v->dict->n >= ncodes && (uint32) v->epoch == epoch ? v->dict : NULL;
}

uint64
sift_vstore_epoch(SiftVStore *vs)
{
	(void) sift_vstore_view(vs);
	return vs->view->epoch;
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

/* Writes the strings [from, from + n) of src as one run of the epoch that starts at code lo, and publishes it, replacing the entries in old (nold may be 0) */
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
		const char *s = sift_cdict_str(src, from + c, &len);

		(void) sift_cdict_intern(t, s, len);
	}
	initStringInfo(&out);
	appendBinaryStringInfo(&out, (const char *) &head, sizeof(head));
	sift_cdict_serialize(t, pol, 0, &out);
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

/* The smallest window of adjacent runs, merged into one (the codes do not change) */
static void
merge_runs(SiftVStore *vs, const SiftCcPolicy *pol, const SiftDirSnapshot *snap)
{
	SiftSegEntry *runs = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	uint64		epoch;
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
	write_run(vs, vs->view->dict, runs[best].sum_dl, runs[best].sum_dl,
			  runs[best + VRUN_MERGE_WIDTH - 1].sum_dl + runs[best + VRUN_MERGE_WIDTH - 1].ndocs - runs[best].sum_dl, pol, &runs[best], VRUN_MERGE_WIDTH);
	pfree(runs);
}

/* map[i]: the code of the value i of the batch (batch->n entries); the values the store has not got are appended as one run. Returns how many were */
uint32
sift_vstore_intern(SiftVStore *vs, const SiftCDict *batch, const SiftCcPolicy *pol, uint32 *map)
{
	SiftCDict  *view;
	uint32		missing = 0,
				appended = 0;
	SiftDirSnapshot *snap;

	view = sift_vstore_view(vs);
	for (uint32 i = 0; i < batch->n; i++)
	{
		uint32		len;
		const char *s = sift_cdict_str(batch, i, &len);
		int64		code = sift_cdict_find(view, s, len);

		map[i] = code < 0 ? PG_UINT32_MAX : (uint32) code;
		missing += code < 0;
	}
	if (missing == 0)
		return 0;
	LockRelationForExtension(vs->rel, ExclusiveLock);
	snap = view_refresh(vs);
	view = vs->view->dict;
	{
		SiftCDict  *fresh = sift_cdict_create(CurrentMemoryContext);
		uint32		lo = view->n;

		for (uint32 i = 0; i < batch->n; i++)
		{
			uint32		len;
			const char *s;

			if (map[i] != PG_UINT32_MAX)
				continue;
			s = sift_cdict_str(batch, i, &len);
			if (sift_cdict_find(view, s, len) < 0)
				(void) sift_cdict_intern(fresh, s, len);
		}
		if (fresh->n > 0)
		{
			write_run(vs, fresh, 0, lo, fresh->n, pol, NULL, 0);
			/* published: the view takes the values (the directory update is logged after the pages of the run) */
			for (uint32 c = 0; c < fresh->n; c++)
			{
				uint32		len;
				const char *s = sift_cdict_str(fresh, c, &len);

				if (sift_cdict_intern(view, s, len) != lo + c)
					corrupt(vs->rel, "a value of the batch is new and already there");
			}
			appended = fresh->n;
			pfree(snap);
			snap = view_refresh(vs);
		}
	}
	merge_runs(vs, pol, snap);
	UnlockRelationForExtension(vs->rel, ExclusiveLock);
	for (uint32 i = 0; i < batch->n; i++)
		if (map[i] == PG_UINT32_MAX)
		{
			uint32		len;
			const char *s = sift_cdict_str(batch, i, &len);

			map[i] = (uint32) sift_cdict_find(vs->view->dict, s, len);
		}
	return appended;
}
