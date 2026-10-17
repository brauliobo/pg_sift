/*
 * sqvtab.c - the SQLite virtual table module "sift":
 *
 *   CREATE VIRTUAL TABLE docs USING sift(title, body, positions=1, stemmer='english');
 *   SELECT rowid, rank FROM docs WHERE docs MATCH 'quick fox*' ORDER BY rank LIMIT 10;
 *   SELECT sift_count('docs', 'quick fox*');
 *   INSERT INTO docs(docs) VALUES('merge');
 *
 * rank is the negated BM25 score (lower is better, as FTS5's). Documents go to an in-memory inverted buffer;
 * at xSync (or before a query, or at a savepoint) the buffer is written as an immutable segment inside the
 * SQLite transaction, so the table commits and rolls back with it. See README.md.
 */
#include "sqapi.h"

#include <ctype.h>

#include "sift_cpu.h"
#include "sift_eval.h"

SQLITE_EXTENSION_INIT1

#define MERGE_AT		16			/* segments from which a flush merges the smallest ones */
#define MERGE_COUNT		8
#define FLUSH_POSTINGS	(64 * 1000 * 1000)	/* about 1 GB of buffer */

typedef struct SqVtab
{
	sqlite3_vtab base;
	sqlite3    *db;
	char	   *schema;
	char	   *name;
	int			ncols;
	char	  **cols;
	SqStore    *st;
	SqOptions	opts;
	MemoryContext acx;				/* analyzer */
	SiftAnalyzer *analyzer;
	SqInv	   *pend;				/* documents not yet in a segment */
	int			ncursors;
	bool		in_txn;
	sqlite3_stmt *rowstmt;			/* reusable SELECT of one content row */
	struct SqVtab *next;
} SqVtab;

typedef struct SqCursor
{
	sqlite3_vtab_cursor base;
	SqVtab	   *vt;
	MemoryContext cx;
	enum {SC_EOF, SC_RANK, SC_ITER, SC_ROWS} mode;
	SiftIndexState *state;
	SiftPlan   *plan;
	SiftRanker *ranker;
	SiftMatchIter *iter;
	SiftPageMatch pm;
	uint64		left[SIFT_OFFMASK_WORDS];	/* unreported offsets of pm */
	bool		have_pm;
	int64_t		rowid;
	double		score;
	sqlite3_stmt *rows;
	sqlite3_stmt *row;				/* content row of the current rowid */
	bool		eof;
} SqCursor;

static SqVtab *registry;

/* ---------- errors and contexts ---------- */

static MemoryContext
enter(void)
{
	MemoryContext cx = AllocSetContextCreate(TopMemoryContext, "sift call", ALLOCSET_DEFAULT_SIZES);

	return MemoryContextSwitchTo(cx);
}

static void
leave(MemoryContext old)
{
	MemoryContext cx = CurrentMemoryContext;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static int
fail(sqlite3_vtab *vt)
{
	sqlite3_free(vt->zErrMsg);
	vt->zErrMsg = sqlite3_mprintf("%s", sq_errmsg());
	return SQLITE_ERROR;
}

/* The body runs in a context of its own that the end deletes; an ERROR inside lands in SQ_CATCH and becomes SQLITE_ERROR */
#define TRY_BEGIN(vt) \
	int rc = SQLITE_OK; \
	MemoryContext old_ = enter(), \
				call_ = CurrentMemoryContext; \
	SQ_TRY();

#define TRY_END(vt) \
	SQ_CATCH(); \
	rc = fail(&(vt)->base); \
	SQ_END(); \
	MemoryContextSwitchTo(call_); \
	leave(old_); \
	return rc;

/* ---------- options ---------- */

static void
options_default(SqOptions *o)
{
	memset(o, 0, sizeof(*o));
	o->base.k1 = SIFT_DEFAULT_K1;
	o->base.b = SIFT_DEFAULT_B;
	o->base.case_folding = true;
	o->base.accent_folding = true;
	o->base.positions = true;
	o->base.analyzer = SIFT_ANALYZER_TEXT;
	strcpy(o->stemmer, "english");
}

static char *
unquote(const char *s)
{
	char	   *d = sqlite3_mprintf("%s", s),
			   *p = d;
	size_t		n;

	while (isspace((unsigned char) *p))
		p++;
	n = strlen(p);
	while (n > 0 && isspace((unsigned char) p[n - 1]))
		p[--n] = '\0';
	if (n >= 2 && (p[0] == '\'' || p[0] == '"') && p[n - 1] == p[0])
	{
		p[n - 1] = '\0';
		p++;
	}
	p = sqlite3_mprintf("%s", p);
	sqlite3_free(d);
	return p;
}

/* key=value: false when arg is a column name */
static bool
option_set(SqOptions *o, const char *arg, char **err)
{
	const char *eq = strchr(arg, '=');
	char	   *key,
			   *val;

	if (eq == NULL)
		return false;
	key = sqlite3_mprintf("%.*s", (int) (eq - arg), arg);
	{
		char	   *k = unquote(key);

		sqlite3_free(key);
		key = k;
	}
	val = unquote(eq + 1);
	if (strcmp(key, "positions") == 0)
		o->base.positions = atoi(val) != 0;
	else if (strcmp(key, "stemmer") == 0 && strlen(val) < sizeof(o->stemmer))
		strcpy(o->stemmer, strcmp(val, "none") == 0 ? "" : val);
	else if (strcmp(key, "k1") == 0)
		o->base.k1 = atof(val);
	else if (strcmp(key, "b") == 0)
		o->base.b = atof(val);
	else if (strcmp(key, "case_fold") == 0)
		o->base.case_folding = atoi(val) != 0;
	else if (strcmp(key, "accent_fold") == 0)
		o->base.accent_folding = atoi(val) != 0;
	else if (strcmp(key, "prefix_index") == 0)
		o->base.prefix_index = atoi(val);
	else
		*err = sqlite3_mprintf("sift: unknown option \"%s\"", key);
	sqlite3_free(key);
	sqlite3_free(val);
	return true;
}

/* ---------- statements ---------- */

static char *
shadow(const SqVtab *vt, const char *suffix)
{
	return sqlite3_mprintf("\"%w\".\"%w_%s\"", vt->schema, vt->name, suffix);
}

static char *
col_list(const SqVtab *vt, const char *prefix)
{
	char	   *s = sqlite3_mprintf("%s", "");

	for (int i = 0; i < vt->ncols; i++)
	{
		char	   *t = sqlite3_mprintf("%s%s%sc%d", s, i ? ", " : "", prefix, i);

		sqlite3_free(s);
		s = t;
	}
	return s;
}

static sqlite3_stmt *
prepare(SqVtab *vt, const char *fmt, const char *a, const char *b)
{
	sqlite3_stmt *q;
	char	   *sql = sqlite3_mprintf(fmt, a, b);
	int			rc = sqlite3_prepare_v3(vt->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &q, NULL);

	sqlite3_free(sql);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	return q;
}

static sqlite3_stmt *
row_stmt(SqVtab *vt)
{
	sqlite3_stmt *q = vt->rowstmt;
	char	   *t = shadow(vt, "content"),
			   *cl = col_list(vt, "");

	vt->rowstmt = NULL;
	if (q == NULL)
	{
		char	   *sql = sqlite3_mprintf("SELECT %s FROM %s WHERE id = ?1", cl, t);
		int			rc = sqlite3_prepare_v3(vt->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &q, NULL);

		sqlite3_free(sql);
		if (rc != SQLITE_OK)
			elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	}
	sqlite3_free(t);
	sqlite3_free(cl);
	return q;
}

static void
row_stmt_give_back(SqVtab *vt, sqlite3_stmt *q)
{
	sqlite3_reset(q);
	if (vt->rowstmt)
		sqlite3_finalize(q);
	else
		vt->rowstmt = q;
}

/* The text the index holds for the content row id: the columns joined by the field separator */
static text *
fetch_text(SqVtab *vt, int64_t id)
{
	sqlite3_stmt *q = row_stmt(vt);
	StringInfoData buf;
	text	   *t;

	sqlite3_bind_int64(q, 1, id);
	if (sqlite3_step(q) != SQLITE_ROW)
	{
		row_stmt_give_back(vt, q);
		elog(ERROR, "sift: document %lld has no stored text", (long long) id);
	}
	initStringInfo(&buf);
	for (int i = 0; i < vt->ncols; i++)
	{
		if (i)
			appendStringInfoChar(&buf, SIFT_FIELD_SEP);
		appendBinaryStringInfo(&buf, sqlite3_column_text(q, i), sqlite3_column_bytes(q, i));
	}
	row_stmt_give_back(vt, q);
	t = cstring_to_text_with_len(buf.data, buf.len);
	return t;
}

static bool
vt_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SqVtab	   *vt = arg;

	if (want_doc)
		*doc = fetch_text(vt, sq_rowid(tid->blk, tid->off));
	return true;
}

/* ---------- analyzer and indexing ---------- */

static SiftAnalyzer *
analyzer(SqVtab *vt)
{
	if (vt->analyzer == NULL)
	{
		SiftAnalyzerConfig cfg;

		sift_analyzer_config_from_options(&vt->opts.base, &cfg);
		vt->acx = AllocSetContextCreate(TopMemoryContext, "sift analyzer", ALLOCSET_DEFAULT_SIZES);
		vt->analyzer = sift_analyzer_create(&cfg, vt->acx);
		sift_analyzer_set_positions(vt->analyzer, vt->opts.base.positions);
	}
	return vt->analyzer;
}

static void
check_rowid(sqlite3_int64 rowid)
{
	if (rowid < 0 || rowid >= SQ_ROWID_MAX)
		elog(ERROR, "sift: rowid %lld is out of range (0 to %lld)", (long long) rowid, (long long) SQ_ROWID_MAX - 1);
}

static void
index_doc(SqVtab *vt, sqlite3_int64 rowid, sqlite3_value **cols)
{
	StringInfoData buf;
	SiftTermFreq *terms;
	uint32		dl;
	int			n;

	initStringInfo(&buf);
	for (int i = 0; i < vt->ncols; i++)
	{
		if (i)
			appendStringInfoChar(&buf, SIFT_FIELD_SEP);
		appendBinaryStringInfo(&buf, sqlite3_value_text(cols[i]), sqlite3_value_bytes(cols[i]));
	}
	n = sift_analyze_doc(analyzer(vt), buf.data, buf.len, &terms, &dl);
	sq_inv_add(vt->pend, sq_key(rowid), terms, n, dl);
	sift_analyzer_reset(analyzer(vt));
	pfree(buf.data);
}

/* ---------- segments: flush, merge ---------- */

static int
by_docs(const void *a, const void *b)
{
	const SiftSegEntry *x = a,
			   *y = b;

	return (x->ndocs > y->ndocs) - (x->ndocs < y->ndocs);
}

static void
merge_some(SqVtab *vt, bool all)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	uint64		gens[SIFT_MAX_SEGMENTS];
	int			n = all ? dir->nsegs : Min(MERGE_COUNT, dir->nsegs);

	memcpy(segs, dir->segs, dir->nsegs * sizeof(SiftSegEntry));
	if (!all)
		qsort(segs, dir->nsegs, sizeof(SiftSegEntry), by_docs);
	for (int i = 0; i < n; i++)
		gens[i] = segs[i].gen;
	sq_merge(vt->st, gens, n, vt->opts.base.positions);
}

static bool
any_dirty(const SqVtab *vt)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].flags & SIFT_SEG_DIRTY)
			return true;
	return false;
}

static void
flush(SqVtab *vt, bool automerge)
{
	if (!sq_inv_empty(vt->pend))
	{
		if (sq_store_dir(vt->st)->nsegs >= SIFT_MAX_SEGMENTS - 1)
			merge_some(vt, true);
		sq_inv_write(vt->pend, vt->st);
	}
	else
		sq_inv_clear(vt->pend);
	sq_store_flush_pages(vt->st);
	while (automerge && sq_store_dir(vt->st)->nsegs >= MERGE_AT)
		merge_some(vt, false);
}

/* ---------- module: create, connect ---------- */

static void
free_vtab(SqVtab *vt)
{
	SqVtab	  **pp = &registry;

	while (*pp && *pp != vt)
		pp = &(*pp)->next;
	if (*pp)
		*pp = vt->next;
	sqlite3_finalize(vt->rowstmt);
	if (vt->st)
		sq_store_free(vt->st);
	if (vt->pend)
		sq_inv_free(vt->pend);
	if (vt->acx)
		MemoryContextDelete(vt->acx);
	for (int i = 0; i < vt->ncols; i++)
		sqlite3_free(vt->cols[i]);
	sqlite3_free(vt->cols);
	sqlite3_free(vt->schema);
	sqlite3_free(vt->name);
	sqlite3_free(vt);
}

static int
declare(SqVtab *vt, char **pzErr)
{
	char	   *sql = sqlite3_mprintf("CREATE TABLE x(");
	int			rc;

	for (int i = 0; i < vt->ncols; i++)
	{
		char	   *t = sqlite3_mprintf("%s\"%w\", ", sql, vt->cols[i]);

		sqlite3_free(sql);
		sql = t;
	}
	{
		char	   *t = sqlite3_mprintf("%s\"%w\" HIDDEN, rank HIDDEN)", sql, vt->name);

		sqlite3_free(sql);
		sql = t;
	}
	rc = sqlite3_declare_vtab(vt->db, sql);
	sqlite3_free(sql);
	if (rc != SQLITE_OK)
		*pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(vt->db));
	return rc;
}

static int
sqConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr, bool create)
{
	SqVtab	   *vt = sqlite3_malloc(sizeof(SqVtab));
	char	   *err = NULL;
	int			rc = SQLITE_OK;

	if (vt == NULL)
		return SQLITE_NOMEM;
	memset(vt, 0, sizeof(*vt));
	vt->db = db;
	vt->schema = sqlite3_mprintf("%s", argv[1]);
	vt->name = sqlite3_mprintf("%s", argv[2]);
	vt->cols = sqlite3_malloc(argc * sizeof(char *));
	options_default(&vt->opts);
	for (int i = 3; i < argc; i++)
		if (!option_set(&vt->opts, argv[i], &err))
			vt->cols[vt->ncols++] = unquote(argv[i]);
	if (err == NULL && vt->ncols == 0)
		err = sqlite3_mprintf("sift: a table needs at least one column");
	if (err == NULL && vt->ncols > SIFT_MAX_FIELDS)
		err = sqlite3_mprintf("sift: at most %d columns", SIFT_MAX_FIELDS);
	if (err)
	{
		*pzErr = err;
		free_vtab(vt);
		return SQLITE_ERROR;
	}
	if (vt->ncols > 1)
	{
		/* queries can name a column: title:word; unqualified queries see the plain concatenation */
		char	   *fields = sqlite3_mprintf("%s", "");

		for (int i = 0; i < vt->ncols; i++)
		{
			char	   *t = sqlite3_mprintf("%s%s%s", fields, i ? "," : "", vt->cols[i]);

			sqlite3_free(fields);
			fields = t;
		}
		snprintf(vt->opts.fields, sizeof(vt->opts.fields), "%s", fields);
		sqlite3_free(fields);
	}
	vt->st = sq_store_new(db, vt->schema, vt->name, &vt->opts);
	vt->pend = sq_inv_new(vt->opts.base.positions);
	rc = declare(vt, pzErr);
	if (rc == SQLITE_OK)
	{
		SQ_TRY();
		if (create)
		{
			char	   *t = shadow(vt, "content"),
					   *cl = col_list(vt, ""),
					   *sql = sqlite3_mprintf("CREATE TABLE %s(id INTEGER PRIMARY KEY, %s)", t, cl);
			int			r = sqlite3_exec(db, sql, NULL, NULL, NULL);

			sqlite3_free(sql);
			sqlite3_free(t);
			sqlite3_free(cl);
			if (r != SQLITE_OK)
				elog(ERROR, "sift: %s", sqlite3_errmsg(db));
			sq_store_create_tables(vt->st);
		}
		sq_store_refresh(vt->st, false);
		SQ_CATCH();
		*pzErr = sqlite3_mprintf("%s", sq_errmsg());
		rc = SQLITE_ERROR;
		SQ_END();
	}
	if (rc != SQLITE_OK)
	{
		free_vtab(vt);
		return rc;
	}
	sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS);
	vt->next = registry;
	registry = vt;
	*ppVtab = &vt->base;
	return SQLITE_OK;
}

static int
sqCreate(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **pzErr)
{
	return sqConnect(db, aux, argc, argv, pp, pzErr, true);
}

static int
sqConnectM(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **pzErr)
{
	return sqConnect(db, aux, argc, argv, pp, pzErr, false);
}

static int
sqDisconnect(sqlite3_vtab *pVtab)
{
	free_vtab((SqVtab *) pVtab);
	return SQLITE_OK;
}

static int
sqDestroy(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		char	   *t = shadow(vt, "content");
		char	   *sql = sqlite3_mprintf("DROP TABLE IF EXISTS %s", t);

		sq_store_drop_tables(vt->st);
		sqlite3_exec(vt->db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
		sqlite3_free(t);
	}
	TRY_END(vt)
}

static int
sqRename(sqlite3_vtab *pVtab, const char *newname)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	static const char *const sfx[] = {"pages", "segs", "meta", "content"};
	int			rc = SQLITE_OK;

	for (int i = 0; i < 4 && rc == SQLITE_OK; i++)
	{
		char	   *sql = sqlite3_mprintf("ALTER TABLE \"%w\".\"%w_%s\" RENAME TO \"%w_%s\"", vt->schema, vt->name, sfx[i],
										  newname, sfx[i]);

		rc = sqlite3_exec(vt->db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
	}
	if (rc == SQLITE_OK)
	{
		sq_store_rename(vt->st, newname);
		sqlite3_free(vt->name);
		vt->name = sqlite3_mprintf("%s", newname);
	}
	return rc;
}

static int
sqShadowName(const char *name)
{
	return strcmp(name, "pages") == 0 || strcmp(name, "segs") == 0 || strcmp(name, "meta") == 0 ||
		strcmp(name, "content") == 0;
}

/* ---------- best index ---------- */

#define IDX_MATCH	0x01			/* MATCH constraints, count in the bits above IDX_NSHIFT */
#define IDX_RANK	0x02			/* ranked: top-k in rank order */
#define IDX_LIMIT	0x04
#define IDX_ROWID	0x08
#define IDX_OFFSET	0x10
#define IDX_NSHIFT	8

static int
sqBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *info)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	int			idx = 0,
				nmatch = 0,
				argc = 0,
				limit = -1,
				offset = -1,
				rowid = -1;

	for (int i = 0; i < info->nConstraint; i++)
	{
		const struct sqlite3_index_constraint *c = &info->aConstraint[i];

		if (!c->usable)
			continue;
		if (c->op == SQLITE_INDEX_CONSTRAINT_MATCH && c->iColumn == vt->ncols)
		{
			info->aConstraintUsage[i].argvIndex = ++argc;
			info->aConstraintUsage[i].omit = 1;
			nmatch++;
		}
		else if (c->op == SQLITE_INDEX_CONSTRAINT_LIMIT)
			limit = i;
		else if (c->op == SQLITE_INDEX_CONSTRAINT_OFFSET)
			offset = i;
		else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn == -1)
			rowid = i;
	}
	if (nmatch > 0)
	{
		bool		rank_used = (info->colUsed >> (vt->ncols + 1)) & 1,
					by_rank = info->nOrderBy == 1 && info->aOrderBy[0].iColumn == vt->ncols + 1 && !info->aOrderBy[0].desc,
					by_rowid = info->nOrderBy == 1 && info->aOrderBy[0].iColumn == -1 && !info->aOrderBy[0].desc;

		idx |= IDX_MATCH | (nmatch << IDX_NSHIFT);
		/* the matches arrive in rank order when rank is read or asked for, in rowid order otherwise */
		if (rank_used || by_rank)
			idx |= IDX_RANK;
		info->orderByConsumed = (idx & IDX_RANK) ? by_rank : by_rowid;
		if ((idx & IDX_RANK) && limit >= 0)
		{
			info->aConstraintUsage[limit].argvIndex = ++argc;
			idx |= IDX_LIMIT;
			if (offset >= 0)
			{
				info->aConstraintUsage[offset].argvIndex = ++argc;
				idx |= IDX_OFFSET;
			}
		}
		info->estimatedCost = (idx & IDX_LIMIT) ? 50 : 1000;
		info->estimatedRows = (idx & IDX_LIMIT) ? 10 : 10000;
	}
	else if (rowid >= 0)
	{
		info->aConstraintUsage[rowid].argvIndex = ++argc;
		info->aConstraintUsage[rowid].omit = 1;
		idx |= IDX_ROWID;
		info->estimatedCost = 2;
		info->estimatedRows = 1;
		info->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
	}
	else
	{
		info->estimatedCost = 1e6;
		info->estimatedRows = 1000000;
		if (info->nOrderBy == 1 && info->aOrderBy[0].iColumn == -1 && !info->aOrderBy[0].desc)
			info->orderByConsumed = 1;
	}
	info->idxNum = idx;
	return SQLITE_OK;
}

/* ---------- cursors ---------- */

static int
sqOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	SqCursor   *c = sqlite3_malloc(sizeof(SqCursor));

	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof(*c));
	c->vt = vt;
	c->mode = SC_EOF;
	vt->ncursors++;
	*ppCursor = &c->base;
	return SQLITE_OK;
}

static void
cursor_reset(SqCursor *c)
{
	if (c->cx)
		MemoryContextDelete(c->cx);
	c->cx = NULL;
	c->state = NULL;
	c->plan = NULL;
	c->ranker = NULL;
	c->iter = NULL;
	c->have_pm = false;
	sqlite3_finalize(c->rows);
	c->rows = NULL;
	if (c->row)
		row_stmt_give_back(c->vt, c->row);
	c->row = NULL;
	c->mode = SC_EOF;
}

static int
sqClose(sqlite3_vtab_cursor *cur)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;

	cursor_reset(c);
	if (--vt->ncursors == 0)
		sq_store_release_garbage(vt->st);
	sqlite3_free(c);
	return SQLITE_OK;
}

static int
sqEof(sqlite3_vtab_cursor *cur)
{
	return ((SqCursor *) cur)->eof;
}

static void
ranked_next(SqCursor *c)
{
	ItemPointerData tid;
	float4		score;

	c->eof = !sift_rank_next(c->ranker, &tid, &score);
	if (!c->eof)
	{
		c->rowid = sq_rowid(tid.blk, tid.off);
		c->score = score;
	}
}

static void
iter_next(SqCursor *c)
{
	for (;;)
	{
		while (c->have_pm)
		{
			int			w = 0,
						off;

			while (w < SIFT_OFFMASK_WORDS && c->left[w] == 0)
				w++;
			if (w == SIFT_OFFMASK_WORDS)
				break;
			off = w * 64 + pg_rightmost_one_pos64(c->left[w]) + 1;
			c->left[w] &= c->left[w] - 1;
			c->rowid = sq_rowid(c->pm.blk, off);
			if (c->pm.recheck && !sift_page_sure(&c->pm, off) &&
				!sift_plan_recheck(c->plan, fetch_text(c->vt, c->rowid)))
				continue;
			return;
		}
		c->have_pm = sift_iter_next(c->iter, &c->pm);
		if (!c->have_pm)
		{
			c->eof = true;
			return;
		}
		memcpy(c->left, c->pm.offmask, sizeof(c->left));
	}
}

static void
rows_next(SqCursor *c)
{
	int			rc = sqlite3_step(c->rows);

	c->eof = rc != SQLITE_ROW;
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		elog(ERROR, "sift: %s", sqlite3_errmsg(c->vt->db));
	if (!c->eof)
		c->rowid = sqlite3_column_int64(c->rows, 0);
}

static void
start_match(SqCursor *c, int idx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = c->vt;
	int			nmatch = idx >> IDX_NSHIFT,
				a = 0;
	SiftQuery **qs = palloc(nmatch * sizeof(SiftQuery *));
	int64		k = -1;

	c->state = sift_state_open(sq_store_rel(vt->st), c->cx);
	for (; a < nmatch; a++)
	{
		const char *q = (const char *) sqlite3_value_text(argv[a]);

		qs[a] = sift_state_query(c->state, q ? q : "", sqlite3_value_bytes(argv[a]));
	}
	c->plan = sift_plan_bind(c->state, qs, nmatch, (idx & IDX_RANK) ? SIFT_BIND_SCORE : 0);
	if (idx & IDX_RANK)
	{
		if (idx & IDX_LIMIT)
		{
			k = sqlite3_value_int64(argv[a++]);
			if (idx & IDX_OFFSET)
			{
				int64		skip = sqlite3_value_int64(argv[a++]);

				k += Max(skip, 0);
			}
		}
		c->ranker = sift_rank_begin(c->plan, k > 0 ? k : (k == 0 ? 1 : -1), vt_verify, vt);
		c->mode = SC_RANK;
		ranked_next(c);
	}
	else
	{
		c->iter = sift_iter_begin(c->plan);
		c->mode = SC_ITER;
		iter_next(c);
	}
}

static void
start_rows(SqCursor *c, int idx, sqlite3_value **argv)
{
	SqVtab	   *vt = c->vt;
	char	   *t = shadow(vt, "content"),
			   *cl = col_list(vt, ""),
			   *sql = sqlite3_mprintf("SELECT id FROM %s %s", t, (idx & IDX_ROWID) ? "WHERE id = ?1" : "ORDER BY id");
	int			rc = sqlite3_prepare_v2(vt->db, sql, -1, &c->rows, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	sqlite3_free(cl);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	if (idx & IDX_ROWID)
		sqlite3_bind_value(c->rows, 1, argv[0]);
	c->mode = SC_ROWS;
	rows_next(c);
}

static int
sqFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;
	TRY_BEGIN(vt)
	{
		cursor_reset(c);
		c->eof = false;
		c->cx = AllocSetContextCreate(TopMemoryContext, "sift scan", ALLOCSET_DEFAULT_SIZES);
		MemoryContextSwitchTo(c->cx);
		if (!vt->in_txn)
			sq_store_refresh(vt->st, true);
		if (idxNum & IDX_MATCH)
		{
			flush(vt, false);
			start_match(c, idxNum, argc, argv);
		}
		else
			start_rows(c, idxNum, argv);
	}
	SQ_CATCH();
	cursor_reset(c);
	c->eof = true;
	rc = fail(&vt->base);
	SQ_END();
	MemoryContextSwitchTo(call_);
	leave(old_);
	return rc;
}

static int
sqNext(sqlite3_vtab_cursor *cur)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;
	TRY_BEGIN(vt)
	{
		MemoryContextSwitchTo(c->cx);
		if (c->row)
		{
			row_stmt_give_back(vt, c->row);
			c->row = NULL;
		}
		if (c->mode == SC_RANK)
			ranked_next(c);
		else if (c->mode == SC_ITER)
			iter_next(c);
		else if (c->mode == SC_ROWS)
			rows_next(c);
	}
	TRY_END(vt)
}

static int
sqRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid)
{
	*pRowid = ((SqCursor *) cur)->rowid;
	return SQLITE_OK;
}

static int
sqColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;

	if (i == vt->ncols)
		return SQLITE_OK;
	if (i == vt->ncols + 1)
	{
		if (c->mode == SC_RANK)
			sqlite3_result_double(ctx, -c->score);
		return SQLITE_OK;
	}
	if (c->row == NULL)
	{
		MemoryContext old = enter();
		bool		failed = false;

		SQ_TRY();
		c->row = row_stmt(vt);
		sqlite3_bind_int64(c->row, 1, c->rowid);
		if (sqlite3_step(c->row) != SQLITE_ROW)
		{
			row_stmt_give_back(vt, c->row);
			c->row = NULL;
		}
		SQ_CATCH();
		failed = true;
		SQ_END();
		leave(old);
		if (failed)
		{
			sqlite3_result_error(ctx, sq_errmsg(), -1);
			return SQLITE_ERROR;
		}
	}
	if (c->row)
		sqlite3_result_value(ctx, sqlite3_column_value(c->row, i));
	return SQLITE_OK;
}

/* ---------- writes ---------- */

static void
content_write(SqVtab *vt, sqlite3_int64 oldid, sqlite3_value *newid, sqlite3_value **cols, sqlite3_int64 *rowid)
{
	char	   *t = shadow(vt, "content"),
			   *cl = col_list(vt, ""),
			   *sql;
	sqlite3_stmt *q;
	int			rc;

	if (oldid >= 0)
	{
		char	   *set = sqlite3_mprintf("id = ?1");

		for (int i = 0; i < vt->ncols; i++)
		{
			char	   *s = sqlite3_mprintf("%s, c%d = ?%d", set, i, i + 3);

			sqlite3_free(set);
			set = s;
		}
		sql = sqlite3_mprintf("UPDATE %s SET %s WHERE id = ?2", t, set);
		sqlite3_free(set);
	}
	else
	{
		char	   *ph = sqlite3_mprintf("?1");

		for (int i = 0; i < vt->ncols; i++)
		{
			char	   *s = sqlite3_mprintf("%s, ?%d", ph, i + 3);

			sqlite3_free(ph);
			ph = s;
		}
		sql = sqlite3_mprintf("INSERT INTO %s(id, %s) VALUES(%s)", t, cl, ph);
		sqlite3_free(ph);
	}
	rc = sqlite3_prepare_v2(vt->db, sql, -1, &q, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	sqlite3_free(cl);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	if (sqlite3_value_type(newid) == SQLITE_NULL)
		sqlite3_bind_null(q, 1);
	else
		sqlite3_bind_value(q, 1, newid);
	if (oldid >= 0)
		sqlite3_bind_int64(q, 2, oldid);
	for (int i = 0; i < vt->ncols; i++)
		sqlite3_bind_value(q, i + 3, cols[i]);
	rc = sqlite3_step(q);
	if (rc != SQLITE_DONE)
	{
		char	   *msg = sqlite3_mprintf("%s", sqlite3_errmsg(vt->db));

		sqlite3_finalize(q);
		sq_error_set(ERRCODE_INTERNAL_ERROR, "%s", msg);
	}
	sqlite3_finalize(q);
	*rowid = oldid >= 0 && sqlite3_value_type(newid) == SQLITE_NULL ? oldid : sqlite3_last_insert_rowid(vt->db);
	if (oldid >= 0 && sqlite3_value_type(newid) != SQLITE_NULL)
		*rowid = sqlite3_value_int64(newid);
}

static void
index_remove(SqVtab *vt, sqlite3_int64 rowid)
{
	SiftKey		key = sq_key(rowid);

	if (!sq_inv_remove(vt->pend, key))
		sq_delete_key(vt->st, key);
}

static void
content_delete(SqVtab *vt, sqlite3_int64 rowid)
{
	char	   *t = shadow(vt, "content"),
			   *sql = sqlite3_mprintf("DELETE FROM %s WHERE id = %lld", t, (long long) rowid);
	int			rc = sqlite3_exec(vt->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
}

static void
command(SqVtab *vt, const char *cmd)
{
	if (strcmp(cmd, "merge") == 0 || strcmp(cmd, "optimize") == 0)
	{
		flush(vt, false);
		if (sq_store_dir(vt->st)->nsegs > 1 || any_dirty(vt))
			merge_some(vt, true);
	}
	else if (strcmp(cmd, "flush") == 0)
		flush(vt, true);
	else if (strcmp(cmd, "rebuild") == 0)
	{
		char	   *t = shadow(vt, "content"),
				   *sql = sqlite3_mprintf("SELECT id, %s FROM %s ORDER BY id", col_list(vt, ""), t);
		sqlite3_stmt *q;
		SiftDirSnapshot dir;

		sq_inv_clear(vt->pend);
		sq_store_flush_pages(vt->st);
		dir = *sq_store_dir(vt->st);
		for (int i = 0; i < dir.nsegs; i++)
			sq_store_retire(vt->st, dir.segs[i].gen);
		if (sqlite3_prepare_v2(vt->db, sql, -1, &q, NULL) != SQLITE_OK)
			elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
		while (sqlite3_step(q) == SQLITE_ROW)
		{
			sqlite3_value *cols[SIFT_MAX_FIELDS];

			for (int i = 0; i < vt->ncols; i++)
				cols[i] = sqlite3_column_value(q, i + 1);
			index_doc(vt, sqlite3_column_int64(q, 0), cols);
			if (sq_inv_postings(vt->pend) > FLUSH_POSTINGS)
				flush(vt, false);
		}
		sqlite3_finalize(q);
		sqlite3_free(sql);
		sqlite3_free(t);
		flush(vt, true);
	}
	else
		elog(ERROR, "sift: unknown command \"%s\"", cmd);
}

static int
sqUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite3_int64 *pRowid)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		if (argc == 1)
		{
			index_remove(vt, sqlite3_value_int64(argv[0]));
			content_delete(vt, sqlite3_value_int64(argv[0]));
		}
		else if (sqlite3_value_type(argv[2 + vt->ncols]) != SQLITE_NULL)
			command(vt, (const char *) sqlite3_value_text(argv[2 + vt->ncols]));
		else
		{
			sqlite3_int64 oldid = sqlite3_value_type(argv[0]) == SQLITE_NULL ? -1 : sqlite3_value_int64(argv[0]);

			if (oldid >= 0)
				index_remove(vt, oldid);
			if (sqlite3_value_type(argv[1]) != SQLITE_NULL)
				check_rowid(sqlite3_value_int64(argv[1]));
			content_write(vt, oldid, argv[1], argv + 2, pRowid);
			check_rowid(*pRowid);
			index_doc(vt, *pRowid, argv + 2);
			if (sq_inv_postings(vt->pend) > FLUSH_POSTINGS)
				flush(vt, false);
		}
	}
	TRY_END(vt)
}

/* ---------- transactions ---------- */

static int
sqBegin(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		sq_store_refresh(vt->st, true);
		vt->in_txn = true;
	}
	TRY_END(vt)
}

static int
sqSync(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		flush(vt, true);
	}
	TRY_END(vt)
}

static int
sqCommit(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;

	vt->in_txn = false;
	if (vt->ncursors == 0)
		sq_store_release_garbage(vt->st);
	return SQLITE_OK;
}

static int
sqRollback(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		sq_inv_clear(vt->pend);
		sq_store_rollback(vt->st);
		vt->in_txn = false;
	}
	TRY_END(vt)
}

/* What came before the savepoint must be in the tables SQLite rolls back to */
static int
sqSavepoint(sqlite3_vtab *pVtab, int n)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		flush(vt, false);
	}
	TRY_END(vt)
}

static int
sqRelease(sqlite3_vtab *pVtab, int n)
{
	return SQLITE_OK;
}

static int
sqRollbackTo(sqlite3_vtab *pVtab, int n)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		sq_inv_clear(vt->pend);
		sq_store_rollback(vt->st);
	}
	TRY_END(vt)
}

static const sqlite3_module sift_module = {
	.iVersion = 3,
	.xCreate = sqCreate,
	.xConnect = sqConnectM,
	.xBestIndex = sqBestIndex,
	.xDisconnect = sqDisconnect,
	.xDestroy = sqDestroy,
	.xOpen = sqOpen,
	.xClose = sqClose,
	.xFilter = sqFilter,
	.xNext = sqNext,
	.xEof = sqEof,
	.xColumn = sqColumn,
	.xRowid = sqRowid,
	.xUpdate = sqUpdate,
	.xBegin = sqBegin,
	.xSync = sqSync,
	.xCommit = sqCommit,
	.xRollback = sqRollback,
	.xRename = sqRename,
	.xSavepoint = sqSavepoint,
	.xRelease = sqRelease,
	.xRollbackTo = sqRollbackTo,
	.xShadowName = sqShadowName,
};

/* ---------- SQL functions ---------- */

static SqVtab *
lookup_table(sqlite3 *db, const char *name)
{
	for (SqVtab *vt = registry; vt; vt = vt->next)
		if (vt->db == db && sqlite3_stricmp(vt->name, name) == 0)
			return vt;
	return NULL;
}

/* The tables of a connection connect on first use: touching the table by name connects it */
static SqVtab *
find_table(sqlite3 *db, const char *name)
{
	SqVtab	   *vt = name ? lookup_table(db, name) : NULL;

	if (vt == NULL && name != NULL)
	{
		char	   *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" LIMIT 0", name);

		sqlite3_exec(db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
		vt = lookup_table(db, name);
	}
	return vt;
}

/* sift_count(table, query): matching documents, from the bitmaps without visiting them */
static void
fn_count(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = find_table(sqlite3_context_db_handle(ctx), (const char *) sqlite3_value_text(argv[0]));
	int64		n = 0;
	MemoryContext old;
	bool		failed = false;

	if (vt == NULL)
	{
		sqlite3_result_error(ctx, "sift_count: no such sift table", -1);
		return;
	}
	old = enter();
	SQ_TRY();
	{
		const char *q = (const char *) sqlite3_value_text(argv[1]);
		SiftIndexState *state;
		SiftQuery  *qs[1];
		SiftPlan   *plan;
		SiftVis		vis;

		if (!vt->in_txn)
			sq_store_refresh(vt->st, true);
		flush(vt, false);
		state = sift_state_open(sq_store_rel(vt->st), CurrentMemoryContext);
		qs[0] = sift_state_query(state, q ? q : "", sqlite3_value_bytes(argv[1]));
		plan = sift_plan_bind(state, qs, 1, 0);
		memset(&vis, 0, sizeof(vis));
		vis.use_vm = true;
		n = sift_eval_count(plan, &vis, vt_verify, vt);
	}
	SQ_CATCH();
	failed = true;
	SQ_END();
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	else
		sqlite3_result_int64(ctx, n);
	leave(old);
}

/* sift_stat(table): one line of facts about the table */
static void
fn_stat(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = find_table(sqlite3_context_db_handle(ctx), (const char *) sqlite3_value_text(argv[0]));
	SqStoreStats s;
	const SiftDirSnapshot *d;
	char	   *msg;

	if (vt == NULL)
	{
		sqlite3_result_error(ctx, "sift_stat: no such sift table", -1);
		return;
	}
	d = sq_store_dir(vt->st);
	sq_store_stats(vt->st, &s);
	msg = sqlite3_mprintf("segments=%d docs=%llu pages=%llu bytes=%llu cached_pages=%llu pages_read=%llu pending_postings=%llu",
						  d->nsegs, (unsigned long long) d->meta.ndocs, (unsigned long long) s.pages_total,
						  (unsigned long long) s.bytes_total, (unsigned long long) s.pages_cached,
						  (unsigned long long) s.pages_read, (unsigned long long) sq_inv_postings(vt->pend));
	sqlite3_result_text(ctx, msg, -1, sqlite3_free);
}

static void
fn_read_mode(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	sq_read_mode = sqlite3_value_int(argv[0]);
	sqlite3_result_int(ctx, sq_read_mode);
}

static void
fn_drop_cache(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	SqVtab	   *vt = find_table(sqlite3_context_db_handle(ctx), (const char *) sqlite3_value_text(argv[0]));

	if (vt == NULL)
		sqlite3_result_error(ctx, "sift_drop_cache: no such sift table", -1);
	else
	{
		sq_store_invalidate(vt->st);
		if (vt->ncursors == 0)
			sq_store_release_garbage(vt->st);
	}
}

static void
fn_isa(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	sqlite3_result_text(ctx, sift_simd_level_name(sift_cpu.active), -1, SQLITE_STATIC);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int
sqlite3_sift_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
	int			rc;

	SQLITE_EXTENSION_INIT2(pApi);
	sq_mem_init();
	sift_cpu_init();
	rc = sqlite3_create_module(db, "sift", &sift_module, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_count", 2, SQLITE_UTF8, NULL, fn_count, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_stat", 1, SQLITE_UTF8, NULL, fn_stat, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_read_mode", 1, SQLITE_UTF8, NULL, fn_read_mode, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_drop_cache", 1, SQLITE_UTF8, NULL, fn_drop_cache, NULL, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_create_function(db, "sift_isa", 0, SQLITE_UTF8, NULL, fn_isa, NULL, NULL);
	return rc;
}
