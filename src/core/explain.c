/*
 * core/explain.c - the counter lines of the custom scans in EXPLAIN (core/explain.h)
 */
#include "postgres.h"

#include "core/explain.h"

void
sift_explain_begin(SiftExplainLine *ln, ExplainState *es, const char *label)
{
	ln->es = es;
	ln->label = label;
	ln->open = false;
	initStringInfo(&ln->buf);
}

static void
text_item(SiftExplainLine *ln, const char *num, const char *unit, const char *words, int flags)
{
	StringInfo	b = &ln->buf;

	if ((flags & SIFT_XI_SUB) && b->len > 0)
	{
		appendStringInfoString(b, ln->open ? ", " : " (");
		ln->open = true;
	}
	else
	{
		if (ln->open)
			appendStringInfoChar(b, ')');
		ln->open = false;
		if (b->len > 0)
			appendStringInfoString(b, ", ");
	}
	appendStringInfoString(b, num);
	if (unit)
		appendStringInfo(b, " %s", unit);
	if (words)
		appendStringInfo(b, " %s", words);
}

void
sift_explain_int(SiftExplainLine *ln, const char *prop, const char *unit, const char *words, int64 v, int flags)
{
	if (ln->es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (v != 0 || !(flags & SIFT_XI_OPT))
			ExplainPropertyInteger(prop, unit, v, ln->es);
	}
	else if (v != 0 || ln->es->verbose || (flags & SIFT_XI_KEEP))
		text_item(ln, psprintf(INT64_FORMAT, v), unit, words, flags);
}

void
sift_explain_ms(SiftExplainLine *ln, const char *prop, const char *words, double ms, int flags)
{
	if (ln->es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyFloat(prop, "ms", ms, 3, ln->es);
	else if (ms != 0 || ln->es->verbose || (flags & SIFT_XI_KEEP))
		text_item(ln, psprintf("%.3f", ms), "ms", words, flags);
}

void
sift_explain_end(SiftExplainLine *ln)
{
	if (ln->open)
		appendStringInfoChar(&ln->buf, ')');
	if (ln->buf.len > 0)
		ExplainPropertyText(ln->label, ln->buf.data, ln->es);
	pfree(ln->buf.data);
}
