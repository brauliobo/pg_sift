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
	Page		page;			/* copy of the current page */
	PageCursor	cur;
	uint16		pflags;
	bool		torn;			/* next_rec skipped a row whose last piece is missing */
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
	const uint8 *payload;
	Size		len;
} RawRec;

extern uint32	wbuf_limit_pages(Relation index);

extern void	wbuf_cursor_set(PageCursor *c, Page page);

extern const SiftColRecHead *	wbuf_rec_at(PageCursor *c, Size *size);

extern uint32	wbuf_encode_row(StringInfo buf, const SiftColSchema *schema, const SiftColRow *row, uint32 *jnull);

extern void	wbuf_put_piece(Page page, const ItemPointerData *tid, bool cont, const char *payload, Size len);

extern bool	wbuf_write_row(Relation index, SiftCWbufHint *h, int lane, Buffer cur, const ItemPointerData *tid, const char *payload, Size len, BlockNumber *tail);

extern void	wbuf_flag_overflow(Relation index, SiftCWbufHint *h, uint32 mask, uint32 jmask);

extern void	wbuf_flush_for_read(Relation index);

extern uint64	wbuf_count_rows(Relation index, const SiftSegEntry *entry, bool first_only);

extern bool	wbuf_advance_page(SiftCWbufReader *r);

extern bool	wbuf_next_rec(SiftCWbufReader *r, RawRec *rec);

extern const uint8 *	wbuf_get_value(SiftCWbufReader *r, const SiftColDef *def, const uint8 *p, const uint8 *end, SiftColValue *v);

extern void	wbuf_decode_row(SiftCWbufReader *r, const RawRec *rec, SiftColRow *row);

#endif