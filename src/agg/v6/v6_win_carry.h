/*
 * agg/v6/v6_win_carry.h - the columns a window row gives back, taken from the batch the row was read in.
 *
 * The window node gives each row its columns of the table (the Vars of its target list) and fetches them from the heap by the tag of the row: a heap fetch of 300 ns for a row the scan
 * has already decoded. The carry keeps those columns of the rows of every batch the engine reads (integers, dates, timestamps and float8, which a batch holds as the heap does) under
 * the tag of the row, and gives them back, once, when the engine emits the row. A row that is not in it (the carry holds at most max_bytes: the rows past that, or one a Top-K left
 * unread, are not kept) is fetched from the heap as before.
 */
#ifndef SIFT_AGG_V6_WIN_CARRY_H
#define SIFT_AGG_V6_WIN_CARRY_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/v6/v6_vexpr.h"

#define SIFT_WIN_CARRY_MAX_VARS	8

typedef struct SiftWinCarrySpec
{
	int			nvars;
	int			slot[SIFT_WIN_CARRY_MAX_VARS];	/* the column of the batch of each variable */
	Oid			typid[SIFT_WIN_CARRY_MAX_VARS];
	uint64		max_bytes;
} SiftWinCarrySpec;

typedef struct SiftWinCarry SiftWinCarry;

/* a column of this type of the table, held by a vector of this type, is the same Datum as the heap's */
extern bool sift_win_carry_type_ok(Oid typid, SiftVecType type);

extern SiftWinCarry *sift_win_carry_create(MemoryContext cx, const SiftWinCarrySpec *spec);
/* The carry of these Vars of the table when every one is a column of the input held as the heap holds it (the Vars join the columns the scan decodes), NULL when one is not */
extern SiftWinCarry *sift_win_carry_open(MemoryContext cx, SiftV6Input *in, const Var *const *vars, int nvars, uint64 max_bytes);
/* the rows of the batch that pass its mask, under their tags */
extern void sift_win_carry_put(SiftWinCarry *c, const SiftV6Batch *b);
/* The variables of the row of the tag (values[i], isnull[i] of variable i) and the row forgotten; false when the carry does not hold the row */
extern bool sift_win_carry_take(SiftWinCarry *c, uint64 tag, Datum *values, bool *isnull);
extern void sift_win_carry_free(SiftWinCarry *c);

#endif							/* SIFT_AGG_V6_WIN_CARRY_H */
