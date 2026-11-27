/*
 * col/f2/compact/adopt.h - the deletes that reach the inputs of a compaction while it writes its outputs (compact.c): found in the outputs and put into their deletion vectors under the
 * locks of the commit, which a cancel interrupts, in allocations of what they hold however many deletes there are.
 */
#ifndef SIFT_COMPACT_ADOPT_H
#define SIFT_COMPACT_ADOPT_H

#include "col/f2/compact/compact.h"
#include "col/f2/del/pend.h"

/* The deletes that reached the inputs: at most SIFT_COMPACT_ADOPT_MAX, so that the pairs of sift_compact_adoption_ambiguous count in an int */
typedef struct SiftCompactDeletes
{
	SiftPendDelRec *recs;
	int			n;
} SiftCompactDeletes;

#define SIFT_COMPACT_ADOPT_MAX	(INT_MAX / 2)

/* del grown to hold more deletes, at their size */
extern void sift_compact_deletes_reserve(SiftCompactDeletes *del, int64 more);
extern bool sift_compact_adoption_ambiguous(Relation rel, uint64 part_id, const SiftCompactDeletes *del, SiftPartDesc *out, int nout);
extern void sift_compact_adopt_deletes(Relation rel, uint64 part_id, const SiftCompactDeletes *del, SiftPartDesc *out, int nout);

#ifdef SIFT_TESTING
/* The first rows of the input d, deleted as settled deletes through the bulk path, as a deleter that reaches the partition while a compaction writes its outputs */
extern void sift_compact_test_delete_meanwhile(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartDesc *d, const SiftPartSpec *spec);
#endif

#endif
