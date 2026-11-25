/*
 * col/f2/del/slab.h - the deletes of a partition that has a slab of heap references (spec 49 2): the slab is read before it is turned into a part, without a heap fetch, and a delete
 * that names none of its references leaves it where it is; one that does flushes it with the deletes whose deleter is settled left out of the part, so that no heap tuple is fetched and no
 * row is written for a version that is already dead, and no deletion vector or pending delete is made for it.
 *
 * A slab younger than one flush's rows and pages whose inserters may still be seen by a snapshot also waits (sift_l0_href_flush_due): a second statement that rewrites a row seconds later
 * lands in the same slab, and the first version is dropped by that flush.
 */
#ifndef SIFT_COL_F2_DEL_SLAB_H
#define SIFT_COL_F2_DEL_SLAB_H

#include "core/host.h"

#include "col/f2/l0/href.h"
#include "col/f2/l0/slab.h"
#include "col/f2/del/pend.h"

#define SIFT_L0_GRACE_PAGES		(SIFT_L0_HEAPREF_MAX_PAGES / 2)

/* Of the n deletes (successor records and the flag bits of a pair are not addresses of a version), those that name one of the nrefs addresses (ascending); the number */
extern int	sift_del_slab_hits(const uint64 *sorted_tids, uint32 nrefs, const SiftPendDelRec *recs, int n, bool *hit);

/* Whether the pure part of the rule, over the slab's rows and pages and whether every inserter is before the horizon, asks for the flush now */
extern bool sift_l0_grace_due(uint32 rows, uint32 pages, bool inserters_settled);

#ifndef SIFT_S49_STANDALONE
/*
 * The slab of the partition at tail, under the partition's lock the caller holds. A slab that is not of references is flushed as it was. For one that is: -1 when none of the n deletes names
 * a reference (nothing is done), else the slab is flushed, done[i] is set for every delete of a settled deleter whose version it held (nothing of it was written), and their number returned.
 */
extern int	sift_del_slab_flush(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, BlockNumber tail, const SiftPendDelRec *recs, int n, bool *done);

/* Whether the slab at tail of a partition is to be flushed by a background pass: its head page tells the rows, the pages and the greatest inserter (an explicit call flushes whatever it holds) */
extern bool sift_l0_href_flush_due(Relation rel, BlockNumber tail, FullTransactionId horizon);

#endif

#endif							/* SIFT_COL_F2_DEL_SLAB_H */
