/*
 * col/scope_v6/overlay.h - a summary served while the pairs move under it: the cube of a key after pair edits, the rows the summary does
 * not hold folded in, and the rows of a slice of a time window read from the key's own rows. Portable: the entity side (granules, deletion vectors, ownership) is SiftScopeEntity.
 */
#ifndef SIFT_COL_SCOPE_V6_OVERLAY_H
#define SIFT_COL_SCOPE_V6_OVERLAY_H

#include "core/host.h"

#include "col/scope_v6/meta.h"

#define SIFT_SCOPE_OVERLAY_MAX_PAIR_EDITS	256U

/* A window that holds a part of a month is read as the month's cube less the rest of the month when the rest is at most this part of the window */
#define SIFT_SCOPE_REST_RATIO				4

/* How a read is served: the summary as it is, or with the rows it lacks or counts too many (slabs, parts it does not name, deletes, inserters not settled) and the pair edits of the journal overlaid */
typedef enum SiftScopeServeMode
{
	SIFT_SERVE_CLEAN_CUBE,
	SIFT_SERVE_DELTA_CUBE
} SiftScopeServeMode;

/*
 * The members of a key whose pairs changed since the summary was written. The summary cannot say who was in the key before (a level-1 node keeps its members as a Bloom filter or, when few, as a
 * list), so a member is told added, removed or unchanged (exact) only when no node, or a node with a list, answers for it; else the read rebuilds the key from the nodes that may hold one.
 */
typedef struct SiftScopeEdit
{
	int64	   *changed;			/* ascending: the members the journal names, less those that kept their key */
	uint32		nchanged;
	int64	   *own;				/* ascending: of those, the ones the key holds now */
	uint32		nown;
	int64	   *added;				/* when exact: the ones that were not in the key and are */
	uint32		nadded;
	int64	   *removed;			/* when exact: the ones that were in the key and are not */
	uint32		nremoved;
	bool		exact;
} SiftScopeEdit;

/* changed (ascending), kept[i] (may be NULL): the key held the member for every snapshot, so it did not change. e->owns tells the pairs as they are now; r NULL: the key is not in the summary */
extern void sift_scope_overlay_classify(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 key, const SiftScopeEntity *e, const int64 *changed, const bool *kept, uint32 n, SiftScopeEdit *out);
extern void sift_scope_edit_free(SiftScopeEdit *ed);

/*
 * The cube of key r (NULL: the key is not in the summary) for the query, its live rows under the pairs as they are now. No edit: the cube with the dead rows out. A told-apart edit of up to
 * SIFT_SCOPE_PATCH_MEMBERS members: that cube patched with the cubes of the members. Else, or when a patch cannot keep a minimum or maximum: the level-1 nodes that hold no changed member and no dead
 * row are read as they are, the others from their live rows, and the rows of the members the key holds now that no touched node reads are added from the member's own rows. Exact whatever the Bloom
 * filters say. false: the key has no row for the query. *exact false: a minimum or maximum the read needs cannot be kept: the read must not answer from the summary.
 */
extern bool sift_scope_overlay_key_cube(const SiftScopeMeta *m, const SiftScopeL2Root *r, SiftScopeShape shape, const SiftScopeQueryNeeds *q, const SiftScopeEntity *e, const SiftScopeEdit *ed, SiftScopeCubeAcc *out, bool *exact);

/* The rows of r that no cube holds (a slab, a new part, a transaction's stage), the ones the pairs of the keys own and the window [lo, hi) holds, added to acc; skip[i] set: row i is not live */
typedef struct SiftScopeFold
{
	const int64 *keys;			/* the queried keys, ascending, distinct */
	uint32		nkeys;
	bool		semi;				/* a row counts once, whatever the number of keys that own its member; else once for each */
	int64		lo;
	int64		hi;					/* the half-open window of the time leaf; lo == PG_INT64_MIN and hi == PG_INT64_MAX: none */
	int32		month;				/* SIFT_SCOPE_NO_MONTH: every month, else only the rows of the month */
} SiftScopeFold;

extern uint32 sift_scope_overlay_fold(SiftScopeCubeAcc *acc, const SiftScopeRows *r, const uint8 *skip, const SiftScopeEntity *e, const SiftScopeFold *f);

/*
 * The live rows of key (r: its root, NULL when the summary has none) whose time is in the span, a part of a month or more, added to acc (initialized by the caller); the granules read. They are the rows
 * of the granules of the key (those of its month map and strides that overlap) that it owns under the pairs as they are, the rows of the members in changed (ascending: those whose pairs changed
 * since the summary, which may be in granules the key never had) that it owns now, and the rows the summary lacks (added_cube). Every row is added as the build adds it, so count, sum, avg, min and max
 * of every leaf are exact whatever was deleted or moved.
 */
extern uint32 sift_scope_overlay_slice(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 key, const SiftScopeEntity *e, const int64 *changed, uint32 nchanged, SiftScopeSpan span, SiftScopeCubeAcc *acc);

#endif							/* SIFT_COL_SCOPE_V6_OVERLAY_H */
