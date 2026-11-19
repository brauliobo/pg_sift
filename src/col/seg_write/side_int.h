/*
 * col/seg_write/side_int.h - the build of the scope sidecar (side.c) and its text, varchar and uuid members (side_member.c)
 */
#ifndef SIFT_COL_SEG_WRITE_SIDE_INT_H
#define SIFT_COL_SEG_WRITE_SIDE_INT_H

#include "postgres.h"

#include "core/host.h"
#include "utils/hsearch.h"
#include "col/seg_int.h"
#include "col/seg_write/seg_write_int.h"
#include "col/side.h"

#define SIDE_MAX_CELLS	256		/* months of one run; a run of more keeps the cube of all its rows only */

typedef struct SideRows
{
	int			nleaf;
	int			leaf[SIFT_COL_MAX_COLS];
	int64	  **vals;			/* [nleaf] the key's rows of the open run, in unit order; room for the rows of a run (cap) and the padding a decode writes */
	uint64	  **nulls;
	uint64	  **ovf;
	bool	   *anynull;
	bool	   *anyovf;
	bool	   *ready;
	int64	   *time;			/* the time leaf of the rows, and its null bits */
	uint64	   *tnull;
	bool		tany;
	int64	   *tmp;			/* a decoded leaf of a unit that holds rows of other keys, before its rows are picked */
	uint64	   *tmpnull;
	uint64	   *tmpovf;
	uint32	   *sel;
	uint32		rows;
	uint32		cap;			/* the rows of a run: side_run_rows */
	int32	   *cellix;			/* [rows] the month of a row: its id in cells */
	int64		cells[SIDE_MAX_CELLS];
	uint16		order[SIDE_MAX_CELLS];	/* the ids by month */
	uint32		ncells;
	bool		cells_over;
} SideRows;

/*
 * The bytes of the sidecar in the order of the segment's pages. They wait in a buffer of side_sink_bytes() while the sidecar fits it, and the pages are then asked for to the byte (the
 * stream is exactly as long as the sidecar); a sidecar that outgrows the buffer is put to the pages as it is made, in the runs the writer takes for a stream of no known length.
 */
typedef struct SideSink
{
	SiftWriteTarget *wt;
	MemoryContext cx;			/* what the stream keeps: it outlives the scope that starts it */
	uint64		gen;
	SiftStreamWriter *sw;		/* NULL while the buffer holds the sidecar */
	char	   *buf;
	Size		cap;
	Size		len;
	uint64		pos;			/* the bytes put since the start of the sidecar: the offset of the next one */
} SideSink;

/* The values of a dictionary as members of the pairs: the code in the map of the pairs (SIDE_NO_MEMBER when no pair has the value) and the journal word of each */
#define SIDE_NO_MEMBER	PG_INT64_MAX
typedef struct MemberCodes
{
	uint32		n;
	int64	   *member;
	uint64	   *word;
} MemberCodes;

typedef struct FilterAt
{
	uint32		units[SIDE_RUN_UNITS];	/* the key: the units of a run, 0 past the last (ascending, so only the first can be 0) */
	int64		lo;
	int64		nbits;
} FilterAt;

struct SiftSideBuild
{
	SiftColSegment *seg;
	const SiftColSchema *schema;
	SiftCPaggSpec spec;
	SiftCPaggSpec calspec;
	int			member;
	int			time;
	bool		time_date;
	SideRows	rows;
	uint32		nunits;
	uint32		units[SIDE_RUN_UNITS];
	int64		lo;
	int64		hi;
	const int64 *zmin;			/* the zone of the member leaf of each unit (an integer member; NULL for a string one) */
	const int64 *zmax;
	SiftKeyMap *mm;				/* a string member: the map its pairs' codes are of */
	MemberCodes *segmap;		/* ... the dictionary of the segment's values as members */
	uint64	   *words;			/* ... the journal words of the members of the open run's units, with repeats (room for the rows of SIDE_RUN_UNITS of the largest units) */
	uint32		nwords;
	HTAB	   *filter_at;		/* the member filters of the runs are put once for the runs of the same units: where the filter of a list of units is (FilterAt) */
	MemoryContext ucx;			/* what a unit with strings of its own makes, reset after the unit */
	uint32	   *key_us;			/* ... the units of the key being made (room for every unit of the segment) and the bits of those in it */
	uint64	   *key_seen;
	double		row_bytes;
	SideSink   *sink;
	SideCell	cellrec[SIDE_MAX_CELLS + 1];	/* the cells of the open run, put after their cubes */
	StringInfoData keys;		/* the tables of the scope wait for its end: int64 per key, SideKey per key, SideRun per run */
	StringInfoData recs;
	StringInfoData runs;
	Size		tables_cap;		/* ... and take at most this much: the runs past it are left to be read by reference */
	bool		tables_full;
	int64		pmax;
	SideCounts *cnt;
	MemoryContext work;
};

/* The member of each of the packed entries (member << 32 | unit) */
#define MU_MEMBER(e)	((e) >> 32)

MemberCodes *side_member_codes(SiftKeyMap *mm, const SiftCDict *d, MemoryContext cx);
MemberCodes *side_unit_members(SiftSideBuild *b, uint32 u, const SiftCUnitView *v, const uint64 **nulls);
bool		side_member_in(const SiftScopePair *m, uint64 n, int64 v);
int			side_cmp_unit(const void *a, const void *b);
int			side_cmp_word(const void *a, const void *b);
void		side_put_filter(SiftSideBuild *b, SideRun *run);
uint64		side_put(SideSink *k, const void *p, Size n);
uint64	   *side_member_units(SiftSideBuild *b, uint64 *n);
uint32	   *side_key_units(SiftSideBuild *b, const uint64 *mu, uint64 nmu, const SiftScopePair *m, uint64 nm, uint32 *n);
uint64		side_max_unit_rows(const SiftColSegment *seg);

#endif							/* SIFT_COL_SEG_WRITE_SIDE_INT_H */
