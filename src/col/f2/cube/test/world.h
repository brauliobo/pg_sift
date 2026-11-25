/*
 * col/f2/cube/test/world.h - a synthetic entity table for the tests of the scope summary (col/f2/cube/): parts of granules of rows sorted by
 * (member, time), the sources and the entity side the summary reads, and the brute-force answers it is compared with.
 */
#ifndef SIFT_COL_TEST_SCOPE_V6_WORLD_H
#define SIFT_COL_TEST_SCOPE_V6_WORLD_H

#include "postgres.h"

#ifdef SIFT_TESTING

#include "common/pg_prng.h"

#include "col/f2/cube/meta.h"

typedef struct TCheck
{
	pg_prng_state rng;
	int64		checks;
	int64		failures;
	StringInfoData out;
} TCheck;

#define TCHK(t, cond, ...) \
	do { \
		(t)->checks++; \
		if (!(cond)) \
		{ \
			if (++(t)->failures <= 5) \
				elog(WARNING, "scope summary check failed at line %d: %s", __LINE__, psprintf(__VA_ARGS__)); \
		} \
	} while (0)

#define TRAND(t, n)		((uint32) pg_prng_uint64_range(&(t)->rng, 0, (n) - 1))

#define TW_NSTR			6
#define T0_USEC			INT64CONST(86400000000) * 9132		/* 2025-01-01: days from 2000-01-01 */
#define DAY_USEC		INT64CONST(86400000000)

typedef struct TGran
{
	uint32		part;
	uint32		n;
	int64	   *member;
	uint8	   *mnull;
	int64	   *skey;			/* the hidden scope key of a clustered scope */
	int64	   *time;
	uint8	   *tnull;
	int64	   *dim0;			/* a part-local dictionary code */
	int64	   *dim1;
	int64	   *m0;
	uint8	   *m0null;
	int64	   *m1;				/* float8 bits */
	int64	   *x[3];			/* the order leaves of rings 1..3 */
	uint8	   *xnull[3];
	uint32	   *dset0;			/* the id of the declared distinct set of a rich world */
	uint8	   *dset0null;
	uint8	   *dead;
	uint32		ndead;
	uint8	   *soft;			/* the null_filter leaf (a soft-delete column) is not NULL: a scope with a null filter leaves the row out */
	bool		retired;		/* its part was compacted away: nothing refers to it any more */
	SiftScopeCubeAcc cube;
	SiftScopeMonthTable months;
	SiftTimeStride *str;
	uint32		nstr;
} TGran;

typedef struct TWorldSpec
{
	uint32		nmembers;
	uint32		maxrows;		/* rows of a member: 0..maxrows */
	uint32		long_pct;		/* members with a few granules of their own */
	uint32		granule_rows;
	uint32		nparts;
	uint32		months;
	bool		coded;			/* member ids scattered over a wide range */
	bool		big_float;		/* a few floats of 1e15 */
	uint32		nkeys;
	uint32		keys_per_member;	/* 0..keys_per_member keys a member */
	bool		clustered;		/* each member one key, rows ordered by key */
	bool		clustered_pp;	/* a member has its first key and often one or two more (multi_pct of them), rows ordered by the first key */
	bool		by_member;		/* clustered_pp members and keys, but the rows ordered by member: the layout without clustering */
	uint32		multi_pct;		/* the members with more keys, per mille; 0: 30 */
	uint32		nrings;			/* the extra order leaves, rings 1..nrings (at most 3) */
	bool		rich;			/* the cubes also hold two rollups, a distinct set and the minimum and maximum of each group */
	bool		null_filtered;	/* some members have soft-deleted rows (one in three of them: a few, or all) and the scope is built with a null filter on that leaf */
} TWorldSpec;

typedef struct TWorld
{
	TWorldSpec	spec;
	SiftScopeShape shape;
	uint32		ngran;
	TGran	   *g;
	uint32		nparts;
	uint32	   *part_base;		/* first granule of each part */
	uint32	   *part_n;
	uint64		part_gen[8];
	bool		part_retired[8];
	const uint32 *dmap[8];		/* canonical code of each local code, by part */
	const char *dstr[8][TW_NSTR];
	uint32		nmembers;
	int64	   *member_ids;
	SiftScopeMemberKey *pairs;
	uint32		npairs;
	SiftScopePairMap *cur_pm;	/* the pairs the object under test was built from */
	bool		filter;			/* a query filter: dim1 % 4 == 0 */
	uint64		strides_scanned;
	uint64		rows_decoded;
	uint64		dead_calls;		/* granules asked for their dead rows */
	uint64		dead_calls_by_part[8];
	uint64		part_calls;		/* parts asked whether they have dead rows */
	uint64		runs_calls;		/* granules asked for their entity runs */
	uint64		ring_scans;		/* granules decoded for an order leaf at query time */
	uint32		unnamed_from;	/* the granules from this one on are of parts no summary names (rows the entity side adds); 0: every granule is named */
	uint32		npart_gen;		/* parts so far, retired ones too: the part_gen of the next is npart_gen + 1 */
} TWorld;

#define TW_FILTER(w, g, r)	(!(w)->filter || ((g)->dim1[r] % 4) == 0)

extern int tw_newest_desc_cmp(const void *a, const void *b);
extern void tw_gran_rows_of(const TGran *g, SiftScopeRows *r);
extern void tw_gen(TWorld *w, TCheck *t, const TWorldSpec *spec);
extern void tw_pairs_gen(TWorld *w, TCheck *t);
extern SiftScopePairMap *tw_pm(const TWorld *w);
extern void tw_dict_maps(TWorld *w, SiftScopeBuild *b);
extern SiftScopeGranSource tw_source(TWorld *w);
extern SiftScopeEntity tw_entity(TWorld *w);
extern void tw_build(TWorld *w, SiftScopeBuildSpec *spec, SiftScopePairMap *pm, bool plain, StringInfo out, SiftScopeBuildStats *st);
extern void tw_spec(const TWorld *w, SiftScopeBuildSpec *spec, uint32 flags);
#define TW_NULL_FILTER_LEAF		7

/* The rows the scope holds: every row, or with a null filter those that are not soft-deleted */
#define TW_IN_SCOPE(w, g, r)	(!(w)->spec.null_filtered || !(g)->soft[r])

extern void tw_kill_rows(TWorld *w, TCheck *t, uint32 per_mille);
/* Kills the rows of one part only */
extern void tw_kill_part_rows(TWorld *w, TCheck *t, uint32 part, uint32 per_mille);
/* Compacts part p: its live rows are laid out again in granules of new_rows rows in a new part (the part_gen after the others) that follows the others; the old part is retired */
extern void tw_compact(TWorld *w, TCheck *t, uint32 part, uint32 new_rows, SiftScopePartSwap *swap, SiftScopeNewPart *added, uint64 *old_gen);

/* The cube of the rows of key (all rows when key is NULL) in month, live rows only when live is set; false when it has no row */
extern bool tw_truth_cube(const TWorld *w, SiftScopePairMap *pm, const int64 *key, int32 month, bool live, SiftScopeCubeAcc *out);
/* The newest n rows of a key that pass the filter and are live, newest first */
extern uint32 tw_truth_newest(const TWorld *w, SiftScopePairMap *pm, int64 key, uint32 n, SiftScopeNewest *out);
/* The same for the order leaf of ring 1..3: the rows that hold a value, largest first */
extern uint32 tw_truth_ring(const TWorld *w, SiftScopePairMap *pm, int64 key, uint32 ring, uint32 n, SiftScopeNewest *out);
/* Compares every key's all-time and monthly cubes and the key count with the brute force over pm; returns the keys that have rows */
extern uint32 tw_check_meta(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label);
/* Every entity of the live rows is in the route of m under exactly the keys its rows are stored under */
extern void tw_check_route(TCheck *t, const TWorld *w, const SiftScopeMeta *m, const char *label);
/* The distinct (entity, key) of the rows of the world that have an entity, ascending: what the route must hold */
extern uint32 tw_world_homes(const TWorld *w, SiftEntHome **out);
extern void tw_open(TCheck *t, const StringInfoData *obj, SiftScopeMeta *m);
extern void tw_free(TWorld *w);

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

static inline void
reseed(TCheck *t, uint64 base, uint32 round)
{
	pg_prng_seed(&t->rng, base + round * UINT64CONST(0x9E3779B97F4A7C15));
}

static inline int
newest_desc(const void *a, const void *b)
{
	const SiftScopeNewest *x = a,
			   *y = b;

	if (x->time != y->time)
		return x->time > y->time ? -1 : 1;
	if (x->gidx != y->gidx)
		return x->gidx > y->gidx ? -1 : 1;
	return x->row_ord > y->row_ord ? -1 : x->row_ord < y->row_ord;
}

#endif							/* SIFT_TESTING */
#endif							/* SIFT_COL_TEST_SCOPE_V6_WORLD_H */
