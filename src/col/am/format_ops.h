/*
 * col/am/format_ops.h - the column access method by on-disk format: the callbacks PostgreSQL calls and the transaction hooks, one table per format major.
 * col/am/am.c is the only handler; it resolves the table of a relation once (the format major of its block 0, core/meta_peek.h, kept in its relcache entry)
 * and of a scan once (its first field), so a callback is one pointer load away from its format. A format's files include no file of another format.
 */
#ifndef SIFT_COL_FORMAT_OPS_H
#define SIFT_COL_FORMAT_OPS_H

#include "access/amapi.h"
#include "fmgr.h"

struct SiftCheck;

/* The SQL functions that read or drive the file of an index (sift.col_space ...): each format answers them in its own way */
typedef enum SiftColSqlFn
{
	SIFT_SQL_SPACE,
	SIFT_SQL_EXTENTS,
	SIFT_SQL_SHRINK_HISTORY,
	SIFT_SQL_INDEX_INFO,
	SIFT_SQL_SEGMENTS,
	SIFT_SQL_LEAVES,
	SIFT_SQL_SECTIONS,
	SIFT_SQL_VERIFY,
	SIFT_SQL_MAINTAIN,
	SIFT_SQL_MAINTENANCE_DUE,
	SIFT_SQL_MAINTENANCE_STATUS,
	SIFT_SQL_TRACKING,
	SIFT_SQL_WORKER_STATE,
	SIFT_SQL_WORKER_HISTORY,
	SIFT_SQL_REFRESH
} SiftColSqlFn;

typedef struct SiftColFormatOps
{
	uint16		format_major;
	const char *name;

	ambuild_function ambuild;
	ambuildempty_function ambuildempty;
	ambuildphasename_function ambuildphasename;
	aminsert_function aminsert;
	ambulkdelete_function ambulkdelete;
	amvacuumcleanup_function amvacuumcleanup;
	amcostestimate_function amcostestimate;
	ambeginscan_function ambeginscan;
	amrescan_function amrescan;
	amgetbitmap_function amgetbitmap;
	amgettuple_function amgettuple;
	amendscan_function amendscan;
	amcanreturn_function amcanreturn;
	ammarkpos_function ammarkpos;
	amrestrpos_function amrestrpos;

	/* The rows and tombstones the transaction holds back: written before it reads the index or commits */
	void		(*flush_held) (void);

	/* track_changes: the body of sift.col_track_old_rows for a trigger of this format, and the triggers following the option after ALTER INDEX */
	Datum		(*track_trigger) (PG_FUNCTION_ARGS);
	void		(*track_reconcile) (Relation index);

	/* track_changes follows the address of each row, by one trigger for both events, whatever the option names but "off" (format major 2); else by a key column or ctid, a trigger for each event */
	bool		tracks_addresses;

	Datum		(*sql) (SiftColSqlFn fn, FunctionCallInfo fcinfo);

	/* sift.check: the steps of the verification of the index, reporting or raising by ck (core/check.h) */
	void		(*check) (struct SiftCheck *ck, Oid indexoid, bool heapallindexed);
} SiftColFormatOps;

/* The first field of the relcache state of every sift_col index (the amcache of the entry) */
typedef struct SiftColRelHeader
{
	const SiftColFormatOps *ops;
} SiftColRelHeader;

/* The first field of the opaque state of every scan of a sift_col index */
typedef struct SiftColScanHeader
{
	const SiftColFormatOps *ops;
} SiftColScanHeader;

extern const SiftColFormatOps sift_col_v1_ops;
extern const SiftColFormatOps sift_col_v2_ops;

/* The table of the relation (col/am/rel.c) and of a format major (col/am/am.c) */
extern const SiftColFormatOps *sift_col_ops(Relation index);
extern const SiftColFormatOps *sift_col_ops_of_major(uint16 major);

/* The table of the relations a CREATE INDEX makes (core/meta_peek.h: sift_new_relation_format_major) */
extern const SiftColFormatOps *sift_col_ops_for_build(void);

#endif
