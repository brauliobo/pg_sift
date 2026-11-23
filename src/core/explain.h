/*
 * core/explain.h - the counters of a custom scan in EXPLAIN: one line in text, one field each in the other formats
 *
 *   SiftExplainLine ln;
 *   sift_explain_begin(&ln, es, "Units");
 *   sift_explain_int(&ln, "Units", NULL, "read", n_read, SIFT_XI_KEEP);
 *   sift_explain_int(&ln, "Units Pruned", NULL, "pruned", n_pruned, 0);
 *   sift_explain_int(&ln, "Units Pruned By Set Summary", NULL, "by set summary", n_set, SIFT_XI_SUB);
 *   sift_explain_end(&ln);
 *
 * prints "Units: 384 read, 64773 pruned (2 by set summary)" in text and the three fields named in the calls in JSON, YAML and XML,
 * which machine readers keep using. A zero is left out of the text line unless EXPLAIN (VERBOSE) or SIFT_XI_KEEP; no line, if all are.
 * SIFT_XI_OPT leaves a zero out of the other formats too (the counters that exist for some scans only).
 */
#ifndef SIFT_EXPLAIN_H
#define SIFT_EXPLAIN_H

#include "commands/explain.h"
#include "utils/relcache.h"
#include "core/compat.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_state.h"
#endif

#define SIFT_XI_KEEP	1		/* text shows the counter at zero too */
#define SIFT_XI_SUB		2		/* text puts it in parentheses after the previous item (a part of it) */
#define SIFT_XI_OPT		4		/* the structured formats leave it out at zero as well */

typedef struct SiftExplainLine
{
	ExplainState *es;
	const char *label;
	StringInfoData buf;
	bool		open;			/* a parenthesis of sub items is open */
} SiftExplainLine;

extern void sift_explain_begin(SiftExplainLine *ln, ExplainState *es, const char *label);

/* A counter: field prop (with unit, if any) in the structured formats, "v unit words" in text */
extern void sift_explain_int(SiftExplainLine *ln, const char *prop, const char *unit, const char *words, int64 v, int flags);

/* A time in milliseconds, three decimals */
extern void sift_explain_ms(SiftExplainLine *ln, const char *prop, const char *words, double ms, int flags);

/* A percentage with one decimal */
extern void sift_explain_pct(SiftExplainLine *ln, const char *prop, const char *words, double pct, int flags);

/* Emits the text line; the other formats printed their fields at each call */
extern void sift_explain_end(SiftExplainLine *ln);

/*
 * EXPLAIN (SIFT) (PostgreSQL 18 and later; sift.explain_refusals is the way on 17): after the plan it says for each candidate path that was not used
 * why (the slug and sentence of sift.explain_why), and the node of a scan of a sift or sift_col index says what the index holds. Nothing changes
 * without the option.
 */
typedef struct SiftIndexStats
{
	int64		segments;		/* immutable segments */
	int64		buffered_rows;	/* rows in write buffers, not in a segment yet */
	int64		rows;			/* rows of the segments, dead ones included */
	int64		dead_rows;
} SiftIndexStats;

/* the counts of an index, from its directory and the headers of its segments (bm25/segment_sql.c, col/v1/maint/funcs.c, col/f2/f2_inspect.c) */
extern void sift_bm25_stats(Relation index, SiftIndexStats *st);
extern void sift_col_stats(Relation index, SiftIndexStats *st);
extern void sift_f2_index_stats(Relation index, SiftIndexStats *st);

extern void sift_explain_init(void);
#if PG_VERSION_NUM >= 180000
extern void sift_explain_index(ExplainState *es, Oid indexoid);
#else
#define sift_explain_index(es, indexoid) ((void) 0)
#endif

#endif
