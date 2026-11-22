/*
 * core/f2/f2_ux.h - the words of the live feedback of Format 2.0 (core/f2/f2_progress.h): what a CREATE INDEX, a VACUUM (VERBOSE) and a maintenance of a sift_col index say, and what a
 * VACUUM (VERBOSE) of a sift_table says, written with the writers of core/format_ux.h. Portable (C library only): every writer fills the buffer of the caller and returns it, and a
 * line that does not fit is cut, never overrun.
 */
#ifndef SIFT_F2_UX_H
#define SIFT_F2_UX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/format_ux.h"

/* The chunks of the granules read, by the way they are coded */
typedef struct SiftF2Codecs
{
	uint64_t	constant;
	uint64_t	delta;
	uint64_t	packed;
	uint64_t	inlined;
	uint64_t	coded;
} SiftF2Codecs;

/* What an index holds: its parts and granules, and the codecs of a sample of its granules */
typedef struct SiftF2Census
{
	uint64_t	parts;
	uint64_t	granules;
	SiftF2Codecs codecs;
} SiftF2Census;

/* What the build chose by itself */
typedef struct SiftF2Choices
{
	bool		key_order;
	bool		paths;
	bool		scope;
} SiftF2Choices;

/* What a VACUUM or a maintenance of an index saw before and after */
typedef struct SiftF2IndexFacts
{
	const char *name;
	int64_t		elapsed_us;
	uint64_t	dead_rows;
	uint32_t	block_size;
	uint64_t	pages_before;
	uint64_t	pages_after;
	SiftF2Census before;
	SiftF2Census after;
	bool		shrink_deferred;
	uint64_t	free_blocks;
} SiftF2IndexFacts;

/* What a VACUUM of a sift_table saw before and after */
typedef struct SiftF2TableFacts
{
	const char *name;
	int64_t		elapsed_us;
	uint64_t	removed;
	uint32_t	batches;
	uint64_t	live;
	uint64_t	dead_not_removable;
	uint64_t	parts_before;
	uint64_t	parts_after;
	uint64_t	bytes_before;
	uint64_t	bytes_after;
} SiftF2TableFacts;

/* "3 const, 12 delta, 4 packed, 9 inline, 2 coded": the codecs that are used, "" when none is */
extern const char *sift_f2_ux_codecs(char *buf, size_t len, const SiftF2Codecs *c);

/* "4 parts, 123 granules, codecs: 3 const, 12 delta": the inside of the parentheses of the build line after the size */
extern const char *sift_f2_ux_build_extra(char *buf, size_t len, const SiftF2Census *c);

/* "key_order, paths, scope": what was chosen, "" when nothing */
extern const char *sift_f2_ux_choices(char *buf, size_t len, const SiftF2Choices *c);
extern bool sift_f2_ux_chose(const SiftF2Choices *c);

/* Is an operation of elapsed_us over the sift.log_min_duration of log_min_ms (-1: never) */
extern bool sift_f2_ux_log_due(int64_t elapsed_us, int log_min_ms);

/* The operations for the writers of core/format_ux.h: a VACUUM names the parts it compacted only when their number changed, a maintenance always says them */
extern void sift_f2_ux_vacuum_op(SiftUxOp *op, const SiftF2IndexFacts *v);
extern void sift_f2_ux_maintain_op(SiftUxOp *op, const SiftF2IndexFacts *v);

/* `sift_table "t": removed 120,400 rows (1 batch), 9,879,600 live, 12 dead-not-removable; parts 18->12, size 312.4 MB (reclaimed 24.8 MB) in 1.14 s` */
extern const char *sift_f2_ux_table_vacuum_line(char *buf, size_t len, const SiftF2TableFacts *t);

#endif							/* SIFT_F2_UX_H */
