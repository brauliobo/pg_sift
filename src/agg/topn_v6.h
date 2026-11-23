/*
 * agg/topn_v6.h - the Top-N tournament: ORDER BY k0 [, k1 ..] LIMIT n OFFSET m over parts of granules whose zones (the range of each key, whether it holds NULLs)
 * are known before a granule is read. The units that may hold a better row are scheduled best first: a priority queue of partitions, parts and granules, each
 * with the best value its zone allows for the leading key. The best n + m rows so far are a heap whose root is the worst of them, theta; when the next unit
 * cannot beat theta, no unit left can on the leading key and the tournament ends without reading them. A unit is expanded only when it is popped: a
 * partition is opened into parts, a part into granules, and a part sorted by the leading key into one frontier granule at a time (the next is queued when
 * the frontier is read), so that a partition that never reaches the top is never opened.
 *
 * The rows are those of the keys as int64 whose order is the order of the key (sift_f64_order_i64 for a float8; a scaled numeric, a date, a timestamp as they
 * are), a NULL a flag. ASC and DESC, NULLS FIRST and NULLS LAST are per key, up to SIFT_TOURN_MAX_KEYS keys. Rows are compared on all the keys, then by the tid
 * the source gives them, so the result does not depend on the order the granules were read in. A row that ties theta on every key may be left out (SQL
 * leaves the choice among ties to the executor); WITH TIES keeps every row that ties the last of the n + m rows. The rows are not decoded here: a winner is
 * (part, granule, row in the granule), and the caller reads the rest of its columns for the rows it outputs.
 *
 * Parallel: each participant runs a tournament of its own over the units it was given and the queue (core/par/morsel.h) carries the bound of the best-placed one
 * in the bound of the queue (topn_theta, core/par/topk.h) as an integer that only grows; a unit whose best value on the leading key is below it is not read by any of them, and the winners of all are
 * merged (sift_tourn_merge).
 */
#ifndef SIFT_AGG_TOPN_V6_H
#define SIFT_AGG_TOPN_V6_H

#include <math.h>

#include "core/par/morsel.h"

#define SIFT_TOURN_MAX_KEYS		4
#define SIFT_TOURN_MAX_K		100000

typedef enum SiftTournKind
{
	STOK_L0_SLAB,				/* rows not yet in a part: read first, they tighten theta before a part is opened */
	STOK_PARTITION,				/* expands into parts */
	STOK_PART,					/* expands into granules, or into the frontier granule of a part sorted by the leading key */
	STOK_GRANULE,				/* read */
	STOK_KEY_FRONTIER			/* a granule of a part sorted by the leading key: queues the next one, then is read */
} SiftTournKind;

typedef struct SiftTournKey
{
	bool		desc;
	bool		nulls_first;
} SiftTournKey;

/* What a unit can hold of a key: the range of its values that are not NULL, and whether it has NULL rows; known = false: nothing is known of the unit */
typedef struct SiftTournBound
{
	bool		known;
	bool		has_null;
	bool		all_null;
	int64		min;
	int64		max;
} SiftTournBound;

typedef struct SiftTournSpec
{
	uint8		nkeys;
	SiftTournKey keys[SIFT_TOURN_MAX_KEYS];
	uint64		offset;
	uint64		limit;
	bool		with_ties;
} SiftTournSpec;

/* A unit of the queue: the best leading key its zone allows (best_val, in prio 1; prio 2 = NULL first is possible, 0 = only NULLs and they come last) and of the second key */
typedef struct SiftTopNSchedTok
{
	int64		best_val;
	int64		best_tie;
	uint32		part_slot;
	uint32		local_g;
	uint32		part_idx;
	uint8		kind;
	uint8		null_prio;
	uint8		tie_prio;		/* like null_prio for the second key; SIFT_TOURN_NO_TIE: no bound */
	uint8		pad;
} SiftTopNSchedTok;

#define SIFT_TOURN_NO_TIE	3

StaticAssertDecl(sizeof(SiftTopNSchedTok) == 32, "SiftTopNSchedTok is 32 bytes");

typedef struct SiftTopNWinRow
{
	int64		key[SIFT_TOURN_MAX_KEYS];
	uint64		tid;
	uint32		part_slot;
	uint16		local_g;
	uint16		row_ord;
	uint8		nulls;			/* bit k: key k is NULL */
	uint8		pad[7];
} SiftTopNWinRow;

typedef struct SiftTournament SiftTournament;

typedef struct SiftTournOps
{
	/* a STOK_PARTITION or STOK_PART popped: push its parts or granules (sift_tourn_push); the frontier granule alone for a part sorted by the leading key */
	void		(*expand) (void *src, SiftTournament *t, const SiftTopNSchedTok *tok);
	/* a STOK_KEY_FRONTIER popped: push the granule after it; the granule itself is read after this returns */
	void		(*frontier_next) (void *src, SiftTournament *t, const SiftTopNSchedTok *tok);
	/* a STOK_L0_SLAB or STOK_GRANULE popped: offer the rows that pass the filter (sift_tourn_offer), skipping those that cannot beat theta (sift_tourn_theta) */
	void		(*scan) (void *src, SiftTournament *t, const SiftTopNSchedTok *tok);
} SiftTournOps;

/* queue: the parallel participants' shared bound, NULL when serial; max_bytes bounds the rows kept beyond n + m for WITH TIES */
extern SiftTournament *sift_tourn_create(MemoryContext cxt, const SiftTournSpec *spec, uint64 max_bytes, SiftMorselQueue *queue);
extern void sift_tourn_free(SiftTournament *t);

/* Queues a unit with the bounds of its leading key and, when known, of the second (NULL: unknown); a unit that cannot beat theta is counted and dropped */
extern void sift_tourn_push(SiftTournament *t, SiftTournKind kind, uint32 part_slot, uint32 local_g, uint32 part_idx, const SiftTournBound *k0, const SiftTournBound *k1);

/* Offers a row; false when it cannot be among the best rows (the caller may skip decoding what is left of it) */
extern bool sift_tourn_offer(SiftTournament *t, const SiftTopNWinRow *row);

/* Whether n + m rows are held, and then the leading key of the worst: a row whose leading key is worse than it (in the order of the key, NULLs by NULLS FIRST or LAST) cannot win */
extern bool sift_tourn_theta(const SiftTournament *t, int64 *key0, bool *null0);

/* One unit popped and handled; false when the queue is empty or no unit left can beat theta */
extern bool sift_tourn_step(SiftTournament *t, const SiftTournOps *ops, void *src);
extern void sift_tourn_run(SiftTournament *t, const SiftTournOps *ops, void *src);

/* The winners of src are offered to dst (the participants of a parallel scan, in any order) */
extern void sift_tourn_merge(SiftTournament *dst, const SiftTournament *src);

/* The rows to output, in ORDER BY order, past the offset: at most limit of them, and with WITH TIES every row that ties the last (valid until the next call) */
extern const SiftTopNWinRow *sift_tourn_result(SiftTournament *t, uint32 *n);

typedef struct SiftTournStats
{
	uint64		partitions_pruned;
	uint64		parts_pruned;
	uint64		granules_pruned;
	uint64		granules_scanned;
	uint64		strides_skipped;	/* kept by the source */
	bool		early_terminated;
} SiftTournStats;

extern const SiftTournStats *sift_tourn_stats(const SiftTournament *t);
extern void sift_tourn_add_strides_skipped(SiftTournament *t, uint64 n);

/* float8 to int64 in the order of PostgreSQL: -Infinity < numbers < +Infinity < NaN, and -0 = 0 */
static inline int64
sift_f64_order_i64(double d)
{
	int64		s;

	if (isnan(d))
		return INT64_MAX;
	if (d == 0.0)
		d = 0.0;
	memcpy(&s, &d, sizeof(s));
	return s < 0 ? s ^ INT64_MAX : s;
}

#endif							/* SIFT_AGG_TOPN_V6_H */
