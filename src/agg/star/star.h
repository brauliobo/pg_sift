/*
 * agg/star/star.h - the small relations a scan of SiftAggV6 joins to its fact rows before it groups them: each (a dimension) is read whole into a table of columns and a hash index, then the
 * 2048-row batches of the fact table probe it. An inner, a left, a semi and an anti join on one to four equalities of integer, date, timestamp or text columns, and the join of a range of the
 * fact (recorded_on BETWEEN lo AND hi) with the rows of a table of at most SIFT_STAR_RANGE_MAX periods, which one fact row may match more than once.
 *
 * A key whose dimension rows are all different (unique_keys) is answered by one lookup a row and a gather of the payload columns into the vectors of the batch; any other join is expanded to
 * the pairs of (fact row, dimension row), at most a batch of them at a time, which the cursor resumes. A key with a NULL is never equal to another: the rows of the dimension that have one are
 * not in the index, a fact row that has one finds no match. The dimension also tells what it proves of the fact scan (sift_star_push): the exact keys of a short list, the range of a longer one.
 *
 * Portable: only core/host.h and agg/vec.h. The reading of a PlanState into a dimension, the planner and the executor node are in v6_star_plan.c and exec.c.
 */
#ifndef SIFT_AGG_V6_STAR_H
#define SIFT_AGG_V6_STAR_H

#include "core/host.h"

#include "agg/vec.h"

#define SIFT_STAR_MAX_DIMS		4
#define SIFT_STAR_MAX_KEYS		4
#define SIFT_STAR_MAX_PAY		16
#define SIFT_STAR_RANGE_MAX		256
#define SIFT_STAR_EXACT_MAX		1024	/* the keys a dimension tells the scan as a list; above it only the range of them and a filter do */
#define SIFT_STAR_IN_MAX		64		/* ... and as the values of an IN, the ranges the marks of a part can be asked */

typedef enum SiftV6JoinMode
{
	SV6J_INNER_EQUI = 1,
	SV6J_LEFT_EQUI,
	SV6J_SEMI_EQUI,
	SV6J_ANTI_EQUI,
	SV6J_RANGE_SMALL,
	SV6J_RIGHT_EQUI,			/* the dimension is the preserved side: its rows no fact row matched come out once, with a NULL fact side */
	SV6J_FULL_EQUI				/* both sides are preserved */
} SiftV6JoinMode;

/* The rows of the dimension are kept whether or not a fact row matches them: they are marked as the scan matches them and the ones left come out when it ends (v6_join_lookup.h) */
static inline bool
sift_star_preserves_dim(SiftV6JoinMode mode)
{
	return mode == SV6J_RIGHT_EQUI || mode == SV6J_FULL_EQUI;
}

/* The fact rows that match nothing stay, with a NULL dimension side */
static inline bool
sift_star_keeps_fact(SiftV6JoinMode mode)
{
	return mode == SV6J_LEFT_EQUI || mode == SV6J_FULL_EQUI;
}

typedef enum SiftStarCls
{
	SSC_INT,					/* int2, int4, int8, date, timestamp, timestamptz, bool: the int64 of the value */
	SSC_F64,					/* the bits of a float8 */
	SSC_STR						/* a string of bytes */
} SiftStarCls;

/* One row for the dimension, as the caller read it: keys and payload by column; a string is a pointer and a length (it is copied) */
typedef struct SiftStarRow
{
	int64		key_i[SIFT_STAR_MAX_KEYS];
	const char *key_p[SIFT_STAR_MAX_KEYS];
	uint32		key_len[SIFT_STAR_MAX_KEYS];
	bool		key_null[SIFT_STAR_MAX_KEYS];
	int64		pay_i[SIFT_STAR_MAX_PAY];	/* an int64, or the bits of a float8 */
	const char *pay_p[SIFT_STAR_MAX_PAY];
	uint32		pay_len[SIFT_STAR_MAX_PAY];
	bool		pay_null[SIFT_STAR_MAX_PAY];
	int64		lo,
				hi;				/* SV6J_RANGE_SMALL: the bounds of the period */
	bool		lo_null,
				hi_null;
} SiftStarRow;

typedef struct SiftStarShape
{
	SiftV6JoinMode mode;
	int			nkeys;
	SiftStarCls kcls[SIFT_STAR_MAX_KEYS];
	int			npay;
	SiftStarCls pcls[SIFT_STAR_MAX_PAY];
	bool		left;			/* SV6J_RANGE_SMALL: a left join (a fact row of no period is kept); the equi joins say it by their mode */
	bool		on;				/* a condition of the join runs on the pairs (v6_join_lookup.h): sift_star_expand gives the pairs of the keys, whether or not a fact row has one, and the caller keeps the fact rows no pair passes */
	bool		lo_inc;			/* SV6J_RANGE_SMALL: the bounds are in the period (BETWEEN) or not (< or >) */
	bool		hi_inc;
	uint64		budget;			/* the bytes the table may take: the work_mem a join may use; adding past it fails */
} SiftStarShape;

typedef struct SiftStarDim
{
	SiftStarShape shape;
	uint32		nrows;
	uint32		cap;
	uint64		bytes;
	bool		finished;
	bool		unique_keys;
	uint64	   *col[SIFT_STAR_MAX_KEYS + SIFT_STAR_MAX_PAY];	/* the int64 or float8 bits of a value, the offset of a string in arena */
	uint32	   *len[SIFT_STAR_MAX_KEYS + SIFT_STAR_MAX_PAY];
	uint8	   *null[SIFT_STAR_MAX_KEYS + SIFT_STAR_MAX_PAY];
	char	   *arena;
	uint64		arena_used;
	uint64		arena_cap;
	int64	   *lo;
	int64	   *hi;
	uint8	   *period_ok;
	uint32	   *rng_ord;		/* a range join, from sift_star_dim_finish: the periods that are not NULL by (lo, hi, row): their rows (0-based) */
	int64	   *rng_lo;			/* ... their bounds, dense, for the search */
	int64	   *rng_hi;
	int64	   *rng_max;		/* a segment tree over rng_hi (2 * rng_size entries): the largest hi of each span of periods */
	uint32		nrng;
	uint32		rng_size;		/* the leaves of the tree: nrng up to a power of 2 */
	bool		rng_disjoint;	/* no period reaches the lo of the next: a point is in at most one */
	int64		hull_min;
	int64		hull_max;
	uint32		ht_cap;
	uint32	   *head;			/* the first row of each key, 1-based; 0 empty */
	uint32	   *tail;
	uint32	   *next;			/* the next row of the same key, 0 at the last */
	uint32		ndistinct;
	uint64	   *matched;		/* a dimension of RIGHT or FULL join (sift_star_preserves_dim): bit i = row i (0-based) was matched by a fact row; allocated by sift_star_dim_finish */
	int64	   *exact;			/* the distinct values of the first key, sorted: while nrows <= SIFT_STAR_EXACT_MAX and it is an integer */
	uint32		nexact;
} SiftStarDim;

extern void sift_star_dim_init(SiftStarDim *d, const SiftStarShape *shape);
/* The bytes the table takes for nrows rows of str_bytes bytes of strings each, as sift_star_dim_add counts them: what the planner expects of a dimension */
extern double sift_star_table_est(const SiftStarShape *sh, double nrows, double str_bytes);
/* false when the row would take the table past its budget, or is the SIFT_STAR_RANGE_MAX + 1st period of a range join (nothing is added) */
extern bool sift_star_dim_add(SiftStarDim *d, const SiftStarRow *row);
/* the index over the rows added */
extern void sift_star_dim_finish(SiftStarDim *d);
extern void sift_star_dim_free(SiftStarDim *d);

/* The key columns of the fact batch the dimension is probed with (a range join: key[0] is the column the periods are of) */
typedef struct SiftStarProbe
{
	const SiftVec *key[SIFT_STAR_MAX_KEYS];
	uint16		n;
	const uint64 *active;
} SiftStarProbe;

/*
 * A dimension too big for its budget is joined in parts: the keys fall into nparts classes by hash, the dimension is read once for each class (its rows of the class only, a row with a NULL key
 * in none: sift_star_row_part gives it part 0 and no row of the part would match it) and the fact rows of the class are probed against it, so that every fact row is joined in exactly one pass.
 */
/* whether parts split the rows of the dimension: a range join or one with no key puts every row in part 0, so a table too big for its budget is read in chunks */
static inline bool
sift_star_parts_split(const SiftStarShape *sh)
{
	return sh->mode != SV6J_RANGE_SMALL && sh->nkeys > 0;
}
extern uint32 sift_star_row_part(const SiftStarShape *sh, const SiftStarRow *row, uint32 nparts);
/* whether the rows of the table fall in more than one of nparts parts: when not (one key, or NULL keys), more parts never make the part of the table smaller */
extern bool sift_star_dim_splits(const SiftStarDim *d, uint32 nparts);
extern uint32 sift_star_probe_part(const SiftStarShape *sh, const SiftStarProbe *p, uint32 r, uint32 nparts);

/* match[r] = the 1-based first dimension row of the key of fact row r (0: none) for the rows of active; returns the rows that matched. An equi join only */
extern uint32 sift_star_probe(const SiftStarDim *d, const SiftStarProbe *p, uint32 *match);
/*
 * mask &= the rows the join keeps (inner and semi: those with a match; anti: those without; left: all); gate, when not NULL, are the rows the join applies to: a row that is not in it has no match.
 * Returns the rows of mask left.
 */
extern uint16 sift_star_apply(const SiftStarDim *d, const uint32 *match, const uint64 *gate, uint16 n, uint64 *mask);
/* column pay of the dimension for each of n rows: dim_row[i] (1-based, 0: none, which is NULL); bits for an int or a float8, views for a string; valid_out is the rows that are not NULL */
extern void sift_star_gather_bits(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, uint64 *out, uint64 *valid_out);
extern void sift_star_gather_str(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, SiftStrView *out, uint64 *valid_out);
/* the same for a payload column that holds the bytes of datums (a type no native vector holds, passed by reference): out[i] is the address of the value, aligned, valid while the dimension lives */
extern void sift_star_gather_datum(const SiftStarDim *d, int pay, const uint32 *dim_row, uint16 n, uint64 *out, uint64 *valid_out);

/* The pairs of a join that can match a row more than once, resumed where the last call ended */
typedef struct SiftStarCursor
{
	uint32		row;
	uint32		chain;
	bool		in_row;
	bool		matched;
	uint32		nhit;			/* a range join: the periods (rows, ascending) that hold the point of the row, and how many were given */
	uint32		hit_at;
	uint32		hit[SIFT_STAR_RANGE_MAX];
} SiftStarCursor;

/* The rows (0-based, ascending) of the periods that hold x, up to cap of them: how many hold it, O(log periods + that many) */
extern uint32 sift_star_range_hits(const SiftStarDim *d, int64 x, uint32 *out, uint32 cap);

/* fact_idx[k] and dim_row[k] (0: none) of at most cap pairs; returns how many; a left join gives an unmatched row one pair with dim_row 0 */
extern uint32 sift_star_expand(const SiftStarDim *d, const SiftStarProbe *p, const uint64 *gate, SiftStarCursor *cur, uint16 *fact_idx, uint32 *dim_row, uint32 cap);
extern bool sift_star_cursor_done(const SiftStarCursor *cur, uint16 n);

/* What the dimension tells the scan of the fact table, from the keys it holds */
typedef enum SiftStarPushKind
{
	SSP_NONE,					/* nothing (an anti join, a left join, a first key that is not an integer) */
	SSP_EXACT,					/* the first key is one of keys[0 .. nkeys - 1] (sorted, at most SIFT_STAR_IN_MAX): an IN list the marks of a part answer exactly */
	SSP_RANGE					/* the first key is in [lo, hi]: the zones and marks of the parts and granules; a filter on the keys (agg/sip_scan.h) on top when there are many */
} SiftStarPushKind;

typedef struct SiftStarPush
{
	SiftStarPushKind kind;
	const int64 *keys;
	uint32		nkeys;
	int64		lo;
	int64		hi;
	bool		empty;			/* the join keeps no fact row: the scan need not run */
} SiftStarPush;

extern SiftStarPush sift_star_push(const SiftStarDim *d);

#endif							/* SIFT_AGG_V6_STAR_H */
