/*
 * col/f2/col_delta/idx.h - the column delta chain (SIFT_PT_COL_DELTA, table/f2/col_delta/col_delta.h) of a sift_col index (spec 49 3, 4): an UPDATE that does not change what the index holds
 * of a row, or changes a few leaves of it, appends a record that names the row by its place in its part (part, granule, row) and holds its new heap address (SIFT_CD_ATT_TID_REMAP, a
 * column of its own: attribute 0) and the new cells of the leaves that changed (attribute = leaf + 1), instead of ending the row's entry and writing a new one. A UNIQUE index keeps the entry of
 * each version: the check of every row a writer adds reads the chain of the partition it probes, so the chain of a unique index stays empty.
 *
 * This file holds what needs no server: the records a trigger writes, the map of the address changes a snapshot sees (what a vacuum and a delete find a row by), the patch of the views and
 * the addresses of a granule a scan decoded, and the exceptions a patched leaf is given so that no zone, dictionary or chunk filter of the part decides for it. f2_col_delta_srv.c holds the
 * server side: the chain read for a partition, the hooks of the scan, the locate, the stage of the transaction.
 *
 * What a row has at a snapshot is the part's value patched by the records the snapshot sees (the newest of each column wins): before the record commits the row has its old address and
 * values, after it the new ones, and a rolled back writer leaves nothing. A vacuum offers the callback the newest committed address of a row, never the one it had, so that the heap
 * reclaims the old tuple and the row stays; a delete finds a row by its newest address, and an address a row has left is no row of the index however it is reused by the heap.
 */
#ifndef SIFT_COL_F2_COL_DELTA_IDX_H
#define SIFT_COL_F2_COL_DELTA_IDX_H

#include "postgres.h"

#include "col/f2/col_delta/diff.h"
#include "col/f2/sink/sink.h"
#include "col/f2/vacuum/vm.h"
#include "col/granule/granule.h"
#include "core/xid_cache.h"
#include "table/f2/col_delta/col_delta.h"

/* ---- the records ---- */

/*
 * The record for the row at target (its address then is target->tid) whose new address is new_tid and whose leaves in leaf[0..nleaf) now hold cell[]: the bytes, 0 when they do not fit. kinds:
 * the kind of each leaf of the index; key_absent: per changed leaf, when the leaf is of a top-level key of a document and the document has none (NULL: none is). subxid: the writer is a
 * subtransaction; time_flags: SIFT_CDENT_FLAG_HAS_TIME0/1 for each time column of the index among the leaves (sift_cdi_time_flags).
 */
extern uint32 sift_cdi_encode(uint8 *dst, uint32 cap, const SiftColDeltaTarget *target, TransactionId xmin, CommandId cid, bool subxid, uint8 time_flags, uint64 new_tid, const SiftCellKind *kinds,
							  const int16 *leaf, const SiftCell *cell, const bool *key_absent, int nleaf);

/* The time flags of a record that changes leaf[0..nleaf): the time bounds of the parts do not hold for a time column a patch changes (time_cols: 1 + the leaf of each, 0: none) */
extern uint8 sift_cdi_time_flags(const int *time_cols, const int16 *leaf, int nleaf);

/* ---- the address changes of a chain ---- */

typedef struct SiftCdRemap
{
	uint64		part_gen;
	uint32		granule_id;
	uint16		row;
	uint64		new_tid;
	uint32		seq;			/* the place of the change in the chain: of two rows that were given one address by the heap in turn, the greater has it now */
} SiftCdRemap;

/* What the address changes of a part, or of a granule of it, say of the heap blocks of the new addresses: their range and the bloom of their blocks (sift_tid_block_bit) */
typedef struct SiftCdSummary
{
	uint64		part_gen;
	uint32		granule_id;		/* the summary of a part: 0 */
	BlockNumber blk_min;
	BlockNumber blk_max;
	uint64		blk_bloom;
	int			first;			/* the entries of slot it sums: [first, first + n) */
	int			n;
} SiftCdSummary;

/*
 * The newest address change of each row the writers the ops accept made, found by the row or by the address it has now: by search of the arrays, and by a hash of each when there are more
 * than SIFT_CD_REMAP_HASH_MIN of them. parts and grans sum the changes of each part and of each granule (the same order as slot).
 */
typedef struct SiftCdRemapMap
{
	int			n;
	SiftCdRemap *slot;			/* ascending by (part_gen, granule, row) */
	SiftCdRemap *by_new;		/* ascending by (new_tid, seq) */
	uint32	   *slot_ht;		/* the place in slot of the change of a row, plus one; NULL for few changes */
	uint32	   *new_ht;			/* the place in by_new of the change that made an address, plus one */
	uint32		ht_mask;
	int			nparts;
	SiftCdSummary *parts;
	int			ngrans;
	SiftCdSummary *grans;
} SiftCdRemapMap;

#define SIFT_CD_REMAP_HASH_MIN	16

/* The changes of the granule, in the row order: [pos, end) of slot, advanced as the rows of the granule are asked in ascending order */
typedef struct SiftCdRemapCursor
{
	const SiftCdRemapMap *rm;
	int			pos;
	int			end;
} SiftCdRemapCursor;

/* NULL when the chain has none; ops->visible says which writers count (a vacuum and a delete: the committed ones and the transaction itself) */
extern SiftCdRemapMap *sift_cd_remap_build(const SiftColDeltaChain *chain, const SiftColDeltaOps *ops);
extern void sift_cd_remap_free(SiftCdRemapMap *m);

/* The row that has the address now */
extern const SiftCdRemap *sift_cd_remap_by_new(const SiftCdRemapMap *m, uint64 tid);

/* The address the row has now, 0 when the chain moved it never */
extern uint64 sift_cd_remap_new_tid(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, uint16 row);

extern void sift_cd_remap_cursor_init(SiftCdRemapCursor *cur, const SiftCdRemapMap *rm, uint64 part_gen, uint32 granule_id);

/* The address the row has now, 0 when the chain moved it never; rows are asked in ascending order */
extern uint64 sift_cd_remap_cursor_new_tid(SiftCdRemapCursor *cur, uint16 row);

/* The row of the place has moved to an address other than raw_tid: raw_tid is no row of the index any more, and the heap may give it to another */
extern bool sift_cd_remap_supersedes(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, uint16 row, uint64 raw_tid);

/* The addresses a vacuum offers for the rows of a part: whether an address change of the part (of the granule) lies in a block the heap's visibility map does not say all-visible */
extern bool sift_cd_remap_part_in_dirty(const SiftCdRemapMap *m, uint64 part_gen, const SiftVmDirtySet *ds);
extern bool sift_cd_remap_granule_in_dirty(const SiftCdRemapMap *m, uint64 part_gen, uint32 granule_id, const SiftVmDirtySet *ds);

#ifndef SIFT_S49_STANDALONE
struct F2Scan;
struct SiftColSchema;
typedef struct SiftCdiScan SiftCdiScan;

/*
 * The exceptions of the part (exc, *nexc of them) as the scan of the snapshot must read them: with an exception for every granule whose leaf a predicate reads and a patch of the chain changes.
 * Called for each part a scan reads, before its predicates are judged on its zones, dictionaries and chunks.
 */
extern const SiftVariantExcSummary *sift_cdi_scan_exceptions(struct F2Scan *s, Relation rel, const struct SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPart *part, Snapshot snap,
															 const SiftVariantExcSummary *exc, int *nexc);

/* The overlay of one part a reader keeps, built in cx: a scan keeps one, and each cursor of an ordered scan one of its own, so that a merge of parts builds the overlay of each part once */
typedef struct SiftCdiMapSlot
{
	uint64		part_id;
	uint64		part_gen;
	bool		built;			/* map is the overlay of the part (NULL: the snapshot sees no patch of it) */
	SiftColDeltaPartMap *map;
	MemoryContext cx;
} SiftCdiMapSlot;

/* The same with the overlay kept in slot (NULL: the scan's own) */
extern const SiftVariantExcSummary *sift_cdi_slot_exceptions(struct F2Scan *s, SiftCdiMapSlot *slot, Relation rel, const struct SiftColSchema *schema, const SiftPartitionEntry *e,
															 const SiftPart *part, Snapshot snap, const SiftVariantExcSummary *exc, int *nexc);

/* The patches the snapshot sees of granule g laid over its decoded views and addresses; true when an address changed, so that the granule's range of addresses does not tell it holds no pending delete */
extern bool sift_cdi_scan_patch(struct F2Scan *s, const struct SiftColSchema *schema, uint32 g, ColView *views, uint64 *tids, int n, MemoryContext cx);

/* The addresses of granule g as the snapshot sees them, for a decision taken before any column is decoded; whether the chain moved a row of g, so that the range of addresses of g is not the range of its rows */
extern void sift_cdi_scan_remap_tids(const struct F2Scan *s, uint32 g, uint64 *tids, int n);
extern bool sift_cdi_scan_remaps(const struct F2Scan *s, uint32 g);
/* Whether a column delta changes a value of granule g of the part being scanned */
extern bool sift_cdi_scan_granule_patched(const struct F2Scan *s, uint32 g);
/* The map the scan patches its granules with (NULL: none), and the one it had: a granule of another part read again later is read with the map of that part */
extern const SiftColDeltaPartMap *sift_cdi_scan_swap_map(struct F2Scan *s, const SiftColDeltaPartMap *map);

/*
 * The update of a row of the index from oldt (at old_tid) to newt as a record of the chain, when the row is in a part, none of its key or partial-index columns changed, its leaves that changed
 * are few and the chain has room: the new version's entry, which aminsert staged, is withdrawn from the stage (it is not written) and the old version's entry stays, at the new address
 * from the record's commit. False when the update takes the way of a tombstone and a new entry.
 */
extern bool sift_cdi_stage_update(const SiftF2DiffEnv *env, struct SiftTxnStage *st, HeapTuple oldt, HeapTuple newt, uint64 old_tid, bool index_unchanged);

/*
 * aminsert is told that an UPDATE that is not HOT changed no column of the index (indexUnchanged): the row trigger of track_changes, which stages the update as a record of the chain, then
 * writes the new address of the row and nothing else, without reading a column of either version. The note is of the new version's address and lasts until its trigger takes it or the transaction ends.
 */
extern void sift_f2_track_note_unchanged(Relation index, ItemPointer new_tid);

/* The overlay of the part of the partition for the snapshot xc stands for, in the current memory context; NULL when the partition has no chain or the snapshot sees no patch of it (f2_col_delta_srv.c) */
extern SiftColDeltaPartMap *sift_cdi_part_map(Relation rel, const SiftPartitionEntry *e, const SiftPart *part, SiftXidCache *xc);

/* The address changes of the committed writers and the transaction itself in the partition's chain; NULL when the partition has none (f2_col_delta_srv.c) */
extern SiftCdRemapMap *sift_cdi_remap_load(Relation rel, const SiftPartitionEntry *entry);
#endif

/* ---- what a scan lays over a part ---- */

/*
 * The cells and addresses of the n rows of a granule a scan decoded, patched with what the snapshot sees of the granule: tids[row] is the new address, the decoded leaves (views[leaf].isnull
 * not NULL) the new cells, copied into cx before they are written (the arrays the scan decoded are not changed). ncols: the leaves of the index.
 */
extern void sift_cdi_patch_granule(const SiftColDeltaGranMap *gm, int ncols, ColView *views, uint64 *tids, int n, MemoryContext cx);

/* Only the addresses: tids[row] is the address the chain gave the row (what a TID bridge and a pending delete name it by) */
extern void sift_cdi_remap_tids(const SiftColDeltaGranMap *gm, uint64 *tids, int n);

/* Whether the chain gave an address to a row of the granule (NULL: no patch of it): the addresses the part holds are not those a pending delete names its rows by */
static inline bool
sift_cdi_gran_remaps(const SiftColDeltaGranMap *gm)
{
	return gm != NULL && (gm->attnum_bloom & sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP)) != 0;
}

/*
 * The exceptions of a part with, for every granule the map patches in a leaf the scan prunes by, a predicate's or the sort bound's (pruned_leaf[leaf]), the exception that tells a scan that the stored column cannot decide for
 * that leaf of that granule: its zones, its dictionary, its chunk filters and its skipping indexes are left out for it. exc[nexc] is returned as it is when nothing of it is patched.
 */
extern const SiftVariantExcSummary *sift_cdi_exceptions(const SiftColDeltaPartMap *map, const SiftVariantExcSummary *exc, int nexc, const bool *pruned_leaf, int ncols, int *nout);

/* Whether the leaves of an update are any of the leaves (scope_leaf[leaf]) a scope summary of the index reads */
static inline bool
sift_cdi_scope_dirty(const int16 *leaf, int nleaf, const bool *scope_leaf)
{
	for (int i = 0; i < nleaf; i++)
		if (scope_leaf[leaf[i]])
			return true;
	return false;
}

#endif							/* SIFT_COL_F2_COL_DELTA_IDX_H */
