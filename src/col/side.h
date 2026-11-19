/*
 * col/side.h - the scope sidecar of an entity segment (docs/COL-SCOPE.md "Scope sidecar"): for each scope key that has members in the segment, the runs of the units that hold their rows,
 * each with the summaries of the key's rows in them, and no copy of any row.
 *
 * A run is at most SIDE_RUN_UNITS units of the segment (not necessarily adjacent) and SIDE_RUN_ROWS rows of the key; a unit that also holds the rows of other keys' members is summarised for the
 * key's rows alone. The summary of a run is the cube of its rows for each calendar month they fall in (the bytes a unit's cube has: a month of a run is a unit that lies inside one calendar
 * cell) and the merge of those cubes for all time. The sidecar is whole pages appended to the segment's stream before its net pre-aggregates: the bodies of its scopes, then (after the padding that ends the last
 * page) the SideScope records, the header and a trailer that says where the sidecar starts, so that it is written in one pass whatever its length.
 * The pairs, the journal and the dirty bits of the units decide whether a run may be read (col/scope.h); nothing here says that a run is valid.
 */
#ifndef SIFT_COL_SIDE_H
#define SIFT_COL_SIDE_H

#include "col/am.h"
#include "col/seg.h"
#include "col/scope.h"

#define SIDE_MAGIC			0x45444953U		/* "SIDE" */
#define SIDE_TRAILER		(2 * sizeof(uint64))	/* the last bytes of the sidecar: SIDE_MAGIC and where it starts in the segment's stream */
#define SIDE_VERSION		1
#define SIDE_RUN_UNITS		16
#define SIDE_RUN_ROWS		SIFT_SUPER_KEY_ROWS
#define SIDE_MIN_UNITS		16				/* a segment of fewer units has no sidecar: its rows are read by reference */
#define SIDE_CELL_ALL		PG_INT64_MIN	/* SideCell.cell of the cube of all the rows of a run */
#define SIDE_CELL_NONE		(PG_INT64_MIN + 1)	/* ... of the rows with no month (a NULL time or one a calendar cell cannot hold) */
#define SIDE_MEMBER_RANGE	0				/* SideScope.member_kind: an integer member, a run says which members it may hold by the range of its units (lo, hi) */
#define SIDE_MEMBER_WORDS	1				/* ... a text, varchar or uuid member: by a Bloom filter over the journal words of the members its units hold */
#define SIDE_FILTER_BITS_PER_MEMBER	10		/* the bits a member costs in the filter of a run (about 1% of the words it does not hold pass) */
#define SIDE_FILTER_HASHES	7
#define SIDE_FILTER_BITS_PER_ROW	4		/* the filter of a run has at most this many bits for each row of its units (a larger one gives up precision, never an answer) */

/* Ends the sidecar, in front of its trailer, and the SideScope[nscope] are in front of it */
typedef struct SideHeader
{
	uint32		magic;
	uint32		version;
	uint32		nsb;			/* the units of the segment the sidecar was made for */
	uint32		nscope;			/* SideScope records */
	uint64		scope_off;		/* SideScope[nscope] */
	uint64		len;			/* up to the end of the bodies of the scopes */
} SideHeader;

/* The keys of one scope of the index in this segment, ascending (the key, or the code of a text, varchar or uuid key in the value store of the key column): int64 key[nkeys] at keys_off, SideKey[nkeys] at recs_off, SideRun[nruns] at runs_off */
typedef struct SideScope
{
	uint32		ordinal;		/* the ordinal of the scope in the declaration */
	uint32		nkeys;
	uint32		nruns;
	uint32		member_kind;	/* SIDE_MEMBER_RANGE, SIDE_MEMBER_WORDS */
	uint64		keys_off;
	uint64		recs_off;
	uint64		runs_off;
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];	/* the journals of the scope's relations as the pairs of the runs were read: a pair that changed since is not in the runs (sift_scope_changed) */
} SideScope;

typedef struct SideKey
{
	uint32		first_run;		/* index into the runs of the scope: SideRun at runs_off */
	uint32		nruns;
} SideKey;

/* Followed by the runs of the scope ordered by (key, first unit): SideRun[], at runs_off of the scope's record */
typedef struct SideRun
{
	int64		lo;				/* the smallest and largest member key in the units of the run (the zone bounds of its units); of a scope of a string member: where its member filter is, and */
	int64		hi;				/* ... its bits (a multiple of 64): a Bloom filter of the journal words (sift_member_hash) of every member its units hold, the key's and the others' */
	int64		pmax;			/* the largest hi of the key's runs up to this one (0 for a filter) */
	uint32		rows;			/* the key's rows in the run */
	uint32		nunits;
	uint64		units_off;		/* nunits varints: the first unit, then the gaps */
	uint32		ncells;			/* SideCell records at cells_off, ascending by cell, the cube of all the rows first */
	uint32		first;			/* the first unit of the run: the runs of a key ascend by it */
	uint64		cells_off;
} SideRun;

typedef struct SideCell
{
	int64		cell;			/* the first day (since 2000-01-01) of the month; SIDE_CELL_ALL, SIDE_CELL_NONE */
	uint32		rows;
	uint32		len;			/* bytes of the cube blob */
	uint64		off;			/* in the blob area */
} SideCell;

StaticAssertDecl(sizeof(SideHeader) == 32 && sizeof(SideScope) == 88 && sizeof(SideKey) == 8 && sizeof(SideRun) == 56 && sizeof(SideCell) == 24, "the sidecar is a file format");

/* Reader (col/seg/side.c): the sidecar of the segment, NULL when it has none. The keys, runs and cells point into memory the segment's context owns */
typedef struct SiftSide SiftSide;
extern const SiftSide *sift_cseg_side(SiftColSegment *seg);
extern const SideScope *sift_side_scope(const SiftSide *sd, uint32 ordinal);
/* The keys of the scope ascending, and with recs the first run and the runs of each */
extern const int64 *sift_side_keys(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, const SideKey **recs);
/* The runs of key in the scope (NULL, *n 0: none) */
extern const SideRun *sift_side_runs(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, int64 key, uint32 *n);
extern uint32 sift_side_run_units(SiftColSegment *seg, const SiftSide *sd, const SideRun *r, uint32 *units);
extern const SideCell *sift_side_run_cells(SiftColSegment *seg, const SiftSide *sd, const SideRun *r);
extern const uint8 *sift_side_cell_cube(SiftColSegment *seg, const SiftSide *sd, const SideCell *c);
/* The member filter of a run of a scope of a string member (nbits bits, in the caller's memory context) */
extern uint64 *sift_side_run_filter(SiftColSegment *seg, const SiftSide *sd, const SideRun *r, uint64 *nbits);
/* A leaf of a string column of unit u as codes (dst, a row each; *nulls as for sift_side_unit_leaf) of the dictionary of the segment (the return false) or, for a unit that stores its strings inline, of one the call makes in mcxt (true) */
extern bool sift_side_unit_strs(SiftColSegment *seg, uint32 u, const SiftCUnitView *v, int leaf, MemoryContext mcxt, int64 *dst, const uint64 **nulls, SiftCDict **dict);
/* A leaf of unit u decoded into dst (SIFT_COL_PAD of room past the rows); *nulls, *ovf point into the chunk or are NULL; false: the chunk holds no keys (a string stored inline) */
extern bool sift_side_unit_leaf(SiftColSegment *seg, uint32 u, const SiftCUnitView *v, int leaf, int64 *dst, const uint64 **nulls, const uint64 **ovf);
/* The pages of the stream the sidecar takes (0: none) and where it starts: a new one starts there, before the net pre-aggregates */
/* Where the sidecar starts in the payload of the segment (it has one) */
extern uint64 sift_side_at(SiftColSegment *seg);

/*
 * Writer (col/seg_write/side.c): the sidecar of one entity segment from the pairs of its scopes, in whole pages (padded, with the scopes, the header and the trailer at the end). It is put to the
 * segment's pages in the order of its bytes: each run's cubes, cells and units as the run is closed, the keys and runs of a scope when it ends. The bytes wait in memory (a quarter of
 * maintenance_work_mem) while the sidecar fits, so that its pages are taken to the page, and past it are put to the pages as they come; no temporary file is written. A scope whose run records would pass
 * an eighth of maintenance_work_mem keeps the runs it has: the keys after them are read by reference.
 */
typedef struct SiftSideBuild SiftSideBuild;
typedef struct SiftSideImage SiftSideImage;
typedef struct SideCounts
{
	uint64		keys;			/* scope keys that have a run */
	uint64		runs;
	uint64		units;			/* units in runs */
	uint64		impure_units;	/* ... of them with rows of other keys */
	uint64		rows;			/* the key rows summarised: the entries the sidecar stands for */
	uint64		cells;
	uint64		cell_bytes;
	uint64		filters;		/* runs with a member filter (a string member) */
	uint64		filter_bytes;
	uint64		bytes;			/* the sidecar, in whole pages */
} SideCounts;
/* The pairs of one scope of the index and the stamps of its journals as they were read: what the sidecar of a segment is made from. mm: the map the members of the pairs are codes of (a text, varchar or uuid member), NULL for an integer member */
typedef struct SideScopeInput
{
	int			scope;
	const char *name;			/* the key name of the scope, for what is said about its sidecar */
	const SiftScopePair *pairs;	/* ascending by (key, member) */
	uint64		npairs;
	SiftKeyMap *mm;
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];
} SideScopeInput;
/* The sidecar of a segment of nsb units for nscope scopes, to start at at in its stream and be put to wt's pages; the scopes are added in the order of the inputs */
extern SiftSideImage *sift_side_image_begin(SiftWriteTarget *wt, uint64 gen, uint32 nsb, uint32 nscope, uint64 at);
extern void sift_side_image_scope(SiftSideImage *im, SiftColSegment *seg, const SiftColRel *r, const SideScopeInput *in, SideCounts *cnt);
/* Ends the sidecar and its stream: the extents of the pages it took */
extern SiftExtent *sift_side_image_finish(SiftSideImage *im, SideCounts *cnt, int *nextents);
/* The Bloom filter of nbits bits (a multiple of 64) over the 64-bit words: add and test (false: the word is not in it) */
extern void sift_side_filter_add(uint64 *bits, uint64 nbits, uint64 word);
extern bool sift_side_filter_has(const uint64 *bits, uint64 nbits, uint64 word);

/*
 * Which runs a read may take (col/side_valid.c): the read's index, scope, snapshot and keys; sift_side_run_valid is the SiftColRunValid of sift_col_side_read, with the object as its argument
 */
typedef struct SideValid SideValid;
extern SideValid *sift_side_valid_open(Relation index, int scope, Snapshot snap, SiftKeyMap *km, const int64 *keys, uint32 nkeys, MemoryContext mcxt);
extern bool sift_side_run_valid(void *arg, SiftColSegment *seg, const SideRun *run);
/* A text, varchar or uuid member: the words of the members the read lists (a copy is kept); sift_side_members_covered is the SiftColSegCovers of sift_col_side_strict, with the object as its argument */
extern void sift_side_valid_members(SideValid *v, const uint64 *words, uint32 n);
extern bool sift_side_members_covered(void *arg, SiftColSegment *seg);
/* The members that changed since a sidecar's stamp past which its runs are not taken: the read pays for them one by one in the journal, and the segment is cheaper read by reference (or made again) */
#define SIDE_CHANGED_CAP	65536
/* No member of changed (ascending) lies in the span of the run, or, for a string member, has its word in the run's filter */
extern bool sift_side_run_untouched(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, const SiftConnKeys *changed, const SideRun *run);

/* What the sidecars hold of a key (col/side_count.c): its runs, their units and rows, the rows of the segments that have a sidecar, and the units and rows of those that have none (read by reference) */
typedef struct SideKeyCounts
{
	uint64		runs;
	uint64		units;
	uint64		rows;
	uint64		side_rows;
	uint64		other_units;
	uint64		other_rows;
} SideKeyCounts;
extern void sift_side_key_counts(Relation index, int scope, int64 key, SideKeyCounts *c);

/* Maintenance (col/maint/side.c): writes the sidecar of every entity segment that qualifies and has none; returns the segments written */
extern bool sift_col_side_due(Relation index);
extern int	sift_col_side_run(Relation index);
/* The end of a build, or of the validation pass of a concurrent one: the sidecars the index lacks, under the maintenance lock, from the pairs as of now */
extern void sift_col_side_build(Relation index);
extern int	sift_col_side_fill(Relation index, const SideScopeInput *in, int nin, SideCounts *cnt);

#endif							/* SIFT_COL_SIDE_H */
