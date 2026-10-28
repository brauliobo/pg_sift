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

/* Emits the text line; the other formats printed their fields at each call */
extern void sift_explain_end(SiftExplainLine *ln);

#endif
