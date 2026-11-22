/*
 * core/sql_api.h - what the unified SQL functions of 6.0 (sift.space, sift.parts, sift.maintain, sift.status, sift.col_granules, sift.scope_stats) share: the open of a relation of any of the three access
 * methods and either format, the checks of the result row, and the readers of the space and of the manifest that more than one of them needs.
 *
 * The functions answer for sift (BM25), sift_col and sift_table; Format 1 and Format 2.0 (core/meta_peek.h). The result columns are those of sql/6.0/15-unified-api.sql, in that order: every
 * function lists its column types and checks them against the row the SQL declared (sift_api_check_desc), so a column that moves in one place and not in the other fails at once.
 */
#ifndef SIFT_SQL_API_H
#define SIFT_SQL_API_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/lockdefs.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "core/meta_peek.h"

typedef enum SiftApiAm
{
	SIFT_API_BM25,
	SIFT_API_COL,
	SIFT_API_TABLE
} SiftApiAm;

typedef struct SiftApiRel
{
	Relation	rel;
	SiftApiAm	am;
	SiftMetaPeek peek;
	bool		f2;				/* the metapage is Format Major 2 */
} SiftApiRel;

/* The relation, locked, after the checks of every inspector (a sift access method, not a partitioned index, the right to look: core/inspect.h); a block 0 of a newer format or damaged is an error */
extern SiftApiRel sift_api_open(Oid relid, LOCKMODE lock);
extern void sift_api_close(SiftApiRel *r, LOCKMODE lock);
extern const char *sift_api_am_name(SiftApiAm am);

/* The access method of a relation by its name in pg_am ("sift", "sift_col", "sift_table"); false for any other */
extern bool sift_api_am_of(Relation rel, SiftApiAm *am);
extern bool sift_api_am_by_oid(Oid amoid, SiftApiAm *am);

/* An error unless the row the SQL declared has these columns (the types of the first ncols attributes): the guard against a column that moved in the SQL or in the C */
extern void sift_api_check_desc(TupleDesc desc, const Oid *types, int ncols);

/* numeric of hundredths ("12.34" for 1234), the value of a numeric(n, 2) column; the share and the ratio round half up; the whole must not be 0 */
extern Datum sift_api_numeric2(int64 hundredths);
extern Datum sift_api_percent(uint64 part, uint64 whole);
extern Datum sift_api_ratio(uint64 num, uint64 den);

/* The name of why the end of the file is not cut (the SiftShrinkBlock of a metapage), NULL when nothing blocks it */
extern const char *sift_api_shrink_blocked_by(uint16 shrink_blocked);

/*
 * The space of a relation: the file, the blocks the allocator holds free (reusable: no snapshot reads them; pending: one may), the parts or segments and the rows. The row of sift.space, and what
 * sift.status takes its footprint from. busy_known is false on a standby (a lock probe writes nothing, but nobody maintains there).
 */
typedef struct SiftApiSpace
{
	int64		relation_blocks;
	int64		high_water;
	int64		used_blocks;
	int64		free_blocks;
	int64		reusable_blocks;
	int64		pending_blocks;
	int64		free_extents;
	int64		largest_free_extent;
	bool		has_largest;
	int32		parts;
	int64		rows;
	int64		dead_rows;
	int64		bytes;
	bool		has_buffered;
	int64		buffered_rows;	/* the rows of the write buffers, not in a segment yet (Format 1 sift and sift_col) */
	bool		busy_known;
	bool		busy;
	TimestampTz last_shrink;	/* 0: never */
	const char *shrink_blocked_by;
	bool		has_vblocks;
	int64		vblocks_left;
} SiftApiSpace;

extern SiftApiSpace sift_api_space(const SiftApiRel *r);

/* sift.maintain: the parts the manifest of a Format 2.0 relation lists, and the slabs; for the before and after of a step */
typedef struct SiftApiF2Count
{
	int64		parts;
	int64		slabs;
} SiftApiF2Count;

extern SiftApiF2Count sift_api_f2_count(Relation rel);

#endif							/* SIFT_SQL_API_H */
