/*
 * agg/colsrc/provider.h - the column source: the provider table
 * The SiftSrcProvider table every module registers behind the seam, the source that holds its
 * implementation and the register; the third of the four parts of agg/colsrc.h.
 */
#ifndef SIFT_AGG_COLSRC_PROVIDER_H
#define SIFT_AGG_COLSRC_PROVIDER_H

#include "agg/colsrc/exec.h"

typedef struct SiftSrcProvider
{
	bool		(*owns_index) (Oid indexrelam);
	bool		(*owns_table) (Oid tablerelam);
	SiftSrcLayout *(*layout) (Relation rel);
	void		(*stats) (Relation rel, SiftSrcStats *st);
	/* Optional, required with open_part: what changes the layout, statistics and pre-aggregates of an index besides its catalog entries and its table's (sift_colsrc_stamp) */
	void		(*stamp) (Relation index, SiftSrcStamp *stamp);
	void	   *(*open) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	/* Optional (NULL: the provider has no partitioned tables, sift_colsrc_can_part): open for a partition read as part of its partitioned table, whose privileges and row security were checked */
	void	   *(*open_part) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	void		(*close) (void *impl);
	/* Optional: release the buffers the source keeps pinned between units; a partitioned table's scan calls it when it leaves a partition */
	void		(*unpin) (void *impl);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
	/*
	 * Optional: sets bit sb of kept for every superblock (below nspace) in which a begin with these prune entries would find a unit, and counts the units it would
	 * prune everywhere in the source's counters when count (a begin then counts none); false when the source cannot tell.
	 */
	bool		(*kept) (void *impl, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept, uint64 *rows, bool count);
	bool		(*next) (void *impl, SiftSrcUnit *unit);
	void		(*read) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
	void		(*live) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	void		(*vet) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* Optional (NULL: always): false when vet would clear no row of the unit (every page is all-visible, or the source is MVCC by itself) */
	bool		(*vet_needed) (void *impl, const SiftSrcUnit *unit);
	void		(*counters) (const void *impl, SiftSrcCounters *c);
	/*
	 * Optional (NULL: the executor tests the values itself). prune_str, called before `begin`, adds text tests to the zone pruning of the
	 * scan: units whose zones prove that no value can satisfy one are skipped (and counted in units_pruned); for a unit that stores its
	 * text as codes this reads the segment dictionary once, for an inline unit the prefix zone. filter_str ANDs `rows` (the unit's nrow bits,
	 * positions as in `read` and `live`) with the rows whose value satisfies the test, NULL never; it returns false, leaving rows as
	 * they were, when it cannot (the executor then reads the leaf and tests it). The tests are exact, not conservative.
	 */
	void		(*prune_str) (void *impl, const SiftSrcStrTest *t, int nt);
	bool		(*filter_str) (void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows);
	/*
	 * Optional, both or neither (sift_colsrc_can_wide): the tests of a conjunct on a wide leaf. set_wide, called before `begin`, takes the tests of the scan (operands as Datums, in
	 * the type of the leaf), joins them to the zone pruning and returns how many it will apply: the executor does not evaluate them. filter_wide
	 * ANDs `rows` with the rows that pass test i and says whether any is left; NULL never passes, a NaN as PostgreSQL orders it.
	 */
	int			(*set_wide) (void *impl, const SiftSrcWideTest *t, int nt);
	bool		(*filter_wide) (void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);
	/*
	 * Optional (NULL: sift_colsrc_read_into reads with `read` and copies). leaf_n: the values a read of the leaf yields (rows, or for a child
	 * the elements of the whole unit; an upper bound for a unit restricted to a match set), without reading it. read_into: `read` into the caller's storage (SiftSrcBuf), the form `read` itself
	 * is a wrapper of in a provider that has it.
	 */
	int64		(*leaf_n) (void *impl, const SiftSrcUnit *unit, int leaf);
	void		(*read_into) (void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);
	/*
	 * Optional (NULL: the source cannot be restricted to a match set, sift_colsrc_can_select says so to the planner). select, called before
	 * `begin`, restricts the units `begin` starts to the rows whose key (blk << 16 | off) the filter has (NULL: all rows again): a unit
	 * without such a row is not returned (it counts in units_pruned), and in the others `nrow`, `read`, `live` and `vet` are about the kept
	 * rows only, which the source may renumber 0..nrow-1 in key order (docs/COL-SEARCH.md, src/agg/sel.h).
	 */
	void		(*select) (void *impl, const SiftFilter *keep);
	/* Optional (NULL: no unit has pre-aggregates), see SiftSrcUnitAgg; `preagg` is the planner's, over the index relation */
	void		(*preagg) (Relation rel, SiftSrcPreagg *pa);
	bool		(*unit_exact) (void *impl, const SiftSrcUnit *unit);
	/*
	 * Optional: a unit that is not exact because rows of it are dead answers net of them (counts and integer sums, min and max loose) while SSD_ON; returns the number of dead rows, -1
	 * when it cannot (then nothing is on). SSD_BOUNDS only checks that it could, for the cost of its pages' visibility: unit_agg then gives the zone (min, max, has_minmax) of the unit
	 * with its dead rows, a bound of the live ones, and nothing else is valid. SSD_OFF reads the unit as its own rows again.
	 */
	int			(*unit_dead) (void *impl, const SiftSrcUnit *unit, int mode);
	/*
	 * Optional (NULL: no unit has exceptions). Leaf >= 0: when the exact unit holds the pre-aggregates of its rows where `leaf` is not NULL (docs/COL-PREAGG.md 11), the
	 * next unit_hist and unit_rollup calls on it return what the rows where `leaf` IS NULL add up to (counts and sums net of those rows, no minimum or maximum; false
	 * from them when they cannot tell) and this returns true. Leaf -1: the unit's own rows again. unit_agg and unit_presence refuse a unit set to a leaf.
	 */
	bool		(*unit_except) (void *impl, const SiftSrcUnit *unit, int leaf);
	bool		(*unit_agg) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
	bool		(*unit_hist) (void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h);
	/* Optional (NULL: none): the presence of an exact unit's leaf of SiftSrcPreagg.pres, valid until the next call; false when the unit has none */
	bool		(*unit_presence) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p);
	/* Optional (NULL: none): the HyperLogLog sketch of an exact unit's leaf of SiftSrcPreagg.hll (col/hll.h), valid until the next call; false when the unit has none */
	bool		(*unit_hll) (void *impl, const SiftSrcUnit *unit, int leaf, const uint8 **blob);
	uint32		(*unit_rollup_tuples) (void *impl, const SiftSrcUnit *unit, int list);
	bool		(*unit_rollup) (void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r);
	/* Optional: the consumer scanned an exact unit because its rollup lacks what the query reads; the source may arrange for the unit to get it. True: a refresh is queued for it */
	bool		(*unit_rollup_miss) (void *impl, const SiftSrcUnit *unit);
	/* Optional (NULL: none): `next` may hand over a group of exact units as one unit that has pre-aggregates only (nsu > 0); split gives the units of the group back, one by one, to the next calls */
	void		(*super) (void *impl, bool on);
	bool		(*whole) (void *impl, int leaf, const int *meas, int nmeas);	/* a unit may be a whole segment whose key table of leaf, with these measures, answers (a superunit that has that histogram only); true: an item of the scan has to span a segment for that, which a source with no superunits does not do alone */
	/* Optional (NULL: none), after super: the count(*) of the rows of text leaf that pass one test (op a SiftSrcStrOp of EQ, LT, LE, GT, GE or PREFIX against s) and nothing else; a segment whose dictionary holds the rows of each string may be handed over as one unit (nsu > 0) whose unit_count is the rows that pass: the consumer folds them and reads nothing */
	void		(*count_range) (void *impl, int leaf, SiftSrcStrOp op, const char *s, uint32 len);
	/* Optional: the same for the comparisons of an integer leaf, all the conditions of the scan: a segment whose global order of the leaf holds the keys counts them */
	void		(*count_cmp) (void *impl, int leaf);
	/* Optional: the same for the only condition, an equality on an element of a list (the prune_str test that stands for it): a segment whose point directory lists the rows of the value counts them */
	void		(*count_elem) (void *impl);
	bool		(*unit_count) (void *impl, const SiftSrcUnit *unit, int64 *rows);
	/* Optional (NULL: none): the element tests (prune_str's tests that stand for an EXISTS over a list, bit i: the i-th test given) whose rows the unit's point directory lists exactly: the unit comes narrowed to them, and the executor need not test them again */
	uint64		(*point_exact) (void *impl, const SiftSrcUnit *unit);
	void		(*split) (void *impl, const SiftSrcUnit *unit);
	uint32		(*super_span) (void *impl);	/* the superblocks a range of a parallel scan should span for a group of units to fit in it */
	/*
	 * Optional (NULL: no ordered top-N scan on this source, sift_colsrc_can_tids says so to the planner). tids: the heap TIDs of the rows
	 * `rows` (ascending, in the numbering `read` and `live` use) of the unit the source is at. zone: the zone of an integer-typed leaf
	 * of the unit the source is at, over the unit as stored.
	 */
	void		(*tids) (void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
	/* Optional (NULL: yes): is the number of every unit the heap superblock its rows are in? Not when a segment cuts its units by key or TID range: a TID range then selects no units by number */
	bool		(*sb_heap) (void *impl);
	void		(*zone) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
	/* zone_range: the zone of a leaf over all the units of the superblocks [lo, hi) together, without opening them; returns how many units it counts (required with tids) */
	uint32		(*zone_range) (void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
	/*
	 * Optional (both or neither): the zone of the stored units alone, and what those zones depend on, so that a top-N may keep the order of its walk between queries:
	 * walk_stamp fills the identity of the stored units (a REINDEX or any change of the segments gives another) and lists, ascending, the superblocks that also hold
	 * rows of a write buffer (at most cap); false: the zones of this source are not kept
	 */
	uint32		(*zone_range_segs) (void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
	bool		(*walk_stamp) (void *impl, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem);
	/* optional (NULL: none does): can the unit the source is at hold, in `leaf`, a value the source cannot store (SiftSrcVec.ovf)? */
	bool		(*overflow) (void *impl, const SiftSrcUnit *unit, int leaf);
	/*
	 * Optional (NULL: no sorted runs): the sorted run of a leaf of the unit the source is at, in the numbering `read` uses: false when the
	 * unit has none or its rows are not numbered as stored (a unit of a match set, rows read again from the heap, a unit narrowed to a few rows). The rows
	 * of positions [0, nvals) of the run have a value, ascending by (value, row), the next nnull are the NULL rows ascending, the rest are the rows whose
	 * value the leaf cannot store, ascending. run_read gives the rows at positions [pos, pos + cnt) of that run. id names the unit within the scan. walk: the
	 * caller will read the rows of the run (not only its head); a source may refuse a run that is the rows in their own order, which reads no faster than the unit.
	 */
	/*
	 * Optional: the keys of the leaf in the unit never decrease (1) or never increase (-1) in row order, so the best rows of a one-key top-N are the first (or last) rows;
	 * 2: the order is not known but read_rows of a run of rows costs by the rows it reads, not by the unit; 0: neither, or the unit is not the stored one (a match set,
	 * narrowed, read again from the heap)
	 */
	int			(*key_order) (void *impl, const SiftSrcUnit *unit, int leaf);
	bool		(*run_info) (void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
	void		(*run_read) (void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);
	/*
	 * The global order of a leaf (optional, col/gorder.h): every row's key and heap address sorted, in sources (stored segments, the rows of the write
	 * buffers). gorder_open: their number, or -1 when the order cannot answer. gorder_info: the entries of source i and how many have a value (the first ones;
	 * the others are NULL rows by address). gorder_head: the first max(w, up to p) entries of the chunk that holds entry p, as keys and heap addresses into buf (SiftCGOrdBuf: the chunk's first entry, its entries, the count decoded). The entries are
	 * of rows that may have died since: the caller has the heap say which are live and what they hold.
	 */
	int			(*gorder_open) (void *impl, int leaf, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid);	/* collid: the collation of a text key (0: an integer or time key; a block of another collation or kind cannot answer); grouped: the leaf the blocks are grouped by, -1; pins: conditions leaf = value of the query, which a block grouped by one of the leaves needs */
	bool		(*gorder_info) (void *impl, int i, int leaf, uint32 *entries, uint32 *values);
	uint32		(*gorder_strrank) (void *impl, int i, int leaf, const char *s, uint32 len, bool after);	/* a text key: the ranks below s in source i (after: below or equal), which is the rank of the first string that is not */
	const char *(*gorder_str) (void *impl, int i, int leaf, int64 key, uint32 *len);	/* a text key: the string of the entries with that key in source i, valid until the next call */
	void		(*gorder_head) (void *impl, int i, int leaf, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
	void		(*gorder_range) (void *impl, int i, int leaf, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);	/* the value entries [vs, ve) and the NULL ones [ns, ne) of the pinned group (all of a source that is not grouped) */
	bool		(*gorder_exact) (void *impl, int i, BlockNumber blk);	/* optional: the entry of source i at heap block blk is of a row that is live and visible to every snapshot, so the heap need not confirm it */
	uint32		(*gorder_seek) (void *impl, int i, int leaf, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);	/* the first entry of [vs, ve) with key >= key (after: > key); ve when none; a segment leaves the chunk it ends in in keep (when not NULL) */
	/*
	 * Optional (needs run_info found a run): the head of that run, the best SiftSrcRunHeads.n entries of the run read ascending (lo) and descending (hi)
	 * with the heap address of each row; 0 when the run has none. Stored when the unit was written: sound only if every row of the unit still passes.
	 */
	/* Optional: run_heads for the unit of superblock sb without reading it, only when every row of the unit is live and visible (the unit of a table, of a key-cut segment or of a write buffer has none) */
	bool		(*sb_heads) (void *impl, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
	int			(*run_heads) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
	/* read for the rows `rows` (ascending) only, valid until the next read of the leaf: vec->n == n, element k is row rows[k] */
	void		(*read_rows) (void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
	/* optional: the int filters above. set_filters (before begin) returns the bits of those it will apply, filter narrows the rows of a unit by them */
	uint64		(*set_filters) (void *impl, const SiftSrcFilter *f, int nf);
	bool		(*filter) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* optional: read only the rows of bits (the other rows of the vector are zero; a source restricted to a match set may read the whole leaf) */
	void		(*read_sel) (void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
	/*
	 * optional: the rows of bits, which are few, are all the executor needs of the unit. When true, the unit goes on as those rows alone,
	 * renumbered 0..unit->nrow-1 as in a unit of a match set: `nrow`, `read`, `live`, `vet` and the tests are about them, and what the executor
	 * read of the unit before is stale. False: nothing changed.
	 */
	bool		(*narrow) (void *impl, SiftSrcUnit *unit, const uint64 *bits);
	/*
	 * Optional, both or neither (sift_colsrc_can_types): the JSON types of the values of a leaf (SIFT_SRC_JT_*), for a leaf that holds the
	 * values of one type and sets aside the others (overflow). filter_types clears the rows of bits whose type is not one of the types
	 * (SIFT_SRC_JT_NONE: jsonb_typeof is NULL, a missing key; SIFT_SRC_JT_NULL: a JSON null) and says whether any row is left. It raises the
	 * value-overflow error when SIFT_SRC_JT_READ is set (the query reads the leaf elsewhere) and a row of one of the types has a value the
	 * leaf cannot hold, unless no snapshot sees the row, and when the type of a row is not known (an index written before the types were kept,
	 * and for a NULL or an astext value before the kinds). tolerate_ovf, called before `begin`, makes the reads of the leaf give a value it
	 * cannot hold as NULL instead of raising that error: the executor has filter_types drop or refuse every row that has one.
	 */
	bool		(*filter_types) (void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
	void		(*tolerate_ovf) (void *impl, int leaf);
	/*
	 * Optional (sift_colsrc_can_raw). set_raw, called before `begin`, makes the reads of an image leaf (SiftSrcLeaf.wide: uuid, bytea) give the codes
	 * of the dictionary the unit stores, whose entries are the stored images, instead of the per-unit dictionary of aligned datums: for a leaf the
	 * executor only compares and copies, never hands to PostgreSQL's functions.
	 */
	void		(*set_raw) (void *impl, int leaf);
	/*
	 * Optional. set_numsum, called before `begin`, says that the executor only adds the values of a numeric leaf (a sum or average that needs no numeric): the
	 * source may give its dictionary as one DNum record for each row (SiftSrcDict.numrec) instead of the datums of its distinct values. A source that does not
	 * keeps the dictionary; the executor tells by the flag of each read.
	 */
	void		(*set_numsum) (void *impl, int leaf);
	/*
	 * Optional, both or neither: the rows of a unit that hold, in a leaf the query reads, a value the
	 * leaf cannot store are read again from the heap as the query's SQL reads them. set_recheck, called before `begin`, says how the query reads
	 * the leaf (a SiftSrcRecheck); the rows of a unit with such a value leave it (`live` does not have them) and the source returns them as the
	 * unit that follows it, from the heap (the snapshot's version of each), their leaf now holding the value SQL gives, or still an
	 * overflow value where SQL raises. key (SRK_CAST of a leaf that is the value column of an object with free keys): only the member of this key is
	 * read as the cast to the type cast reads it, a value of another key is not an error. overflow_raise raises the error SQL raises for the first of `rows` (rows of the unit just returned, which
	 * has such values) when the source recorded one, and returns when it did not.
	 */
	void		(*set_recheck) (void *impl, int leaf, int mode, const char *key, Oid cast);
	void		(*overflow_raise) (void *impl, const SiftSrcUnit *unit, const uint64 *rows);
	/*
	 * The rows `live` took out of the unit the source is at, to be read again from the heap (NULL: none, or the source reads them all). They are
	 * the source's to read, but the executor may clear those that a conjunct on a column the stored unit holds rejects: they are not read.
	 */
	uint64	   *(*recheck_rows) (void *impl, const SiftSrcUnit *unit);
	/*
	 * Also with them: read_types gives the JSON type of the value of the leaf of each row of the unit in tags[0..nrow): the index of the
	 * name jsonb_typeof gives (0 string, 1 number, 2 boolean, 3 object, 4 array, 5 null) or SIFT_SRC_TYPE_NONE (its NULL). It raises when
	 * the index did not store the type of some row (as filter_types does).
	 */
	void		(*read_types) (void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
	/*
	 * Optional (sift_colsrc_can_jnull): the JSON nulls among the NULL slots of a leaf, bit k of bits for the k-th value of the leaf as `read`
	 * gives it (an element leaf: its elements); the other NULL slots are missing keys. Raises when the index did not store the kinds of the
	 * slots (as filter_types does).
	 */
	void		(*read_jnull) (void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
	/*
	 * Optional (NULL: the Vulkan path does not run on this source). gpu_unit: the unit just returned by `next` is stored, has no dead row and
	 * every page it has rows on is all-visible, so its chunks are the rows a query sees. gpu_dense: the leaf has no NULL and no overflowed
	 * value in the unit (from its zone). gpu_chunk: the leaf's codec blob; false when the leaf has none (a NULL, an inline text value).
	 * gpu_dict: the dictionary of a text leaf (the one its codes are of).
	 */
	bool		(*gpu_unit) (void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g);
	bool		(*gpu_dense) (void *impl, const SiftSrcUnit *unit, int leaf);
	bool		(*gpu_chunk) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c);
	void		(*gpu_dict) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict);
} SiftSrcProvider;

struct SiftSrcSource
{
	const SiftSrcProvider *prov;
	void	   *impl;
};

extern void sift_colsrc_register(const SiftSrcProvider *prov);

#endif							/* SIFT_AGG_COLSRC_PROVIDER_H */
