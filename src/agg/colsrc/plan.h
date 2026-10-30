/*
 * agg/colsrc/plan.h - the column source: the calls of the planner and the executor
 * The dispatch of the functions of the seam over the access method of a relation, and the
 * recheck modes; the last of the four parts of agg/colsrc.h.
 */
#ifndef SIFT_AGG_COLSRC_PLAN_H
#define SIFT_AGG_COLSRC_PLAN_H

#include "agg/colsrc/provider.h"

/* ---- planner side ---- */

/* Is the access method of an index (table) one a provider owns? Cheap; used by the planner on every rel */
extern bool sift_colsrc_is_index(Oid indexrelam);
extern bool sift_colsrc_is_table(Oid tablerelam);

/* The layout of an open index (table) relation; palloc'd in CurrentMemoryContext */
extern SiftSrcLayout *sift_colsrc_layout(Relation rel);
extern void sift_colsrc_stats(Relation rel, SiftSrcStats *st);
extern void sift_colsrc_stamp(Relation index, SiftSrcStamp *stamp);

/*
 * Opens the source of an index relation (heap == its table) or of a sift_table table (index == NULL). The snapshot is the
 * scan's. mcxt holds everything the source allocates until sift_colsrc_close.
 */
extern SiftSrcSource *sift_colsrc_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);

/* Can the sources of an index (or, with index false, a table) of this access method be read as the partitions of one table (src/agg/part.h)? sift_colsrc_open_part is sift_colsrc_open for one of them */
extern bool sift_colsrc_can_part(Oid relam, bool index);
extern SiftSrcSource *sift_colsrc_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
extern void sift_colsrc_close(SiftSrcSource *src);
extern void sift_colsrc_unpin(SiftSrcSource *src);
extern const SiftSrcLayout *sift_colsrc_source_layout(const SiftSrcSource *src);

/* Superblocks of the heap now: the morsels of a parallel scan are ranges of [0, nsb) */
extern uint32 sift_colsrc_nsb(SiftSrcSource *src);

/* Bit i of a row (or element) bitmap */
static inline bool
sift_colsrc_bit(const uint64 *bits, size_t i)
{
	return (bits[i >> 6] >> (i & 63)) & 1;
}

/* the only value a numeric(p <= 18, s) leaf cannot hold is NaN: the executor evaluates those rows as the numeric NaN they are */
static inline bool
sift_colsrc_nan_leaf(const SiftSrcLeaf *l)
{
	return l->typid == NUMERICOID && l->kind == SCL_SCALAR;
}

/*
 * A NaN of a numeric leaf is a value to the executor, the greatest of the leaf: PG_INT64_MAX (no scaled number reaches it), not NULL, and
 * its bit in vec->ovf for the code that evaluates it as PostgreSQL does. The engine stores it as NULL with the flag; a source calls this
 * after it filled data and isnull.
 */
/* The bytes of one value of each vector type, by SiftSrcVecType */
extern const size_t sift_src_vtype_size[];
extern void sift_colsrc_nan_values(const SiftSrcLeaf *l, void *data, uint8 *isnull, const SiftSrcVec *vec);

/* The leaf of leaf def i of a shredded jsonb attribute (a sift_col index, or the declared paths of a sift_table); desc names the attribute and gives its type */
struct SiftColDef;
extern void sift_colsrc_def_leaf(const struct SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l);
/* The numbers of SiftSrcStats of the leaves of cols from the zones and dictionaries; wovf[i], wjnull[i]: a write buffer holds an overflow value, a JSON null, of leaf i */
struct SiftColLeafStats;
extern void sift_colsrc_leaf_stats(const struct SiftColDef *cols, int ncols, const struct SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st);

/* Optional text tests of the provider (see SiftSrcProvider.prune_str / filter_str); no-ops that say "not done" without them */
static inline void
sift_colsrc_prune_str(SiftSrcSource *src, const SiftSrcStrTest *t, int nt)
{
	if (src->prov->prune_str)
		src->prov->prune_str(src->impl, t, nt);
}

static inline bool
sift_colsrc_filter_str(SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return src->prov->filter_str != NULL && src->prov->filter_str(src->impl, unit, t, rows);
}

/* Can the source of this access method test the wide leaves (SiftSrcProvider.set_wide / filter_wide)? */
extern bool sift_colsrc_can_wide(Oid relam, bool index);
extern int sift_colsrc_set_wide(SiftSrcSource *src, const SiftSrcWideTest *t, int nt);
extern bool sift_colsrc_filter_wide(SiftSrcSource *src, const SiftSrcUnit *unit, int i, uint64 *rows);

/* Can the source of this access method be restricted to a match set (SiftSrcProvider.select)? */
extern bool sift_colsrc_can_select(Oid relam, bool index);

/* Can the source of this access method give the TIDs of its rows and the zones of its units (SiftSrcProvider.tids, zone)? */
extern bool sift_colsrc_can_tids(Oid relam, bool index);
extern void sift_colsrc_tids(SiftSrcSource *src, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
extern bool sift_colsrc_sb_heap(SiftSrcSource *src);
extern void sift_colsrc_zone(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
extern uint32 sift_colsrc_zone_range(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern uint32 sift_colsrc_zone_range_segs(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern bool sift_colsrc_walk_stamp(SiftSrcSource *src, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem);
/* Does the source give the zone of a range of superblocks (zone_range is optional: a source without a top-N has none)? */
extern bool sift_colsrc_has_zone_range(SiftSrcSource *src);
extern bool sift_colsrc_overflow(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern void sift_colsrc_read_rows(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
/* The sorted run of a leaf of the unit (SiftSrcProvider.run_info): false when there is none to use, then the rows of the run [pos, pos + cnt) */
extern bool sift_colsrc_run_info(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
/* The head of the run (SiftSrcProvider.run_heads): its H, 0 when there is none */
extern bool sift_colsrc_sb_heads(SiftSrcSource *src, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
/* The order of the keys of a leaf in the unit (SiftSrcProvider.key_order) */
extern int	sift_colsrc_key_order(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern int	sift_colsrc_run_heads(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
extern int sift_colsrc_gorder_open(SiftSrcSource *src, int leaf, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid);
extern const char *sift_colsrc_gorder_str(SiftSrcSource *src, int i, int leaf, int64 key, uint32 *len);
extern uint32 sift_colsrc_gorder_strrank(SiftSrcSource *src, int i, int leaf, const char *s, uint32 len, bool after);
extern bool sift_colsrc_gorder_info(SiftSrcSource *src, int i, int leaf, uint32 *entries, uint32 *values);
extern bool sift_colsrc_gorder_exact(SiftSrcSource *src, int i, BlockNumber blk);
extern void sift_colsrc_gorder_head(SiftSrcSource *src, int i, int leaf, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
extern void sift_colsrc_gorder_range(SiftSrcSource *src, int i, int leaf, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);
extern uint32 sift_colsrc_gorder_seek(SiftSrcSource *src, int i, int leaf, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);
extern void sift_colsrc_run_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);

/* Restricts the rows of the units the next sift_colsrc_begin starts to the keys of the filter, which must outlive them (NULL: no restriction) */
extern void sift_colsrc_select(SiftSrcSource *src, const SiftFilter *keep);

/* Pre-aggregates of the unit just returned by sift_colsrc_next: false without a provider callback or when the unit cannot answer */
extern void sift_colsrc_preagg(Relation rel, SiftSrcPreagg *pa);

static inline bool
sift_colsrc_unit_exact(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_exact != NULL && src->prov->unit_exact(src->impl, unit);
}

static inline int
sift_colsrc_unit_dead(SiftSrcSource *src, const SiftSrcUnit *unit, int mode)
{
	return src->prov->unit_dead != NULL ? src->prov->unit_dead(src->impl, unit, mode) : -1;
}

static inline bool
sift_colsrc_unit_except(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	return src->prov->unit_except != NULL && src->prov->unit_except(src->impl, unit, leaf);
}

static inline bool
sift_colsrc_unit_agg(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return src->prov->unit_agg != NULL && src->prov->unit_agg(src->impl, unit, leaf, agg);
}

static inline bool
sift_colsrc_unit_hll(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint8 **blob)
{
	return src->prov->unit_hll != NULL && src->prov->unit_hll(src->impl, unit, leaf, blob);
}

static inline bool
sift_colsrc_unit_presence(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	return src->prov->unit_presence != NULL && src->prov->unit_presence(src->impl, unit, leaf, p);
}

static inline bool
sift_colsrc_unit_hist(SiftSrcSource *src, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	return src->prov->unit_hist != NULL && src->prov->unit_hist(src->impl, unit, dim, h);
}

static inline bool
sift_colsrc_gpu_unit(SiftSrcSource *src, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	return src->prov->gpu_unit != NULL && src->prov->gpu_unit(src->impl, unit, g);
}

static inline bool
sift_colsrc_gpu_dense(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	return src->prov->gpu_dense != NULL && src->prov->gpu_dense(src->impl, unit, leaf);
}

static inline bool
sift_colsrc_gpu_chunk(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	return src->prov->gpu_chunk != NULL && src->prov->gpu_chunk(src->impl, unit, leaf, c);
}

static inline void
sift_colsrc_gpu_dict(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	src->prov->gpu_dict(src->impl, unit, leaf, dict);
}

/* the tuples of the unit's rollup of the list, 0 when the unit has none or the source cannot tell */
static inline uint32
sift_colsrc_unit_rollup_tuples(SiftSrcSource *src, const SiftSrcUnit *unit, int list)
{
	return src->prov->unit_rollup_tuples == NULL ? 0 : src->prov->unit_rollup_tuples(src->impl, unit, list);
}

static inline bool
sift_colsrc_unit_rollup(SiftSrcSource *src, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r)
{
	return src->prov->unit_rollup != NULL && src->prov->unit_rollup(src->impl, unit, list, need, r);
}

static inline bool
sift_colsrc_unit_rollup_miss(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_rollup_miss != NULL && src->prov->unit_rollup_miss(src->impl, unit);
}

/* Asks the source for superunits (docs/COL-PREAGG.md section 10): true when it has them */
static inline bool
sift_colsrc_super(SiftSrcSource *src, bool on)
{
	if (src->prov->super == NULL)
		return false;
	src->prov->super(src->impl, on);
	return true;
}

static inline bool
sift_colsrc_whole(SiftSrcSource *src, int leaf, const int *meas, int nmeas)
{
	return src->prov->whole != NULL && src->prov->whole(src->impl, leaf, meas, nmeas);
}

static inline void
sift_colsrc_count_range(SiftSrcSource *src, int leaf, SiftSrcStrOp op, const char *s, uint32 len)
{
	if (src->prov->count_range != NULL)
		src->prov->count_range(src->impl, leaf, op, s, len);
}

static inline void
sift_colsrc_count_elem(SiftSrcSource *src)
{
	if (src->prov->count_elem != NULL)
		src->prov->count_elem(src->impl);
}

static inline void
sift_colsrc_count_cmp(SiftSrcSource *src, int leaf)
{
	if (src->prov->count_cmp != NULL)
		src->prov->count_cmp(src->impl, leaf);
}

static inline uint64
sift_colsrc_point_exact(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->point_exact != NULL ? src->prov->point_exact(src->impl, unit) : 0;
}

static inline bool
sift_colsrc_unit_count(SiftSrcSource *src, const SiftSrcUnit *unit, int64 *rows)
{
	return src->prov->unit_count != NULL && src->prov->unit_count(src->impl, unit, rows);
}

static inline uint32
sift_colsrc_super_span(SiftSrcSource *src)
{
	return src->prov->super_span ? src->prov->super_span(src->impl) : 0;
}

static inline void
sift_colsrc_split(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	src->prov->split(src->impl, unit);
}

/* Units of the superblocks [sb_lo, sb_hi), ascending sb, in the source's order within an sb */
extern void sift_colsrc_begin(SiftSrcSource *src, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
extern bool sift_colsrc_kept(SiftSrcSource *src, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept, uint64 *rows, bool count);
extern bool sift_colsrc_next(SiftSrcSource *src, SiftSrcUnit *unit);

/*
 * Leaf `leaf` of the unit decoded to its vtype. Valid until the next sift_colsrc_read of the same leaf or the next
 * sift_colsrc_next. A group leaf yields the SCV_I32 element counts per row (isnull: array absent); a child yields all the
 * elements of the unit.
 */
extern void sift_colsrc_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);

/*
 * The same into storage of the caller (sift_colsrc_leaf_n values and SIFT_AGG_PAD more, see SiftSrcBuf): vec->data and vec->isnull point into
 * buf, valid as long as the caller keeps it. For a caller that keeps its vectors across units or wants them next to each other, and the one
 * way to read without a copy by the source: sift_colsrc_read is this over the source's own buffers.
 */
extern int64 sift_colsrc_leaf_n(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern void sift_colsrc_read_into(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);

/*
 * Row bitmaps of the unit (nrow bits in ceil(nrow / 64) words, zero past nrow). live: the rows that exist (an index entry of a
 * deleted row is not). vet: clears the rows of bits that the snapshot does not see; call it after the value filters, it reads the heap
 * for the rows of pages that are not all-visible (a sift_table table has nothing to check).
 */
extern void sift_colsrc_live(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);
extern void sift_colsrc_vet(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);
extern bool sift_colsrc_vet_needed(SiftSrcSource *src, const SiftSrcUnit *unit);

extern void sift_colsrc_counters(const SiftSrcSource *src, SiftSrcCounters *c);

/*
 * Offers the conjuncts (at most SIFT_COLSRC_MAX_FILTERS, the constants outlive the scan) to the source before sift_colsrc_begin; returns the
 * bits of those it applies (0: none, or the source has no filters). sift_colsrc_filter then clears the rows of `bits` that fail any of them
 * (all of them: the order is the source's) and says whether any row is left. A raise of the value-overflow error is the same as a read of the leaf.
 */
extern uint64 sift_colsrc_set_filters(SiftSrcSource *src, const SiftSrcFilter *f, int nf);
extern bool sift_colsrc_filter(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);

/* The JSON types of a value (jsonb_typeof), as the bits of the types a filter keeps; the bit t - 1 is the chunk tag t (SIFT_COLTAG_*) */
#define SIFT_SRC_JT_STRING		0x01
#define SIFT_SRC_JT_NUMBER		0x02
#define SIFT_SRC_JT_BOOLEAN		0x04
#define SIFT_SRC_JT_OBJECT		0x08
#define SIFT_SRC_JT_ARRAY		0x10
#define SIFT_SRC_JT_NONE		0x20	/* the path has no value: jsonb_typeof is NULL */
#define SIFT_SRC_JT_NULL		0x40	/* a JSON null: jsonb_typeof is 'null' */
#define SIFT_SRC_JT_ALL			0x7F
#define SIFT_SRC_TYPE_NONE		0xFF	/* SiftSrcProvider.read_types: the path has no value */
#define SIFT_SRC_JT_READ		0x80	/* not a type: the query reads the leaf's values besides, so a row of the types whose value the leaf cannot hold is an error */

/* How the query reads a leaf whose value the leaf cannot store (SiftSrcProvider.set_recheck) */
typedef enum SiftSrcRecheck
{
	SRK_NONE,					/* not at all: such a value is an error for a row of the snapshot */
	SRK_TEXT,					/* the text of the scalar (a record column of jsonb_to_recordset, `->>` and a cast of it) through the input function of the leaf's type; a key of an array is NULL */
	SRK_RECORDSET,				/* an array leaf unnested by jsonb_to_recordset: the whole list of a row raises when one of its values does, and a list that is not an array of objects raises */
	SRK_CAST,					/* a member of jsonb_each read only through the cast of its jsonb value to the leaf's number type: a value of another JSON type raises where the row reaches the aggregate */
	SRK_RECORDSET_LAZY,			/* a spec's SRK_RECORDSET whose error the EXISTS test of the list raises when it reaches the row, not the rows' conjuncts (the source is given SRK_RECORDSET) */
	SRK_CAST_TESTED,			/* a spec's SRK_CAST that a conjunct of the members casts too: the cast of the conjunct raises first, for every row that reaches the members, so the error is raised before they are tested (the source is given SRK_CAST) */
	SRK_IOCAST					/* a path of the document read only as `(doc ->> 'k')::T`, T the leaf's type: the text of the scalar through the input function of T, which raises where the row reaches the aggregate (the source is given SRK_TEXT) */
} SiftSrcRecheck;

/* Can the source of this access method recheck its overflow rows (SiftSrcProvider.set_recheck)? */
extern bool sift_colsrc_can_recheck(Oid relam, bool index);
extern void sift_colsrc_set_recheck(SiftSrcSource *src, int leaf, SiftSrcRecheck mode, const char *key, Oid cast);
extern void sift_colsrc_overflow_raise(SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rows);
extern uint64 *sift_colsrc_recheck_rows(SiftSrcSource *src, const SiftSrcUnit *unit);

/* Can the source of this access method filter by the JSON type of a leaf (SiftSrcProvider.filter_types)? */
extern bool sift_colsrc_can_types(Oid relam, bool index);
extern bool sift_colsrc_filter_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void sift_colsrc_read_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void sift_colsrc_tolerate_ovf(SiftSrcSource *src, int leaf);

/* Can the source of this access method read an image leaf as the codes of its stored dictionary (SiftSrcProvider.set_raw)? */
extern bool sift_colsrc_can_raw(Oid relam, bool index);
extern void sift_colsrc_set_raw(SiftSrcSource *src, int leaf);

/* Tell the source (if it can) that a numeric leaf is only summed (SiftSrcProvider.set_numsum) */
extern void sift_colsrc_set_numsum(SiftSrcSource *src, int leaf);

/* Does the source of this access method tell a JSON null from a missing key per value of a leaf (SiftSrcProvider.read_jnull)? */
extern bool sift_colsrc_can_jnull(Oid relam, bool index);
extern void sift_colsrc_read_jnull(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint64 *bits);

/* Leaf `leaf` decoded for the rows of bits only (the other rows read as zero, not NULL); the whole leaf when the source cannot */
extern bool sift_colsrc_can_read_sel(const SiftSrcSource *src);
extern bool sift_colsrc_can_narrow(const SiftSrcSource *src);
extern bool sift_colsrc_narrow(SiftSrcSource *src, SiftSrcUnit *unit, const uint64 *bits);
extern void sift_colsrc_read_sel(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);

#endif							/* SIFT_AGG_COLSRC_PLAN_H */
