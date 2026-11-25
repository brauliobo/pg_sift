/*
 * table/dispatch.h - the table access method by on-disk format: the TableAmRoutine of each format major.
 * table/am.c is the only handler; it resolves the format of a relation once (the format major of its block 0, core/meta_peek.h, kept in its relcache entry) and of a
 * scan once (the field after the scan descriptor), so a callback is one pointer load away from its format. A format's files include no file of another format.
 */
#ifndef SIFT_TABLE_DISPATCH_H
#define SIFT_TABLE_DISPATCH_H

#include "access/tableam.h"
#include "fmgr.h"

struct SiftCheck;

typedef struct SiftTableFormatOps
{
	uint16		format_major;
	const char *name;
	const TableAmRoutine *tam;
	/* the per-table options of sift.table_set_option, table_reset_option and table_option_names; check_set and known are NULL for a format whose options table/v1/options.c keeps */
	bool		(*option_check_set) (Relation rel, const char *name, const char *value, const char **why);
	bool		(*option_known) (const char *name);
	Datum		(*option_names) (void);
	/* the text of what a table of the format that sets none has for the option; setting: the name of the setting of the cluster that it follows, or NULL */
	char	   *(*option_default) (const char *name, const char **setting);
	/* the statements of table/utility.c, NULL for a format that has no use for them */
	void		(*ddl_before) (Relation rel, AlterTableStmt *stmt);
	void		(*ddl_after) (Relation rel, Node *parsetree);
	void		(*ddl_rename_column) (Relation rel, const char *oldname, const char *newname);
	/* the rows of sift.table_paths of the table are about to change: what the table holds under its old declaration is written out, and its store is replaced in every backend; NULL for a format whose layout is read with the relcache entry */
	void		(*paths_changed) (Relation rel);
	/* the rows of the virtual block of a table an index points into that the snapshot sees (core/visibility.c) */
	int			(*filter_page) (IndexFetchTableData *fetch, Snapshot snapshot, BlockNumber vblock, uint64 *offmask);
	/* where the row at an address lies, as a key that rows of one granule share the high bits of: fetching rows in the order of it reads each granule once; NULL for a format whose fetches are not dearer by their order */
	uint64		(*fetch_order) (IndexFetchTableData *fetch, const ItemPointerData *tid);
	/* the virtual blocks of a table, the block space its TIDs are handed out in */
	uint64		(*nvblocks) (Relation rel);
	/* the sift_table side of sift.check(): the problems of an open table, a row each */
	void		(*check) (struct SiftCheck *ck, Relation rel);
} SiftTableFormatOps;

/* The first field of the relcache state (the amcache of the entry) of every sift_table */
typedef struct SiftTblRelHeader
{
	const SiftTableFormatOps *ops;
} SiftTblRelHeader;

/* The scan of every format starts with these: the descriptor, then the table of its format */
typedef struct SiftTblScanHeader
{
	TableScanDescData base;
	const SiftTableFormatOps *ops;
} SiftTblScanHeader;

typedef struct SiftTblFetchHeader
{
	IndexFetchTableData base;
	const SiftTableFormatOps *ops;
} SiftTblFetchHeader;

extern const SiftTableFormatOps sift_tbl_v1_ops;
extern const SiftTableFormatOps sift_tbl_v2_ops;

/* Registers the settings and the hooks of Format 1 and the callbacks of Format 2.0 (from sift_tbl_init) */
extern void sift_tbl_v1_init(void);
extern void sift_tbl_v2_init(void);

/* The hook of ALTER TABLE, SET SCHEMA and RENAME COLUMN on a table of a format that has callbacks for them */
extern void sift_tbl_utility_init(void);

/* The table of the relation, and of a format major */
extern const SiftTableFormatOps *sift_tbl_ops(Relation rel);
extern const SiftTableFormatOps *sift_tbl_ops_of_major(uint16 major);

/* The table of the relations a CREATE TABLE, TRUNCATE or rewrite makes (core/meta_peek.h: sift_new_relation_format_major) */
extern const SiftTableFormatOps *sift_tbl_ops_for_new(void);

extern const TableAmRoutine *sift_tbl_routine(void);
extern bool sift_tbl_is_table(Relation rel);
/* A relation of the planner is a sift_table of Format 2.0 (the lock of the statement is held): its aggregates are the engine of Format 2.0's, never that of Format 1 */
extern bool sift_tbl_is_f2_oid(Oid relid);
/* An error unless rel is a sift_table */
extern void sift_tbl_require(Relation rel);
/* The table of a statement that inspects it, AccessShareLock held: a sift_table the role may maintain */
extern Relation sift_tbl_open_verifiable(Oid relid);

#endif
