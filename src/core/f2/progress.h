/*
 * core/f2/progress.h - what a user sees while an index or a table of format major 2 is built, vacuumed or maintained, and the one line it says when it is done.
 *
 * Live: pg_stat_progress_create_index (the subphase, the tuples and the scopes of a CREATE INDEX or a REINDEX, serial or parallel, concurrent or not), pg_stat_progress_vacuum (a VACUUM of a
 * sift_table) and sift.progress (the VACUUM and the maintenance of a sift_col index, which has no row in pg_stat_progress_vacuum: that one belongs to the heap). The counters are written
 * once for each granule of rows, never for each row.
 *
 * Said: one NOTICE after a CREATE INDEX or a REINDEX that was not quick and small and chose nothing by itself, one INFO for each relation of a VACUUM (VERBOSE), one NOTICE after a maintenance
 * that took long enough to be noticed. A plain VACUUM, an autovacuum and a small quick build say nothing. No option: client_min_messages and VACUUM (VERBOSE) are the switches.
 */
#ifndef SIFT_F2_PROGRESS_H
#define SIFT_F2_PROGRESS_H

#include "access/genam.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/granule/granule.h"
#include "core/f2/ux.h"
#include "core/worker.h"

/*
 * The subphases of a build (PROGRESS_CREATEIDX_SUBPHASE; 1 is the initializing that PostgreSQL itself reports). ambuildphasename gets the number and no relation, in the session that reads
 * pg_stat_progress_create_index, so the numbers of the formats are disjoint: major 1 reports 2 and 3 (col/v1/build.h), major 2 from 10 up.
 */
#define SIFT_F2_PHASE_SAMPLE	10	/* sampling schema and key order */
#define SIFT_F2_PHASE_SCAN		11	/* scanning table */
#define SIFT_F2_PHASE_WRITE		12	/* writing parts */
#define SIFT_F2_PHASE_SCOPE		13	/* building scope summaries */
#define SIFT_F2_PHASE_TRACK		14	/* installing change tracking */

/* The counters of the rows are written when a granule of them has been read */
#define SIFT_F2_PROGRESS_GRANULE_ROWS	8192

/* The name of a subphase for ambuildphasename (NULL for none) */
extern char *sift_f2_ambuildphasename(int64 phase);

/* ---- CREATE INDEX and REINDEX ---- */

/* What the options say the build chooses from the data; read before the build writes the choices down */
extern SiftF2Choices sift_f2_choices(Relation index);

/* The leader enters a subphase (a parallel worker has no progress of its own) */
extern void sift_f2_progress_phase(int phase);

/* The tuples the heap is expected to have, from the statistics of the table */
extern void sift_f2_progress_heap(Relation heap);

/* The rows a participant has read, counted for the whole build whoever the participant is */
typedef struct SiftF2RowTicker
{
	uint32		pending;
} SiftF2RowTicker;

extern void sift_f2_progress_rows_flush(SiftF2RowTicker *t);

static inline void
sift_f2_progress_row(SiftF2RowTicker *t)
{
	if (++t->pending >= SIFT_F2_PROGRESS_GRANULE_ROWS)
		sift_f2_progress_rows_flush(t);
}

/* The scopes of the index are built: the subphase, the number of them, and all of them done (a scope is not counted when it is done: they are built together) */
extern void sift_f2_progress_scopes(Relation heap, Relation index);
extern void sift_f2_progress_scopes_done(Relation heap, Relation index);

/* The line after the build: rows are those the index holds; start the time the build began at */
extern void sift_f2_say_build(Relation heap, Relation index, TimestampTz start, uint64 rows, SiftF2Choices choices);

/* ---- the file of an index ---- */

/* The parts and granules of the relation, and with deep the codecs of the first granule of each of the first parts */
extern SiftF2Census sift_f2_census(Relation rel, bool deep);

typedef enum SiftF2CodecClass
{
	SIFT_F2_CODEC_CONST,
	SIFT_F2_CODEC_DELTA,
	SIFT_F2_CODEC_PACKED,
	SIFT_F2_CODEC_INLINE,
	SIFT_F2_CODEC_CODED
} SiftF2CodecClass;

extern SiftF2CodecClass sift_f2_codec_class(const SiftChunkRef *ref);

/* ---- VACUUM and maintenance of an index ---- */

typedef struct SiftF2Trace
{
	Relation	index;
	TimestampTz start;
	uint64		pages_before;
	SiftF2Census before;
	bool		began;
	bool		census;
} SiftF2Trace;

/* A job that reports in sift.progress (SIFT_JOB_NONE: none), and the file as it is when census is asked for (what the line after it compares with) */
extern void sift_f2_trace_begin(SiftF2Trace *t, Relation index, SiftJob job, bool census);
extern void sift_f2_trace_phase(const SiftF2Trace *t, const char *phase);
extern void sift_f2_trace_step(int64 done, uint64 bytes);
extern void sift_f2_trace_end(SiftF2Trace *t);

/* The INFO line of a VACUUM (VERBOSE); nothing at any other level of message */
extern void sift_f2_say_vacuum(const SiftF2Trace *t, int elevel, double tuples_removed, bool shrink_deferred);

/* The NOTICE line of a maintenance that took long enough to be noticed */
extern void sift_f2_say_maintain(const SiftF2Trace *t);

/* ---- VACUUM of a table ---- */

/* The VACUUM starts and ends its row of pg_stat_progress_vacuum; the table has no heap blocks to scan, so the blocks of its file are shared evenly among its partitions */
extern void sift_f2_vacuum_progress_begin(Relation rel);
extern void sift_f2_vacuum_progress_total(Relation rel);
extern void sift_f2_vacuum_progress_scanned(int done, int nunits);
extern void sift_f2_vacuum_progress_phase(int phase);

/* The addresses of dead rows the VACUUM holds for the indexes (ids, and the bytes of them against what maintenance_work_mem allows) and the cycles of the indexes it has made */
extern void sift_f2_vacuum_progress_dead(uint64 ids, uint64 bytes, uint64 max_bytes, uint32 cycles);
extern void sift_f2_vacuum_progress_indexes(int nindexes, int processed);
extern void sift_f2_vacuum_progress_end(void);

/* The INFO line of a VACUUM (VERBOSE) of a table; the facts are taken as the VACUUM begins and ends */
typedef struct SiftF2TableTrace
{
	TimestampTz start;
	uint64		parts_before;
	uint64		bytes_before;
} SiftF2TableTrace;

extern void sift_f2_table_trace_begin(SiftF2TableTrace *t, Relation rel);
extern void sift_f2_say_table_vacuum(const SiftF2TableTrace *t, Relation rel, int elevel, uint64 removed, uint32 batches, double live, double dead);

#endif							/* SIFT_F2_PROGRESS_H */
