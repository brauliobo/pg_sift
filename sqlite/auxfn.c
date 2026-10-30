/*
 * auxfn.c - what FTS5 calls auxiliary functions: highlight, snippet and bm25 on a row or on any text; registers them and the facets of auxfn_facets.c.
 *
 *   sift_highlight(t, col, open, close)               the text of column col with the matches of the MATCH wrapped
 *   sift_snippet(t, col, open, close, ellipsis, n)    the n tokens of column col that hold the most matches
 *   sift_bm25(t)                                      the rank of the current row (negated BM25), as the rank column
 *   sift_highlight_text(table, text, query, open, close), sift_snippet_text(table, text, query, open, close, ellipsis, n)
 *                                                     the same on any text, with the analyzer of a table
 *   SELECT value, n FROM sift_facets('docs', 'docs_category', 'friendly OR case'[, lim[, nulls]])
 *                                                     matches of docs per value of a keyword-analyzer table
 *
 * The first three are overloads (xFindFunction): t is the table-named column of the row being read, which the table
 * answers with its cursor. Matches are found by running the analyzer over the text, so offsets are those of its tokens,
 * and a token matches when it is a term, prefix, wildcard, regex or fuzzy leaf of the query outside a NOT, or part of an
 * occurrence of a phrase of it.
 */
#include "vtab.h"
#include "core/cmp.h"

SQLITE_EXTENSION_INIT3

#define SNIPPET_MAX_TOKENS	64

/* ---------- which tokens match ---------- */

typedef struct Marks
{
	SiftToken  *tok;
	int			n;
	uint32	   *span;					/* 0: no match; tokens of one phrase occurrence share a span */
	uint32		nspan;
	int			col;					/* the column the text is of, -1 unknown */
} Marks;

static bool
leaf_hits(const SiftQNode *leaf, const SiftToken *t)
{
	SiftQNode	tmp = *leaf;

	/* field and prefix terms start with control bytes that no word has */
	while (tmp.textlen > 0 && (unsigned char) tmp.text[0] < ' ')
	{
		tmp.text++;
		tmp.textlen--;
	}
	tmp.gram = false;
	return sift_query_leaf_match(&tmp, t->str, t->len);
}

static bool
phrase_term_is(const SiftQNode *n, int k, const SiftToken *t)
{
	const char *w = n->terms[k];
	int			len = n->termlens[k];

	while (len > 0 && (unsigned char) w[0] < ' ')
	{
		w++;
		len--;
	}
	return t->len == len && memcmp(t->str, w, len) == 0;
}

static void
mark_phrase(Marks *m, const SiftQNode *n)
{
	for (int i = 0; i + n->nterms <= m->n; i++)
	{
		int			k = 0;

		while (k < n->nterms && m->tok[i + k].pos == m->tok[i].pos + k && phrase_term_is(n, k, &m->tok[i + k]))
			k++;
		if (k < n->nterms)
			continue;
		m->nspan++;
		for (k = 0; k < n->nterms; k++)
			m->span[i + k] = m->nspan;
	}
}

static void
mark_node(Marks *m, const SiftQNode *n)
{
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				mark_node(m, n->child[i]);
			break;
		case SQ_NOT:
		case SQ_NOTHING:
			break;
		case SQ_PHRASE:
			if (!n->field || m->col < 0 || n->field - 1 == m->col)
				mark_phrase(m, n);
			break;
		default:
			if (n->field && m->col >= 0 && n->field - 1 != m->col)
				break;
			for (int i = 0; i < m->n; i++)
				if (leaf_hits(n, &m->tok[i]))
					m->span[i] = ++m->nspan;
	}
}

static Marks *
find_matches(SqVtab *vt, SiftQuery *const *qs, int nqs, const char *text, int len, int col)
{
	Marks	   *m = palloc0(sizeof(Marks));

	m->n = sift_analyze_tokens(sq_vt_analyzer(vt), text, len, &m->tok);
	m->span = palloc0(Max(m->n, 1) * sizeof(uint32));
	m->col = col;
	for (int i = 0; i < nqs; i++)
		mark_node(m, qs[i]->root);
	return m;
}

/* ---------- output ---------- */

static void
append_range(StringInfo out, const Marks *m, const char *text, int from, int to, const char *open, const char *close)
{
	uint32		inside = 0;
	int			at = m->tok[from].start;

	for (int i = from; i < to; i++)
	{
		const SiftToken *t = &m->tok[i];

		if (inside && m->span[i] != inside)
		{
			appendStringInfoString(out, close);
			inside = 0;
		}
		appendBinaryStringInfo(out, text + at, t->start - at);
		if (m->span[i] && !inside)
		{
			appendStringInfoString(out, open);
			inside = m->span[i];
		}
		appendBinaryStringInfo(out, text + t->start, t->end - t->start);
		at = t->end;
	}
	if (inside)
		appendStringInfoString(out, close);
}

static int
matches_in(const Marks *m, int from, int to)
{
	int			n = 0;

	for (int i = from; i < to; i++)
		n += m->span[i] != 0;
	return n;
}

/* The window of n tokens with the most matched ones (the first of equals), moved so that its matches are in the middle */
static int
best_window(const Marks *m, int n)
{
	int			cur = matches_in(m, 0, Min(n, m->n)),
				best = cur,
				at = 0,
				first = 0,
				last;

	for (int i = 1; i + n <= m->n; i++)
	{
		cur += (m->span[i + n - 1] != 0) - (m->span[i - 1] != 0);
		if (cur > best)
		{
			best = cur;
			at = i;
		}
	}
	last = Min(n, m->n - at) - 1;
	while (first < last && !m->span[at + first])
		first++;
	while (last > first && !m->span[at + last])
		last--;
	at += (n - (last - first + 1)) / 2 * -1 + first;
	return Max(0, Min(at, m->n - n));
}

static char *
highlight(SqVtab *vt, SiftQuery *const *qs, int nqs, const char *text, int len, int col, const char *open, const char *close)
{
	StringInfoData out;
	Marks	   *m = find_matches(vt, qs, nqs, text, len, col);

	initStringInfo(&out);
	if (m->n == 0)
		return pstrdup(text);
	appendBinaryStringInfo(&out, text, m->tok[0].start);
	append_range(&out, m, text, 0, m->n, open, close);
	appendStringInfoString(&out, text + m->tok[m->n - 1].end);
	return out.data;
}

static char *
snippet(SqVtab *vt, SiftQuery *const *qs, int nqs, const char *text, int len, int col, const char *open, const char *close,
		const char *ellipsis, int ntokens)
{
	StringInfoData out;
	Marks	   *m = find_matches(vt, qs, nqs, text, len, col);
	int			from,
				to;

	initStringInfo(&out);
	if (m->n == 0)
		return out.data;
	ntokens = Max(1, Min(ntokens, SNIPPET_MAX_TOKENS));
	from = best_window(m, ntokens);
	to = Min(from + ntokens, m->n);
	if (from > 0)
		appendStringInfoString(&out, ellipsis);
	append_range(&out, m, text, from, to, open, close);
	if (to < m->n)
		appendStringInfoString(&out, ellipsis);
	return out.data;
}

/* ---------- the overloads, on the cursor of the row being read ---------- */

static SqCursor *
cursor_arg(sqlite3_context *ctx, sqlite3_value *v, const char *fn)
{
	SqCursor   *c = sqlite3_value_pointer(v, "sift_cursor");

	if (c == NULL || c->mode == SC_EOF || c->nqs == 0)
	{
		char	   *msg = sqlite3_mprintf("%s: the first argument is the table, in a query with MATCH", fn);

		sqlite3_result_error(ctx, msg, -1);
		sqlite3_free(msg);
		return NULL;
	}
	return c;
}

static const char *
text_arg(sqlite3_value *v)
{
	const char *s = (const char *) sqlite3_value_text(v);

	return s ? s : "";
}

typedef char *(*TextFn) (SqCursor *c, char *text, int len, int col, sqlite3_value **argv);

static void
on_row_text(sqlite3_context *ctx, sqlite3_value **argv, const char *fn, TextFn run)
{
	SqCursor   *c = cursor_arg(ctx, argv[0], fn);
	SqVtab	   *vt;
	int			col = sqlite3_value_int(argv[1]),
				len = 0;
	MemoryContext old;
	SqStore    *prev;
	char	   *res = NULL;
	bool		failed = false;

	if (c == NULL)
		return;
	vt = c->vt;
	old = sq_enter();
	prev = sq_store_enter(vt->st);
	SQ_TRY();
	{
		char	   *text = sq_vt_doc_text(vt, c->rowid, col, &len);

		if (text)
			res = run(c, text, len, col, argv);
	}
	SQ_CATCH();
	failed = true;
	sq_store_recover(vt->st);
	SQ_END();
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	else if (res)
		sqlite3_result_text(ctx, res, -1, SQLITE_TRANSIENT);
	sq_store_leave(prev);
	sq_leave(old);
}

static char *
run_highlight(SqCursor *c, char *text, int len, int col, sqlite3_value **argv)
{
	return highlight(c->vt, c->qs, c->nqs, text, len, col, text_arg(argv[2]), text_arg(argv[3]));
}

static char *
run_snippet(SqCursor *c, char *text, int len, int col, sqlite3_value **argv)
{
	return snippet(c->vt, c->qs, c->nqs, text, len, col, text_arg(argv[2]), text_arg(argv[3]), text_arg(argv[4]),
				   sqlite3_value_int(argv[5]));
}

static void
fn_highlight(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	on_row_text(ctx, argv, "sift_highlight", run_highlight);
}

static void
fn_snippet(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	on_row_text(ctx, argv, "sift_snippet", run_snippet);
}

static void
fn_bm25(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqCursor   *c = cursor_arg(ctx, argv[0], "sift_bm25");

	if (c == NULL)
		return;
	if (c->mode != SC_RANK)
		sqlite3_result_error(ctx, "sift_bm25: the scan is not ranked; select rank or ORDER BY rank", -1);
	else
		sqlite3_result_double(ctx, -c->score);
}

int
sq_aux_find_function(sqlite3_vtab *pVtab, int nArg, const char *zName,
					 void (**pxFunc) (sqlite3_context *, int, sqlite3_value **), void **ppArg)
{
	if (nArg == 4 && strcmp(zName, "sift_highlight") == 0)
		*pxFunc = fn_highlight;
	else if (nArg == 6 && strcmp(zName, "sift_snippet") == 0)
		*pxFunc = fn_snippet;
	else if (nArg == 1 && strcmp(zName, "sift_bm25") == 0)
		*pxFunc = fn_bm25;
	else
		return 0;
	return 1;
}

/* ---------- the same on any text ---------- */

static SiftQuery *
analyzed_query(SqVtab *vt, const char *q, int len)
{
	return sift_query_analyze(sift_query_parse(q, len), sq_vt_analyzer(vt));
}

static void
text_function(sqlite3_context *ctx, sqlite3_value **argv, bool snip)
{
	SqVtab	   *vt = sq_find_table(sqlite3_user_data(ctx), (const char *) sqlite3_value_text(argv[0]));
	MemoryContext old;
	SqStore    *prev;
	char	   *res = NULL;
	bool		failed = false;

	if (vt == NULL)
	{
		sqlite3_result_error(ctx, "no such sift table", -1);
		return;
	}
	old = sq_enter();
	prev = sq_store_enter(vt->st);
	SQ_TRY();
	{
		SiftQuery  *q = analyzed_query(vt, text_arg(argv[2]), sqlite3_value_bytes(argv[2]));

		res = snip ? snippet(vt, &q, 1, text_arg(argv[1]), sqlite3_value_bytes(argv[1]), -1, text_arg(argv[3]),
							 text_arg(argv[4]), text_arg(argv[5]), sqlite3_value_int(argv[6]))
			: highlight(vt, &q, 1, text_arg(argv[1]), sqlite3_value_bytes(argv[1]), -1, text_arg(argv[3]), text_arg(argv[4]));
	}
	SQ_CATCH();
	failed = true;
	sq_store_recover(vt->st);
	SQ_END();
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	else
		sqlite3_result_text(ctx, res, -1, SQLITE_TRANSIENT);
	sq_store_leave(prev);
	sq_leave(old);
}

static void
fn_highlight_text(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	text_function(ctx, argv, false);
}

static void
fn_snippet_text(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	text_function(ctx, argv, true);
}

/* ---------- registration ---------- */

int
sq_aux_init(sqlite3 *db, SqConn *conn)
{
	int			rc = SQLITE_OK;

	rc = sqlite3_overload_function(db, "sift_highlight", 4);
	if (rc == SQLITE_OK)
		rc = sqlite3_overload_function(db, "sift_snippet", 6);
	if (rc == SQLITE_OK)
		rc = sqlite3_overload_function(db, "sift_bm25", 1);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_highlight_text", 5, SQLITE_UTF8, conn, fn_highlight_text, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_snippet_text", 7, SQLITE_UTF8, conn, fn_snippet_text, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sq_facets_init(db, conn);
	return rc;
}
