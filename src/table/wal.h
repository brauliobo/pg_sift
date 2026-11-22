/*
 * table/wal.h - logical replication of a sift_table (table/wal.c): a published table under wal_level = logical also writes one record of the custom resource
 * manager per row change, whatever the format of the table
 */
#ifndef SIFT_TABLE_WAL_H
#define SIFT_TABLE_WAL_H

#include "core/host.h"

#include "access/htup.h"
#include "utils/relcache.h"

typedef enum SiftTblOp
{
	SIFT_TBL_OP_INSERT,
	SIFT_TBL_OP_UPDATE,
	SIFT_TBL_OP_DELETE
} SiftTblOp;

/* Registers the resource manager when pg_sift is preloaded (called from _PG_init) */
extern void sift_tbl_init(void);
/* True when changes to rel must be logged for decoding; 0A000 when they must but pg_sift is not preloaded */
extern bool sift_tbl_check_publication(Relation rel);
/* Is the table published for logical decoding (wal_level = logical): the same test without the error for a missing resource manager */
extern bool sift_tbl_published(Relation rel);
/* newrow: the row after the change (NULL for DELETE); old: the row before it (NULL for INSERT), reduced to its replica identity */
extern void sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old);
extern void sift_tbl_log_key(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple key);
extern HeapTuple sift_tbl_old_key(Relation rel, HeapTuple old);

#endif
