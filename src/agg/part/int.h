/*
 * agg/part/int.h - what the files of agg/part/ share
 * The partition source record, its slot macros, the provider table and the small accessors
 * the opening, the pruning, the tests and the provider callbacks use.
 */
#ifndef SIFT_AGG_PART_DIR_INT_H
#define SIFT_AGG_PART_DIR_INT_H

#include "agg/part.h"

#define PART_DICT_ID_BASE	UINT64CONST(0x6000000000000000)	/* no id of a provider has these bits: the engine's counters and pointers are below, its flags above */
/* the id the wrapper gave the dictionary of a slot (a leaf, a histogram or a rollup dimension) last, with the partition and the id it came from */
typedef struct PartId
{
	int			child;
	const void *in;
	const void *out;
} PartId;

typedef struct PartSource
{
	int			n;
	Relation	parent;
	Oid		   *heapoid;
	Relation   *heap;			/* NULL until a partition is opened or its bound read the first time; the pruned ones are not opened at all (the first's always is: the layout) */
	Relation   *index;			/* NULL until open_rest, as src, for a partition the scan reads; always NULL for a sift_table */
	SiftSrcSource **src;
	Oid		   *indexoid;
	Snapshot	snapshot;
	MemoryContext mcxt;
	bool		opened;			/* open_rest ran: the partitions can no longer be pruned */
	uint32	   *off;			/* n + 1: the first superblock of each partition, the total last */
	bool		have_off;
	bool	   *skip;			/* sift_aggpart_prune: the partitions the scan does not read (no superblocks), NULL: all are read */
	int			cur;			/* the partition of the unit the source is at */
	int			next;			/* the first partition begin has not started */
	uint32		lo,
				hi;				/* the superblocks of the begin */
	const SiftSrcPrune *prune;
	int			nprune;
	SiftSrcUnit cunit;			/* the unit as the partition returned it */
	int			nleaves;
	PartId	   *id;
	SiftSrcDict *dict;
	uint64		idseq;
} PartSource;

/* slots of dictionaries: the leaves, the dimensions of a histogram, the dimensions of a rollup */
#define SLOT_HIST(ps, leaf)		((ps)->nleaves + (leaf))
#define SLOT_ROLLUP(ps, j)		((ps)->nleaves + SIFT_COLSRC_MAX_LEAVES + (j))
#define NSLOTS(nleaves)			((nleaves) + SIFT_COLSRC_MAX_LEAVES + SIFT_SRC_MAX_RDIMS)

typedef struct QualVars
{
	Oid			part;
	Oid			parent;
	bool		ok;
} QualVars;

static inline PartSource *
ps_of(void *impl)
{
	return impl;
}

static inline SiftSrcSource *
cur_of(void *impl)
{
	PartSource *ps = impl;

	return ps->src[ps->cur];
}

static inline const SiftSrcUnit *
unit_of(void *impl)
{
	return &ps_of(impl)->cunit;
}

extern const SiftSrcProvider part_provider;

extern void part_close(void *impl);
extern void part_counters(const void *impl, SiftSrcCounters *c);
extern bool part_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool part_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows);
extern bool part_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern bool part_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);
extern bool part_gpu_chunk(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c);
extern bool part_gpu_dense(void *impl, const SiftSrcUnit *unit, int leaf);
extern void part_gpu_dict(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict);
extern bool part_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g);
extern bool part_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits);
extern uint32 part_nsb(void *impl);
extern bool part_overflow(void *impl, const SiftSrcUnit *unit, int leaf);
extern void part_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows);
extern void part_prune_str(void *impl, const SiftSrcStrTest *t, int nt);
extern void part_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
extern void part_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern uint64 * part_recheck_rows(void *impl, const SiftSrcUnit *unit);
extern bool part_sb_heap(void *impl);
extern uint64 part_set_filters(void *impl, const SiftSrcFilter *f, int nf);
extern void part_set_numsum(void *impl, int leaf);
extern void part_set_raw(void *impl, int leaf);
extern void part_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast);
extern int part_set_wide(void *impl, const SiftSrcWideTest *t, int nt);
extern const SiftSrcLayout * part_source_layout(const void *impl);
extern void part_tolerate_ovf(void *impl, int leaf);
extern bool part_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
extern int part_unit_dead(void *impl, const SiftSrcUnit *unit, int mode);
extern bool part_unit_exact(void *impl, const SiftSrcUnit *unit);
extern bool part_unit_except(void *impl, const SiftSrcUnit *unit, int leaf);
extern bool part_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h);
extern bool part_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r);
extern bool part_unit_rollup_miss(void *impl, const SiftSrcUnit *unit);
extern uint32 part_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list);

extern void open_part(PartSource *ps, int i);
extern void open_rest(PartSource *ps);
extern void count_sbs(PartSource *ps);

/* the values a condition leaves a column of the partitioned table (a NULL matches nothing: n can be 0) */
typedef struct KeyVals
{
	AttrNumber	attno;
	Oid			typid;
	Oid			opno;			/* the operator of the condition and its collation: a hash partitioning takes it only if it is the equality of its operator family */
	Oid			collid;
	int			n;
	Datum	   *val;
} KeyVals;
extern Node * qual_vars_mutator(Node *n, QualVars *qv);
extern KeyVals * key_vals(Node *cl);

extern int prune(PartSource *ps, Oid parent, List *conds, List *sets, bool none);

extern const SiftSrcDict * presented(PartSource *ps, int slot, const SiftSrcDict *d);

#endif							/* SIFT_AGG_PART_DIR_INT_H */
