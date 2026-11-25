/*
 * core/v1_col/tomb/dv.h - the deletion record of the tombstone log: one record says that rows of one part (a unit of a segment) were ended by one transaction.
 *
 * A record is a run of slots of the log (core/v1_col/tomb/tomb.h): a head of kind SIFT_TOMB_DVEC that carries the transaction and command, then body slots of kind SIFT_TOMB_DVEC_BODY that carry
 * the bytes of the rows (a list of 16-bit positions or a bitmap, whichever is smaller). A record never crosses a page of the log, so the appenders cannot interleave two of them, and it
 * is never split by the sort of the log: every slot has the key of the part, and the slots of one key stay in the order they were appended in.
 *
 * The record names a part by two numbers and knows nothing of what the part is: the generation of the segment and the unit's number today, the tail of a partition later.
 */
#ifndef SIFT_CORE_TOMB_DV_H
#define SIFT_CORE_TOMB_DV_H

#include "core/v1_col/tomb/tomb.h"

/* The bytes a record carries (the slots of a page less the head and the descriptor, 16 bytes each) */
#define SIFT_DV_DATA_MAX		((SIFT_TOMB_CAP - 2) * SIFT_DV_SLOT_BYTES)
#define SIFT_DV_SLOT_BYTES		16

/* Head flags */
#define SIFT_DV_BITMAP			0x0001	/* the bytes are a bitmap of span rows from row_lo, else count 16-bit positions after row_lo */

/* The key of every slot of the records of a part: above KEYS_KEY, ordered by generation and then unit, so that all the records of one segment lie in one range of the sorted log */
#define SIFT_TOMB_DV_BASE		(UINT64CONST(1) << 49)
#define SIFT_DV_UNIT_BITS		24
#define SIFT_DV_KEY(gen, part)	(SIFT_TOMB_DV_BASE | (((gen) & ((UINT64CONST(1) << 25) - 1)) << SIFT_DV_UNIT_BITS) | ((part) & ((UINT32_C(1) << SIFT_DV_UNIT_BITS) - 1)))

typedef struct SiftDvDesc
{
	uint32		part;
	uint32		row_lo;			/* the first row the record covers */
	uint32		count;			/* rows it names */
	uint32		span;			/* rows from row_lo to its last row, inclusive */
} SiftDvDesc;

StaticAssertDecl(sizeof(SiftDvDesc) == SIFT_DV_SLOT_BYTES, "the descriptor fills one slot");

typedef struct SiftDvRec
{
	uint64		gen;
	TransactionId xid;
	CommandId	cid;
	SiftDvDesc	desc;
	bool		bitmap;
	uint8		data[SIFT_DV_DATA_MAX];
} SiftDvRec;

/* The slots the records of rows (ascending, n of them) take: a unit's rows are written as records only when they take fewer slots than marks would (one slot a row) */
extern uint32 sift_dv_count(const uint32 *rows, uint32 n);

/*
 * Appends to *out (grown in cxt, *n slots used, *cap allocated) the records of rows (ascending positions of the part, n of them, nrows in the part) ended by xid at cid: as many as the
 * rows need, none for no row. Returns the slots appended.
 */
extern uint32 sift_dv_encode(SiftTomb **out, uint32 *nout, uint32 *cap, MemoryContext cxt, uint64 gen, uint32 part, TransactionId xid, CommandId cid, const uint32 *rows, uint32 n);

/* The slots of the record whose head is at e[0]; 0 when the slots do not describe one */
extern uint32 sift_dv_slots_of(const SiftTomb *e, uint32 avail);

/* The rows the record whose head is at e names (the descriptor is the slot after it) */
extern uint32 sift_dv_head_rows(const SiftTomb *e);

/* Makes the n slots of a record (copied from the log) the record of part in generation gen: the key of every slot, the generation of the head and the part of the descriptor */
extern void sift_dv_rekey_slots(SiftTomb *slots, uint32 n, uint64 gen, uint32 part);

/* Reads the record at e (slots_of(e) slots) */
extern void sift_dv_decode(const SiftTomb *e, uint32 nslots, SiftDvRec *r);

/* The next record of the entries of one key, in the order they come: false at the end. The walk is the one of sift_tomb_key_begin */
extern bool sift_dv_next(SiftTombKeyWalk *w, SiftDvRec *r);

/* Sets the bit of each row the record names in bits (the part's rows) */
extern void sift_dv_or_bits(const SiftDvRec *r, uint64 *bits);

/* The rows the record names, ascending, into rows (r->desc.count of them) */
extern void sift_dv_rows(const SiftDvRec *r, uint32 *rows);

#endif							/* SIFT_CORE_TOMB_DV_H */
