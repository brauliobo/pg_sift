/*
 * table/f2/f2_lock_2pc.h - the row locks of a prepared transaction of a sift_table in the lock manager of the server.
 *
 * The entries of tier 1 (f2_rowlock.h) are shared memory and a crash empties them. PREPARE TRANSACTION therefore takes, for each row the transaction still holds by entry, the tuple lock of the
 * lock manager at the address of the row, in the mode of the lock that maps to it one to one (KeyShare, Share, NoKeyExclusive and Exclusive are AccessShareLock, RowShareLock, ExclusiveLock and
 * AccessExclusiveLock, which conflict as the four modes of a tuple lock of a heap do): the server writes it to the state of the prepared transaction and takes it again for the transaction when
 * recovery finds it, and COMMIT PREPARED and ROLLBACK PREPARED let go of it. A page lock of the relation at SIFT_F2_ROW_2PC_LOCK_BLKNO, taken beside it, says that the relation has such rows, so a
 * transaction asks for the tuple lock of a row it locks only when some prepared transaction holds that page lock (a cluster without prepared transactions asks nothing).
 */
#ifndef SIFT_TABLE_F2_LOCK_2PC_H
#define SIFT_TABLE_F2_LOCK_2PC_H

#include "postgres.h"

#include "access/tableam.h"
#include "storage/lock.h"

#define SIFT_F2_ROW_2PC_LOCK_BLKNO	0xFFFFF810U		/* between the F2 locks and the compaction locks of core/part_lock.h */

StaticAssertDecl(SIFT_F2_ROW_2PC_LOCK_BLKNO > 0xFFFFF803U && SIFT_F2_ROW_2PC_LOCK_BLKNO < 0xFFFFFC00U, "the 2PC row lock block is between the F2 locks and the compaction locks");

/* The tuple lock for the row of the packed address in the mode, taken for the transaction that prepares, and the page lock of the relation that says there is one */
extern void sift_lock_2pc_hold(Oid dbid, Oid relid, uint64 tid, LockTupleMode mode, bool first_of_relation);

/* Whether some prepared transaction holds row locks of the relation in the lock manager */
extern bool sift_lock_2pc_present(Oid dbid, Oid relid);

/* TM_BeingModified with tmfd->xmax a prepared transaction that holds the row in a mode that conflicts with the request, else TM_Ok */
extern TM_Result sift_lock_2pc_probe(Oid dbid, Oid relid, uint64 tid, LockTupleMode mode, TM_FailureData *tmfd);

#endif
