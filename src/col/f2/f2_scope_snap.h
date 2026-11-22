/*
 * col/f2/f2_scope_snap.h - the settled change of a summary, kept by the backend for the statements that follow: the rows of the parts the summary names that the snapshot does not see (deletion
 * vectors, pending deletes) and the rows it lacks that the snapshot does (slabs, parts written since), each ordered by member (f2_scope_over.c), and the cubes of the keys the reads made from them.
 *
 * The rows are those of the snapshot that read them, so they serve another only while every writer of the rows precedes the xmin of both (settled: what a transaction that ended decided cannot
 * change) and the parts, slabs and pending deletes are as they were (the stamp). A slab of heap references is read from the heap and is never kept.
 */
#ifndef SIFT_F2_SCOPE_SNAP_H
#define SIFT_F2_SCOPE_SNAP_H

#include "col/f2/f2_scope_int.h"
#include "col/scope_v6/overlay.h"

/* The cube a read of a key made for the same query and the same edits of the pairs of the key: it depends on the summary, on the rows kept, on the members whose pairs changed and whether the key holds them */
typedef struct SiftF2ScopeRead
{
	int64		key;
	int32		month;
	bool		minmax;
	bool		distinct;
	int64	   *changed;
	int64	   *own;
	uint32		nchanged;
	uint32		nown;
	bool		has;
	SiftScopeCubeAcc cube;
} SiftF2ScopeRead;

struct SiftF2ScopeSnap
{
	uint64		stamp;
	MemoryContext cx;
	struct SiftF2ScopeSnap **slot;	/* where the cache entry holds it; NULL: it belongs to the statement */
	struct SiftF2ScopeRowSet *added;
	struct SiftF2ScopeRowSet *gone;
	bool		gone_read;
	bool		gone_many;
	SiftF2ScopeRead *reads;
	uint32		nreads;
	uint32		next;			/* the oldest read, the one the next replaces: no read goes before 128 others have */
};
typedef struct SiftF2ScopeSnap SiftF2ScopeSnap;

/*
 * The stamp of the parts, slabs and pending deletes of the manifest as p holds them, and whether what they name is settled for the snapshot of p: a statement that holds a snapshot of another kind,
 * or a slab of heap references, or a writer that has not ended before the xmin, keeps nothing.
 */
extern bool sift_f2s_overlay_stamp(SiftF2ScopeParts *p, const SiftManifestRead *mr, uint64 *stamp);

/* The settled change of the summary cached (an entry of the cache) as of stamp, NULL when it has none; a new one, to be filled and kept (cx is its own context), or dropped */
extern SiftF2ScopeSnap *sift_f2_scope_snap_find(const SiftScopeMeta *cached, uint64 stamp);
extern SiftF2ScopeSnap *sift_f2_scope_snap_start(const SiftScopeMeta *cached, uint64 stamp);
extern void sift_f2_scope_snap_keep(const SiftScopeMeta *cached, SiftF2ScopeSnap *sn);
extern void sift_f2_scope_snap_drop(SiftF2ScopeSnap *sn);
/* The change grew: it stays with the backend when the budget of the caches has room for it, else it goes with the statement of reader */
extern void sift_f2_scope_snap_grown(SiftF2ScopeSnap *sn, MemoryContext reader);

/* The cube a read of key made before for the same query and the same edits (*has false: the key had no row; else out holds a copy) */
extern bool sift_f2_scope_snap_recall(const SiftF2ScopeSnap *sn, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed, SiftScopeCubeAcc *out, bool *has);
/* Keeps the cube (NULL: no row) of an exact read of key */
extern void sift_f2_scope_snap_remember(SiftF2ScopeSnap *sn, int64 key, const SiftScopeQueryNeeds *q, const SiftScopeEdit *ed, const SiftScopeCubeAcc *cube);

#endif							/* SIFT_F2_SCOPE_SNAP_H */
