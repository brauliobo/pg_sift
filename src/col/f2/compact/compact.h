/*
 * col/compact.h - partition-local compaction of format major 2: the L0 to L1 merge and the rewrite of a part with many dead rows are the same step. The rows of the chosen parts of
 * one partition are read, those whose deletion is settled are dropped, an inserter that committed before the horizon is frozen and one that aborted is dropped, the rest is sorted
 * and written as one part of the next level (with a pre-aggregate when the spec asks for one), and one change of the manifest swaps the parts. Nothing outside the partition is read.
 */
#ifndef SIFT_COMPACT_H
#define SIFT_COMPACT_H

#include "core/host.h"

#include "col/f2/del/delvec.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "core/f2/manifest/manifest.h"

extern PGDLLIMPORT int sift_dead_rewrite_pct;

#ifdef SIFT_TESTING
/* Testing: the rows the next compaction deletes from its first input between its two steps, as a deleter that reaches the partition meanwhile does (0: none) */
extern PGDLLIMPORT int sift_test_compact_delete;
#endif

/* Parts of a level are merged into one of the next level when this many are there; a part under the floor is merged with another one under it */
#define SIFT_COMPACT_FANOUT			4
#define SIFT_COMPACT_FLOOR_BYTES	(256 * 1024)

/*
 * The parts of level 0 wait for this many of them or this many bytes before they are merged (a part written once is not rewritten at once), and no part is of a level above SIFT_MAX_PART_LEVEL.
 * Beside parts of a level above a few small ones are not worth a merge, which writes them again and the summaries that name them: they wait until they are a SIFT_COMPACT_L0_SHARE-th of those parts,
 * or until SIFT_COMPACT_L0_MAX of them are there.
 */
/* STOR[STOR-F2-01]: L0 fanout 6 and direct L1 from 1024 rows write each row once fewer (F5 WA, B/row) */
#define SIFT_COMPACT_L0_FANOUT		6
#define SIFT_COMPACT_L0_BYTES		(UINT64_C(4) << 20)
#define SIFT_COMPACT_L0_SHARE		4
#define SIFT_COMPACT_L0_MAX			(4 * SIFT_COMPACT_L0_FANOUT)
#define SIFT_MAX_PART_LEVEL			2

/* A batch of at least this many rows is written as a part of level 1 at once, not as one of level 0 that the next compaction rewrites */
#define SIFT_L0_TO_L1_DIRECT_ROWS	1024
/*
 * Rows whose first time column is older than the hot cut (SIFT_HOT_WINDOW_DAYS before now, from a day boundary) are cold: a compaction writes them as a part of their own
 * (SIFT_PART_FLAG_TIME_COLD) and the rest as a hot one, so that a late arrival or an old row's change does not stretch the time bounds of the hot parts. Cold parts are
 * compacted with cold parts only; a hot part that has aged past the cut is rewritten, its rows going cold.
 */
#define SIFT_HOT_WINDOW_DAYS		30

/*
 * Where a compaction cuts its rows in two: those whose time column of the manifest slot is older than at go to parts of their own, the others to the parts of the other side; at PG_INT64_MIN:
 * no cut. tier: the sides are the hot and the cold tier of the hot cut (SIFT_PART_FLAG_TIME_HOT / _COLD); a cut that is not a tier (the cutoff of a TTL) flags no output.
 */
typedef struct SiftCompactCut
{
	int			slot;
	int64		at;
	bool		tier;
} SiftCompactCut;

/* The hot cut of the first time column */
#define SIFT_COMPACT_HOT_CUT(hot_cut)	((SiftCompactCut) {.slot = 0,.at = (hot_cut),.tier = true})

typedef struct SiftCompactResult
{
	uint64		new_gen;		/* 0: no row was left, no part written */
	uint64		rows_in;
	uint64		rows_out;
	uint64		dead_dropped;
	uint64		aborted_dropped;
	uint64		frozen;
	FullTransactionId stripped;	/* the greatest transaction whose commit the compaction made invisible to the snapshots that precede it (a row frozen, a delete made plain): invalid when none */
	uint64		granules_copied;	/* the granules of the new part that are those of the parts it replaces, byte for byte */
	uint64		granules_written;	/* the granules of the new part */
	uint64		bytes_out;
	uint64		writer_bytes;	/* what the writer of a joined part held at most: the metadata of its sections and one granule, whatever the size of the part */
	uint64		dict_cols_remapped;	/* the string columns written as the codes of the input dictionaries sent into their merge, no string read */
	uint64		dict_codes_remapped;	/* the values of those columns */
	int			nparts_in;
} SiftCompactResult;

/*
 * The string columns that every input part codes by a dictionary of its part (compact_dict.c): their dictionaries are merged once into dict[c] (n > 0) and map[k * ncols + c] sends a
 * code of the dictionary of part k to its code in it. The rows are read through a spec (sift_dict_merge_spec) that holds those columns as integers, the codes; after the rows are sorted
 * and viewed (sift_rowbuf_rows) sift_dict_merge_finish turns the columns back into what a build from the strings would write.
 */
typedef struct SiftDictMerge
{
	int			ncols;
	int			nparts;
	SiftCellKind *kinds;
	SiftPartDict *dict;
	SiftCodeMap *map;
} SiftDictMerge;

/* Off, a compaction reads the strings of every column (what the tests compare the remapped codes with) */
extern PGDLLIMPORT bool sift_compact_dict_remap;

/* keep_strings (per leaf, NULL for none): the columns whose strings the compaction reads as strings, because its host writes values into them. The dictionaries of the columns that are merged
 * hold at most max_bytes together (what the parts' dictionaries and the maps of their codes take): the columns after that are read as strings */
extern SiftDictMerge *sift_dict_merge_plan(Relation rel, uint32 part_id, const SiftPartDesc *const *at, int nparts, const SiftPartSpec *spec, const bool *keep_strings,
											const SiftColRemap *const *remaps, Size max_bytes);

/* An input of a compaction opened in a context of its own that the caller deletes: of all the inputs only the one that is read is held */
extern SiftPart *sift_compact_open_input(Relation rel, const SiftPartDesc *d, uint32 part_id, MemoryContext *cx);
extern SiftPartSpec sift_dict_merge_spec(const SiftDictMerge *m, const SiftPartSpec *spec);
extern void sift_dict_merge_finish(const SiftDictMerge *m, const SiftRowBuf *rb, SiftColIn *cols, SiftCompactResult *res);

/* The parts one compaction takes: of one level, all together, they become one part of the level above */
typedef struct SiftCompactGroup
{
	uint64	   *gens;
	int			n;
	bool		join;			/* the parts are joined as they are (sift_partition_join_online), their rows not read */
} SiftCompactGroup;

/* The blocks of a part, added to those a commit releases: its deletion vector is not among them, the commit releases the one the manifest holds when it is made */
extern void sift_compact_release_part(const SiftPart *part, SiftExtent **rel_ext, int *nrel);

/* The output parts given back at once, as no manifest ever held them; the deletion vector an output carried over, which is replaced */
extern void sift_compact_release_outputs(Relation rel, uint64 part_id, const SiftPartDesc *out, int nout);
extern void sift_compact_release_outputs_dv(Relation rel, const SiftPartDesc *out);

/*
 * The parts at[0 .. n) as one part of the level above, their granules copied as they are, the blocks of the parts added to *rel_ext: possible when no row is dead, the parts follow one
 * another, share a schema and their dictionaries and have all rows frozen (so that the horizon has nothing to drop or freeze). false: nothing is written and the rows have to be read.
 */
extern bool sift_compact_join_parts(Relation rel, uint64 part_id, const SiftPartDesc **at, int n, const SiftPartSpec *spec, const SiftCompactCut *cut, bool bounds_hold, FullTransactionId horizon,
									SiftExtent **rel_ext, int *nrel, SiftCompactResult *res, SiftPartDesc *out);

/* The parts of a partition whose dead rows are at least sift.dead_rewrite_pct of their rows: their generations, in the order of the list */
extern uint64 *sift_compact_candidates(const SiftPartDesc *parts, int nparts, int *n);

/*
 * What to compact, by level: every part of level 0 once SIFT_COMPACT_L0_FANOUT of them are there and are enough beside the parts above them (SIFT_COMPACT_L0_SHARE); the parts of a level above once SIFT_COMPACT_FANOUT of them are there; the parts of a level that are under
 * SIFT_COMPACT_FLOOR_BYTES, when two are; the parts with sift.dead_rewrite_pct of dead rows; and the hot parts that aged past hot_cut (PG_INT64_MIN: none). The cold parts of a level are
 * planned apart from the others. A group holds at most budget bytes of parts (the smallest first).
 */
extern SiftCompactGroup *sift_compact_plan(const SiftPartDesc *parts, int nparts, uint64 budget, int64 hot_cut, int *ngroups);

/*
 * What the column-delta chain of the partition adds to the plan: the parts it has records for (fold_gens) are taken with the others of their level, and the time bounds of the parts do not hold
 * while it patches a time column (dirty_time, SIFT_CDLA_DIRTY_TIME0/1). The parts of the last level are not merged by count: a merge of them rewrites rows that are in place.
 */
typedef struct SiftCompactChain
{
	const uint64 *fold_gens;
	int			nfold;
	uint16		dirty_time;
} SiftCompactChain;

extern SiftCompactGroup *sift_compact_plan_chained(const SiftPartDesc *parts, int nparts, uint64 budget, int64 hot_cut, const SiftCompactChain *chain, int *ngroups);

/* What the VACUUM of a table adds to the parts it chose itself (those with dead rows, old or of level 0): the small parts that are not frozen merged, the parts of a bulk insert re-tiered, the parts joined */
extern SiftCompactGroup *sift_compact_plan_rewrites(const SiftPartDesc *parts, int nparts, uint64 budget, int *ngroups);

/*
 * What a host folds into the parts a compaction rewrites besides their rows: a table, the column deltas of the partition (table/f2/col_delta/col_delta.h). The host installs it once; with none,
 * a compaction is what it was. A record may reach the partition while the outputs are written, so open serves every compaction and commit reads the host's records again under the lock.
 */
typedef struct SiftCompactFold
{
	/* Before the rows are read: the fold of what the partition holds for the parts of gens (ascending), as of the horizon; parts are all the partition's */
	void	   *(*open) (Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, FullTransactionId horizon);
	/* The leaves whose values are folded in: read as strings, not as the codes of a merged dictionary */
	const bool *(*folded_leaves) (void *fold);
	/* The part has values to fold in: its granules are read, not copied as they are */
	bool		(*folds_part) (void *fold, uint64 part_gen);
	/* Rows of a granule of the part from row0 on, as stored, with the folds written over them; the extents of the wide values they replace are added to *release */
	void		(*apply) (void *fold, uint64 part_gen, uint32 granule_id, uint32 row0, SiftRowBuf *rows, SiftExtent **release, int *nrelease);
	/* The addresses the n rows of a granule of the part have in the outputs: the address changes that apply folds, laid over the stored ones in tids */
	void		(*row_tids) (void *fold, uint64 part_gen, uint32 granule_id, uint64 *tids, uint32 n);
	/*
	 * Under the partition lock, with the outputs written: what stays of the host's records goes with them into edit; *stripped is raised to the greatest writer folded, the pages of what it replaced
	 * join *release. false, with edit unchanged: two rows of the outputs have an address a record that stays names, so the record cannot tell which of them it is for; the horizon of a later
	 * compaction is past the writers of the records of the row the heap gave the address away from, and folds them.
	 */
	bool		(*commit) (void *fold, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const SiftPartDesc *out, int nout, SiftManifestEdit *edit,
						   SiftExtent **release, int *nrelease, FullTransactionId *stripped);
} SiftCompactFold;

extern PGDLLIMPORT const SiftCompactFold *sift_compact_fold;

/*
 * Compacts the parts of the given generations of the partition; false (nothing changed) when a deletion in them is not settled yet (it neither committed nor aborted before the
 * horizon), which a later pass finds settled. With a time column in the spec and a hot_cut, the rows are written as a hot and a cold part (one when only one side has rows).
 */
extern bool sift_partition_compact_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
								   SiftCompactResult *res);

/*
 * The parts of gens of the partition as one part of the level above, their granules copied as they are: no row is read, so the memory held is that of one granule. true with nothing
 * written (res->bytes_out 0) when they cannot be joined (their keys overlap, their dictionaries differ...); false, as for sift_partition_compact_online, when it has to wait.
 */
extern bool sift_partition_join_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
									   SiftCompactResult *res);

/* sift_partition_compact_online with the rows cut at cut: a part that holds rows on both sides of it is rewritten as one part of each side */
extern bool sift_partition_cut_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, SiftCompactCut cut, FullTransactionId horizon,
									  SiftCompactResult *res);

/*
 * Neighbouring partitions that hold less than SIFT_PARTITION_MERGE_BYTES together and have no slab and no pending deletes become one partition (the manifest keeps the lower one), the
 * parts of both in it: a partition that compaction and VACUUM left small is not kept apart. Returns the partitions that went.
 */
extern int	sift_compact_merge_neighbours(Relation rel);

/* Packs the tiny parts of the index into shared pages (col/f2/pack.c); the parts moved */
extern int	sift_compact_pack_tails(Relation rel);

#endif							/* SIFT_COMPACT_H */
