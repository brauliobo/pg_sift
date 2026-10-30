/*
 * agg/group_int.h - the group keys and the group table (agg/group/)
 */
#ifndef SIFT_AGG_GROUP_INT_H
#define SIFT_AGG_GROUP_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/accum_int.h"
#include "agg/leaf_int.h"
#include "agg/zoneoff.h"

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
	ZoneOff    *zone;			/* a bucket key of a timestamptz leaf read on the session's clock: the offsets of the zone the scan started in; NULL: the buckets are of UTC */
	bool		zcut;			/* zone: the bucket is the instant a date_trunc cuts to (SIFT_ZONE_CUT), whose first row is the one the key is evaluated at */
	int64		witness;		/* zcut: the instant of the row whose bucket is being given an id */
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

/* 32 bits, so that compute_gids hashes a chunk key by key (hash_cols) with the same result */
#define IDS_HASH_MUL 0x9E3779B1U

static inline uint32
ids_hash32(const uint32 *ids, int nk)
{
	uint32		h = 0;

	for (int j = 0; j < nk; j++)
	{
		h = (h ^ ids[j]) * IDS_HASH_MUL;
		h ^= h >> 16;
	}
	return h;
}

static inline uint32
ids_hash(const uint32 *ids, int nk, uint32 nslot)
{
	return ids_hash32(ids, nk) & (nslot - 1);
}

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* group/ */
AggAcc *acc_alloc(AggExec *ex, uint32 cap);
void groups_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);
void direct_layout(AggExec *ex);
void hash_init(AggExec *ex, uint32 groups);
void direct_grow(AggExec *ex);
bool direct_oversized(AggExec *ex, uint64 total);
uint32 ident_add(AggExec *ex, KeyState *k, int64 v);
uint32 ident_wide_id(AggExec *ex, KeyState *k, int64 v);
void ident_go_wide(AggExec *ex, KeyState *k);
void key_range_fill(void *arg, int32 v, void *entry);
bool key_cover(AggExec *ex, int ki, bool any, int32 mn, int32 mx, int nsel);
bool step_numbers(const SiftSrcVec *v, int start, int n, const uint32 *iota, ZoneOff *z, int64 us, uint32 *out, int32 *mn, int32 *mx);
void compute_kids(AggExec *ex, int nsel, uint32 *kid);
void compute_gids(AggExec *ex, int nsel);
void compute_set_gids(AggExec *ex, int nsel, const uint32 *kid);
uint32 ids_gid(AggExec *ex, const uint32 *ids);

#pragma GCC visibility pop

#endif
