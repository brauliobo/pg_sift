/*
 * bm25/wbuf.c - write buffers: aminsert stages the doc records of a statement and appends them to one of SIFT_LANES lane chains of the active
 * segment, a page of docs in one generic WAL record. A full active segment is sealed in one directory
 * record and promoted to an immutable one by maintenance.
 *
 * Lock order: lane tail page, newly allocated page, header page (only when it
 * is free at once: core/lane.h). Sealing takes lane tails one at a time. A doc that does not
 * fit the tail is split into pieces written while the tail is held (lock
 * coupling to the new page), so one doc is always adjacent in one lane.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "tcop/utility.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "bm25/am.h"
#include "bm25/doc.h"
#include "core/lane.h"
#include "core/quarantine/quarantine.h"
#include "core/maint/maint.h"
#include "bm25/wbuf.h"
#include "bm25/wbuf_int.h"

bool		sift_test_wbuf_narrow = false;

#define corrupt SIFT_CORRUPT

#define WBUF_SPLIT_ROOM	1024	/* a doc is split over pages rather than moved whole when the page has this much room left */
#define WBUF_VACUUM_SEAL_PAGES 8	/* pages from which VACUUM seals an open buffer */
/* STOR[STOR-BM25-06]: wbuf 1024B wide bloom (13.1%/pg) + 4B/term hash (33%/rec); ABI-locked, shrink via early seal */
bool
sift_wbuf_bloom_has(const uint64 *bloom, int nwords, uint32 h)
{
	uint32		a,
				b;

	bloom_probes(nwords, h, &a, &b);
	return ((bloom[a >> 6] >> (a & 63)) & (bloom[b >> 6] >> (b & 63)) & 1) != 0;
}

/* the format a buffer opened now has */
static uint16
new_format(void)
{
	return sift_test_wbuf_narrow ? 0 : SIFT_WBUF_FORMAT_WIDE;
}

static uint32
limit_pages(Relation index)
{
	return Max(2 * SIFT_LANES, (uint64) sift_index_options(index)->write_buffer_limit * 1024 / BLCKSZ);
}

uint64
sift_wbuf_seal(Relation index, bool force)
{
	return sift_lane_seal(index, force, hdr_size(new_format()), new_format(), 0, limit_pages(index), sift_wbuf_has_docs);
}

/*
 * VACUUM seals an open buffer of WBUF_VACUUM_SEAL_PAGES pages: a search reads every document of the open buffer (and the heap pages of those not yet
 * all-visible), and the buffer is several times larger than the segment it becomes, so its rows are promoted and merged instead of waiting for a seal
 * that may never come.
 */
uint64
sift_wbuf_seal_vacuum(Relation index)
{
	return sift_lane_seal(index, false, hdr_size(new_format()), new_format(), 0, WBUF_VACUUM_SEAL_PAGES, sift_wbuf_has_docs);
}

/* Cached per relation: the active segment and the lane tails last seen */
typedef struct WbufHint
{
	uint64		gen;			/* 0: unknown */
	uint16		format;			/* of that buffer */
	BlockNumber hdr;
	BlockNumber tail[SIFT_LANES];
} WbufHint;

static WbufHint *
get_hint(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(WbufHint));
	return index->rd_amcache;
}

static void
refresh_hint(Relation index, WbufHint *h, uint32 analyzer_hash)
{
	sift_lane_refresh(index, analyzer_hash, "analyzer options", hdr_size(new_format()), new_format(), 0, sift_maint_make_room,
					  &h->gen, &h->hdr, &h->format, h->tail);
}

/* Largest n whose piece of terms[0..n) fits room; -1 when not even an empty piece does */
static int
fit(Size room, const SiftTermFreq *t, int nterms, int nfields, int *termbytes)
{
	int			n = 0,
				tb = 0;

	*termbytes = 0;
	if (RECSIZE(0, 0, nfields) > room)
		return -1;
	while (n < nterms && RECSIZE(n + 1, tb + t[n].len, nfields) <= room)
		tb += t[n++].len;
	*termbytes = tb;
	return n;
}

static void
put_piece(Page page, ItemPointer tid, uint32 dl, const uint16 *fdl, int nfields, bool cont, const SiftTermFreq *t, int n, int termbytes)
{
	SiftWbufPageHeaderData *ph = WBUFPAGE(page);
	int			nf = cont ? 0 : nfields;
	Size		used = SiftPageGetUsed(page),
				size = RECSIZE(n, termbytes, nf);
	SiftWbufDocData *d = (SiftWbufDocData *) (SiftPageGetData(page) + used);
	uint32	   *hash = (uint32 *) ((char *) (d + 1) + FDL_BYTES(nf));
	uint8	   *tf = (uint8 *) (hash + n),
			   *len = tf + n;
	char	   *tb = (char *) (len + n);

	memset(d, 0, size);
	d->tid = *tid;
	d->flags = cont ? SIFT_WBUF_CONT : nf ? SIFT_WBUF_FIELDDL : 0;
	d->reserved = nf;
	if (nf)
		memcpy(d + 1, fdl, nf * sizeof(uint16));
	d->nterms = n;
	d->dl = cont ? 0 : dl;
	d->termbytes = termbytes;
	for (int i = 0; i < n; i++)
	{
		hash[i] = t[i].hash;
		tf[i] = t[i].tf;
		len[i] = t[i].len;
		memcpy(tb, t[i].term, t[i].len);
		tb += t[i].len;
		bloom_add(ph->bloom, 4, t[i].hash);
	}
	if (!cont)
	{
		ph->ndocs++;
		ph->sum_dl += dl;
	}
	SiftPageSetUsed(page, used + size);
}

typedef struct PieceArg
{
	ItemPointer tid;
	uint32		dl;
	const uint16 *fdl;
	int			nfields;
	bool		cont;
	const SiftTermFreq *t;
	int			n;
	int			termbytes;
	bool		spans;			/* the doc goes on in the next page */
	uint16		format;
	bool		place;			/* the piece goes on the page being left */
	uint64		wide[SIFT_WBUF_WIDE_WORDS];	/* its wide bloom, built before the link is logged */
} PieceArg;

/* The page is full: the terms of its records, and those of the piece that goes in it, go to the wide bloom (the append of a record changes the head of the page, whose log record grows with the bytes it changes); built before the link is logged */
static void
wide_bloom(Page page, const SiftTermFreq *t, int n, uint64 *wide)
{
	memset(wide, 0, SIFT_WBUF_WIDE_WORDS * sizeof(uint64));
	for (Size pos = sizeof(SiftWbufWideHeaderData); pos < SiftPageGetUsed(page);)
	{
		const SiftWbufDocData *d = (const SiftWbufDocData *) (SiftPageGetData(page) + pos);
		const uint32 *hash = REC_HASH(d);

		for (int i = 0; i < d->nterms; i++)
			bloom_add(wide, SIFT_WBUF_WIDE_WORDS, hash[i]);
		pos += RECSIZE(d->nterms, d->termbytes, REC_NFIELDS(d));
	}
	for (int i = 0; i < n; i++)
		bloom_add(wide, SIFT_WBUF_WIDE_WORDS, t[i].hash);
}

static void
fill_piece(Page cp, Page np, void *arg)
{
	PieceArg   *a = arg;

	if (a->place)
	{
		put_piece(cp, a->tid, a->dl, a->fdl, a->nfields, a->cont, a->t, a->n, a->termbytes);
		if (a->spans)
			WBUFPAGE(cp)->flags |= SIFT_WBUFPAGE_SPANS;
	}
	if (a->format >= SIFT_WBUF_FORMAT_WIDE)
	{
		SiftWbufWideHeaderData *ph = (SiftWbufWideHeaderData *) WBUFPAGE(cp);

		memcpy(ph->wide, a->wide, sizeof(ph->wide));
		ph->base.flags |= SIFT_WBUFPAGE_WIDE;
	}
}

/*
 * Writes the doc starting at the locked tail cur; returns whether the segment
 * reached write_buffer_limit. Leaves the final tail block in *tail.
 */
static bool
write_doc(Relation index, WbufHint *h, int lane, Buffer cur, ItemPointer tid, const SiftTermFreq *t,
		  int nterms, uint32 dl, const uint16 *fdl, int nfields, BlockNumber *tail)
{
	bool		crossed = false,
				first = true;
	int			i = 0,
				whole_bytes = 0;

	for (int k = 0; k < nterms; k++)
		whole_bytes += t[k].len;
	for (;;)
	{
		int			rem = nterms - i,
					tb,
					n = fit(ROOM(BufferGetPage(cur)), t + i, rem, first ? nfields : 0, &tb);
		bool		place;
		PieceArg	pa;

		if (n == rem)
		{
			SiftPageEdit ed;

			sift_page_edit_begin(&ed, cur, SIFT_PAGE_DATA_OFFSET + sizeof(SiftWbufPageHeaderData));
			put_piece(ed.page, tid, dl, fdl, nfields, !first, t + i, n, tb);
			sift_page_edit_end(index, &ed, 1);
			break;
		}
		/* STOR[STOR-BM25-05]: wbuf tail-split >=1KiB (uses SIFT_WBUF_CONT) + VACUUM seal >=8p (vs 1/4 limit) */
		place = n > 0 && (ROOM(BufferGetPage(cur)) >= WBUF_SPLIT_ROOM ||
						  !(first && RECSIZE(nterms, whole_bytes, nfields) <= SIFT_PAGE_PAYLOAD - hdr_size(h->format)));
		pa = (PieceArg) {tid, dl, fdl, nfields, !first, t + i, n, tb, i + n < nterms, h->format, place};
		if (h->format >= SIFT_WBUF_FORMAT_WIDE)
			wide_bloom(BufferGetPage(cur), place ? t + i : NULL, place ? n : 0, pa.wide);
		cur = sift_lane_link(index, h->gen, h->hdr, lane, cur, hdr_size(h->format), limit_pages(index),
							 fill_piece, &pa, &crossed);
		if (place)
		{
			first = false;
			i += n;
		}
	}
	*tail = BufferGetBlockNumber(cur);
	UnlockReleaseBuffer(cur);
	return crossed;
}

/* The tail of a lane of the active buffer, locked; h is a copy of the hint: a lock taken here (the maintenance opens the heap) reloads the entry and frees the cached one */
static Buffer
lock_open_tail(Relation index, WbufHint *h, int *lane)
{
	SiftAnalyzerConfig cfg;
	Buffer		buf;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (h->gen == 0)
		{
			sift_analyzer_config_from_index(index, &cfg);
			refresh_hint(index, h, cfg.hash);
		}
		if (BufferIsValid(buf = sift_lane_lock_tail(index, h->gen, h->tail, SIFT_LANES, lane)))
			return buf;
		h->gen = 0;
	}
}

bool
sift_wbuf_append(Relation index, ItemPointer tid, const SiftTermFreq *terms, int nterms, uint32 dl, const uint16 *fdl, int nfields)
{
	WbufHint	hint = *get_hint(index);
	int			lane;
	BlockNumber tail;
	Buffer		buf = lock_open_tail(index, &hint, &lane);
	bool		crossed = write_doc(index, &hint, lane, buf, tid, terms, nterms, dl, fdl, nfields, &tail);

	hint.tail[lane] = tail;
	*get_hint(index) = hint;
	return crossed;
}

/*
 * The docs a statement inserts into an index are staged as the records of one page and written together, one log record for a page of docs and not one for each. The
 * stage is written when it is full, before and after every statement (a statement that reads the index, a trigger or a function among them, finds the docs in it, and so
 * does a parallel worker), and before the commit. A subtransaction that aborts drops the docs it staged: their rows are dead, and VACUUM may reuse their addresses, which it
 * would not for a doc that is in the index.
 */
#define STAGE_BYTES		(SIFT_PAGE_PAYLOAD - sizeof(SiftWbufWideHeaderData))	/* fit an empty page of either format */
#define STAGE_SLOTS		64		/* stages of as many indexes, a page each: 512 KB, allocated as needed and freed at the end of the transaction */
#define STAGE_PAGE(s)	((Page) (s)->page.data)
#define STAGE_HDR		sizeof(SiftWbufPageHeaderData)

typedef struct WbufStage
{
	Oid			relid;			/* InvalidOid: the slot is free */
	RelFileLocator locator;
	SubTransactionId subid;
	PGAlignedBlock page;		/* a page of the narrow format with the staged records */
} WbufStage;

typedef struct StageCopy
{
	const char *recs;
	Size		bytes;
	uint32		ndocs;
	uint32		sum_dl;
	const uint64 *bloom;
} StageCopy;

typedef struct StageFill
{
	StageCopy	copy;
	uint16		format;
	uint64		wide[SIFT_WBUF_WIDE_WORDS];
} StageFill;

static WbufStage *stages[STAGE_SLOTS];
static int	nstages;
static int	speculating;		/* executors running an INSERT ... ON CONFLICT: only theirs may insert a speculative tuple */
static ExecutorRun_hook_type prev_run;
static ProcessUtility_hook_type prev_utility;

static Size
stage_bytes(const WbufStage *s)
{
	return SiftPageGetUsed(STAGE_PAGE(s)) - STAGE_HDR;
}

static const char *
stage_recs(const WbufStage *s)
{
	return SiftPageGetData(STAGE_PAGE(s)) + STAGE_HDR;
}

static void
stage_clear(WbufStage *s)
{
	sift_lane_init_page(STAGE_PAGE(s), STAGE_HDR);
}

/* The bytes of whole records that fit room, and their docs and dl */
static Size
stage_take(const WbufStage *s, Size room, StageCopy *c)
{
	Size		pos = 0;

	while (pos < stage_bytes(s))
	{
		const SiftWbufDocData *d = (const SiftWbufDocData *) (stage_recs(s) + pos);
		Size		size = RECSIZE(d->nterms, d->termbytes, REC_NFIELDS(d));

		if (size > room - pos)
			break;
		pos += size;
		c->ndocs++;
		c->sum_dl += d->dl;
	}
	return pos;
}

static void
append_recs(Page page, const StageCopy *c)
{
	SiftWbufPageHeaderData *ph = WBUFPAGE(page);
	Size		used = SiftPageGetUsed(page);

	memcpy(SiftPageGetData(page) + used, c->recs, c->bytes);
	ph->ndocs += c->ndocs;
	ph->sum_dl += c->sum_dl;
	for (int i = 0; i < 4; i++)
		ph->bloom[i] |= c->bloom[i];
	SiftPageSetUsed(page, used + c->bytes);
}

static void
fill_stage(Page cp, Page np, void *arg)
{
	StageFill  *f = arg;

	if (f->format >= SIFT_WBUF_FORMAT_WIDE)
	{
		memcpy(((SiftWbufWideHeaderData *) WBUFPAGE(cp))->wide, f->wide, sizeof(f->wide));
		WBUFPAGE(cp)->flags |= SIFT_WBUFPAGE_WIDE;
	}
	append_recs(np, &f->copy);
}

/* The staged docs go to the tail of a lane in one record, and those that do not fit it to a new page in another */
static void
flush_stage(Relation index, WbufStage *s)
{
	WbufHint	hint = *get_hint(index);
	const SiftWbufPageHeaderData *sh = WBUFPAGE(STAGE_PAGE(s));
	StageCopy	c = {.recs = stage_recs(s),.bloom = sh->bloom};
	Size		total = stage_bytes(s);
	StageFill	f;
	SiftPageEdit ed;
	Buffer		buf;
	bool		crossed = false;
	int			lane;

	if (total == 0)
		return;
	if (sift_index_known_out(index))
	{
		sift_quarantine_stopped(index, SIFT_SKIP_WRITE);
		stage_clear(s);
		return;
	}
	buf = lock_open_tail(index, &hint, &lane);
	c.bytes = stage_take(s, ROOM(BufferGetPage(buf)), &c);
	if (c.bytes > 0)
	{
		sift_page_edit_begin(&ed, buf, SIFT_PAGE_DATA_OFFSET + sizeof(SiftWbufPageHeaderData));
		append_recs(ed.page, &c);
		sift_page_edit_end(index, &ed, 1);
	}
	if (c.bytes < total)
	{
		f = (StageFill) {{c.recs + c.bytes, total - c.bytes, sh->ndocs - c.ndocs, sh->sum_dl - c.sum_dl, sh->bloom}, hint.format};
		if (hint.format >= SIFT_WBUF_FORMAT_WIDE)
			wide_bloom(BufferGetPage(buf), NULL, 0, f.wide);
		buf = sift_lane_link(index, hint.gen, hint.hdr, lane, buf, hdr_size(hint.format), limit_pages(index), fill_stage, &f, &crossed);
	}
	hint.tail[lane] = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);
	*get_hint(index) = hint;
	stage_clear(s);
	if (crossed)
		sift_maint_after_seal(index);
}

/* The stage of an index that is still in use is written, and its slot is free: of an index that is gone or has another file (a REINDEX or a TRUNCATE rebuilt it from the rows) it is dropped */
static void
flush_slot(WbufStage *s)
{
	Relation	index = try_relation_open(s->relid, RowExclusiveLock);

	if (index != NULL)
	{
		if (RelFileLocatorEquals(s->locator, index->rd_locator))
			flush_stage(index, s);
		relation_close(index, NoLock);
	}
	s->relid = InvalidOid;
}

/* A free slot: a new one while there are fewer than STAGE_SLOTS, else the fullest stage is written, so that every write takes as many docs as it can */
static WbufStage *
free_slot(void)
{
	WbufStage  *victim = NULL;

	for (int i = 0; i < nstages; i++)
		if (!OidIsValid(stages[i]->relid))
			return stages[i];
	if (nstages < STAGE_SLOTS)
	{
		stages[nstages] = MemoryContextAlloc(TopTransactionContext, sizeof(WbufStage));
		return stages[nstages++];
	}
	for (int i = 0; i < nstages; i++)
		if (victim == NULL || stage_bytes(stages[i]) > stage_bytes(victim))
			victim = stages[i];
	flush_slot(victim);
	return victim;
}

static void
free_stages(void)
{
	nstages = 0;
}

static WbufStage *
stage_for(Relation index)
{
	WbufStage  *s = NULL;

	for (int i = 0; i < nstages && s == NULL; i++)
		if (stages[i]->relid == RelationGetRelid(index))
			s = stages[i];
	if (s == NULL)
	{
		s = free_slot();
		s->relid = RelationGetRelid(index);
		s->locator = index->rd_locator;
		stage_clear(s);
	}
	else if (!RelFileLocatorEquals(s->locator, index->rd_locator))
	{
		s->locator = index->rd_locator;
		stage_clear(s);
	}
	else if (s->subid != GetCurrentSubTransactionId())
		flush_stage(index, s);
	s->subid = GetCurrentSubTransactionId();
	return s;
}

static bool
stage_append(Relation index, ItemPointer tid, const SiftTermFreq *terms, int nterms, uint32 dl, const uint16 *fdl, int nfields)
{
	WbufStage  *s = stage_for(index);
	int			tb;

	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (fit(STAGE_BYTES - stage_bytes(s), terms, nterms, nfields, &tb) == nterms)
		{
			put_piece(STAGE_PAGE(s), tid, dl, fdl, nfields, false, terms, nterms, tb);
			return false;
		}
		flush_stage(index, s);
	}
	return sift_wbuf_append(index, tid, terms, nterms, dl, fdl, nfields);
}

static void
flush_all(void)
{
	for (int i = 0; i < nstages; i++)
		if (OidIsValid(stages[i]->relid))
			flush_slot(stages[i]);
}

static bool
staged_any(void)
{
	for (int i = 0; i < nstages; i++)
		if (OidIsValid(stages[i]->relid))
			return true;
	return false;
}

static bool
on_conflict(const Plan *plan)
{
	return plan != NULL && IsA(plan, ModifyTable) && ((const ModifyTable *) plan)->onConflictAction != ONCONFLICT_NONE;
}

/* The statement inserts with ON CONFLICT, at its top or in a WITH */
static bool
speculates(const PlannedStmt *ps)
{
	if (on_conflict(ps->planTree))
		return true;
	foreach_ptr(Plan, sub, ps->subplans)
		if (on_conflict(sub))
			return true;
	return false;
}

#if PG_VERSION_NUM >= 180000
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count
#define RUN_ARGS	qd, dir, count
#else
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count, bool once
#define RUN_ARGS	qd, dir, count, once
#endif

static void
stage_run(RUN_PARAMS)
{
	bool		spec = speculates(qd->plannedstmt);

	flush_all();
	speculating += spec;
	PG_TRY();
	{
		if (prev_run != NULL)
			prev_run(RUN_ARGS);
		else
			standard_ExecutorRun(RUN_ARGS);
	}
	PG_FINALLY();
	{
		speculating -= spec;
	}
	PG_END_TRY();
	flush_all();
}

static void
stage_utility(PlannedStmt *pstmt, const char *query, bool readonly, ProcessUtilityContext context, ParamListInfo params, QueryEnvironment *env, DestReceiver *dest,
			  QueryCompletion *qc)
{
	flush_all();
	if (prev_utility != NULL)
		prev_utility(pstmt, query, readonly, context, params, env, dest, qc);
	else
		standard_ProcessUtility(pstmt, query, readonly, context, params, env, dest, qc);
	flush_all();
}

static void
stage_xact(XactEvent event, void *arg)
{
	if ((event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE) && staged_any())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		flush_all();
		PopActiveSnapshot();
	}
	else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
		free_stages();
}

static void
stage_subxact(SubXactEvent event, SubTransactionId mine, SubTransactionId parent, void *arg)
{
	for (int i = 0; i < nstages; i++)
		if (OidIsValid(stages[i]->relid) && stages[i]->subid == mine)
		{
			if (event == SUBXACT_EVENT_ABORT_SUB)
				stages[i]->relid = InvalidOid;
			else if (event == SUBXACT_EVENT_COMMIT_SUB)
				stages[i]->subid = parent;
		}
}

void
sift_wbuf_stage_init(void)
{
	prev_run = ExecutorRun_hook;
	ExecutorRun_hook = stage_run;
	prev_utility = ProcessUtility_hook;
	ProcessUtility_hook = stage_utility;
	RegisterXactCallback(stage_xact, NULL);
	RegisterSubXactCallback(stage_subxact, NULL);
}

/*
 * A doc of a speculative insertion (ON CONFLICT) that loses is dead at once and VACUUM may reuse its address before the statement ends, so its doc is written and not staged;
 * of a table that is not a heap there is no telling under ON CONFLICT. A non-unique index is told UNIQUE_CHECK_NO under ON CONFLICT too, so the heap tuple is read only while
 * such a statement runs.
 */
static bool
stageable(Relation heap, ItemPointer tid)
{
	Buffer		buf;
	Page		page;
	bool		ok;

	if (speculating == 0)
		return true;
	if (heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		return false;
	buf = ReadBuffer(heap, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	ok = !HeapTupleHeaderIsSpeculative((HeapTupleHeader) PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(tid))));
	UnlockReleaseBuffer(buf);
	return ok;
}

static char out_of_service;		/* the cache of an index that is out of service for the rest of the statement: its address only */

bool
sift_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
			  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	SiftAnalyzer *an;
	SiftTermFreq *terms;
	uint32		dl;
	uint16		fdl[SIFT_MAX_FIELDS];
	int			n;
	uint64		key;
	bool		keyed = sift_bm25_unique_key(index, values, isnull, &key),
				unique = true,
				crossed;
	text	   *t = sift_bm25_assemble_doc(index, values, isnull, CurrentMemoryContext);

	if (t == NULL)
		return true;
	if (indexInfo->ii_AmCache == &out_of_service || sift_index_known_out(index))
	{
		indexInfo->ii_AmCache = &out_of_service;
		sift_quarantine_stopped(index, SIFT_SKIP_WRITE);
		return false;
	}
	if ((an = indexInfo->ii_AmCache) == NULL)
	{
		SiftAnalyzerConfig cfg;

		sift_analyzer_config_from_index(index, &cfg);
		an = indexInfo->ii_AmCache = sift_analyzer_create(&cfg, indexInfo->ii_Context);
	}
	if (keyed && checkUnique == UNIQUE_CHECK_EXISTING)
		return sift_bm25_unique_check(index, heap, values, isnull, heap_tid, checkUnique, key);
	n = sift_analyze_doc(an, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &terms, &dl);
	if (keyed)
		n = sift_bm25_unique_terms(an, terms, n, key, &dl, &terms);
	sift_field_dl(terms, n, sift_analyzer_nfields(an), fdl);
	if (keyed && checkUnique != UNIQUE_CHECK_NO)
		unique = sift_bm25_unique_check(index, heap, values, isnull, heap_tid, checkUnique, key);
	crossed = (keyed || !stageable(heap, heap_tid) ? sift_wbuf_append : stage_append)(index, heap_tid, terms, n, dl, fdl, sift_analyzer_nfields(an));
	sift_analyzer_reset(an);
	if (keyed && checkUnique != UNIQUE_CHECK_NO)
		sift_bm25_unique_release(index, key);
	if (crossed)
		sift_maint_after_seal(index);
	return unique;
}
