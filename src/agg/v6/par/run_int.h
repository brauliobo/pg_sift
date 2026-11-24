/*
 * agg/v6/par/run_int.h - what agg/v6/par/run.c shares with the operators it runs (run_group.c, run_distinct.c, run_rows.c): the shape of a statement, the rows of a granule that a thread decodes, and
 * the table of the kinds.
 */
#ifndef SIFT_AGG_V6_PAR_RUN_INT_H
#define SIFT_AGG_V6_PAR_RUN_INT_H

#include "postgres.h"

#include "agg/gpu_v6.h"
#include "agg/v6/par/derive.h"
#include "agg/v6/par/distinct.h"
#include "agg/v6/par/group.h"
#include "agg/v6/par/rows.h"
#include "agg/v6/par/run.h"
#include "core/par/pin.h"

#define PAR_SIGN_FLIP	(UINT64CONST(1) << 63)
#define PAR_LEAVES_MAX	(SIFT_PAR_GROUP_MAX_KEYS * (1 + SIFT_V6_MAX_CASE) + SIFT_PAR_GROUP_MAX_SUM + SIFT_V6_MAX_KEYS)

struct ParKind;

/* The leaves a thread decodes of every granule, and what the operator of the statement makes of them */
typedef struct ParShape
{
	const struct ParKind *kind;
	int			nleaves;
	int			leaf[PAR_LEAVES_MAX];
	bool		codes[PAR_LEAVES_MAX];	/* the leaf is a text read as the codes of the dictionary of the granule when it has one */
	int			nk;
	ParSrc		key_src[SIFT_PAR_GROUP_MAX_KEYS];
	int			nsum;
	ParSrc		sum_src[SIFT_PAR_GROUP_MAX_SUM];
	SiftParGroupSpec gs;
	SiftParDistinctSpec ds;		/* distinct: the keys (words, then the text), the sums and the counted value */
	int			val_at;			/* distinct: the leaf of the counted value */
	int			mm_at[SIFT_PAR_DISTINCT_MM_MAX];	/* distinct: the leaf of each text minimum or maximum */
	bool		ktext[SIFT_PAR_GROUP_MAX_KEYS];	/* the key is a text, else an integer */
	int			nchk;
	int			chk[PAR_LEAVES_MAX];	/* the leaves in which a NULL is not for a thread: the keys, and the columns of the sums and counts (distinct) */
	SiftParRowsSpec rs;			/* rows */
	int			rkey_at[SIFT_PAR_ROWS_MAX_KEYS];
} ParShape;

/* The memory of a thread: what it needs at least (so that the statement can start), and what it would like */
typedef struct ParSizes
{
	size_t		scratch;
	size_t		work;
	size_t		state_min;
	size_t		state_want;
} ParSizes;

struct SiftV6ParScan
{
	const SiftV6Spec *spec;
	SiftV6Group *g;
	SiftV6Rows *rows;
	SiftV6ScanStats *stats;
	Relation	index;
	SiftPinWin *win;
	ParShape	sh;
	void	   *op;				/* the operator of the kind, made when the threads start */
	bool		finishing;
	uint32		next_job;		/* the first granule of the window the ring has not been fed yet */
	uint32	   *job_of;			/* the granule of the window that each item of the ring holds */
	uint32		npending;
	uint32	   *pending;		/* the granules a thread left to the leader */
	struct
	{
		uint64		rows;
		char		pad[56];
	}			w[SIFT_PAR_MAX_THREADS];
};

typedef struct ParKind
{
	/* Whether the statement is of this kind: fills the shape and the leaves (first: the leaf of each attribute; NULL when only the shape is asked for) */
	bool		(*shape) (const SiftV6Spec *sp, const int *first, ParShape *sh);
	void		(*sizes) (const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz);
	/* The operator, in the arenas; NULL with err set when it does not fit them. The leaves are read through the hooks that take p */
	void	   *(*create) (SiftV6ParScan *p, SiftParArenas *ar, uint64 rows, SiftErr *err);
	const SiftParOps *ops;
	/* The leader, once the window exists (NULL: nothing to set up): what the window tracks of the granules the kind needs */
	void		(*setup) (SiftV6ParScan *p);
	/* The leader, before a granule of the window is fed (NULL: none is left out): whether no row of it can be in the answer */
	bool		(*skip) (SiftV6ParScan *p, uint32 job);
	/* The leader, after a run that every row went through: the answer into the groups or rows of the statement (NULL: the operator's finish did it) */
	void		(*done) (SiftV6ParScan *p);
	/* The operator hands what it folded to the leader when memory runs out (the rest of the items go to the scan); else the whole window is replayed */
	bool		drains;
	/* What the threads find joins what the scan found exactly (the K best rows of both are the K best of all): rows the scan read itself are no reason to replay the granules of the window */
	bool		merges;
} ParKind;

extern const ParKind par_kind_group;
extern const ParKind par_kind_distinct;
extern const ParKind par_kind_rows;

extern int	par_add_leaf(ParShape *sh, int leaf, bool codes);
static inline int
par_leaf_of(const int *first, const SiftV6Col *c)
{
	return first != NULL ? first[c->attno] : 0;
}

/* A column that reads as itself: no derivation, no JSON key, no scale */
extern bool par_plain_col(const SiftV6Col *c);

/* The aggregate that counts or sums: COUNT(*), COUNT(column), SUM and AVG of an int2 or int4 column or of length() of a text column (the sums of sh, and the leaves it reads, which cannot hold a NULL for a thread); false for any other */
extern bool par_count_sum_of(const SiftV6Agg *a, const SiftV6Spec *sp, const int *first, ParShape *sh);

/* Whether the column c is a key or an argument the threads read (a plain column, or one the spec computes by a derive of a plain column and plain conditions), with its leaves added to the shape, and to the ones in which a NULL is not for a thread */
extern bool par_src_of(ParShape *sh, const SiftV6Spec *sp, const int *first, const SiftV6Col *c, ParSrc *s);

/* The keys of the statement: integer or text columns (ktext tells which) the threads read, with their sources in key_src */
extern bool par_keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh);

/* The rows of a granule that a thread folds, and their leaves decoded */
typedef struct ParLoad
{
	uint32		n;				/* the rows of the granule */
	uint32		nact;			/* the rows that pass */
	uint64	   *active;			/* the bitmap of the rows that pass */
	ColView		cv[PAR_LEAVES_MAX];
} ParLoad;

/* false, with err filled, when the granule is not for this thread (SIFT_ERR_ARENA_FULL, SIFT_ERR_SERIAL) or cannot be decoded; ld->nact is 0 when no row passes (no leaf is decoded) */
extern bool par_load(const ParShape *sh, const SiftPinSlot *sl, SiftArena *scratch, SiftErr *err, ParLoad *ld);
extern bool par_arena_full(SiftArena *a, SiftErr *err);
extern bool par_serial(SiftErr *err, const char *why);

static inline SiftStrRef
par_str_at(const ColView *cv, uint32 r)
{
	if (cv->dcode != NULL)
		return (SiftStrRef) {(const uint8 *) cv->dict->bytes + cv->dict->off[cv->dcode[r]], cv->dict->off[cv->dcode[r] + 1] - cv->dict->off[cv->dcode[r]]};
	return (SiftStrRef) {(const uint8 *) cv->s[r], cv->slen[r]};
}

/* The rows of the bitmap in order: r is the row of the granule, i its place among the rows that pass */
#define PAR_EACH_ROW(ld, r, i) \
	for (uint32 i pg_attribute_unused() = 0, w_ = 0; w_ < ((ld)->n + 63) / 64; w_++) \
		for (uint64 bits_ = (ld)->active[w_]; bits_ != 0; bits_ &= bits_ - 1, i++) \
			for (uint32 r = w_ * 64 + __builtin_ctzll(bits_), once_ = 1; once_; once_ = 0)

/* Whether a row that passes has a NULL in the column */
static inline bool
par_has_null(const ParLoad *ld, const ColView *cv)
{
	uint8		any = 0;

	PAR_EACH_ROW(ld, r, i)
		any |= cv->isnull[r];
	return any != 0;
}

#endif							/* SIFT_AGG_V6_PAR_RUN_INT_H */
