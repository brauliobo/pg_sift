/*
 * agg/par/star_shm.h - the dimensions of a star or snowflake join in one block of memory for every participant of the scan of the fact table: the shared
 * hash of each (agg/par/dim_shm.h), the columns they give back, and the join filter of their keys (agg/sip.h), with no pointer anywhere (offsets from the block),
 * so that it is valid at any address of any process, in a shared segment or in the memory of one backend.
 *
 * A dimension is described by how it joins (INNER, SEMI, LEFT, ANTI), the dimension its rows point to when it is a snowflake (parent: built before it) and
 * whether the scan of the fact table joins it directly. The dimensions are given leaves first: a dimension comes after its parent. Each is built in three steps,
 * the participants all in each, between barriers:
 *   pass     the rows of the dimension in blocks, in parallel: a row stays when its key to the parent is found in the parent (the filter of the parent first, then
 *            its table, exactly), or is not (ANTI) or always (LEFT); the payload of a parent with a unique key (N:1) is copied into the row (flattened, so the scan of
 *            the fact table never probes the parent);
 *   compact  one participant, the rows that stayed one after another: the empty inner or semi dimension ends the build there, and no other dimension reads a row;
 *   build    the table and the filter of the rows that stayed, in one pass of the rows (sift_dim_shm_build_with_sip). A semi dimension with no payload and few keys has
 *            the filter alone (exact, no false positive), and no table.
 * So the filter of a dimension that points to a parent holds the keys of the rows that stay after the parent: a filter on the parent prunes the fact table through
 * the dimension that no predicate of the query names. A participant that joins late does the steps that are left, like dim_shm's.
 *
 * The payload columns are int8 (an integer, a date, a dictionary code, a bit pattern); a column of a LEFT parent, or of a LEFT dimension, has a bit of validity.
 */
#ifndef SIFT_AGG_PAR_STAR_SHM_H
#define SIFT_AGG_PAR_STAR_SHM_H

#include "agg/par/dim_shm.h"
#include "agg/sip.h"
#include "agg/vec.h"

#define SIFT_STAR_MAGIC			0x53545236U
#define SIFT_STAR_MAX_DIMS		16
#define SIFT_STAR_MAX_PAY_COLS	16

typedef enum SiftDimJoinKind
{
	SDIM_INNER = 0,
	SDIM_SEMI = 1,
	SDIM_LEFT = 2,
	SDIM_ANTI = 3
} SiftDimJoinKind;

/* What the planner knows of a dimension before its rows are read */
typedef struct SiftStarDimSpec
{
	SiftDimJoinKind kind;
	int8		parent;			/* the dimension its rows point to, -1 if none: an index before this one */
	bool		fact_joined;	/* the scan of the fact table joins it */
	bool		is_unique_key;	/* a key is in one row only: N:1 */
	int16		fact_key_col;	/* the column of the fact table that is its key, for the caller */
	uint32		raw_rows;
	uint8		n_pay_cols;		/* its own columns */
} SiftStarDimSpec;

/* A column of the payload: offsets from the payload */
typedef struct SiftDimPayColMeta
{
	SiftVecType type;
	bool		nullable;
	Size		data_off;		/* int64[nrows] */
	Size		valid_off;		/* the bit of a row: 1 = not NULL, uint64[ceil(nrows / 64)]; 0 when no row is NULL */
} SiftDimPayColMeta;

typedef struct SiftDimPayloadShm
{
	uint32		nrows;
	uint16		n_cols;
	SiftDimPayColMeta cols[SIFT_STAR_MAX_PAY_COLS];
} SiftDimPayloadShm;

typedef struct SiftStarDimSlot
{
	SiftDimJoinKind join_kind;
	int8		parent_dim_id;
	bool		fact_joined;
	bool		is_unique_key;
	bool		flattened_into_child;	/* its payload is in the rows of a dimension that points to it */
	bool		push_sip;		/* the scan of the fact table asks its filter: fact_joined and INNER or SEMI */
	int16		fact_key_col;
	uint8		n_own_cols;
	bool		elided;			/* a semi dimension of a few keys and no payload: the filter, exact, is the join, and there is no table */
	Size		dim_shm_off;	/* the room of the table */
	Size		payload_off;
	Size		sip_off;
	Size		keys_off;		/* int64[raw_rows]: the keys, then the keys of the rows that stayed */
	Size		pass_off;		/* uint8[raw_rows] */
	Size		queue_off;		/* the morsel queue of the pass */
	uint32		raw_rows;
	uint32		surviving_rows;
} SiftStarDimSlot;

typedef struct SiftStarDimBundle
{
	uint32		magic;
	uint16		n_dims;
	uint16		n_participants;
	Size		total_bytes;
	pg_atomic_uint32 any_empty_inner pg_attribute_aligned(PG_CACHE_LINE_SIZE);	/* an inner or semi dimension has no row: the query has none */
	pg_atomic_uint32 next_slot pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	Barrier		barrier pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	SiftStarDimSlot dims[SIFT_STAR_MAX_DIMS];
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftStarDimBundle;

/* The rows of a dimension for the pass: the blocks [first, first + n) of its rows, the key, the key to the parent (NULL when it has none) and the own columns (n_pay_cols arrays of n) */
typedef struct SiftStarSource
{
	void		(*rows) (void *arg, uint32 first, uint32 n, int64 *key, int64 *parent_key, int64 **pay);
	void	   *arg;
} SiftStarSource;

/* The bytes to give sift_star_init for the dimensions (n <= SIFT_STAR_MAX_DIMS, a parent before its children), whatever the alignment of what it gets; and whether they are within the half of work_mem * hash_mem_multiplier a shared bundle may take */
extern Size sift_star_size(const SiftStarDimSpec *dims, int n, uint32 n_participants);
extern bool sift_star_fits(const SiftStarDimSpec *dims, int n, uint32 n_participants);
extern SiftStarDimBundle *sift_star_init(void *raw, const SiftStarDimSpec *dims, int n, uint32 n_participants);
/* Every participant calls it once, with the sources of the dimensions in the order of the specs, at its own addresses: it returns when every dimension is built; the bundle is read-only after */
extern void sift_star_build(SiftStarDimBundle *b, const SiftStarSource *src);

static inline bool
sift_star_empty(SiftStarDimBundle *b)
{
	return pg_atomic_read_u32(&b->any_empty_inner) != 0;
}

static inline SiftSipFilter *
sift_star_sip(SiftStarDimBundle *b, int d)
{
	return (SiftSipFilter *) ((char *) b + b->dims[d].sip_off);
}

/* The table of a dimension, NULL when the filter is the join */
static inline SiftDimShm *
sift_star_table(SiftStarDimBundle *b, int d)
{
	return b->dims[d].elided ? NULL : (SiftDimShm *) TYPEALIGN(PG_CACHE_LINE_SIZE, (char *) b + b->dims[d].dim_shm_off);
}

static inline const SiftDimPayloadShm *
sift_star_payload(const SiftStarDimBundle *b, int d)
{
	return (const SiftDimPayloadShm *) ((const char *) b + b->dims[d].payload_off);
}

/* The value of column c of the row of a dimension, and false when it is NULL */
static inline bool
sift_star_pay(const SiftStarDimBundle *b, int d, int c, uint32 row, int64 *v)
{
	const SiftDimPayloadShm *p = sift_star_payload(b, d);
	const SiftDimPayColMeta *m = &p->cols[c];

	*v = ((const int64 *) ((const char *) p + m->data_off))[row];
	return m->valid_off == 0 || ((((const uint64 *) ((const char *) p + m->valid_off))[row >> 6] >> (row & 63)) & 1);
}

#endif							/* SIFT_AGG_PAR_STAR_SHM_H */
