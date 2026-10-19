/*
 * sift_table.h - the table access method sift_table (docs/COL-TAM.md): formats and the interfaces between its modules.
 *
 *   sift_table.c         handler, the TableAmRoutine callbacks, DML
 *   sift_table_store.c   layout, per-relation cache, write buffers (row format), tombstone segment
 *   sift_table_mvcc.c    visibility of a row version under every snapshot type
 *   sift_table_scan.c    scans, TID fetches, bitmap, analyze, index build
 *   sift_table_vacuum.c  VACUUM, promotion, rewrites
 *   sift_table_seg.c     the column groups: the engine glue (stubs until the column engine lands)
 *
 * A TID is (vblock, offset), offset 1..SIFT_TBL_MAX_OFF, vblock from the metapage counter, never reused.
 */
#ifndef SIFT_TABLE_H
#define SIFT_TABLE_H

#include "sift_host.h"

#include "access/tableam.h"
#include "storage/read_stream.h"
#include "utils/snapshot.h"

#include "sift_col_enc.h"
#include "sift_col_seg.h"
#include "sift_store.h"

/* PostgreSQL 18 gave the minimal tuple constructors an `extra` size */
#if PG_VERSION_NUM >= 180000
#define sift_tbl_form_minimal_tuple(desc, values, isnull)	heap_form_minimal_tuple(desc, values, isnull, 0)
#define sift_tbl_minimal_from_heap_tuple(tup)				minimal_tuple_from_heap_tuple(tup, 0)
#else
#define sift_tbl_form_minimal_tuple(desc, values, isnull)	heap_form_minimal_tuple(desc, values, isnull)
#define sift_tbl_minimal_from_heap_tuple(tup)				minimal_tuple_from_heap_tuple(tup)
#endif

#define SIFT_TBL_META_FLAG		0x00020000	/* SiftMetaPageData.flags: this relation is a sift_table */
#define SIFT_TBL_SEG_WBUF		0x0100		/* SiftSegEntry.flags of a write buffer */
#define SIFT_TBL_SEG_TOMB		0x0200		/* ... of the tombstone segment */
#define SIFT_TBL_WBUF_MAGIC	0x444C5441	/* "DLTA" */
#define SIFT_TBL_TOMB_MAGIC		0x544F4D42	/* "TOMB" */
#define SIFT_TBL_MAX_OFF		MaxHeapTuplesPerPage
#define SIFT_TBL_MAX_WBUF_PAGES 1024
#define SIFT_TBL_VBLOCKS_SB		SIFT_SB_PAGES
#define SIFT_TBL_TOMB_LOCK_BLKNO 3	/* LockPage target: appenders share, the tombstone rewrite of VACUUM excludes them */
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

/* ---------- per relation backend cache (sift_table_store.c) ---------- */

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
	/* tombstones, sorted by (key, seq) */
	uint64		tomb_gen;
	BlockNumber tomb_hdr;
	uint64		tomb_synced;
	BlockNumber tomb_last;
	uint32		tomb_lastcnt;
	SiftTblTomb *ents;			/* sorted by (key, seq): the bulk of the log */
	uint32		nents;
	uint32		capents;
	SiftTblTomb *recent;		/* sorted likewise: entries synced since the last merge into ents (small, cheap to insert into) */
	uint32		nrecent;
	uint32		caprecent;
	SiftTblTomb *run;			/* scratch: the entries of one key found in both arrays */
	uint32		caprun;
	/* the page this backend appends to */
	uint64		tgt_gen;
	BlockNumber tgt_blk;
	/* column groups (sift_table_seg.c) */
	struct SiftTblLayout *layout;
	struct SiftTblGroupCache *gcache;
	struct SiftTblVers *curvers;	/* the unfrozen groups of dir, for readers that go by the directory the cache holds (sift_tbl_filter_page) */
	uint32		curvers_version;
	Relation	curvers_rel;
	MemoryContext curvers_cxt;
	bool		fresh;			/* the new table of a rewrite: nobody else reads it, its released extents are reusable at once */
} SiftTblRel;

/* The cache of rel (created on first use); ReadBuffer-level state is not held */
extern SiftTblRel *sift_tbl_rel(Relation rel);
/* Re-reads the directory; write buffer page tables are extended lazily by sift_tbl_wbuf_page */
extern void sift_tbl_refresh(Relation rel, SiftTblRel *st);
/* Lays the relation out when it is empty (idempotent, any backend); a writer calls it before its first access */
extern void sift_tbl_ensure_init(Relation rel);
extern bool sift_tbl_is_table(Relation rel);
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
 * group whose units keep the xmin and cmin of every row (the engine's version section, sift_col_ver.h); a frozen row has xmin 0, and the
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
/* The row of a record as a HeapTuple (an overflow row is read from its pages); t_self/t_tableOid are the caller's */
extern HeapTuple sift_tbl_rec_tuple(Relation rel, const SiftTblRec *rec, uint64 gen, uint64 key);
extern BlockNumber sift_tbl_wbuf_page(Relation rel, SiftTblSeg *seg, uint32 idx);
extern void sift_tbl_seg_load(Relation rel, SiftTblSeg *seg);
extern bool sift_tbl_load_vblock(Relation rel, SiftTblRel *st, BlockNumber vblock, Page copy, uint64 *gen);
/* The version header of a write buffer record at tid; sift_tbl_rowver_tuple then materializes its row (t_self, t_tableOid set) */
extern void sift_tbl_rowver(SiftTblRowVer *rv, ItemPointer tid, const SiftTblRec *rec);
extern void sift_tbl_rowver_tuple(Relation rel, SiftTblRowVer *rv, const SiftTblRec *rec, uint64 gen, ItemPointer tid);
/* Clears the bits of offmask (bit i: offset i + 1, SIFT_OFFMASK_WORDS words of 64) whose row is not visible to snapshot; returns the bits left */
extern int	sift_tbl_filter_page(Relation rel, Snapshot snapshot, BlockNumber vblock, uint64 *offmask);
/* The same for a reader that has its own copy of the directory: vers are the unfrozen groups of that copy */
extern int	sift_tbl_filter_page_vers(Relation rel, Snapshot snapshot, SiftTblVers *vers, BlockNumber vblock, uint64 *offmask);
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
extern void sift_tbl_rec_flag(Relation rel, SiftTblRel *st, ItemPointer tid, uint16 set);
/* A write buffer page, exclusively locked, for VACUUM: the caller edits records through a GenericXLog copy */
extern Buffer sift_tbl_lock_page(Relation rel, const SiftTblSeg *seg, uint32 idx);

/* Tombstones */
extern void sift_tbl_tomb_sync(Relation rel, SiftTblRel *st);
extern void sift_tbl_tomb_append(Relation rel, SiftTblRel *st, uint64 key, TransactionId xid, CommandId cid,
								 SiftTblTombKind kind, uint64 succ);
/*
 * Rewrites the tombstone segment without the entries keep() refuses, under the lock that keeps appenders out, and switches the
 * new segment in (the old pages are released with a safexid). Returns the entries kept.
 */
typedef bool (*SiftTblTombKeep) (const SiftTblTomb *e, void *arg);
extern uint64 sift_tbl_tomb_rewrite(Relation rel, SiftTblRel *st, SiftTblTombKeep keep, void *arg);

/* The entries of key (sorted by log position), n of them; NULL when none */
extern const SiftTblTomb *sift_tbl_tomb_find(const SiftTblRel *st, uint64 key, int *n);

/* ---------- visibility (sift_table_mvcc.c) ---------- */

extern bool sift_tbl_visible(Relation rel, const SiftTblRel *st, Snapshot snapshot, const SiftTblRowVer *rv);
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
	TransactionId lockxid;		/* an in-progress locker (another transaction), InvalidTransactionId if none */
	bool		self_locked;	/* the current transaction holds a lock entry */
} SiftTblDel;

/* The deletion state of key by the log as it is now (no snapshot: the writers' view) */
extern void sift_tbl_del_state(const SiftTblRel *st, uint64 key, SiftTblDel *del);

/* ---------- table AM callbacks implemented outside sift_table.c ---------- */

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

/* ---------- dml helpers (sift_table.c) ---------- */

extern bool sift_tbl_published(Relation rel);
/* Logical replication (sift_table_wal.c): a published table under wal_level = logical also writes a record per row change */
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
/* newrow: the row after the change (NULL for DELETE); old: the row before it (NULL for INSERT), reduced to its replica identity */
extern void sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old);

/* ---------- column groups: the engine glue (sift_table_seg.c) ---------- */

typedef struct SiftTblLayout SiftTblLayout;
typedef struct SiftTblGroupScan SiftTblGroupScan;

/* ERROR when the table has more columns than a column segment holds (called at CREATE TABLE) */
extern void sift_tbl_layout_check(Relation rel);
/* Builds (once) the column layout of the table: errors when it is not representable, as ALTER TABLE ADD COLUMN can make it */
extern void sift_tbl_layout_ready(Relation rel, SiftTblRel *st);
/* Is there a tombstone entry for a key between lo and hi? */
extern bool sift_tbl_tomb_any(const SiftTblRel *st, uint64 lo, uint64 hi);
/* Is xid's transaction over and aborted */
extern bool sift_tbl_xid_aborted(TransactionId xid);

/*
 * The slot of a sift_table (sift_table_slot.c): a virtual slot that decodes the attributes of a row of a column group on demand.
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

/* ---------- the column source of the aggregate scan (sift_table_src.c) ---------- */

/* The column layout of the table now, and the one segment e was written for (a segment keeps its own after ALTER TABLE) */
extern const SiftColSchema *sift_tbl_layout_schema(Relation rel, SiftTblRel *st);
extern const SiftColSchema *sift_tbl_seg_schema(Relation rel, SiftTblRel *st, const SiftSegEntry *e);
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
/* A writer closed a write buffer: promotes, freezes the groups whose rows are final and merges while the directory is more than half full */
extern void sift_tbl_settle(Relation rel, SiftTblRel *st, TransactionId oldest);
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
/* One merge step over the column segments (the maintenance lock held); true when it merged. Rows that committed before oldest become frozen. Segments of another column layout are left alone. */
extern bool sift_tbl_merge(Relation rel, SiftTblRel *st, TransactionId oldest);
extern bool sift_tbl_rewrite(Relation rel, SiftTblRel *st, const SiftSegEntry *e);
/* Clears the liveness of the group rows cb reports dead (VACUUM); only segments of the superblocks in [sb_lo, sb_hi] */
extern int64 sift_tbl_group_bulkdelete(Relation rel, SiftTblRel *st, IndexBulkDeleteCallback cb, void *state, uint32 sb_lo, uint32 sb_hi);

extern PGDLLIMPORT int sift_tbl_write_buffer_limit;
extern PGDLLIMPORT int sift_tbl_max_segments;
extern PGDLLIMPORT int sift_tbl_vblock_floor;
extern PGDLLIMPORT bool sift_tbl_inline_promote;
extern PGDLLIMPORT char *sift_tbl_compression_spec;

/* Registers sift.table_write_buffer_limit, sift.table_inline_promote and sift.table_compression (from sift_tbl_init) */
extern void sift_tbl_define_gucs(void);

/* ---------- rewrites and dead rows (sift_table_seg.c) ---------- */

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

/* A column segment more than sift.table_dead_rewrite_pct percent dead: the merge policy rewrites it alone */
extern bool sift_tbl_mostly_dead(Relation rel, const SiftSegEntry *e);
extern PGDLLIMPORT int sift_tbl_dead_rewrite_pct;

#endif							/* SIFT_TABLE_H */
