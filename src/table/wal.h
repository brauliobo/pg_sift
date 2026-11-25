/*
 * table/wal.h - logical replication of a sift_table (table/wal.c): a published table under wal_level = logical also writes one record of the custom resource
 * manager per row change, whatever the format of the table
 */
#ifndef SIFT_TABLE_WAL_H
#define SIFT_TABLE_WAL_H

#include "core/host.h"

#include "access/htup.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/relcache.h"

/* The most a change record of a published table may hold (EXT-05): the log record is one allocation, and PostgreSQL refuses one past MaxAllocSize */
#ifndef SIFT_TBL_LOG_MAX_BYTES
#define SIFT_TBL_LOG_MAX_BYTES		((uint64) (MaxAllocSize / 2))
#endif

typedef enum SiftTblOp
{
	SIFT_TBL_OP_INSERT,
	SIFT_TBL_OP_UPDATE,
	SIFT_TBL_OP_DELETE
} SiftTblOp;

/* Registers the resource manager when pg_sift is preloaded (called from _PG_init) */
extern void sift_tbl_init(void);
/* sift._tbl_rmgr_registered() boolean: whether this server registered the resource managers, which only a preloaded library does */
extern Datum sift_tbl_rmgr_registered(PG_FUNCTION_ARGS);
/* True when changes to rel must be logged for decoding; 0A000 when they must but pg_sift is not preloaded */
extern bool sift_tbl_check_publication(Relation rel);
/* Is the table published for logical decoding (wal_level = logical): the same test without the error for a missing resource manager */
extern bool sift_tbl_published(Relation rel);
/* newrow: the row after the change (NULL for DELETE); old: the row before it (NULL for INSERT), reduced to its replica identity */
extern void sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old);
extern void sift_tbl_log_key(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple key);
extern HeapTuple sift_tbl_old_key(Relation rel, HeapTuple old);
/* The same for the slot of the old row, without forming a tuple of all its columns first (a wide value the identity does not name is never copied); always a new tuple, or NULL */
extern HeapTuple sift_tbl_slot_old_key(Relation rel, TupleTableSlot *old);
/* The values of slot: its own array, or a copy with every external TOAST value expanded (no TOAST table exists for this AM, and the table the pointer names may be gone when the record is decoded) */
extern Datum *sift_tbl_slot_values(TupleDesc desc, TupleTableSlot *slot);
/* The row of slot as a MinimalTuple with no external value in it; *should_free: the caller frees it */
extern MinimalTuple sift_tbl_detoast_slot_tuple(Relation rel, TupleTableSlot *slot, bool *should_free);

#endif
