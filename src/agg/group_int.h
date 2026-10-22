/*
 * agg/group_int.h - the group keys and the group table (agg/group.c)
 */
#ifndef SIFT_AGG_GROUP_INT_H
#define SIFT_AGG_GROUP_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/accum_int.h"
#include "agg/leaf_int.h"

typedef struct KeyState
{
	OutTable	ot;				/* distinct output values (ids 1..) */
	ExprState  *expr;
	Param	   *param;
	RangeTab	rt;				/* integer leaves: value -> id */
	bool		ident;			/* integer or float leaf as the key (and the grouping set number): ids are given to the values in order of appearance */
	int64	   *idval;			/* ident: the value of id i + 1 (a float: float_key) */
	uint32		negzero;		/* a float: the id of the zero group when the first zero seen was -0 (the one PG shows), else 0 */
	uint32		nident;
	uint32		capident;
	uint32		wcap;			/* ident, values too far apart for a table: open addressing value -> id (0 = free), power of 2 */
	int64	   *wval;
	uint32	   *wid;
	uint32		nwide;			/* the entries of the open addressing table */
	int64		bucket;			/* LUT keys on a timestamp leaf: the microseconds one evaluation covers (the table maps bucket numbers to ids); 0: not one */
	bool		cal;			/* LUT keys on a date leaf: the units answered from calendar cells map the first day of a cell (the table of wide values) to ids */
	uint32		nullid;			/* LUT keys on integer leaves: the id of expr(NULL) */
	uint32		cap;			/* direct mode: ids 0..cap-1 */
	uint32		mult;
	bool		numk;			/* SAK_NUM: ids of the program's values */
	IdMap		im;
} KeyState;

typedef struct KeyFill
{
	AggExec    *ex;
	int			ki;
} KeyFill;

static inline uint32
key_used(KeyState *k)
{
	return (k->numk ? k->im.n : k->ident ? k->nident : k->ot.n) + 1;
}

static inline uint32
ids_hash(const uint32 *ids, int nk, uint32 nslot)
{
	uint64		h = 0;

	for (int j = 0; j < nk; j++)
	{
		h = (h ^ ids[j]) * UINT64CONST(0x9E3779B97F4A7C15);
		h ^= h >> 32;
	}
	return (uint32) h & (nslot - 1);
}

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* group.c */
AggAcc *acc_alloc(AggExec *ex, uint32 cap);
void groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);
void direct_layout(AggExec *ex);
void hash_init(AggExec *ex);
void direct_grow(AggExec *ex);
uint32 ident_add(AggExec *ex, KeyState *k, int64 v);
uint32 ident_wide_id(AggExec *ex, KeyState *k, int64 v);
void ident_go_wide(AggExec *ex, KeyState *k);
void key_range_fill(void *arg, int32 v, void *entry);
bool key_cover(AggExec *ex, int ki, bool any, int32 mn, int32 mx, int nsel);
void compute_kids(AggExec *ex, int nsel, uint32 *kid);
void compute_gids(AggExec *ex, int nsel);

#pragma GCC visibility pop

#endif
