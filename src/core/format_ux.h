/*
 * core/format_ux.h - how pg_sift writes a duration, a size, a count and a ratio in a NOTICE, an INFO or a LOG line, and the one-line summaries of the operations a user starts
 * (CREATE INDEX, REINDEX, VACUUM (VERBOSE), sift.maintain, sift.upgrade_table, sift.check, sift.verify), so that no two places print the same quantity two ways.
 * Portable (C library only): every writer fills the buffer of the caller and returns it, and a quantity that does not fit is cut, never overrun.
 *
 * Every summary is silent for a small, quick operation (sift_ux_should_emit_build, sift_ux_should_emit_timing): a regression test or a script that makes tiny relations sees nothing new.
 */
#ifndef SIFT_FORMAT_UX_H
#define SIFT_FORMAT_UX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIFT_UX_QUANTITY	40		/* room for any one quantity ("1,844,674,407,370,955,161 rows" is the longest) */
#define SIFT_UX_LINE		256		/* room for a summary line */

#define SIFT_UX_QUICK_US	(1000 * INT64_C(1000))	/* an operation under this long and under SIFT_UX_SMALL_ROWS rows says nothing unless it chose something */
#define SIFT_UX_SMALL_ROWS	10000
#define SIFT_UX_RATIO_FLOOR	(64 * UINT64_C(1024))	/* a heap smaller than this makes no ratio worth printing */

/* "84 ms", "3.42 s", "2m 14.5s": under a second whole milliseconds (1 at least), under a minute seconds, then minutes */
extern const char *sift_fmt_duration(char *buf, size_t len, int64_t usecs);

/* "512 B", "64.0 KB", "14.2 MB", "1.38 GB": binary units */
extern const char *sift_fmt_bytes(char *buf, size_t len, uint64_t bytes);

/* "1,000,000": groups of three digits */
extern const char *sift_fmt_count(char *buf, size_t len, uint64_t n);

/* "11.4x vs heap" for a heap that took heap_bytes and an index that takes bytes; "" when the heap is under SIFT_UX_RATIO_FLOOR or the index is empty */
extern const char *sift_fmt_ratio(char *buf, size_t len, uint64_t heap_bytes, uint64_t bytes);

/* "31.8 MB, 33.4 B/row, 11.4x vs heap": the size of what was made, the bytes of a row when there are rows, the ratio when it is worth printing */
extern const char *sift_fmt_size_summary(char *buf, size_t len, uint64_t bytes, uint64_t rows, uint64_t heap_bytes);

/* Does a CREATE INDEX or REINDEX say what it made? Not when it was quick, small and chose nothing by itself, and did not change the format of an index */
extern bool sift_ux_should_emit_build(int64_t elapsed_us, uint64_t rows, bool chose_by_itself, bool changed_format);

/* Does an explicit maintenance or check say what it did? Only when it took long enough to be noticed */
extern bool sift_ux_should_emit_timing(int64_t elapsed_us);

/* What the summaries are told; a count or a size of 0 is left out of the line (a build of 0 rows says so: rows is always printed) */
typedef struct SiftUxOp
{
	const char *am;				/* "sift_col", "sift_table" or "sift" */
	const char *name;			/* the relation or index, quoted by the caller where it needs quotes */
	const char *noun;			/* what rows are: "rows" (default) or "docs" */
	const char *extra;			/* appended inside the parentheses of the line ("2 segments, Format 1.7"), or NULL */
	const char *format_before;	/* "1.x": the format an upgrade or a REINDEX left, or NULL */
	const char *format_after;	/* "2.0" */
	int64_t		elapsed_us;
	uint64_t	rows;
	uint64_t	dead_rows;		/* VACUUM: the dead rows it removed */
	uint64_t	parts_before;
	uint64_t	parts_after;
	uint64_t	granules;
	uint64_t	bytes_before;
	uint64_t	bytes;			/* what the relation takes now */
	uint64_t	heap_bytes;
	uint64_t	pages_before;
	uint64_t	pages_after;
	const char *shrink_deferred;	/* VACUUM: why the free end of the file stayed ("streaming standby"), or NULL */
	uint64_t	free_blocks;	/* the free blocks that stay with shrink_deferred */
} SiftUxOp;

/* The headline of CREATE INDEX or REINDEX: `sift_col "i": built 1,000,000 rows in 2.41 s (31.8 MB, 33.4 B/row, 11.4x vs heap)`, or `upgraded 1.x -> 2.0 (... 114.1 MB -> 31.8 MB)` with a format_before */
extern const char *sift_ux_build_line(char *buf, size_t len, const SiftUxOp *op);

/* VACUUM (VERBOSE): `sift_col "i": removed 48,210 dead rows, compacted 6->2 parts (123 granules), size 31.8 MB (reclaimed 6.4 MB, 4888->4069 pages) in 412 ms` */
extern const char *sift_ux_vacuum_line(char *buf, size_t len, const SiftUxOp *op);

/* sift.maintain(): `sift.maintain("i"): compacted 8->3 parts, reclaimed 12.4 MB (44.2 MB -> 31.8 MB) in 640 ms`, or `already compact (0 parts changed)` */
extern const char *sift_ux_maintain_line(char *buf, size_t len, const SiftUxOp *op);

/* sift.upgrade_table(): `sift_table "t": upgraded Format 1.x -> 2.0 (1,000,000 rows, 4 parts, 118.4 MB -> 32.1 MB, 3.7x smaller) in 3.18 s` */
extern const char *sift_ux_upgrade_line(char *buf, size_t len, const SiftUxOp *op);

/* sift.check() and sift.verify() on a clean relation: `sift.check("i"): ok (Format 2.0, 4 parts, 123 granules, 1,000,000 rows verified in 310 ms)` */
extern const char *sift_ux_check_line(char *buf, size_t len, const SiftUxOp *op);

#endif							/* SIFT_FORMAT_UX_H */
