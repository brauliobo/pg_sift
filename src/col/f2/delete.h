/*
 * col/delete.h - deleting rows by TID (format major 2): a handful goes to the pending buffer of the partition (no part is read), a bulk goes through the TID indexes of the parts
 * to new deletion vectors, committed in one change of the manifest. A fold resolves what the buffer holds the same way.
 */
#ifndef SIFT_DELETE_H
#define SIFT_DELETE_H

#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/l0_slab.h"
#include "col/f2/pend_del.h"

#define SIFT_BULK_DEL_MIN		64

struct SiftCdRemapMap;

/* Marks the rows of the part that the unresolved TIDs of recs name (done[i] set for each found, none for a row already dead in base); the number of marks, which the caller sized to n. rm: the address changes of the column delta chain, which name a row by the address it has now (NULL: none) */
extern int	sift_del_locate(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const SiftPendDelRec *recs, int n, bool *done, SiftDvMark *marks, const SiftDv *base, const struct SiftCdRemapMap *rm);

/* Resolves the TIDs (and, when fold is set, the pending buffer of the partition) into the deletion vectors of the parts of the partition; a TID that names no row of a part is dropped: its row was compacted away with its part (or its inserter aborted), so its delete has nothing left to do, and a fold that stopped on it could never finish */
extern int	sift_del_bulk(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n, bool fold, const SiftPartSpec *spec);

/* The path of a delete of n rows of a partition: the buffer below SIFT_BULK_DEL_MIN rows (folded when it has SIFT_PEND_DEL_FOLD_PAGES pages), the bulk path above */
extern void sift_del_route(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPendDelRec *recs, int n, const SiftPartSpec *spec);

#endif							/* SIFT_DELETE_H */
