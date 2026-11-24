/*
 * agg/v6/group_int.h - the state of the groups of SiftAggV6 that group.c shares with the files of its operators (group_dist.c: the exact COUNT(DISTINCT), group_filter.c: FILTER (WHERE ..)).
 */
#ifndef SIFT_AGG_V6_GROUP_INT_H
#define SIFT_AGG_V6_GROUP_INT_H

#include "postgres.h"

#include "executor/executor.h"
#include "utils/tuplestore.h"

#include "agg/group/ht.h"
#include "agg/group/spill.h"
#include "agg/group/state.h"
#include "agg/v6/group_ext.h"
#include "agg/v6/v6.h"
#include "col/granule/granule.h"
#include "agg/v6/v6_acc.h"
#include "agg/v6/v6_dictseen.h"
#include "agg/v6/v6_distinct.h"
#include "agg/v6/v6_distinct_u64.h"
#include "agg/v6/v6_filter_eval.h"
#include "agg/v6/v6_filter_pivot.h"
#include "agg/v6/v6_group_fast.h"
#include "agg/v6/v6_group_lock.h"

/* The strings of one COUNT(DISTINCT) aggregate of a set of no keys that the rows named by the codes of their part's dictionary (group_dist.c) */
typedef struct V6DictSlot
{
	struct SiftV6Group *g;
	struct V6Set *set;
	uint32		slot;
	SiftDictSeen *seen;
	bool		mixed;			/* some of its strings went to the set of the group, not to seen: the count is that of the two together */
	bool		direct;			/* what seen holds is all the strings, and direct_n their number */
	uint64		direct_n;
} V6DictSlot;

typedef struct V6Set
{
	struct SiftV6Group *owner;
	uint32		mask;
	int			nk;
	int			kidx[SIFT_V6_MAX_KEYS];
	bool		wide;
	bool		packed;			/* the keys are fixed-width integers that fit one inline key (v6_group_fast.h) */
	SiftPackSpec pack;
	SiftSaltedHT ht;
	V6Acc	   *plain;
	bool		plain_row;		/* the set of no keys has a row to emit: a state of one partition of a parallel run has it only when records of the set were merged into it */
	uint32		emit_at;
	bool		emitted;
	SiftRadixShape shape;
	SiftGroupSpill *spill;
	SiftDistinct *dist;			/* the COUNT(DISTINCT) of the set: one slot for each such aggregate (group_dist.c) */
	V6DictSlot *dict;			/* a set of no keys: the strings of the dictionaries of each slot, made when a batch brings codes */
	SiftDistinctU64 **flat;		/* a set with no keys: the flat set of each integer COUNT(DISTINCT), NULL once its values moved to dist */
	bool	   *flat_moved;
	SiftDu64G **gflat;			/* a set of keys: the grouped flat set of each integer COUNT(DISTINCT), NULL once its pairs moved to dist */
	bool	   *gflat_moved;
	struct SiftCompact *cmp;	/* the compact group table the set has until something it cannot hold comes (v6_group_compact.h) */
	bool		cmp_off;		/* it has none, and will not have */
	SiftLockSet lock;			/* a set of keys of a statement with spec->lock_k: the first keys of the locked group table (v6_group_lock.h) */
} V6Set;

/* The rows r < n of the bitmap mk that are set, in order, a word at a time: r is declared by the caller */
static inline uint64
v6_mask_word(const uint64 *mk, uint32 w, uint32 n)
{
	const uint32 rest = n - (w << 6);

	return rest >= 64 ? mk[w] : mk[w] & ((UINT64CONST(1) << rest) - 1);
}

#define V6_FOR_EACH_ROW(mk, n, r) \
	for (uint32 w_ = 0; w_ < (((n) + 63) >> 6); w_++) \
		for (uint64 bits_ = v6_mask_word(mk, w_, n); bits_ != 0; bits_ &= bits_ - 1) \
			if (((r) = (w_ << 6) + (uint32) __builtin_ctzll(bits_)), true)

/* The key of the group of a row of the batch, as the bytes its COUNT(DISTINCT) pairs name it by */
typedef struct V6RowKey
{
	const void *p;
	uint32		len;
	uint64		nulls;
} V6RowKey;

/* The pairs of a batch of one COUNT(DISTINCT), for the grouped flat set (group_dist.c): the group number, the value and the position in the batch of each */
typedef struct V6DistBatch
{
	uint32		gid[SIFT_VEC_LEN];
	uint64		val[SIFT_VEC_LEN];
	uint16		row[SIFT_VEC_LEN];
} V6DistBatch;

struct SiftV6Group
{
	const SiftV6Spec *spec;
	MemoryContext cx;
	int			nsets;
	V6Set	   *set;
	int			cur;
	uint32		stride;
	uint32	   *off;
	uint32		dist_off;
	int			ndist;			/* COUNT(DISTINCT) aggregates */
	int		   *dagg;			/* the aggregate of each of them */
	int		   *dslot;			/* the slot of each aggregate in a distinct set (-1 for the others) */
	uint64		dist_budget;	/* of budget: what the distinct sets of all the sets of keys may hold; the groups have the rest */
	uint64		budget;
	TupleDesc	desc;
	Tuplestorestate *store;
	TupleTableSlot *mslot;
	bool		spilled;
	StringInfoData kbuf;
	SiftWideBatch wb;
	SiftStrSlab *slab;			/* the strings of the minimum and maximum of text columns */
	SiftDistinctKey dkey;
	SiftV6DistPut dist_put;		/* a participant of a parallel run: the rows of a COUNT(DISTINCT) go to the owner of their group instead of the set of this state */
	void	   *dist_arg;
	SiftDistinctKey pkey;		/* the bytes of the group of a row: its nulls and its key */
	int			nfilters;		/* FILTER (WHERE ..): the distinct clauses of the aggregates (group_filter.c) */
	SiftClause *clause;
	SiftFilterMasks *fmasks;	/* their bitmaps for the batch */
	const uint64 *frows;		/* bit f of frows[r]: row r passes clause f (a set of keys only) */
	uint16		sel[SIFT_VEC_LEN + SIFT_SEL_SLACK];
	uint64		hash[SIFT_VEC_LEN];
	uint32		gid[SIFT_VEC_LEN];	/* the group number of each row of a batch of the compact table (v6_group_compact.h), which v6g_dist_group_rows takes when gid_direct */
	bool		gid_direct;
	SiftInlineKey16 ikey[SIFT_VEC_LEN];
	SiftAggSlot64 *slots[SIFT_VEC_LEN];
	V6DistBatch *dbatch;
	SiftAggSlot64 **code_slots;	/* the group of each code of the dictionary of a batch (code_slot), valid when its code_epoch is code_epoch_cur */
	uint32	   *code_epoch;
	uint32		code_cap;
	uint32		code_epoch_cur;
	SiftTextArena tx;			/* the winners of MIN and MAX of text (group_ext.h) */
};

/* the Datums of the keys of a slot of a set of keys (only: just key number only, -1: all), and of an aggregate of its state */
/* The key columns of a slot into values and isnull, those whose bit of want is set (V6_KEYS_ALL: all of them); a wide key is read from its start each time */
#define V6_KEYS_ALL		UINT32_MAX
extern void v6g_key_datums(const SiftV6Group *g, const V6Set *set, const SiftSaltedHT *ht, const SiftAggSlot64 *s, Datum *values, bool *isnull, uint32 want);
/* The Datum of an integer key or value of a column of type typid */
extern Datum v6g_int_datum(Oid typid, int64 x);
extern Datum v6g_agg_datum(const SiftV6Group *g, const SiftV6Agg *a, const V6Acc *acc, uint32 mask, bool *isnull);

static inline bool
is_valid(const SiftVec *v, uint16 r)
{
	return v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);
}

static inline int64
int_at(const SiftVec *v, uint16 r)
{
	switch (v->type)
	{
		case SVEC_I32:
			return ((const int32 *) v->data)[r];
		case SVEC_BOOL:
			return ((const uint8 *) v->data)[r];
		default:
			return ((const int64 *) v->data)[r];
	}
}

/* group_ext.c: stddev, variance, bool_or, bool_and, and min, max of text */
extern void v6g_ext_fold_row(SiftV6Group *g, const SiftV6Agg *a, V6Acc *acc, const SiftVec *v, uint16 r);
extern void v6g_ext_plain(SiftV6Group *g, const SiftV6Agg *a, V6Acc *acc, const SiftVec *v, const uint64 *take, uint16 n_raw);
extern void v6g_ext_merge(SiftV6Group *g, const SiftV6Agg *a, V6Acc *d, const V6Acc *s);
extern Datum v6g_ext_datum(const SiftV6Group *g, const SiftV6Agg *a, const V6Acc *acc, bool *isnull);

static inline bool
v6g_is_ext(const SiftV6Agg *a)
{
	return a->kind >= SV6_STDDEV_POP || ((a->kind == SV6_MIN || a->kind == SV6_MAX) && sift_v6_class(a->arg.typid) == SV6C_TEXT);
}

/* How the states of aggregate a combine: the one operation of v6_acc.h for the merge of the spill of a serial run and the partitions of a parallel one */
static inline V6AccOp
v6g_acc_op(const SiftV6Agg *a)
{
	const bool	f64 = sift_v6_class(a->arg.typid) == SV6C_F64;

	switch (a->kind)
	{
		case SV6_COUNT_STAR:
		case SV6_COUNT:
			return V6A_COUNT;
		case SV6_COUNT_DISTINCT:
			return V6A_COUNT_PIECE;
		case SV6_SUM:
		case SV6_AVG:
			return f64 ? V6A_SUM_F64 : V6A_SUM_INT;
		case SV6_MIN:
			return f64 ? V6A_MIN_F64 : V6A_MIN_INT;
		case SV6_MAX:
			return f64 ? V6A_MAX_F64 : V6A_MAX_INT;
		case SV6_ARG_MAX:
			return V6A_ARG_MAX;
		case SV6_ARG_MIN:
			return V6A_ARG_MIN;
		case SV6_GROUPING:
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
		case SV6_PCT_CONT:
		case SV6_GEN_PG:
		case SV6_OSA:
			break;
	}
	return V6A_NONE;
}

/* the budget the tables of the groups may take: what the distinct sets do not */
extern void v6g_spill_if_over(SiftV6Group *g);
/* Every group table of the state to its spill, now */
extern void v6g_spill_all(SiftV6Group *g);

/* group_filter.c */
extern void v6g_filter_init(SiftV6Group *g);
/* The rows of the batch that pass each clause; the vectors of the clauses' columns are in argvec */
extern void v6g_filter_eval(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *argvec);
/* The rows aggregate a takes of the batch: its clause's rows, or the active rows; their number into *n */
extern const uint64 *v6g_filter_take(const SiftV6Group *g, const SiftV6Agg *a, const SiftBatchSel *bs, uint16 *n);
extern void v6g_filter_free(SiftV6Group *g);

/* does row r of a batch of a set of keys take part in aggregate a */
static inline bool
v6g_row_takes(const SiftV6Group *g, const SiftV6Agg *a, uint16 r)
{
	return a->fclause == 0 || ((g->frows[r] >> (a->fclause - 1)) & 1);
}

/* The hash a group of a set of keys has in the table of the set (the one its records are partitioned by) */
extern uint64 v6g_group_hash(const V6Set *set, const void *key, uint32 klen, uint64 nulls);

/* group_dist.c */
extern void v6g_dist_init(SiftV6Group *g);
/* The rows of take (a bitmap of n_raw rows) of the batch for the COUNT(DISTINCT) aggregate j of a set of no keys */
extern void v6g_dist_plain(SiftV6Group *g, V6Set *set, int j, uint16 n_raw, const uint64 *take, SiftVec *const *argvec);
/* The rows of a batch of a set of keys, whose groups are g->slots */
extern void v6g_dist_group_rows(SiftV6Group *g, V6Set *set, int j, SiftVec *const *argvec, uint16 n);
/* The pairs the grouped flat sets hold by group number go to the generic set, before the group table is emptied and its numbers mean other groups */
extern void v6g_dist_gflat_flush(SiftV6Group *g, V6Set *set);
/* A key of COUNT(DISTINCT) aggregate slot that a participant of a parallel run sent here: the bytes v6g_dist_group_row or v6g_dist_plain built */
extern void v6g_dist_add(SiftV6Group *g, V6Set *set, uint32 slot, const void *key, uint32 len);
/* The distinct sets of the state are emptied for the records of another partition */
extern void v6g_dist_recycle(SiftV6Group *g);
/* The number of distinct values of aggregate slot k in the set of no keys, final (the partitions that went to files are read back); the set is kept to be recycled */
extern uint64 v6g_dist_count(SiftV6Group *g, uint32 k);
/* Every string the dictionary path holds, as the key bytes of a row of its set (the key is valid for the call), each once; the path is empty afterwards */
typedef void (*V6DistKeyFn) (void *arg, uint32 set_idx, uint32 slot, const void *key, uint32 len);
extern void v6g_dist_dict_drain(SiftV6Group *g, V6DistKeyFn fn, void *arg);
/* The strings the dictionary path holds, at most: what a participant of a parallel run counts as its load */
extern uint64 v6g_dist_dict_load(const SiftV6Group *g);
/* A statement that is nothing but COUNT(DISTINCT) of columns of text: a part with no predicate has all the strings of its dictionaries, whichever its rows (scan.c) */
extern bool v6g_dist_dict_only(const SiftV6Group *g);
extern bool v6g_dist_dict_fits(const SiftDictSrc *src);
extern void v6g_dist_dict_part(SiftV6Group *g, int attno, const SiftDictSrc *src);
/* The sets resolve their partitions: the counts of the groups reach their accumulators */
extern void v6g_dist_finish(SiftV6Group *g);
extern void v6g_dist_free(SiftV6Group *g);

#endif							/* SIFT_AGG_V6_GROUP_INT_H */
