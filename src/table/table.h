/*
 * table/table.h - the table access method sift_table (docs/COL-TAM.md): formats and the interfaces between its modules.
 *
 *   table/am.c         handler, the TableAmRoutine callbacks, DML
 *   table/store.c   layout, per-relation cache
 *   table/wbuf.c    write buffers (row format)
 *   table/tomb.c    tombstone segment
 *   table/mvcc.c    visibility of a row version under every snapshot type
 *   table/scan.c    sequential, TID range and parallel scans
 *   table/fetch.c   TID fetches, bitmap scans, TABLESAMPLE
 *   table/analyze.c ANALYZE sampling, index build
 *   table/vacuum.c  VACUUM, promotion, rewrites
 *   table/seg.c     the column groups: the layout of a table, datum <-> leaf values, the engine glue
 *   table/gscan.c   the group scan: rows of the column groups in order (gscan_row.c: their values; gscan_cache.c: a vblock, TID fetches)
 *   table/vers.c    versions of the rows of the groups with unfrozen rows
 *   table/feed.c    what the groups give the aggregate scan: schemas, write buffer batches, path statistics
 *   table/promote.c promotion of the sealed write buffers into column segments
 *   table/freeze.c  freezing, aborted rows, live rows, merges of the column segments
 *   table/bulk.c    the new table of a rewrite written as column segments
 *   table/funcs.c   SQL functions that show the segments and the space
 *   table/src.c     the column source of the aggregate scan (SiftAgg) (src_read.c: the reads)
 *   table/wide.c    its wide leaves: the dictionary of exact datum images, the conjuncts on them
 *
 * A TID is (vblock, offset), offset 1..SIFT_TBL_MAX_OFF, vblock from the metapage counter, never reused.
 */
#ifndef SIFT_TABLE_H
#define SIFT_TABLE_H

#include "core/host.h"

#include "access/tableam.h"
#include "storage/read_stream.h"
#include "utils/snapshot.h"

#include "col/enc.h"
#include "col/seg.h"
#include "core/store.h"

/* PostgreSQL 18 gave the minimal tuple constructors an `extra` size */
#if PG_VERSION_NUM >= 180000
#define sift_tbl_form_minimal_tuple(desc, values, isnull)	heap_form_minimal_tuple(desc, values, isnull, 0)
#define sift_tbl_minimal_from_heap_tuple(tup)				minimal_tuple_from_heap_tuple(tup, 0)
#else
#define sift_tbl_form_minimal_tuple(desc, values, isnull)	heap_form_minimal_tuple(desc, values, isnull)
#define sift_tbl_minimal_from_heap_tuple(tup)				minimal_tuple_from_heap_tuple(tup)
#endif

#define SIFT_TBL_META_FLAG		0x00020000	/* SiftMetaPageData.flags: this relation is a sift_table */
#define SIFT_TBL_WBUF_MAGIC	0x444C5441	/* "DLTA" */
#define SIFT_TBL_TOMB_MAGIC		0x544F4D42	/* "TOMB" */
#define SIFT_TBL_MAX_OFF		MaxHeapTuplesPerPage
#define SIFT_TBL_MAX_WBUF_PAGES 1024
#define SIFT_TBL_VBLOCKS_SB		SIFT_SB_PAGES
#define SIFT_TBL_TOMB_LOCK_BLKNO 4	/* LockPage target: appenders share, the tombstone rewrite of VACUUM excludes them. Not SIFT_WORKER_LOCK_BLKNO: a worker holds that one for a whole pass, and every delete would wait for it */
StaticAssertDecl(SIFT_TBL_TOMB_LOCK_BLKNO != SIFT_WORKER_LOCK_BLKNO && SIFT_TBL_TOMB_LOCK_BLKNO != SIFT_ACTIVE_LOCK_BLKNO &&
				 SIFT_TBL_TOMB_LOCK_BLKNO != SIFT_MAINT_LOCK_BLKNO && SIFT_TBL_TOMB_LOCK_BLKNO != SIFT_DIRECTORY_BLKNO,
				 "the lock targets of a relation are distinct");
#define SIFT_TBL_ROW_MAX_INLINE	(BLCKSZ / 2)	/* a row above this goes to overflow pages */

/* The next vblock to hand out: the field a table has no other use for (col-core: SIFT_COL_META_NEXT_VBLOCK) */
#define SIFT_TBL_NEXT_VBLOCK(meta)	((meta)->sum_dl)

/* Write buffer header page: SiftWbufHeaderData (lane 0 is the page chain) then this */
typedef struct SiftTblWbufHdr
{
	uint32		magic;
	uint32		flags;			/* reserved (was a sealed flag: a sealed segment is told by the flags of its pages) */
	uint64		vbase;			/* first vblock; page i is vblock vbase + i */
	uint32		vcap;			/* vblocks reserved (>= the pages the segment may take) */
	uint32		npages;
	BlockNumber page[SIFT_TBL_MAX_WBUF_PAGES];
} SiftTblWbufHdr;

#define TBL_WBUF_HDR(page) \
	((SiftTblWbufHdr *) (SiftPageGetData(page) + MAXALIGN(sizeof(SiftWbufHeaderData))))
#define TBL_WBUF_HDR_USED	(MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTblWbufHdr))

/*
 * Write buffer data page: SiftTblPageHdr (next first: the store walks the chain to mark referenced blocks, flags at the offset of
 * SIFT_WBUFPAGE_SEALED), then records forward. slot[k] is the offset (from the page data start) of the record of TID offset k + 1, 0 = none.
 */
typedef struct SiftTblPageHdr
{
	BlockNumber next;
	uint16		nrows;			/* slots used */
	uint16		flags;
	uint32		vblock;
	BlockNumber ovf_next;		/* OVERFLOW pages: the next page of the row's bytes */
	uint16		slot[SIFT_TBL_MAX_OFF];
} SiftTblPageHdr;

#define SIFT_TBL_PAGE_SEALED	0x0001	/* = SIFT_WBUFPAGE_SEALED: set on every page when the segment is sealed (sift_lane_flag_sealed_pages) */
#define SIFT_TBL_PAGE_OVFDATA	0x0100	/* continuation bytes of an overflow row, no slots */
#define SIFT_TBL_PAGE_TAIL		0x0200	/* an OVFDATA page the write buffer's page table does not list (no vblock): the bytes of a row too wide for the table of its buffer */

#define TBL_PAGE(page)	((SiftTblPageHdr *) SiftPageGetData(page))
#define TBL_PAGE_INIT	MAXALIGN(sizeof(SiftTblPageHdr))

/* A row record; a MinimalTuple of len bytes follows (an overflow head: its first piece; see aux) */
typedef struct SiftTblRec
{
	uint32		len;			/* bytes of the row (MinimalTuple t_len) */
	TransactionId xmin;
	CommandId	cmin;
	uint16		flags;
	uint16		inline_len;		/* bytes of the row stored in this record (0 for an overflow row) */
	uint32		aux;			/* OVF: first overflow block */
	uint32		token;			/* SPEC: the speculative insertion token */
} SiftTblRec;

#define SIFT_TBL_REC_DEAD		0x0001	/* removed (killed speculative insertion, or VACUUM) */
#define SIFT_TBL_REC_SPEC		0x0002	/* speculative insertion in progress: token is set */
#define SIFT_TBL_REC_OVF		0x0004	/* the row continues in overflow pages */
#define TBL_REC_SIZE(inline_len)	MAXALIGN(sizeof(SiftTblRec) + (inline_len))

/* Tombstone segment header page: SiftWbufHeaderData then this; data pages: SiftTblTombPage then SiftTblTomb[] */
typedef struct SiftTblTombHdr
{
	uint32		magic;
	uint32		pad;
	uint64		nentries;		/* appended so far: the version of the log */
} SiftTblTombHdr;

typedef struct SiftTblTombPage
{
	BlockNumber next;
	uint16		nent;
	uint16		flags;
} SiftTblTombPage;

#define TBL_TOMB_HDR(page) \
	((SiftTblTombHdr *) (SiftPageGetData(page) + MAXALIGN(sizeof(SiftWbufHeaderData))))
#define TBL_TOMB_PAGE(page)	((SiftTblTombPage *) SiftPageGetData(page))
#define TBL_TOMB_PAGE_INIT	MAXALIGN(sizeof(SiftTblTombPage))
#define TBL_TOMB_ENTS(page)	((SiftTblTomb *) (SiftPageGetData(page) + TBL_TOMB_PAGE_INIT))

typedef enum SiftTblTombKind
{
	SIFT_TBL_TOMB_DELETE = 1,
	SIFT_TBL_TOMB_UPDATE,		/* succ is the new version */
	SIFT_TBL_TOMB_LOCK			/* row lock: never deletes */
} SiftTblTombKind;

/* UPDATE entries: the new version is a member of the chain of this one, no index has an entry for it */
#define SIFT_TBL_TOMB_HOT		0x0001

typedef struct SiftTblTomb
{
	uint64		key;			/* SiftKey of the row version */
	TransactionId xid;
	CommandId	cid;
	uint64		succ;			/* UPDATE: key of the new version */
	uint16		kind;
	uint16		flags;
	uint32		seq;			/* position in the log (not stored: set when loaded) */
} SiftTblTomb;

#define TBL_TOMB_CAP	((SIFT_PAGE_PAYLOAD - TBL_TOMB_PAGE_INIT) / sizeof(SiftTblTomb))

StaticAssertDecl(sizeof(SiftTblTomb) == 32, "tombstone entries are 32 bytes");
StaticAssertDecl(sizeof(SiftTblPageHdr) + 16 < BLCKSZ / 4, "page header leaves room for rows");
StaticAssertDecl(TBL_WBUF_HDR_USED <= SIFT_PAGE_PAYLOAD, "write buffer header fits a page");

typedef struct SiftTblSeg
{
	uint64		gen;
	BlockNumber hdr;
	uint64		vbase;
	uint32		vcap;
	uint32		npages;			/* pages known */
	BlockNumber page[SIFT_TBL_MAX_WBUF_PAGES];
} SiftTblSeg;

typedef struct SiftTblRel
{
	Oid			relid;
	RelFileNumber relnumber;
	MemoryContext cxt;
	bool		initialized;	/* blocks exist and carry the table layout */
	bool		invalid;		/* relcache invalidation: replaced by the next sift_tbl_rel */
	SiftDirSnapshot dir;
	int			nwbuf;
	SiftTblSeg *wbuf;			/* [nwbuf], directory order = ascending vbase */
	uint32		wbuf_version;	/* the directory version wbuf was built for (dir.version is also set by the readers of the directory that do not rebuild it) */
	/* tombstones, sorted by (key, seq) */
	uint64		tomb_gen;
	BlockNumber tomb_hdr;
	uint64		tomb_synced;
	BlockNumber tomb_last;
	uint32		tomb_lastcnt;
	uint32		tomb_seqbase;	/* the entries of the logs read before the current one, which only follows them (tomb_follow) */
	bool		tomb_followed;
	SiftTblTomb *ents;			/* sorted by (key, seq): the bulk of the log */
	uint32		nents;
	uint32		capents;
	SiftTblTomb *recent;		/* sorted likewise: entries synced since the last merge into ents (small, cheap to insert into) */
	uint32		nrecent;
	uint32		caprecent;
	struct HTAB *hotm;			/* the keys that are the succ of a HOT entry: chain members (NULL until the log has one) */
	SiftTblTomb *run;			/* scratch: the entries of one key found in both arrays */
	uint32		caprun;
	/* the page this backend appends to */
	uint64		tgt_gen;
	BlockNumber tgt_blk;
	/* column groups (table/seg.c) */
	struct SiftTblLayout *layout;
	struct SiftTblGroupCache *gcache;
	struct SiftTblVers *curvers;	/* the unfrozen groups of dir, for readers that go by the directory the cache holds (sift_tbl_filter_page) */
	uint32		curvers_version;
	Relation	curvers_rel;
	MemoryContext curvers_cxt;
	bool		fresh;			/* the new table of a rewrite: nobody else reads it, its released extents are reusable at once */
	/* what this backend saw when it last asked for the background pass (auto_defrag_check), and when it last looked (sift_tbl_auto_defrag_tick) */
	uint64		defrag_tombs;
	int64		defrag_free;
	int64		defrag_at;
	uint64		defrag_vblock;	/* the next virtual block then: a write buffer opened since is a writer that reuses the free blocks */
	TransactionId settle_horizon;	/* the oldest xmin when this backend last asked for the settling of closed write buffers: they wait for it to move */
} SiftTblRel;

/* The cache of rel (created on first use); ReadBuffer-level state is not held */
extern SiftTblRel *sift_tbl_rel(Relation rel);
/* Re-reads the directory unless its version is the cached one (_full: always); write buffer page tables are extended lazily by sift_tbl_wbuf_page */
extern void sift_tbl_refresh(Relation rel, SiftTblRel *st);
extern void sift_tbl_refresh_full(Relation rel, SiftTblRel *st);
/* Lays the relation out when it is empty (idempotent, any backend); a writer calls it before its first access */
extern void sift_tbl_ensure_init(Relation rel);
extern bool sift_tbl_is_table(Relation rel);
/* An error unless rel is a sift_table */
extern void sift_tbl_require(Relation rel);
extern const TableAmRoutine *sift_tbl_routine(void);
extern uint64 sift_tbl_nvblocks(Relation rel);

/* A row version found in storage */
typedef struct SiftTblRowVer
{
	SiftKey		key;
	bool		wbuf;			/* write buffer row: xmin/cmin/flags valid, else a column group row (frozen unless segxmin) */
	TransactionId xmin;
	CommandId	cmin;
	uint16		flags;
	uint32		token;			/* SPEC: the speculative insertion token */
	HeapTuple	tuple;			/* the row, palloc'd (HeapTuple format, t_self set) */
} SiftTblRowVer;

/*
 * Column groups with rows that are not final (docs/COL-TAM.md section 7). A write buffer promoted while its rows are not final becomes a
 * group whose units keep the xmin and cmin of every row (the engine's version section, col/ver.h); a frozen row has xmin 0, and the
 * directory entry says SIFT_SEG_VERS while some row is not. The visibility of a row is sift_tbl_visible's, as for a write buffer record; a
 * group whose rows are all final loses the flag (VACUUM, a merge of its rows) and its vectors with the next rewrite. A group of 3.0.1 carried
 * one xid and one command for all its rows; the engine reads that as constant vectors, so there is one case here.
 */
typedef struct SiftTblVers SiftTblVers;

/* What a snapshot makes of the insertions of the rows of a unit */
typedef enum SiftTblVerState
{
	SIFT_TBL_VER_ALL,			/* every row was inserted before the snapshot (or the unit has no unfrozen row): only the tombstones decide */
	SIFT_TBL_VER_NONE,			/* none was: the unit has no visible row */
	SIFT_TBL_VER_ROWS			/* decide row by row */
} SiftTblVerState;

/* The versions of the rows of one page of a unit, for sift_tbl_vers_rowver; xmin NULL: the page's rows are frozen */
typedef struct SiftTblVerPage
{
	const uint32 *xmin;
	const uint32 *cmin;
	uint32		first;
	uint64		offmask[SIFT_OFFMASK_WORDS];
} SiftTblVerPage;

static inline bool
sift_tbl_seg_unfrozen(const SiftSegEntry *e)
{
	/* 3.0.1 wrote the xid of the one command that wrote the whole group in reserved (format 1.3); 3.1 sets SIFT_SEG_VERS */
	return e->kind == SIFT_SEG_IMMUTABLE && ((e->flags & SIFT_SEG_VERS) || (e->format < SIFT_COL_FORMAT_VERS && e->reserved != 0));
}

/* The groups with unfrozen rows of a directory copy (the copy must stay protected by a snapshot of the caller) */
extern SiftTblVers *sift_tbl_vers_build(Relation rel, SiftTblRel *st, const SiftDirSnapshot *dir, MemoryContext cxt);
/* What the snapshot makes of the unit of superblock sb, from its summary; a live writer's rows an MVCC snapshot does not see are a serializable conflict out */
extern SiftTblVerState sift_tbl_vers_sb(Relation rel, Snapshot snapshot, SiftTblVers *vers, uint32 sb);
/* The versions of the rows of page vblock when sift_tbl_vers_sb said ROWS */
extern void sift_tbl_vers_page(SiftTblVers *vers, BlockNumber vblock, SiftTblVerPage *vp);
/* The insertion of the row at offset off of the page: xmin and cmin (0, 0 when frozen or not a row of the unit) */
extern void sift_tbl_vers_rowver(const SiftTblVerPage *vp, uint32 off, TransactionId *xmin, CommandId *cmin);
/* The same decision for the summary of one unit (what a scan holds) */
extern SiftTblVerState sift_tbl_ver_state(Relation rel, Snapshot snapshot, const SiftColVerSum *sum);

/* The record of TID offset off in a write buffer page (locked or a copy); NULL when there is none */
extern const SiftTblRec *sift_tbl_page_rec(Page page, uint64 gen, uint32 off);
/* The bytes of an overflow row that a continuation page of write buffer gen holds; a page that is none, or holds no bytes, is corruption */
extern uint32 sift_tbl_ovf_piece(Page page, uint64 gen);
/* The row of a record as a HeapTuple (an overflow row is read from its pages); t_self/t_tableOid are the caller's */
extern HeapTuple sift_tbl_rec_tuple(Relation rel, const SiftTblRec *rec, uint64 gen, uint64 key);
extern BlockNumber sift_tbl_wbuf_page(Relation rel, SiftTblSeg *seg, uint32 idx);
extern void sift_tbl_seg_load(Relation rel, SiftTblSeg *seg);
extern bool sift_tbl_load_vblock_in(Relation rel, SiftTblSeg *segs, int nsegs, BlockNumber vblock, Page copy, uint64 *gen, bool *held);
extern bool sift_tbl_load_vblock(Relation rel, SiftTblRel *st, BlockNumber vblock, Page copy, uint64 *gen);
/* The version header of a write buffer record at tid; sift_tbl_rowver_tuple then materializes its row (t_self, t_tableOid set) */
extern void sift_tbl_rowver(SiftTblRowVer *rv, ItemPointer tid, const SiftTblRec *rec);
extern void sift_tbl_rowver_tuple(Relation rel, SiftTblRowVer *rv, const SiftTblRec *rec, uint64 gen, ItemPointer tid);
/* Clears the bits of offmask (bit i: offset i + 1, SIFT_OFFMASK_WORDS words of 64) whose row is not visible to snapshot; returns the bits left */
/* Does the table have HOT chains (as the log this backend has read tells) */
extern bool sift_tbl_has_chains(Relation rel);
typedef void (*SiftTblKeyFn) (void *arg, uint64 key);
/* Calls fn with the key of every member of the HOT chain of each row of vblock that offmask names: a search match is the root's, the table's own scans see the members */
extern void sift_tbl_chain_members(Relation rel, BlockNumber blk, const uint64 *offmask, SiftTblKeyFn fn, void *arg);
extern int	sift_tbl_filter_page(Relation rel, Snapshot snapshot, BlockNumber vblock, uint64 *offmask);
/* The same for a reader that has its own copy of the directory: vers are the unfrozen groups of that copy */
extern int	sift_tbl_filter_page_vers(Relation rel, Snapshot snapshot, SiftTblVers *vers, BlockNumber vblock, uint64 *offmask, bool chain);
extern void sift_tbl_store(TupleTableSlot *slot, HeapTuple tup);
extern bool sift_tbl_fetch_row(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv);
/* As sift_tbl_fetch_row, but a row of a column group comes without its tuple (rv->tuple NULL) and costs no decoding: for callers that
 * need the version, not the values; sift_tbl_fetch_tuple completes it */
extern bool sift_tbl_fetch_lazy(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv);
static inline void
sift_tbl_free_row(SiftTblRowVer *rv)
{
	if (rv->tuple)
		heap_freetuple(rv->tuple);
}
extern void sift_tbl_fetch_tuple(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv);

/* Stops the open write buffer segment taking rows if it has at least minpages pages (a fresh one takes over): it can then be promoted */
extern void sift_tbl_close_wbuf(Relation rel, SiftTblRel *st, uint32 minpages);

/* Appends a row (a MinimalTuple) to the open write buffer segment; sets *tid. spec_token != 0: speculative insertion */
extern void sift_tbl_append(Relation rel, SiftTblRel *st, MinimalTuple mtup, TransactionId xmin, CommandId cmin,
							uint32 spec_token, ItemPointer tid);
/* Sets flag bits of the record of tid (killed speculative insertion) */
extern void sift_tbl_rec_flag(Relation rel, SiftTblRel *st, ItemPointer tid, uint16 set, uint16 clear);
/* A write buffer page, exclusively locked, for VACUUM: the caller edits records through a GenericXLog copy */
extern Buffer sift_tbl_lock_page(Relation rel, const SiftTblSeg *seg, uint32 idx);

/* Tombstones */
extern void sift_tbl_tomb_sync(Relation rel, SiftTblRel *st);
extern uint64 sift_tbl_tomb_entries(Relation rel, SiftTblRel *st);
extern void sift_tbl_tomb_append(Relation rel, SiftTblRel *st, uint64 key, TransactionId xid, CommandId cid,
								 SiftTblTombKind kind, uint16 flags, uint64 succ);
/*
 * Rewrites the tombstone segment without the entries keep() refuses, under the lock that keeps appenders out, and switches the
 * new segment in (the old pages are released with a safexid). Returns the entries kept.
 */
typedef bool (*SiftTblTombKeep) (SiftTblTomb *e, void *arg);	/* may rewrite the entry it keeps */
extern uint64 sift_tbl_tomb_rewrite(Relation rel, SiftTblRel *st, SiftTblTombKeep keep, void *arg);

/* The entries of key (sorted by log position), n of them; NULL when none */
extern const SiftTblTomb *sift_tbl_tomb_find(const SiftTblRel *st, uint64 key, int *n);
/* Is the version a member of a chain (the next of a HOT update: no index has an entry for it); *pred: the version the update was of */
extern bool sift_tbl_hot_pred(const SiftTblRel *st, uint64 key, uint64 *pred);
#define sift_tbl_hot_member(st, key)	sift_tbl_hot_pred((st), (key), NULL)
/* The offsets of the rows of vblock a HOT update entry names, as bits (SIFT_OFFMASK_WORDS words) */
extern void sift_tbl_hot_offsets(const SiftTblRel *st, BlockNumber vblock, uint64 *mask);

extern bool sift_tbl_visible(Relation rel, const SiftTblRel *st, Snapshot snapshot, const SiftTblRowVer *rv);
extern bool sift_tbl_deleted_by_self(const SiftTblRel *st, Snapshot snapshot, uint64 key);
/* The insertion of the version is visible; *dead set when no snapshot will ever see it */
extern bool sift_tbl_inserted_visible(Snapshot snapshot, const SiftTblRowVer *rv, bool *dead);

typedef enum SiftTblDelState
{
	SIFT_TBL_DEL_NONE,			/* not deleted by anyone (locks aside) */
	SIFT_TBL_DEL_SELF,			/* deleted or updated by the current transaction */
	SIFT_TBL_DEL_INPROGRESS,	/* another transaction is deleting or updating it */
	SIFT_TBL_DEL_DELETED,		/* committed delete */
	SIFT_TBL_DEL_UPDATED		/* committed update: succ is the newer version */
} SiftTblDelState;

typedef struct SiftTblDel
{
	SiftTblDelState state;
	TransactionId xid;			/* the deleter (state != NONE) */
	CommandId	cid;
	uint64		succ;
	bool		hot;			/* the update is a HOT one: the new version is the next of the chain of this one */
	TransactionId lockxid;		/* an in-progress locker (another transaction), InvalidTransactionId if none */
	bool		self_locked;	/* the current transaction holds a lock entry */
} SiftTblDel;

/* The version the HOT update of key (the last one the log tells of, aborted ones left out) led to: false when it has none */
extern bool sift_tbl_hot_next(const SiftTblRel *st, uint64 key, uint64 *succ);
/* The deletion state of key by the log as it is now (no snapshot: the writers' view) */
extern void sift_tbl_del_state(const SiftTblRel *st, uint64 key, SiftTblDel *del);

extern TableScanDesc sift_tbl_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key,
										 ParallelTableScanDesc pscan, uint32 flags);
extern void sift_tbl_scan_end(TableScanDesc scan);
extern void sift_tbl_scan_rescan(TableScanDesc scan, struct ScanKeyData *key, bool set_params, bool allow_strat,
								 bool allow_sync, bool allow_pagemode);
extern bool sift_tbl_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
/* The next stored row version of a scan, visible to its snapshot or not (VACUUM FULL): rv->tuple is palloc'd, rv->xmin unset for a row of a column group */
extern bool sift_tbl_scan_next_stored(TableScanDesc scan, SiftTblRowVer *rv);
extern void sift_tbl_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid);
extern bool sift_tbl_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
extern Size sift_tbl_parallelscan_estimate(Relation rel);
extern Size sift_tbl_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
extern void sift_tbl_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
extern IndexFetchTableData *sift_tbl_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM);
extern void sift_tbl_index_fetch_reset(IndexFetchTableData *data);
extern void sift_tbl_index_fetch_end(IndexFetchTableData *data);
extern bool sift_tbl_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot,
									   TupleTableSlot *slot, bool *call_again, bool *all_dead);
extern bool sift_tbl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
extern bool sift_tbl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
extern void sift_tbl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);
extern bool sift_tbl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
extern TransactionId sift_tbl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
#if PG_VERSION_NUM >= 180000
extern bool sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck,
											uint64 *lossy_pages, uint64 *exact_pages);
#else
extern bool sift_tbl_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres);
extern bool sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TBMIterateResult *tbmres, TupleTableSlot *slot);
#endif
extern bool sift_tbl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream);
extern bool sift_tbl_scan_analyze_next_tuple(TableScanDesc scan, SIFT_TAM_ANALYZE_XMIN double *liverows,
											 double *deadrows, TupleTableSlot *slot);
extern double sift_tbl_index_build_range_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info,
											  bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno,
											  BlockNumber numblocks, IndexBuildCallback callback, void *callback_state,
											  TableScanDesc scan);
extern void sift_tbl_index_validate_scan(Relation table_rel, Relation index_rel, struct IndexInfo *index_info,
										 Snapshot snapshot, struct ValidateIndexState *state);
extern void sift_tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
											   TransactionId OldestXmin, SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff,
											   MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed,
											   double *tups_recently_dead);
extern void sift_tbl_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy);
extern bool sift_tbl_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate);
extern bool sift_tbl_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate,
											TupleTableSlot *slot);

extern bool sift_tbl_published(Relation rel);
/* Logical replication (table/wal.c): a published table under wal_level = logical also writes a record per row change */
typedef enum SiftTblOp
{
	SIFT_TBL_OP_INSERT,
	SIFT_TBL_OP_UPDATE,
	SIFT_TBL_OP_DELETE
} SiftTblOp;

/* Registers the resource manager when pg_sift is preloaded (called from _PG_init) */
extern void sift_tbl_init(void);
/* True when changes to rel must be logged for decoding; 0A000 when they must but pg_sift is not preloaded */
extern bool sift_tbl_check_publication(Relation rel);
/* Is the table published for logical decoding (wal_level = logical): the same test without the error for a missing resource manager */
extern bool sift_tbl_published(Relation rel);
/* newrow: the row after the change (NULL for DELETE); old: the row before it (NULL for INSERT), reduced to its replica identity */
extern void sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old);

typedef struct SiftTblLayout SiftTblLayout;
typedef struct SiftTblGroupScan SiftTblGroupScan;

/* ERROR when the table has more columns than a column segment holds (called at CREATE TABLE) */
extern void sift_tbl_layout_check(Relation rel);
/* Builds (once) the column layout of the table: errors when it is not representable, as ALTER TABLE ADD COLUMN can make it */
extern void sift_tbl_layout_ready(Relation rel, SiftTblRel *st);
/* The layout of the new table of a rewrite: the columns of `rel` with the paths declared for the table it replaces */
extern void sift_tbl_layout_adopt(Relation rel, SiftTblRel *st, Relation from);
/* Is there a tombstone entry for a key between lo and hi? */
extern bool sift_tbl_tomb_any(const SiftTblRel *st, uint64 lo, uint64 hi);
/* Is xid's transaction over and aborted */
extern bool sift_tbl_xid_aborted(TransactionId xid);
/* No snapshot sees xid as running (a frozen xid, that of a chain's root the log keeps, always) */
extern bool sift_tbl_xid_removable(Relation rel, TransactionId xid);

/*
 * The slot of a sift_table (table/slot.c): a virtual slot that decodes the attributes of a row of a column group on demand.
 * store_group: the row at position pos of the vectors of the current unit of g (token), tid its TID; store_tuple: a row as a HeapTuple
 * that stays valid until the slot is cleared (t_self and t_tableOid are the row's TID and table).
 */
extern void sift_tbl_slot_init(void);
extern const TupleTableSlotOps *sift_tbl_slot_ops(void);
extern void sift_tbl_slot_store_group(TupleTableSlot *slot, Relation rel, ItemPointer tid, SiftTblGroupScan *g, uint32 token, uint32 pos);
extern void sift_tbl_slot_store_tuple(TupleTableSlot *slot, HeapTuple tup);
/* Stores the live row tid of a column group lazily; false when no group holds it */
extern bool sift_tbl_slot_locate(TupleTableSlot *slot, Relation rel, SiftTblRel *st, ItemPointer tid);
static inline bool
sift_tbl_slot_is_lazy(TupleTableSlot *slot)
{
	return slot->tts_ops == sift_tbl_slot_ops();
}

/* Does a column group hold the row of tid (live)? On true *rv is filled, rv->tuple palloc'd. */
extern bool sift_tbl_group_fetch(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv, bool lazy);

/*
 * The rows of the column groups of a directory copy, a unit at a time (the directory copy must stay protected by a snapshot of the caller).
 * next: the next live row of the groups in the range of superblocks, rv->key and its xmin and cmin (xmin 0: frozen) only; the units that an
 * MVCC snapshot sees no row of are skipped; plain: no tombstone and no unfrozen row can concern the rows of its unit (every snapshot sees
 * them); tuple: the row as a HeapTuple (t_self set), palloc'd.
 */
extern void sift_tbl_gscan_want(SiftTblGroupScan *g, Bitmapset *want);
extern bool sift_tbl_gscan_fill(SiftTblGroupScan *g, uint32 token, uint32 pos, int from, int to, Datum *values, bool *isnull);
extern uint32 sift_tbl_gscan_cursor(const SiftTblGroupScan *g, ItemPointer tid, uint32 *pos);
extern void sift_tbl_gscan_ref(SiftTblGroupScan *g);
extern void sift_tbl_gscan_unref(SiftTblGroupScan *g);
/* Makes the unit of the live row tid the current one of the table's fetch scan: *token and *pos are what sift_tbl_slot_store_group takes */
extern bool sift_tbl_group_locate(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblGroupScan **g, uint32 *token, uint32 *pos);
extern SiftTblGroupScan *sift_tbl_gscan_begin(Relation rel, TupleDesc desc, SiftTblRel *st, const SiftDirSnapshot *dir, Snapshot snapshot);
extern void sift_tbl_gscan_end(SiftTblGroupScan *g);
extern void sift_tbl_gscan_range(SiftTblGroupScan *g, uint32 sb_lo, uint32 sb_hi);	/* [sb_lo, sb_hi), restarts */
extern bool sift_tbl_gscan_next(SiftTblGroupScan *g, SiftTblRowVer *rv);
/* Backward: the row before the one last returned, in the order sift_tbl_gscan_next goes; seek_end first starts from the last row */
extern bool sift_tbl_gscan_prev(SiftTblGroupScan *g, SiftTblRowVer *rv);
extern void sift_tbl_gscan_seek_end(SiftTblGroupScan *g);
extern bool sift_tbl_gscan_plain(const SiftTblGroupScan *g);
extern HeapTuple sift_tbl_gscan_tuple(SiftTblGroupScan *g);
/* The live rows of vblock whose offset bit is in offmask (NULL: all), as tuples into out (room for SIFT_TBL_MAX_OFF) and their xmin and cmin
 * into xmin / cmin (0: frozen; either may be NULL); their number */
extern int	sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out, TransactionId *xmin, CommandId *cmin);

/* The column layout of the table now, and the one segment e was written for (a segment keeps its own after ALTER TABLE) */
extern const SiftColSchema *sift_tbl_layout_schema(Relation rel, SiftTblRel *st);
/* The pre-aggregates the segments of a layout are written with: the writers and the planner read the same spec */
extern void sift_tbl_pagg_spec(Relation rel, const SiftColSchema *schema, SiftCPaggSpec *spec);
extern Datum sift_tbl_leaf_datum(Relation rel, const SiftColDef *def, const SiftColVec *vec, uint32 i);
/* Does every column segment hold the leaves of the table's declared paths? (a layout of the paths is exposed to the aggregate scan only then) */
extern bool sift_tbl_paths_complete(Relation rel, SiftTblRel *st);
/*
 * The leaves of the declared paths, for the planner (SiftSrcStats): ls[i] what the zones of the column segments say about layout leaf i, wovf[i]
 * whether a write buffer row holds a value the leaf cannot (decoded once per page, not at every call). Both [ncols of the layout].
 */
extern void sift_tbl_path_stats(Relation rel, SiftTblRel *st, SiftColLeafStats *ls, bool *wovf, bool *wjnull);
extern const SiftColSchema *sift_tbl_seg_schema(Relation rel, SiftTblRel *st, const SiftSegEntry *e);
/* segments in the order of their first superblock */
extern int	sift_tbl_cmp_sb_lo(const void *a, const void *b);
/* The rows of superblock sb in the write buffers seg (private copies, pages known) as an engine batch of the table layout, in mcxt; DEAD records
 * are left out, visibility is the reader's */
extern SiftColBatch *sift_tbl_wbuf_batch(Relation rel, SiftTblRel *st, SiftTblSeg *seg, int nseg, uint32 sb, MemoryContext mcxt);
/* Registers the table as a column source of SiftAgg (from sift_tbl_init) */
extern void sift_tbl_src_register(void);

/*
 * Promotes the write buffers that stopped taking rows and whose rows are all final (decided, and older than oldest) into column
 * segments; returns the number promoted. vacuum: rows flagged DEAD by this VACUUM are dropped, otherwise a segment with one waits for it.
 * While the directory is more than half full a write buffer is promoted whatever its rows are (any mix of writers, commands and
 * subtransactions, none speculative): the rows that are not final keep their xmin and cmin in the group. Takes the maintenance lock (not
 * waiting unless vacuum).
 */
extern int	sift_tbl_promote(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum);
/* A writer closed a write buffer: promotes, freezes the groups whose rows are final and merges (steps at most) while the directory is more
 * than half full; returns the write buffers promoted. A close takes SIFT_TBL_SETTLE_STEPS, a writer held back by sift.table_backpressure
 * SIFT_TBL_ROOM_STEPS. */
extern int	sift_tbl_settle(Relation rel, SiftTblRel *st, TransactionId oldest, bool wait, int steps);
#define SIFT_TBL_SETTLE_STEPS	4
#define SIFT_TBL_ROOM_STEPS		16
/* Clears the unfrozen flag of the groups whose live rows all committed before oldest; the maintenance lock held. The groups changed. */
extern int	sift_tbl_freeze_groups(Relation rel, SiftTblRel *st, TransactionId oldest);

/* Keys (blk << 16 | off), runs of consecutive rows of a unit: nothing else of the table lies between the ends of one */
typedef struct SiftTblKeyRuns
{
	MemoryContext mcxt;			/* of the array */
	int			n;
	int			cap;
	SiftKey	   (*run)[2];
} SiftTblKeyRuns;

/*
 * The live rows of the unfrozen groups whose inserter aborted, as runs of keys appended to runs (the rows are dead: VACUUM clears their liveness
 * once the indexes know, and a merge drops them); [*sb_lo, *sb_hi] widened to their superblocks. The maintenance lock held.
 */
extern void sift_tbl_aborted_runs(Relation rel, SiftTblRel *st, SiftTblKeyRuns *runs, uint32 *sb_lo, uint32 *sb_hi);
/* Drops the unfrozen groups that have no live row left (every row aborted, VACUUM cleared them after the indexes forgot them); the maintenance lock held. The groups dropped. */
extern int	sift_tbl_drop_dead_groups(Relation rel, SiftTblRel *st);
/* The rows of the groups that count as live for the statistics: not those of a writer that is still running or aborted */
extern double sift_tbl_group_live_rows(Relation rel, SiftTblRel *st);
/* One merge step over the column segments (the maintenance lock held); the bytes of the segments it merged, 0 when it did nothing. Rows that committed before oldest become frozen. Segments of another column layout are left alone. */
extern uint64 sift_tbl_merge(Relation rel, SiftTblRel *st, TransactionId oldest);
/* Clears the liveness of the group rows cb reports dead (VACUUM); only segments of the superblocks in [sb_lo, sb_hi] */
extern int64 sift_tbl_group_bulkdelete(Relation rel, SiftTblRel *st, IndexBulkDeleteCallback cb, void *state, uint32 sb_lo, uint32 sb_hi);

extern PGDLLIMPORT int sift_tbl_write_buffer_limit;
extern PGDLLIMPORT int sift_tbl_max_segments;
extern PGDLLIMPORT int sift_tbl_vblock_floor;
extern PGDLLIMPORT int sift_tbl_init_gate;
extern PGDLLIMPORT int sift_tbl_seal_gate;
#ifdef SIFT_TESTING
/* Test only: waits for the advisory lock of this number to be free (0 = no gate), so that a test holds a window open */
extern void sift_tbl_gate(int number);
#else
#define sift_tbl_gate(number)	((void) 0)
#endif
extern PGDLLIMPORT bool sift_tbl_inline_promote;
extern PGDLLIMPORT bool sift_tbl_wide_rows;
extern PGDLLIMPORT int sift_tbl_backpressure;
extern PGDLLIMPORT char *sift_tbl_compression_spec;

/* Why the bulk load of rel (COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW) keeps the write buffers; NULL when its rows may be written straight into groups */
extern const char *sift_tbl_bulk_refusal(Relation rel);
/*
 * sift.table_bulk_groups: the rows of a load are encoded into column groups instead of write buffer pages (table/load.c). wanted: does the load
 * of rel qualify (decided once per load, and kept until it ends); row: encodes mtup under the next TID of the load, which it returns in tid;
 * finish: writes the rows that wait (the end of the command; the end of the transaction is the last chance). frozen: rows with no writer.
 */
extern PGDLLIMPORT bool sift_tbl_bulk_groups;
extern bool sift_tbl_load_wanted(Relation rel, SiftTblRel *st, bool frozen);
extern void sift_tbl_load_row(Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid);
extern void sift_tbl_load_finish(Relation rel);

/* Registers sift.table_write_buffer_limit, sift.table_inline_promote and sift.table_compression (from sift_tbl_init) */
extern void sift_tbl_define_gucs(void);

/*
 * The new table of a rewrite written as column segments directly: the rows get dense TIDs in the order they are added; finish
 * publishes the segments in one directory change. The table is private to the rewrite.
 */
typedef struct SiftTblBulk SiftTblBulk;
/* Does the rewrite keep the stored version rv (rv->tuple unset)? */
typedef bool (*SiftTblKeep) (const SiftTblRowVer *rv, void *arg);
extern SiftTblBulk *sift_tbl_bulk_begin(Relation rel, SiftTblRel *st);
extern void sift_tbl_bulk_tuple(SiftTblBulk *b, HeapTuple tup);
/* Every version of old that keep accepts, the rows of its column units re-keyed without forming tuples; false (nothing added) when a
 * column segment of old has another layout than the new table */
extern bool sift_tbl_bulk_copy(SiftTblBulk *b, Relation old, SiftTblRel *ost, SiftTblKeep keep, void *arg);
extern void sift_tbl_bulk_finish(SiftTblBulk *b);

/* The default of sift.table_dead_rewrite_pct: segments kept up to a third dead hold the file near 1.4 times its live size */
#define SIFT_TBL_LOOKUP_PCT	25		/* the stored orders of the dictionaries of a segment take at most this percent of its chunks */
#define SIFT_TBL_DEAD_PCT	30

/* A column segment more than sift.table_dead_rewrite_pct percent dead: the merge policy rewrites it alone, the most dead first */
extern PGDLLIMPORT int sift_tbl_dead_rewrite_pct;
extern PGDLLIMPORT bool sift_tbl_coalesce;

/*
 * Space maintenance without VACUUM (table/vacuum.c, docs/COL-TAM.md section 7): sift_tbl_maintain is VACUUM's work on the column segments
 * and the file minus what needs the indexes (their cleanup, and so the tombstone rewrite). The background pass is requested by the backend
 * that closes a write buffer, deletes or reads when sift_tbl_auto_defrag_tick finds the table worth it.
 */
typedef struct SiftTblMaintain
{
	bool		busy;			/* another VACUUM or pass has the table: nothing was done */
	int64		dead_rows;		/* liveness bits cleared */
	int			promoted;
	int			merged;
	BlockNumber blocks_before;
	BlockNumber blocks_after;
} SiftTblMaintain;

/* The caller holds ShareUpdateExclusiveLock on rel. background: a worker (throttled, short exclusive window); clear: also clears the dead rows */
extern void sift_tbl_maintain(Relation rel, bool background, bool clear, SiftTblMaintain *r);
/* Asks for the pass when the table is worth it; at most once per sift.maintenance_naptime of a backend. Called where rows are deleted and read as well as where a write buffer closes: a table of small writes may close none */
extern void sift_tbl_auto_defrag_tick(Relation rel, SiftTblRel *st);
extern PGDLLEXPORT void sift_tbl_maint_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_tbl_settle_worker_main(Datum main_arg);
/* sift.table_async_promote: a writer that closed a write buffer asks the worker to settle it instead of doing it; false when it has to settle itself */
extern bool sift_tbl_settle_async(Relation rel, SiftTblRel *st);
extern PGDLLIMPORT bool sift_tbl_async_promote;
extern PGDLLIMPORT bool sift_tbl_auto_defrag;
extern PGDLLIMPORT int sift_tbl_auto_defrag_free_pct;
extern PGDLLIMPORT int sift_tbl_auto_defrag_delay;
extern PGDLLIMPORT int sift_tbl_auto_defrag_budget;
extern PGDLLIMPORT int sift_tbl_auto_defrag_dead_pct;
extern PGDLLIMPORT int sift_tbl_auto_defrag_max_active;
extern PGDLLIMPORT int sift_tbl_auto_defrag_wal_rate;
extern PGDLLIMPORT bool sift_tbl_auto_defrag_coalesce;
extern PGDLLIMPORT bool sift_tbl_auto_defrag_indexes;

#endif							/* SIFT_TABLE_H */
