/*
 * sqaux.c - what FTS5 calls auxiliary functions, and facets.
 *
 *   sift_highlight(t, col, open, close)               the text of column col with the matches of the MATCH wrapped
 *   sift_snippet(t, col, open, close, ellipsis, n)    the n tokens of column col that hold the most matches
 *   sift_bm25(t)                                      the rank of the current row (negated BM25), as the rank column
 *   sift_highlight_text(table, text, query, open, close), sift_snippet_text(table, text, query, open, close, ellipsis, n)
 *                                                     the same on any text, with the analyzer of a table
 *   SELECT value, n FROM sift_facets('docs', 'docs_category', 'friendly OR pool'[, lim[, nulls]])
 *                                                     matches of docs per value of a keyword-analyzer table
 *
 * The first three are overloads (xFindFunction): t is the table-named column of the row being read, which the table
 * answers with its cursor. Matches are found by running the analyzer over the text, so offsets are those of its tokens,
 * and a token matches when it is a term, prefix, wildcard, regex or fuzzy leaf of the query outside a NOT, or part of an
 * occurrence of a phrase of it.
 */
#include "sqvtab.h"

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

/* The window of n tokens with the most matched ones (the first of equals) */
static int
best_window(const Marks *m, int n, int *score)
{
	int			cur = matches_in(m, 0, Min(n, m->n)),
				best = cur,
				at = 0;

	for (int i = 1; i + n <= m->n; i++)
	{
		cur += (m->span[i + n - 1] != 0) - (m->span[i - 1] != 0);
		if (cur > best)
		{
			best = cur;
			at = i;
		}
	}
	*score = best;
	return at;
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
	int			score,
				from,
				to;

	initStringInfo(&out);
	if (m->n == 0)
		return out.data;
	ntokens = Max(1, Min(ntokens, SNIPPET_MAX_TOKENS));
	from = best_window(m, ntokens, &score);
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

/* ---------- facets ---------- */

typedef struct Matches
{
	BlockNumber *blk;
	uint32	   *mask;					/* offsets 1..32 of the block (rowids map to those only) */
	int64_t		n,
				cap;
	int64_t		total;
} Matches;

static void
add_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	Matches    *m = arg;

	if (m->n == m->cap)
	{
		m->cap = m->cap ? m->cap * 2 : 1024;
		m->blk = m->blk ? repalloc_huge(m->blk, m->cap * sizeof(BlockNumber)) : palloc(m->cap * sizeof(BlockNumber));
		m->mask = m->mask ? repalloc_huge(m->mask, m->cap * sizeof(uint32)) : palloc(m->cap * sizeof(uint32));
	}
	m->blk[m->n] = blk;
	m->mask[m->n++] = (uint32) offmask[0];
	m->total += pg_popcount32((uint32) offmask[0]);
}

static bool
is_match(const Matches *m, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int64_t		lo = 0,
				hi = m->n;

	while (lo < hi)
	{
		int64_t		mid = (lo + hi) / 2;

		if (m->blk[mid] < blk)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < m->n && m->blk[lo] == blk && (m->mask[lo] >> ((key & 0xFFFF) - 1) & 1);
}

typedef struct Facet
{
	char	   *value;
	int			len;
	int64_t		n;
} Facet;

typedef struct FacetCtx
{
	const Matches *m;
	Facet	   *f;
	int64_t		nf,
				cap;
} FacetCtx;

static void
count_term(void *arg, const char *term, int len, const SiftKey *keys, uint32 n)
{
	FacetCtx   *fc = arg;
	int64_t		c = 0;

	for (uint32 i = 0; i < n; i++)
		c += is_match(fc->m, keys[i]);
	if (c == 0)
		return;
	if (fc->nf == fc->cap)
	{
		fc->cap = fc->cap ? fc->cap * 2 : 64;
		fc->f = fc->f ? repalloc_huge(fc->f, fc->cap * sizeof(Facet)) : palloc(fc->cap * sizeof(Facet));
	}
	fc->f[fc->nf++] = (Facet) {memcpy(palloc(len), term, len), len, c};
}

static int
facet_cmp(const void *a, const void *b)
{
	const Facet *x = a,
			   *y = b;
	int			c = memcmp(x->value, y->value, Min(x->len, y->len));

	return c ? c : x->len - y->len;
}

static int
facet_by_count(const void *a, const void *b)
{
	const Facet *x = a,
			   *y = b;

	return x->n != y->n ? (x->n < y->n) - (x->n > y->n) : facet_cmp(a, b);
}

/* The same value in several segments of the facet table: one row */
static int64_t
combine(Facet *f, int64_t n)
{
	int64_t		out = 0;

	qsort(f, n, sizeof(Facet), facet_cmp);
	for (int64_t i = 0; i < n; i++)
	{
		if (out > 0 && facet_cmp(&f[out - 1], &f[i]) == 0)
			f[out - 1].n += f[i].n;
		else
			f[out++] = f[i];
	}
	return out;
}

static Matches *
query_matches(SqVtab *vt, const char *q, int qlen)
{
	Matches    *m = palloc0(sizeof(Matches));
	SiftIndexState *state;
	SiftQuery  *qs[1];
	SiftPlan   *plan;
	SiftVis		vis;

	sq_store_refresh(vt->st, true);
	sq_vt_flush(vt, false);
	state = sift_state_open(sq_store_rel(vt->st), CurrentMemoryContext);
	qs[0] = sift_state_query(state, q, qlen);
	plan = sift_plan_bind(state, qs, 1, 0);
	memset(&vis, 0, sizeof(vis));
	vis.use_vm = true;
	sift_eval_pages(plan, &vis, sq_vt_verify, vt, add_page, m);
	return m;
}

static Facet *
facet_values(SqVtab *ft, const Matches *m, int64_t *nf)
{
	FacetCtx	fc = {.m = m};
	const SiftDirSnapshot *dir;

	sq_store_refresh(ft->st, true);
	sq_vt_flush(ft, false);
	dir = sq_store_dir(ft->st);
	for (int i = 0; i < dir->nsegs; i++)
		sq_seg_walk_terms(ft->st, &dir->segs[i], count_term, &fc);
	*nf = combine(fc.f, fc.nf);
	qsort(fc.f, *nf, sizeof(Facet), facet_by_count);
	return fc.f;
}

typedef struct FacetRow
{
	char	   *value;					/* NULL: the matches without a value */
	int64_t		n;
} FacetRow;

typedef struct FacetCursor
{
	sqlite3_vtab_cursor base;
	FacetRow   *rows;
	int64_t		nrows,
				at;
	MemoryContext cx;
} FacetCursor;

typedef struct FacetVtab
{
	sqlite3_vtab base;
	SqConn	   *conn;
} FacetVtab;

#define FC_VALUE	0
#define FC_COUNT	1
#define FC_TABLE	2
#define FC_FACET	3
#define FC_QUERY	4
#define FC_LIMIT	5
#define FC_NULLS	6

static int
fcConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr)
{
	FacetVtab  *v = sqlite3_malloc(sizeof(FacetVtab));
	int			rc = sqlite3_declare_vtab(db, "CREATE TABLE x(value, n, \"table\" HIDDEN, facet HIDDEN, q HIDDEN, lim HIDDEN, nulls HIDDEN)");

	if (v == NULL)
		return SQLITE_NOMEM;
	memset(v, 0, sizeof(*v));
	v->conn = aux;
	*ppVtab = &v->base;
	return rc;
}

static int
fcDisconnect(sqlite3_vtab *p)
{
	sqlite3_free(p);
	return SQLITE_OK;
}

static int
fcBestIndex(sqlite3_vtab *p, sqlite3_index_info *info)
{
	int			argc = 0;
	int			need = 0;

	for (int i = 0; i < info->nConstraint; i++)
	{
		const struct sqlite3_index_constraint *c = &info->aConstraint[i];

		if (c->usable && c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn >= FC_TABLE)
		{
			info->aConstraintUsage[i].argvIndex = c->iColumn - FC_TABLE + 1;
			info->aConstraintUsage[i].omit = 1;
			argc = Max(argc, c->iColumn - FC_TABLE + 1);
			need |= 1 << (c->iColumn - FC_TABLE);
		}
	}
	if ((need & 7) != 7)
		return SQLITE_CONSTRAINT;
	info->idxNum = need;
	info->estimatedCost = 1000;
	return SQLITE_OK;
}

static int
fcOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **pp)
{
	FacetCursor *c = sqlite3_malloc(sizeof(FacetCursor));

	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof(*c));
	*pp = &c->base;
	return SQLITE_OK;
}

static void
fc_reset(FacetCursor *c)
{
	if (c->cx)
		MemoryContextDelete(c->cx);
	c->cx = NULL;
	c->rows = NULL;
	c->nrows = c->at = 0;
}

static int
fcClose(sqlite3_vtab_cursor *cur)
{
	fc_reset((FacetCursor *) cur);
	sqlite3_free(cur);
	return SQLITE_OK;
}

/* The rows, in the cursor's context: values by descending count, then the NULL row if nulls is set and some matches have no value */
static void
facet_rows(FacetCursor *c, SqVtab *vt, SqVtab *ft, const char *q, int qlen, int64_t lim, bool nulls)
{
	Matches    *m;
	Facet	   *f;
	int64_t		nf,
				counted = 0,
				n;
	SqStore    *prev;

	prev = sq_store_enter(vt->st);
	m = query_matches(vt, q, qlen);
	sq_store_leave(prev);
	prev = sq_store_enter(ft->st);
	f = facet_values(ft, m, &nf);
	sq_store_leave(prev);
	for (int64_t i = 0; i < nf; i++)
		counted += f[i].n;
	n = lim >= 0 ? Min(nf, lim) : nf;
	c->rows = palloc((n + 1) * sizeof(FacetRow));
	for (int64_t i = 0; i < n; i++)
		c->rows[c->nrows++] = (FacetRow) {pnstrdup(f[i].value, f[i].len), f[i].n};
	if (nulls && m->total > counted)
		c->rows[c->nrows++] = (FacetRow) {NULL, m->total - counted};
}

static int
fcFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv)
{
	FacetCursor *c = (FacetCursor *) cur;
	FacetVtab  *v = (FacetVtab *) cur->pVtab;
	SqVtab	   *vt,
			   *ft;
	int			rc = SQLITE_OK;
	MemoryContext old,
				call;

	fc_reset(c);
	vt = sq_find_table(v->conn, (const char *) sqlite3_value_text(argv[0]));
	ft = sq_find_table(v->conn, (const char *) sqlite3_value_text(argv[1]));
	if (vt == NULL || ft == NULL)
	{
		sqlite3_free(v->base.zErrMsg);
		v->base.zErrMsg = sqlite3_mprintf("sift_facets: no such sift table: %s", vt == NULL ? sqlite3_value_text(argv[0]) : sqlite3_value_text(argv[1]));
		return SQLITE_ERROR;
	}
	old = sq_enter();
	call = CurrentMemoryContext;
	c->cx = AllocSetContextCreate(NULL, "sift facets", ALLOCSET_DEFAULT_SIZES);
	SQ_TRY();
	MemoryContextSwitchTo(c->cx);
	facet_rows(c, vt, ft, (const char *) sqlite3_value_text(argv[2]), sqlite3_value_bytes(argv[2]),
			   (idxNum & 8) ? sqlite3_value_int64(argv[3]) : -1, (idxNum & 16) && sqlite3_value_int(argv[4]));
	SQ_CATCH();
	sqlite3_free(v->base.zErrMsg);
	v->base.zErrMsg = sqlite3_mprintf("%s", sq_errmsg());
	sq_store_recover(vt->st);
	sq_store_recover(ft->st);
	rc = SQLITE_ERROR;
	SQ_END();
	MemoryContextSwitchTo(call);
	sq_leave(old);
	return rc;
}

static int
fcNext(sqlite3_vtab_cursor *cur)
{
	((FacetCursor *) cur)->at++;
	return SQLITE_OK;
}

static int
fcEof(sqlite3_vtab_cursor *cur)
{
	FacetCursor *c = (FacetCursor *) cur;

	return c->at >= c->nrows;
}

static int
fcColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
	FacetCursor *c = (FacetCursor *) cur;

	if (i == FC_VALUE && c->rows[c->at].value)
		sqlite3_result_text(ctx, c->rows[c->at].value, -1, SQLITE_TRANSIENT);
	else if (i == FC_COUNT)
		sqlite3_result_int64(ctx, c->rows[c->at].n);
	return SQLITE_OK;
}

static int
fcRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *r)
{
	*r = ((FacetCursor *) cur)->at;
	return SQLITE_OK;
}

static const sqlite3_module facet_module = {
	.iVersion = 3,
	.xConnect = fcConnect,
	.xBestIndex = fcBestIndex,
	.xDisconnect = fcDisconnect,
	.xOpen = fcOpen,
	.xClose = fcClose,
	.xFilter = fcFilter,
	.xNext = fcNext,
	.xEof = fcEof,
	.xColumn = fcColumn,
	.xRowid = fcRowid,
};

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
		rc = sqlite3_create_module(db, "sift_facets", &facet_module, conn);
	return rc;
}
