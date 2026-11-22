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

/*
 * What a text is matched against, made once for a query: its positive leaves and phrases in the order a walk of the query meets
 * them (their number is their order). The exact terms are in a hash table, so a token is looked up and not compared with each
 * of them; the other leaves (prefix, wildcard, regex, fuzzy) are tried in turn. Of two nodes that mark a token, the later one in
 * that order has it.
 */
typedef struct MatchLeaf
{
	const SiftQNode *node;
	int			order;
	const char *str;					/* an exact term: its text without the control bytes of the field and prefix markers */
	int			len;
} MatchLeaf;

struct SqMatchIndex
{
	MatchLeaf  *leaf;					/* every leaf; the exact terms are also in slots */
	int			nleaf;
	int		   *slots;					/* open addressing: leaf + 1, 0 = free */
	uint32		mask;
	MatchLeaf  *phrase;
	int			nphrase;
	int			norder;
};

typedef struct SqMatchIndex MatchIndex;

static bool
field_applies(const SiftQNode *n, int col)
{
	return !n->field || col < 0 || n->field - 1 == col;
}

static const SiftQNode *
stripped(const SiftQNode *leaf, SiftQNode *tmp)
{
	*tmp = *leaf;
	/* field and prefix terms start with control bytes that no word has */
	while (tmp->textlen > 0 && (unsigned char) tmp->text[0] < ' ')
	{
		tmp->text++;
		tmp->textlen--;
	}
	tmp->gram = false;
	return tmp;
}

static bool
leaf_hits(const SiftQNode *leaf, const SiftToken *t)
{
	SiftQNode	tmp;

	return sift_query_leaf_match(stripped(leaf, &tmp), t->str, t->len);
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

static uint32
term_hash(const char *s, int len)
{
	uint32		h = 2166136261u;

	for (int i = 0; i < len; i++)
		h = (h ^ (unsigned char) s[i]) * 16777619u;
	return h;
}

static void
index_node(MatchIndex *mi, const SiftQNode *n)
{
	switch (n->kind)
	{
		case SQ_AND:
		case SQ_OR:
			for (int i = 0; i < n->nchild; i++)
				index_node(mi, n->child[i]);
			break;
		case SQ_NOT:
		case SQ_NOTHING:
			break;
		case SQ_PHRASE:
			mi->phrase[mi->nphrase++] = (MatchLeaf) {n, mi->norder++};
			break;
		default:
			{
				SiftQNode	tmp;
				const SiftQNode *s = stripped(n, &tmp);

				mi->leaf[mi->nleaf++] = (MatchLeaf) {n, mi->norder++, s->text, s->textlen};
			}
	}
}

static int
count_nodes(const SiftQNode *n)
{
	int			c = n->kind == SQ_AND || n->kind == SQ_OR ? 0 : n->kind == SQ_NOT || n->kind == SQ_NOTHING ? 0 : 1;

	for (int i = 0; (n->kind == SQ_AND || n->kind == SQ_OR) && i < n->nchild; i++)
		c += count_nodes(n->child[i]);
	return c;
}

/* In the current memory context: the index lives as long as it */
static MatchIndex *
match_index(SiftQuery *const *qs, int nqs)
{
	MatchIndex *mi = palloc0(sizeof(MatchIndex));
	int			total = 0;

	for (int i = 0; i < nqs; i++)
		total += count_nodes(qs[i]->root);
	mi->leaf = palloc(Max(total, 1) * sizeof(MatchLeaf));
	mi->phrase = palloc(Max(total, 1) * sizeof(MatchLeaf));
	for (int i = 0; i < nqs; i++)
		index_node(mi, qs[i]->root);
	mi->mask = 15;
	while (mi->mask < (uint32) mi->nleaf * 2)
		mi->mask = mi->mask * 2 + 1;
	mi->slots = palloc0((mi->mask + 1) * sizeof(int));
	for (int i = 0; i < mi->nleaf; i++)
		if (mi->leaf[i].node->kind == SQ_TERM)
		{
			uint32		h = term_hash(mi->leaf[i].str, mi->leaf[i].len) & mi->mask;

			while (mi->slots[h])
				h = (h + 1) & mi->mask;
			mi->slots[h] = i + 1;
		}
	return mi;
}

/* The last leaf of the query that marks the token (its order), -1 when none does */
static int
last_leaf_order(const MatchIndex *mi, int col, const SiftToken *t)
{
	int			last = -1;

	for (uint32 h = term_hash(t->str, t->len) & mi->mask; mi->slots[h]; h = (h + 1) & mi->mask)
	{
		const MatchLeaf *l = &mi->leaf[mi->slots[h] - 1];

		if (l->len == t->len && memcmp(l->str, t->str, t->len) == 0 && field_applies(l->node, col))
			last = Max(last, l->order);
	}
	for (int i = 0; i < mi->nleaf; i++)
	{
		const MatchLeaf *l = &mi->leaf[i];

		if (l->node->kind != SQ_TERM && l->order > last && field_applies(l->node, col) && leaf_hits(l->node, t))
			last = l->order;
	}
	return last;
}

/* The occurrences of a phrase write their span over the tokens no later leaf has */
static void
mark_phrase(Marks *m, const MatchLeaf *p, const int *leaf_order)
{
	const SiftQNode *n = p->node;

	for (int i = 0; i + n->nterms <= m->n; i++)
	{
		int			k = 0;

		if (!phrase_term_is(n, 0, &m->tok[i]))
			continue;
		while (k < n->nterms && m->tok[i + k].pos == m->tok[i].pos + k && phrase_term_is(n, k, &m->tok[i + k]))
			k++;
		if (k < n->nterms)
			continue;
		m->nspan++;
		for (k = 0; k < n->nterms; k++)
			if (leaf_order[i + k] < p->order)
				m->span[i + k] = m->nspan;
	}
}

static Marks *
find_matches(SqVtab *vt, const MatchIndex *mi, const char *text, int len, int col)
{
	Marks	   *m = palloc0(sizeof(Marks));
	int		   *leaf_order;

	m->n = sift_analyze_tokens(sq_vt_analyzer(vt), text, len, &m->tok);
	m->span = palloc0(Max(m->n, 1) * sizeof(uint32));
	m->col = col;
	leaf_order = palloc(Max(m->n, 1) * sizeof(int));
	for (int i = 0; i < m->n; i++)
		if ((leaf_order[i] = last_leaf_order(mi, col, &m->tok[i])) >= 0)
			m->span[i] = ++m->nspan;
	for (int i = 0; i < mi->nphrase; i++)
		if (field_applies(mi->phrase[i].node, col))
			mark_phrase(m, &mi->phrase[i], leaf_order);
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
highlight(SqVtab *vt, const MatchIndex *mi, const char *text, int len, int col, const char *open, const char *close)
{
	StringInfoData out;
	Marks	   *m = find_matches(vt, mi, text, len, col);

	initStringInfo(&out);
	if (m->n == 0)
		return pstrdup(text);
	appendBinaryStringInfo(&out, text, m->tok[0].start);
	append_range(&out, m, text, 0, m->n, open, close);
	appendStringInfoString(&out, text + m->tok[m->n - 1].end);
	return out.data;
}

static char *
snippet(SqVtab *vt, const MatchIndex *mi, const char *text, int len, int col, const char *open, const char *close,
		const char *ellipsis, int ntokens)
{
	StringInfoData out;
	Marks	   *m = find_matches(vt, mi, text, len, col);
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

/* The index of the scan's queries, made in the cursor's memory by the first row that asks for it */
static const MatchIndex *
cursor_index(SqCursor *c)
{
	if (c->mi == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(c->cx);

		c->mi = match_index(c->qs, c->nqs);
		MemoryContextSwitchTo(old);
	}
	return c->mi;
}

static char *
run_highlight(SqCursor *c, char *text, int len, int col, sqlite3_value **argv)
{
	return highlight(c->vt, cursor_index(c), text, len, col, text_arg(argv[2]), text_arg(argv[3]));
}

static char *
run_snippet(SqCursor *c, char *text, int len, int col, sqlite3_value **argv)
{
	return snippet(c->vt, cursor_index(c), text, len, col, text_arg(argv[2]), text_arg(argv[3]), text_arg(argv[4]),
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

/* The query of a text function as it was parsed, analyzed and indexed for the rows before: kept by the statement (sqlite3_set_auxdata) while its argument is the same */
typedef struct TextAux
{
	MemoryContext cx;
	SqVtab	   *vt;
	char	   *qstr;
	int			qlen;
	const MatchIndex *mi;
} TextAux;

static void
text_aux_free(void *p)
{
	TextAux    *aux = p;

	MemoryContextDelete(aux->cx);
	free(aux);
}

static const MatchIndex *
text_index(sqlite3_context *ctx, SqVtab *vt, sqlite3_value *qarg)
{
	const char *q = text_arg(qarg);
	int			qlen = sqlite3_value_bytes(qarg);
	TextAux    *aux = sqlite3_get_auxdata(ctx, 2);
	MemoryContext old;
	SiftQuery  *query;

	if (aux && aux->vt == vt && aux->qlen == qlen && memcmp(aux->qstr, q, qlen) == 0)
		return aux->mi;
	aux = calloc(1, sizeof(TextAux));
	aux->cx = AllocSetContextCreate(NULL, "sift highlight query", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(aux->cx);
	aux->vt = vt;
	aux->qstr = palloc(qlen + 1);
	memcpy(aux->qstr, q, qlen);
	aux->qlen = qlen;
	query = sift_query_analyze(sift_query_parse(q, qlen), sq_vt_analyzer(vt));
	aux->mi = match_index(&query, 1);
	MemoryContextSwitchTo(old);
	sqlite3_set_auxdata(ctx, 2, aux, text_aux_free);
	return aux->mi;
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
		const MatchIndex *mi = text_index(ctx, vt, argv[2]);

		res = snip ? snippet(vt, mi, text_arg(argv[1]), sqlite3_value_bytes(argv[1]), -1, text_arg(argv[3]),
							 text_arg(argv[4]), text_arg(argv[5]), sqlite3_value_int(argv[6]))
			: highlight(vt, mi, text_arg(argv[1]), sqlite3_value_bytes(argv[1]), -1, text_arg(argv[3]), text_arg(argv[4]));
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
