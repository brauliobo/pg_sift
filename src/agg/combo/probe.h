/*
 * agg/combo/probe.h - the direct probe of a correlated child (combo/probe.c, combo/probe_bt.c)
 *
 * The btree side (probe_bt.c) is an interface of its own: it needs a DProbe (index and heap relation, the key columns with their compare kind, the snapshot,
 * the table fetch and a slot for the heap tuple: combo/probe.c dp_shape builds it from the child's index scan), keys (DKey: the detoasted arguments in the batch
 * context, dp_abbrev, state DKS_TODO) and a sink. dp_batch sorts the keys, descends once and keeps the pages of the key before while the next key lies in their
 * ranges, walks the entries of each key in index order and hands the visible heap tuples (the AM's visibility and HOT chains) to the sink:
 *   begin(k)  before the first tuple of a key (the caller notes where its output of the key starts)
 *   row()     a tuple is in dp->scan; true when it counts as a match, k->n counts them
 * dp->limit = 1 ends a key at its first match (a first-row child; an EXISTS or NOT EXISTS is the same with a sink that keeps nothing and a caller that reads
 * k->n == 0), 0 gives all the matches in the order the index gives them (a multi-row child). A key ends DKS_ROWS or, where the descent cannot serve it, DKS_FALLBACK
 * and the caller runs its own path for it. Nothing is locked between two keys; a Param or SubPlan in what the sink evaluates must be set per key by the caller.
 */
#ifndef SIFT_AGG_COMBO_PROBE_H
#define SIFT_AGG_COMBO_PROBE_H

#include "access/relscan.h"
#include "executor/executor.h"

#include "agg/combo/int.h"

#define DP_MAX_PEND		4096		/* keys of a batch: the memory of a batch is a few hundred kB */
#define DP_MAX_LEVELS	16
#define DP_ABBREV		24			/* bytes of the sort prefix of a key */
#define DP_NOCMP		2			/* no compare to give (the compares are -1, 0 and 1) */

/* test builds count what a batch does (sift._test_probe_stats): the work the probe saves is checked by counts, which do not move with the host */
#ifdef SIFT_TESTING
typedef struct DStats
{
	int64		probes;
	int64		tuple_cmps;				/* key against an index tuple */
	int64		col_cmps;				/* of those, the columns compared (the columns known equal are not) */
	int64		generic_cmps;			/* of those, through the opclass's order function */
	int64		page_reads;				/* pages read from the buffer manager (a page kept pinned is not read again) */
} DStats;

extern DStats sift_probe_stats;
#define DP_COUNT(f, n) (sift_probe_stats.f += (n))
#else
#define DP_COUNT(f, n) ((void) 0)
#endif

typedef enum DKind
{
	DC_GENERIC,						/* the opclass's order function, as the index AM calls it */
	DC_INT,							/* int2, int4, int8, date, timestamp, timestamptz: the integers themselves */
	DC_UUID,						/* the 16 bytes */
	DC_TEXT,						/* memcmp order: text under the C collation, text_pattern_ops */
	DC_TEXTC,						/* text under another collation: equal bytes are equal, else the order function; the bytes sort the batch */
	DC_ARRAY						/* equal bytes are equal, else the order function */
} DKind;

typedef struct DCol
{
	DKind		kind;
	int16		len;
	bool		byval;
	char		align;
	uint8		alignb;				/* align in bytes */
	bool		desc;
	bool		nullsfirst;
	Oid			coll;
	FmgrInfo   *cmp;
} DCol;

typedef struct DArg
{
	Datum		d;					/* the value, detoasted, in the batch context */
	int64		i;					/* DC_INT */
	const char *p;					/* varlena kinds: the bytes */
	Size		n;
} DArg;

typedef enum DKeyState
{
	DKS_TODO,
	DKS_ROWS,						/* n rows of the child's output are in the map from start */
	DKS_FALLBACK					/* the child runs for the key */
} DKeyState;

typedef struct DKey
{
	DArg	   *a;
	uint64		ab[DP_ABBREV / 8];
	uint32		pend;
	DKeyState	state;
	uint32		start;
	int			n;
} DKey;

/*
 * what a probe does with the entries of a key: begin before the first, row for each visible heap tuple (in dp->scan), true when it counts as a match of the key;
 * keep (index-only probes, may be NULL) for each entry before the visibility map or the heap is asked about it, with the entry's columns in dp->scan: false when the
 * child's filter rejects it, whatever the heap says (an entry's columns are those of its tuple version: a rejected one is no match visible or not)
 */
typedef struct DSink
{
	void		(*begin) (struct DSink *s, DKey *k);
	bool		(*keep) (struct DSink *s);
	bool		(*row) (struct DSink *s);
} DSink;

typedef struct DLevel
{
	BlockNumber blk;
	char	   *hik;				/* the high key the page had when it was visited */
	bool		has_hik;
} DLevel;

typedef struct DProbe
{
	int8		state;				/* 0 not examined, 1 usable, -1 not */
	int			nk;					/* index columns the child's index conditions give: 1..nk */
	int			nkeyatts;			/* key columns of the index */
	int			limit;				/* 1: the first row the child gives, 0: all of them */
	PlanState  *child;
	EState	   *estate;
	bool		ioss;				/* an Index Only Scan: the columns are those of the index tuple, the heap is asked for the tuples of pages that are not all-visible */
	ScanKey		skeys;
	int			nskeys;
	IndexRuntimeKeyInfo *rkeys;
	int			nrkeys;
	ExprContext *rctx;
	ExprState  *qual;
	TupleTableSlot *tslot;			/* ioss: the heap tuple of the visibility check */
	Buffer		vmbuf;
	IndexTuple *tup;				/* ioss: the index tuple of each of tids */
	char	   *abuf;				/* ioss: the copies of the matching index tuples of the page */
	Size		alen;
	Size		acap;
	ResultState *res;
	Relation	irel;
	Relation	hrel;
	TupleTableSlot *scan;
	ProjectionInfo *p1;				/* the index scan's projection, the result's */
	ProjectionInfo *p2;
	ExprContext *ec1;
	ExprContext *ec2;
	DCol		col[INDEX_MAX_KEYS];
	int			keyat[INDEX_MAX_KEYS];	/* the scan key of index column i */
	IndexFetchTableData *fetch;
	Snapshot	snap;
	bool		ignore_dead;
	MemoryContext bcxt;
	int			npend;
	int			cap;
	uint32	   *pend;
	DKey	   *keys;
	ItemPointerData *tids;
	int			captids;
	int			first_c;			/* the descent's compare of the key with the entry it ended at, for the walk that follows (DP_NOCMP: none) */
	/* the pages of the last probe, root to leaf, valid within one batch */
	int			nlv;
	DLevel		lv[DP_MAX_LEVELS];
	Buffer		pin[DP_MAX_LEVELS];	/* the pages above the leaves the batch keeps pinned (dp_read) */
	BlockNumber root;
	bool		path_ok;
	uint32		skipped;
	int64		reads0;			/* the heap blocks the backend had read from the OS when the probe began (dp_reads) */
	uint64		leaves;			/* what the batches did, for the Lookup line of EXPLAIN (combo_probe_flush adds them to the statement's): leaf pages moved to, pages above the leaves pinned, */
	uint64		pins;
	uint64		heap;			/* heap fetches, index-only entries the visibility map answered */
	uint64		vm;
	BlockNumber lastleaf;
	uint64		asked;			/* heap blocks of the entries found that were asked of the pool and the OS ahead of their fetch (combo_probe_flush adds them to the statement's) */
} DProbe;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

extern void dp_abbrev(DProbe *dp, DKey *k);
extern int	dp_batch(DProbe *dp, DKey **ord, int n, DSink *sink);
extern int64 dp_reads(Relation rel);

#pragma GCC visibility pop

#endif							/* SIFT_AGG_COMBO_PROBE_H */
