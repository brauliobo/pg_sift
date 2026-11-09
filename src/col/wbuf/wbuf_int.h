/*
 * col/wbuf/wbuf_int.h - what the files of the write buffer share
 * the types of its pages and rows and the statics one file calls from another
 */
#ifndef SIFT_COL_WBUF_INT_H
#define SIFT_COL_WBUF_INT_H

#include "col/wbuf.h"
#include "core/lane.h"

/*
 * col/wbuf/wbuf.c - write buffers of a column index: aminsert appends one row record to one of
 * SIFT_LANES lane chains of the active segment, each append one generic WAL record per page touched. The lane machinery (creation, tail
 * locking, growth, sealing, walks) is core/lane.c's, shared with the BM25 doc records of bm25/wbuf.c; this file is the record.
 *
 * A record is SiftColRecHead and `len` payload bytes, 4-byte aligned. The payload of a row, for every top level leaf in id order, is a flag
 * byte (COLV_NULL, COLV_OVERFLOW) and, unless NULL, the value: INT a zigzag varint, F4 and F8 their raw bytes, STR a varint length and the
 * bytes, GROUP the varint element count (the high nibble of the flag byte of an overflow value is its JSON type, SIFT_COLTAG_*). The
 * elements of the children follow, child by child (id order), count values each, every value with its own flag byte. A row that does not fit the tail page is split in pieces of its payload (first without
 * SIFT_COLR_CONT, the rest with it) written while the tail is held, lock coupling to the new page, so a row is always adjacent in one lane.
 * A writer that dies between two pieces leaves a torn row (pieces without the last one): it never committed, readers skip it.
 */

#define SIFT_COLR_DELETED	0x0001

#define SIFT_COLR_CONT		0x0002	/* more payload of the preceding row: not counted */

#define SIFT_COLR_XMIN		0x0004	/* the payload starts with the xid (4 bytes) of the transaction that inserted the row, ahead of the values; first piece only. A row of an index without track_changes, one a speculative insertion wrote and one of a buffer written before this flag have none: nothing is claimed of their inserter */

#define SIFT_COLR_REF		0x0008	/* with SIFT_COLR_XMIN: the payload is the xid alone, the values are in the heap tuple at the TID (col/wbuf/ref.c fetches them when the record is read) */

static inline uint16
wbuf_rec_flags(TransactionId xmin, bool ref)
{
	return (TransactionIdIsValid(xmin) ? SIFT_COLR_XMIN : 0) | (ref ? SIFT_COLR_REF : 0);
}

#define COLV_NULL			0x01

#define COLV_OVERFLOW		0x02

#define COLV_TAG_SHIFT		4			/* the high nibble of a value's flag byte: its SIFT_COLTAG_* (an overflow value's type, a kind; 0 in what 3.0 wrote, which ignores it) */

typedef struct SiftColRecHead
{
	ItemPointerData tid;
	uint16		flags;
	uint32		len;			/* payload bytes of this piece */
} SiftColRecHead;

#define HDRSIZE			sizeof(SiftLanePageHead)

#define RECSIZE(len)	TYPEALIGN(4, sizeof(SiftColRecHead) + (Size) (len))

#define EMPTY_ROOM		(SIFT_PAGE_PAYLOAD - HDRSIZE)

/* The records of a page copy: the cursor and the checked record at it */
typedef struct PageCursor
{
	Relation	index;
	Page		page;
	Size		pos;			/* record offset in the page */
	Size		used;
} PageCursor;

/* A REF record of the page copy and what the arena or the heap gave for it: the encoded values at off in the reader's refpay, none when the tuple is gone or is another one's */
typedef struct RefRow
{
	Size		pos;
	ItemPointerData tid;
	TransactionId xmin;
	Size		off;
	Size		len;
	bool		live;
	bool		gone;			/* decided: the heap has no tuple of this record (the arena or the heap said so), no row */
	bool		got;			/* the arena has its values, not copied yet */
} RefRow;

/* Element count of a group leaf's value in a row (absent array: none) */
static inline uint64
elem_count(const SiftColValue *v)
{
	return v->isnull ? 0 : v->v.i;
}

struct SiftCWbufReader
{
	SiftSegEntry entry;
	const SiftColSchema *schema;
	MemoryContext mcxt;
	SiftLaneIter it;
	void		(*pagefn) (void *arg, BlockNumber blk, Page page, Size used);	/* called for each page copied (col/wbuf/batches.c) */
	void	   *pagearg;
	Page		page;			/* copy of the current page */
	PageCursor	cur;
	uint16		pflags;
	bool		torn;			/* next_rec skipped a row whose last piece is missing */
	/* the REF records of the page copy from the one that asked, in page order (col/wbuf/ref.c) */
	RefRow	   *ref;
	int			nref;
	int			refcap;
	int			refnext;
	bool		refready;
	StringInfoData refpay;
	/* payload of a row assembled from pieces */
	uint8	   *joined;
	Size		joinedlen;
	Size		joinedcap;
	/* the decoded row */
	SiftColValue *val;
	SiftColValue **elem;
	SiftColValue *evals;
	Size		evalcap;
};

/* A record header with its payload; valid until the next call */
typedef struct RawRec
{
	ItemPointerData tid;
	bool		deleted;
	TransactionId xmin;			/* InvalidTransactionId: the record has none */
	const uint8 *payload;
	Size		len;
} RawRec;

extern uint32	wbuf_limit_pages(Relation index);

extern SiftCWbufReader *	wbuf_open_lazy(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt);

extern void	wbuf_load_heads(SiftCWbufReader *r);

extern void	wbuf_cursor_set(PageCursor *c, Page page);

extern const SiftColRecHead *	wbuf_rec_at(PageCursor *c, Size *size);

extern uint32	wbuf_encode_row(StringInfo buf, const SiftColSchema *schema, const SiftColRow *row, uint32 *jnull);


extern bool	wbuf_ref_values(SiftCWbufReader *r, Size pos, RawRec *rec);

extern bool	wbuf_ref_to_arena(Relation index, uint64 gen, const ItemPointerData *tid, const char *payload, Size *len, uint16 *flags);

/* col/wbuf/arena.c */
typedef enum ArenaPut
{
	ARENA_NOT_STORED,
	ARENA_STORED,
	ARENA_FULL
} ArenaPut;

typedef enum ArenaRebuild
{
	ARENA_REBUILD_OWN,
	ARENA_REBUILD_WAITED,
	ARENA_REBUILD_NO_SLOT
} ArenaRebuild;

#define ARENA_GONE		PG_UINT32_MAX	/* the length of the entry of a record whose tuple is gone for good */

extern ArenaPut	wbuf_arena_put(Relation index, uint64 gen, const ItemPointerData *tid, TransactionId xmin, const char *vals, Size len, Size limit);

extern bool	wbuf_arena_full(Relation index, uint64 gen, Size limit);

extern void	wbuf_arena_read(Relation index, uint64 gen, RefRow *rows, int n, StringInfo pay);

extern ArenaRebuild	wbuf_arena_rebuild_enter(Relation index, uint64 gen);

extern void	wbuf_arena_rebuild_exit(uint64 nrows);

#ifdef SIFT_TESTING
extern void	wbuf_arena_test_slots(int n);

extern void	wbuf_arena_test_stats(int *used, uint64 *stored, uint64 *not_stored, uint64 *rebuilt);
#endif

extern void	wbuf_put_piece(Page page, const ItemPointerData *tid, uint16 flags, const char *payload, Size len);

extern bool	wbuf_write_row(Relation index, SiftCWbufHint *h, int lane, Buffer cur, const ItemPointerData *tid, uint16 flags, const char *payload, Size len, BlockNumber *tail);

extern void	wbuf_flag_overflow(Relation index, SiftCWbufHint *h, uint32 mask, uint32 jmask);

extern void	wbuf_flush_for_read(Relation index);

extern uint64	wbuf_count_rows(Relation index, const SiftSegEntry *entry, bool first_only);

extern bool	wbuf_advance_page(SiftCWbufReader *r);

extern bool	wbuf_next_rec(SiftCWbufReader *r, RawRec *rec);

extern const uint8 *	wbuf_get_value(SiftCWbufReader *r, const SiftColDef *def, const uint8 *p, const uint8 *end, SiftColValue *v);

extern void	wbuf_decode_row(SiftCWbufReader *r, const RawRec *rec, SiftColRow *row);

/* Where the next read of a lane starts: InvalidBlockNumber before the first */
typedef struct WbufLane
{
	BlockNumber blk;
	Size		pos;
} WbufLane;

extern uint32 wbuf_page_dead(Relation index, Page page, Size from, Size upto);

extern void	wbuf_read_lane(SiftCWbufReader *r, BlockNumber head, WbufLane *l, void (*fn) (void *arg, const RawRec *rec), void *arg);

#endif