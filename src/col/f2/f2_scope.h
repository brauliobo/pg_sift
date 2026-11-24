/*
 * col/f2/f2_scope.h - the scope summaries of a relation of format major 2, a sift_col index or a sift_table (col/scope_v6/meta.h): one object for each declared scope, kept as a part of the
 * relation (SIFT_PART_FLAG_SCOPE) that the scope directory of the metapage names. The entity parts are written once; the summary is built from their granules and holds every scope key's
 * aggregates, the newest rows of every key and the granules each key takes, so that a scope key never costs a copy of its rows.
 *
 * rel is the index or the table. The rows of an index are decided by its heap and the pairs by the journal its scope index depends on (track_changes); those of a table by its own versions, its
 * deletion vectors and pending deletes, no heap page being read, and by a journal of the pairs relation that nothing records for it.
 */
#ifndef SIFT_F2_SCOPE_H
#define SIFT_F2_SCOPE_H

#include "col/f2/f2.h"
#include "col/scope.h"
#include "col/scope_v6/meta.h"
#include "col/scope_v6/overlay.h"

/* The map of the codes of the key of scope ord (a text, varchar or uuid key; NULL for an integer key), the reader's own for a statement: made in the current memory context */
extern SiftKeyMap *sift_f2_scope_keymap(Relation rel, int ord);

/* The scopes the relation declares, and whether it declares any */
extern int	sift_f2_nscopes(Relation rel);
extern bool sift_f2_scoped(Relation rel);

/* Does a scope of the index read one of the leaves: a column delta that patches none of them leaves every summary as it is */
extern bool sift_f2_scope_reads_any(Relation rel, const int16 *leaves, int n);

/* The summaries of all the scopes of the relation, built over the entity parts of the manifest as it is, replace the ones the directory names, in one commit; heap: the table of the index, rel for a table */
extern void sift_f2_scope_build(Relation heap, Relation rel);

/* After a compaction, an insert or an edit of the pairs: the summaries that no longer describe the parts or the pairs are built again; the number of them. A table the summary of whose scope cannot be built now (no journal of its pairs, a column delta, a part of another schema) is left as it is */
extern int	sift_f2_scope_refresh(Relation rel);

/* The summary of scope ord written again as it is into the extents sift_part_prefer_extents holds, the blocks it was in released: the move of the part that holds the end of the file */
extern void sift_f2_scope_move(Relation rel, int ord);

/* The blocks of the summary of scope ord (the extents of its part), and the rows of its part header: false when the scope has none */
extern bool sift_f2_scope_blocks(Relation rel, int ord, SiftExtent **ext, int *n);

/* The object of a summary as its part keeps it: packed behind a header (f2_scope_pack.c); the bytes of the object, NULL when the part holds none that reads */
extern void sift_f2_scope_pack(const StringInfoData *obj, StringInfo out);
extern char *sift_f2_scope_read_object(Relation rel, const SiftPart *part, uint64 *len);

/* An opened summary: the object, the entity parts it describes and the entity side the queries over it read (deletion vectors, strides, cubes of whole granules) */
typedef struct SiftF2Scope SiftF2Scope;

struct SiftF2Scope
{
	Relation	rel;
	MemoryContext cx;
	int			ord;
	SiftScopeMeta meta;
	const SiftScopeMeta *cached;	/* the entry of the backend's cache (f2_scope_cache.h) that meta copies: not the scope's to close, its rings unpacked by sift_f2_scope_rings; NULL when the scope read the object itself */
	int64	   *changed;		/* the members whose pairs changed since the summary for the keys the read asked for, ascending, each a member the keys may have gained or lost (f2_scope_delta.c) */
	uint32		nchanged;
	SiftScopeEntity entity;
	SiftScopeGranSource source;	/* the zones and rows of the granules of the entity parts, for a read that looks for the granules of some members */
	Snapshot	snap;
	uint64		max_part_gen;
	Relation	heap;			/* the table of the index, rel itself for a table */
	int			member_leaf;	/* the leaf of the member of the pairs */
	int			nrings;			/* the rings of the summary: its time leaf and the order leaves; ring_col[q] is the column of ring q */
	int			ring_col[SIFT_SCOPE_TOPK_RINGS_CAP];
};

/*
 * NULL when the scope has no summary yet; an ERROR when the parts it describes are gone (the refresh has not run). excl_col (a column of the relation, 0 for none) is a leaf of the query whose non-NULL
 * rows it leaves out (`WHERE deleted_at IS NULL`): the entity side counts them with the rows of the deletion vectors, so that the cubes read less them (sift_f2_scope_excl_ok).
 */
extern SiftF2Scope *sift_f2_scope_open(Relation rel, int ord, Snapshot snap, int excl_col);
/* Opened for the statement's snapshot with what the summary lacks (the parts it does not name, slabs) or counts too many (pending deletes, inserters not settled) read from the parts; NULL when the scope has no summary, the journals cannot name the pair edits, or those rows are more than a read is worth. A read of the scope keys keys[0..nkeys) (ascending, distinct): the pairs of those keys may have been edited since the summary (at most SIFT_SCOPE_OVERLAY_MAX_PAIR_EDITS members, named by the journal), which sift_f2_scope_key_cube overlays; nkeys 0: any key, and no edit is tolerated. km: the map of the codes of the scope key the keys are in (NULL: an integer key), the statement's own */
extern SiftF2Scope *sift_f2_scope_open_keys(Relation rel, int ord, Snapshot snap, int excl_col, const int64 *keys, uint32 nkeys, SiftKeyMap *km);
/* How the opened summary is served */
extern SiftScopeServeMode sift_f2_scope_mode(const SiftF2Scope *sc);
/* The rows the summary lacks that the snapshot sees (slabs, parts written since) hold some of the key that may rank among the first rows of a ring: its order leaf is not the time, or `missing` rows are still wanted, or one is as new as last */
extern bool sift_f2_scope_added_rank(const SiftF2Scope *sc, int64 key, int ring, int missing, const SiftScopeNewest *last);
/*
 * The cube of key for the query (q->month: one month, or all), the summary's with the rows the snapshot does not see out and the rows it lacks in, under the pairs as they are now. false: the key has no row
 * for the query. *exact false: a minimum or maximum the read needs cannot be kept after the edits: the read must not answer from the summary.
 */
extern bool sift_f2_scope_key_cube(SiftF2Scope *sc, int64 key, const SiftScopeQueryNeeds *q, SiftScopeCubeAcc *out, bool *exact);
/* The cube of key for the rows whose time is in [lo, hi), a part of one UTC month, under the pairs as they are now: exact for every aggregate whatever was deleted, moved or added (a minimum or maximum that the rows left out may have held is read from the rows of the window). false: the key has no row in it */
extern bool sift_f2_scope_window_cube(SiftF2Scope *sc, int64 key, const SiftScopeQueryNeeds *q, int64 lo, int64 hi, SiftScopeCubeAcc *out);
/* The ring (0: the time leaf) of an opened summary whose order leaf is the column col, -1 when it has none */
extern int	sift_f2_scope_ring_of(const SiftF2Scope *sc, int col);
/* Can the column be the order of a ring (a timestamp, a date or an integer), and can its NULLs be told from its values for a query that keeps the NULL rows of scope ord */
extern bool sift_f2_scope_ring_eligible(Relation rel, int col);
extern bool sift_f2_scope_excl_ok(Relation rel, int ord, int col);

/* One value of a leaf of a row: what the leaf holds (INT: i, F4 and F8: f, STR: s and len), isnull, and the tag of a value it could not hold (a NULL cell with a tag is not NULL, the heap has the value) */
typedef struct SiftF2ScopeCell
{
	int64		i;
	double		f;
	const char *s;
	uint32		len;
	bool		isnull;
	uint8		tag;
} SiftF2ScopeCell;

/* A row the summary lacks (of a slab, or of a part it does not name) that the snapshot sees: its time leaf and where its cells are */
typedef struct SiftF2ScopeAdded
{
	int64		time;
	uint32		src;
	uint32		g;
	uint32		row;
} SiftF2ScopeAdded;

/* The rows of the key the summary lacks with the n newest values of the time leaf (all that share the n-th), newest first; how many, UINT32_MAX when they are more than a read keeps */
extern uint32 sift_f2_scope_added_top(const SiftF2Scope *sc, int64 key, uint32 n, SiftF2ScopeAdded **out);

/* The leaves[0 .. nleaves - 1] of the rows of the entity parts that rows (gidx, row_ord of a ring) name, as cells[k * nleaves + j]; a string is made in the current memory context */
extern void sift_f2_scope_cells(SiftF2Scope *sc, const SiftScopeNewest *rows, int nrows, const int *leaves, int nleaves, SiftF2ScopeCell *cells);
/* The same for rows of sift_f2_scope_added_top */
extern void sift_f2_scope_added_cells(SiftF2Scope *sc, const SiftF2ScopeAdded *rows, int nrows, const int *leaves, int nleaves, SiftF2ScopeCell *cells);

/* The refreshes this backend has made, by how: the whole summary again, the parts that changed, the keys whose pairs changed, both */
typedef struct SiftF2ScopeRefreshes
{
	int			full;
	int			parts;
	int			dirty;
	int			combined;		/* the parts that changed and the pairs of some keys in one pass */
} SiftF2ScopeRefreshes;

extern SiftF2ScopeRefreshes sift_f2_scope_refreshes;
extern void sift_f2_scope_close(SiftF2Scope *sc);
/* The rings of a summary the cache lent are readable: a statement that reads cubes only never unpacks them; false when the section is damaged */
extern bool sift_f2_scope_rings(SiftF2Scope *sc);

/* The summary describes the entity parts of the manifest and the pairs as they are, and the schema of the leaves: false after a compaction, a part written since, an edit of the pairs or of the table */
extern bool sift_f2_scope_current(Relation rel, int ord);

/*
 * The cubes of the summary count exactly the rows the snapshot sees: current, every part read as it is, no change of this transaction that the relation has not written; of an index also that it
 * heard of every change of the heap (track_changes); of a table that no column delta of a partition patched a column of the cubes.
 */
extern bool sift_f2_scope_servable(Relation rel, int ord, Snapshot snap);

/* The columns of the relation (1-based attributes) a summary is made of: the member, the time (0: none) and the measures of its cubes in the order the cubes keep them; false when the scope is not summarized */
typedef struct SiftF2ScopeLayout
{
	int			member_col;
	int			time_col;
	int			nmeas;
	int			meas_col[SIFT_SCOPE_MEAS_CAP];
	uint32		float_meas;
	uint8		meas_scale[SIFT_SCOPE_MEAS_CAP];	/* a numeric measure: the digits after the point of its scaled integer */
} SiftF2ScopeLayout;

extern bool sift_f2_scope_layout(Relation rel, int ord, SiftF2ScopeLayout *out);

#endif							/* SIFT_F2_SCOPE_H */
