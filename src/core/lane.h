/*
 * core/lane.h - the lane chains of a write buffer (DESIGN.md (c)), shared by the doc records of bm25/wbuf.c and the row records
 * of col/wbuf.c: segment creation and sealing, the tail lock, page growth, page walks. What a record looks like is the caller's.
 *
 * The data pages of both start with SiftLanePageHead, at the offsets of SiftWbufPageHeaderData, so the store's walkers
 * (sift_store_referenced_blocks, sift_store_orphans, sift_store_verify) follow `next` of either unchanged. The caller's page header
 * is `hdrsize` bytes and extends the head.
 */
#ifndef SIFT_LANE_H
#define SIFT_LANE_H

#include "core/host.h"

#include "core/store.h"

typedef struct SiftLanePageHead
{
	BlockNumber next;
	uint16		nrecs;			/* records started on this page (deleted included) */
	uint16		flags;			/* SIFT_WBUFPAGE_* */
} SiftLanePageHead;

StaticAssertDecl(offsetof(SiftWbufPageHeaderData, next) == offsetof(SiftLanePageHead, next) &&
				 offsetof(SiftWbufPageHeaderData, ndocs) == offsetof(SiftLanePageHead, nrecs) &&
				 offsetof(SiftWbufPageHeaderData, flags) == offsetof(SiftLanePageHead, flags),
				 "lane page heads must agree with the store's view of a write buffer page");

#define SiftLaneHdr(page)		((SiftWbufHeaderData *) SiftPageGetData(page))
#define SiftLaneHead(page)		((SiftLanePageHead *) SiftPageGetData(page))
#define SiftLaneRecs(page, hdrsize)	(SiftPageGetData(page) + (hdrsize))
#define SiftLaneRoom(page)		(SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page))

extern void sift_lane_init_page(Page page, Size hdrsize);
/* The header page of a segment whose first nlanes lanes are the consecutive pages first.. (the other lanes are empty: no head) */
extern void sift_lane_init_header(Page page, uint64 gen, BlockNumber first, int nlanes);
extern uint32 sift_lane_npages(const SiftWbufHeaderData *hdr);
extern Buffer sift_lane_read_locked(Relation index, BlockNumber blk, int mode);
extern void sift_lane_read_header(Relation index, const SiftSegEntry *e, SiftWbufHeaderData *hdr);

/* ---------- segments ---------- */

/*
 * The active segment's header block, generation and lane tails, creating the segment when there is none (make_room: called when the
 * directory is full; false when nothing can be merged). format is the owner's directory format for a fresh buffer. hash is checked against meta->analyzer_hash; what names it in the error.
 */
extern void sift_lane_refresh(Relation index, uint32 hash, const char *what, Size hdrsize, uint16 format, bool (*make_room) (Relation),
							  uint64 *gen, BlockNumber *hdr_blk, BlockNumber *tail);

/*
 * Seals the active segment, when it has records (has_records) and, unless force, has limit pages; opens a fresh one. Returns the sealed
 * gen or 0.
 */
extern uint64 sift_lane_seal(Relation index, bool force, Size hdrsize, uint16 format, uint32 limit, bool (*has_records) (Relation, const SiftSegEntry *));

/* Sets the sealed flag on the true tail of every lane */
extern void sift_lane_flag_sealed(Relation index, const SiftSegEntry *entry);
/* The same on every page of every lane, for records that go to any page of the chain, not just its tail (sift_table) */
extern void sift_lane_flag_sealed_pages(Relation index, const SiftSegEntry *entry);
/* Directory edit that opens a fresh active segment: the segment `sealed` (0 for none) becomes SEALED, fresh is added */
extern void sift_lane_swap_active(SiftDirectoryData *dir, uint64 sealed, const SiftSegEntry *fresh);

/* ---------- appending ---------- */

/*
 * Exclusively locks the tail of a lane of segment gen (tail[] as last seen), trying the lanes after MyProcNumber % SIFT_LANES without
 * waiting first (only the first nlanes lanes exist: SIFT_LANES for the record segments, 1 for a page chain). InvalidBuffer when the hint
 * is stale (the segment was sealed).
 */
extern Buffer sift_lane_lock_tail(Relation index, uint64 gen, const BlockNumber *tail, int nlanes, int *lane);

/*
 * Links a new page after the locked tail cur of lane `lane` and counts it in the header, in one WAL record; fill edits cur inside it
 * (may be NULL; must not throw), after the link: cur->next is the new block, np is the initialized new page, hp the header page. Returns
 * the new tail, locked; cur is released. *crossed: the segment now has limit pages.
 */
typedef void (*SiftLaneFill) (Page cur, Page np, Page hp, void *arg);
extern Buffer sift_lane_grow(Relation index, uint64 gen, BlockNumber hdr_blk, int lane, Buffer cur, Size hdrsize, uint32 limit,
							 SiftLaneFill fill, void *arg, bool *crossed);

/*
 * An append to a locked page, logged as the generic WAL record (RM_GENERIC_ID) the page's own change would be, without the copy of the
 * page, the comparison of its bytes and the copy back that GenericXLogFinish makes for every record: begin saves the first headlen bytes
 * (at most SIFT_EDIT_HEAD: the page header and what the caller keeps in front of the records) and opens a critical section; the caller
 * changes the page (no error may be raised: the bytes it touches are those head bytes, the optional region of the end call, and
 * [pd_lower, new pd_lower) for the appended record); end logs the changed head bytes, the region and the appended bytes, marks the
 * buffer dirty and closes the critical section. The record is replayed by generic_redo like any other.
 */
#define SIFT_EDIT_HEAD	96
typedef struct SiftPageEdit
{
	Page		page;
	Size		headlen;
	uint16		lower;
	char		head[SIFT_EDIT_HEAD];
#ifdef USE_ASSERTION_CHECKING
	char		image[BLCKSZ];		/* the page before: the logged regions must rebuild the page from it */
#endif
} SiftPageEdit;

extern void sift_page_edit_begin(SiftPageEdit *e, Page page, Size headlen);
extern void sift_page_edit_end(Relation index, Buffer buf, SiftPageEdit *e, Size extra_off, Size extra_len);

/* ---------- walking ---------- */

/* Copies a data page out under a share lock, so decoding holds no buffer */
extern void sift_lane_copy_page(Relation index, const SiftSegEntry *e, BlockNumber blk, Page dst, Size hdrsize);

/* The data pages lane by lane, as copies */
typedef struct SiftLaneIter
{
	Relation	index;
	const SiftSegEntry *entry;
	BlockNumber head[SIFT_LANES];
	int			lane;			/* next lane to start */
	BlockNumber nextblk;		/* next page of the current lane */
} SiftLaneIter;

extern void sift_lane_iter_init(SiftLaneIter *it, Relation index, const SiftSegEntry *entry);
/* Copies the next page of the segment to dst; false at the end */
extern bool sift_lane_iter_next(SiftLaneIter *it, Page dst, Size hdrsize);

/*
 * VACUUM's pass over the data pages of a segment (the maintenance lock held): collect lists the live records of the page copy in scratch (their
 * offsets and TIDs, room for SIFT_PAGE_PAYLOAD / minrec of them; returns how many), cb names the dead ones, flag marks the record at an offset
 * dead in the page being logged. Returns the records flagged.
 */
typedef int (*SiftLaneCollect) (void *arg, Page page, Size *offs, ItemPointerData *tids);
typedef void (*SiftLaneFlag) (Page page, Size off);
extern int64 sift_lane_bulkdelete(Relation index, const SiftSegEntry *entry, Size hdrsize, Size minrec, Page scratch,
								  SiftLaneCollect collect, SiftLaneFlag flag, void *arg,
								  IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats);

/* Data pages of the lane chains as they are now (appenders may add more); palloc'd */
extern BlockNumber *sift_lane_blocks(Relation index, const SiftSegEntry *entry, int *n);
/* The same plus the header page, as sorted extents, for release after promotion */
extern SiftExtent *sift_lane_extents(Relation index, const SiftSegEntry *entry, int *n);

/* Structural walk: the header, every lane's chain (check: called with a copy of each page); ERRCODE_INDEX_CORRUPTED */
extern void sift_lane_verify(Relation index, const SiftSegEntry *entry, Size hdrsize,
							 void (*check) (void *arg, Page page, BlockNumber blk), void *arg);

#endif							/* SIFT_LANE_H */
