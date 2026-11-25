/*
 * table/f2/f2_vm.h - the visibility map of a sift_table: which virtual blocks an index-only scan may trust without asking the table.
 *
 * A vblock is all visible when no snapshot can need the table to tell it more about its rows: (V1) it is closed, no backend may still put a row in it (below the least lease of the table, and below
 * the vblocks reserved so far) and it is full, so that no address was taken and given up (an index entry for a row that was never written); (V2) no slab holds a row of it; (V3) every part with a row of it is sealed for its inserters and older than the oldest transaction; (V4) no deletion vector
 * or pending delete may still stand for a row of it whose address an index has (a vector settled and cleaned out of the indexes is fine); (V5) no column delta chain touches it. Rows go
 * to a vblock only once, so the bit is never made untrue by an insert, and a delete or an update clears it first (sift_tbl_vm_clear).
 *
 * VACUUM sets the bits under a share lock on the table, which no writer holds: the state it reads is the state of the table, nothing is staged in a backend that it cannot see.
 */
#ifndef SIFT_TBL_F2_VM_H
#define SIFT_TBL_F2_VM_H

#include "postgres.h"

#include "access/transam.h"
#include "utils/relcache.h"

/* Whether this server logs the map of the table: preloaded, and the table is not a temporary one */
extern bool sift_tbl_vm_active(Relation rel);

/*
 * The run of vblocks [lo, hi) a backend reserved for its inserts, and the vblock it fills now: the lease VACUUM reads. open takes the slot (where the registry has none, the backend
 * gives back its least recently used lease and run, or goes without a lease); set publishes a run; roll moves lo as the vblock fills. The leases of a file that is gone are let go when the backend takes another.
 */
extern void sift_tbl_vm_lease_open(Relation rel);
extern void sift_tbl_vm_lease_set(Relation rel, uint64 lo, uint64 hi);
extern void sift_tbl_vm_lease_roll(Relation rel, uint64 open_vblock);

/* Clears the bits of the vblock before the delete or the update of a row of it is staged; an ERROR where the table has a map and the server cannot log it */
extern void sift_tbl_vm_clear(Relation rel, BlockNumber vblock);

/*
 * VACUUM's pass, after the indexes were cleaned of the rows that are dead for everyone and the parts were compacted: sets the bits of the vblocks that are all visible (and all frozen)
 * and stores the counts in the metapage. Where a writer holds the table it sets nothing and keeps the counts it had.
 */
extern void sift_tbl_vm_vacuum(Relation rel, FullTransactionId oldest, FullTransactionId freeze);

/* The all visible and all frozen vblocks, of the vblocks that hold a row, as of the last VACUUM (as pg_class holds them in blocks, scaled to the size of the file) */
extern void sift_tbl_vm_fractions(Relation rel, BlockNumber nblocks, BlockNumber *allvisible, BlockNumber *allfrozen, double *allvisfrac);

#endif							/* SIFT_TBL_F2_VM_H */
