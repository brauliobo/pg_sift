/*
 * agg/first/first.h - DISTINCT ON without the sort: the first row of each group of the DISTINCT ON columns by the ORDER BY that follows them, from the rows of any plan in any order.
 *
 * A hash of the DISTINCT ON key (PostgreSQL's equality, NULLs equal) holds the best row of each group as a whole tuple, so the select list of any type (arrays, expressions, the columns of a join) leaves
 * with it. A row replaces the held one only when it sorts strictly before it by the ORDER BY (PostgreSQL's sort operators, collations and NULLS placement), so of rows that tie the first one offered stays.
 * Over work_mem times hash_mem_multiplier the groups that are not held yet, with every row that follows them, go to partitions on temporary files; each partition is solved the same way, one at a time and
 * with another hash, so the memory is that limit whatever the number of groups.
 */
#ifndef SIFT_AGG_V6_FIRST_H
#define SIFT_AGG_V6_FIRST_H

#include "postgres.h"

#include "access/attnum.h"
#include "executor/tuptable.h"

#define SIFT_V6_FIRST_MAX_COLS	32

/* the columns of the tuple the groups and the order are of, with the operators of the statement */
typedef struct SiftV6FirstKeys
{
	int			nkey;
	AttrNumber	key_att[SIFT_V6_FIRST_MAX_COLS];
	Oid			key_eqop[SIFT_V6_FIRST_MAX_COLS];
	Oid			key_coll[SIFT_V6_FIRST_MAX_COLS];
	int			nord;
	AttrNumber	ord_att[SIFT_V6_FIRST_MAX_COLS];
	Oid			ord_sortop[SIFT_V6_FIRST_MAX_COLS];
	Oid			ord_coll[SIFT_V6_FIRST_MAX_COLS];
	bool		ord_nulls_first[SIFT_V6_FIRST_MAX_COLS];
	int			nsort;
	AttrNumber	sort_att[SIFT_V6_FIRST_MAX_COLS];
	Oid			sort_op[SIFT_V6_FIRST_MAX_COLS];
	Oid			sort_coll[SIFT_V6_FIRST_MAX_COLS];
	bool		sort_nulls_first[SIFT_V6_FIRST_MAX_COLS];
} SiftV6FirstKeys;

typedef struct SiftV6FirstStats
{
	uint64		rows;
	uint64		groups;
	uint64		spilled_rows;
	uint32		passes;
} SiftV6FirstStats;

typedef struct SiftV6First SiftV6First;

/* budget: the bytes a pass holds before the groups it has not met go to files; tupdesc: of the rows offered and returned */
extern SiftV6First *sift_v6_first_new(MemoryContext cx, TupleDesc tupdesc, const SiftV6FirstKeys *keys, uint64 budget);
extern void sift_v6_first_offer(SiftV6First *f, TupleTableSlot *slot);
/* The next group's first row into slot (of tupdesc, minimal tuples); false after the last. The first call ends the input */
extern bool sift_v6_first_next(SiftV6First *f, TupleTableSlot *slot);
extern const SiftV6FirstStats *sift_v6_first_stats(const SiftV6First *f);
extern void sift_v6_first_free(SiftV6First *f);

#endif							/* SIFT_AGG_V6_FIRST_H */
