/*
 * sqvtab.c - the SQLite virtual table module "sift":
 *
 *   CREATE VIRTUAL TABLE docs USING sift(title, body, positions=1, stemmer='english');
 *   SELECT rowid, rank FROM docs WHERE docs MATCH 'quick fox*' ORDER BY rank LIMIT 10;
 *   INSERT INTO docs(docs) VALUES('optimize');
 *   INSERT INTO docs(docs, rank) VALUES('cache-size', 16384);
 *
 * rank is the negated BM25 score (lower is better, as FTS5's). Documents go to an in-memory inverted buffer;
 * at xSync (or before a query, or at a savepoint) the buffer is written as an immutable segment inside the
 * SQLite transaction, so the table commits and rolls back with it. The text comes from <t>_content, from the
 * user's table (content='tbl') or is not kept at all (content=''). See README.md.
 */
#include "sqvtab.h"

#include <ctype.h>

SQLITE_EXTENSION_INIT3

#define FLUSH_POSTINGS	(64 * 1000 * 1000)	/* about 1 GB of buffer */

/* ---------- errors and contexts ---------- */

MemoryContext
sq_enter(void)
{
	MemoryContext cx = AllocSetContextCreate(NULL, "sift call", ALLOCSET_DEFAULT_SIZES);

	sq_mem_init();
	return MemoryContextSwitchTo(cx);
}

void
sq_leave(MemoryContext old)
{
	MemoryContext cx = CurrentMemoryContext;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static int
fail(SqVtab *vt)
{
	int			code = sq_errcode();

	sqlite3_free(vt->base.zErrMsg);
	vt->base.zErrMsg = sqlite3_mprintf("%s", sq_errmsg());
	sq_store_recover(vt->st);
	return code == ERRCODE_INDEX_CORRUPTED ? SQLITE_CORRUPT_VTAB : SQLITE_ERROR;
}

/* The body runs in a context of its own that the end deletes; an ERROR inside lands in SQ_CATCH and becomes SQLITE_ERROR */
#define TRY_BEGIN(vt) \
	int rc = SQLITE_OK; \
	MemoryContext old_ = sq_enter(), \
				call_ = CurrentMemoryContext; \
	SqStore *prev_ = sq_store_enter((vt)->st); \
	SQ_TRY();

#define TRY_END(vt) \
	SQ_CATCH(); \
	rc = fail(vt); \
	SQ_END(); \
	MemoryContextSwitchTo(call_); \
	sq_store_leave(prev_); \
	sq_leave(old_); \
	return rc;

/* ---------- options ---------- */

static void
options_default(SqVtab *vt)
{
	SqOptions  *o = &vt->opts;

	memset(o, 0, sizeof(*o));
	o->base.k1 = SIFT_DEFAULT_K1;
	o->base.b = SIFT_DEFAULT_B;
	o->base.case_folding = true;
	o->base.accent_folding = true;
	o->base.positions = true;
	o->base.analyzer = SIFT_ANALYZER_TEXT;
	strcpy(o->stemmer, "english");
	vt->chunk_bytes = SQ_CHUNK_DEFAULT;
	vt->cache_kib = SQ_CACHE_DEFAULT_KIB;
	vt->automerge = SIFT_MERGE_FANIN;
	vt->pending_postings = FLUSH_POSTINGS;
	vt->ext_rowid = sqlite3_mprintf("rowid");
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

static void
replace(char **dst, char *val)
{
	sqlite3_free(*dst);
	*dst = val;
}

/* key=value: false when arg is a column name */
static bool
option_set(SqVtab *vt, const char *arg, char **err)
{
	SqOptions  *o = &vt->opts;
	const char *eq = strchr(arg, '=');
	char	   *key,
			   *val;

	if (eq == NULL)
		return false;
	key = sqlite3_mprintf("%.*s", (int) (eq - arg), arg);
	replace(&key, unquote(key));
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
	else if (strcmp(key, "analyzer") == 0 && (strcmp(val, "text") == 0 || strcmp(val, "keyword") == 0))
		o->base.analyzer = val[0] == 'k' ? SIFT_ANALYZER_KEYWORD : SIFT_ANALYZER_TEXT;
	else if (strcmp(key, "chunk") == 0)
		vt->chunk_bytes = atoi(val);
	else if (strcmp(key, "cache_size") == 0)
		vt->cache_kib = atoi(val);
	else if (strcmp(key, "automerge") == 0)
		vt->automerge = atoi(val);
	else if (strcmp(key, "pending_postings") == 0)
		vt->pending_postings = atoll(val);
	else if (strcmp(key, "parallel") == 0)
		vt->parallel = atoi(val);
	else if (strcmp(key, "content") == 0)
	{
		vt->content = val[0] ? SQ_CONTENT_EXTERNAL : SQ_CONTENT_NONE;
		replace(&vt->ext_table, val[0] ? sqlite3_mprintf("%s", val) : NULL);
	}
	else if (strcmp(key, "content_rowid") == 0)
		replace(&vt->ext_rowid, sqlite3_mprintf("%s", val));
	else
		*err = sqlite3_mprintf("sift: unknown option \"%s\"", key);
	sqlite3_free(key);
	sqlite3_free(val);
	return true;
}

/* ---------- content: where the text of a document comes from ---------- */

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

/* The table that holds the text, its id column and its columns, as SQL */
static void
source(const SqVtab *vt, char **table, char **idcol, char **cols)
{
	if (vt->content == SQ_CONTENT_INTERNAL)
	{
		*table = shadow(vt, "content");
		*idcol = sqlite3_mprintf("id");
		*cols = col_list(vt, "");
		return;
	}
	*table = sqlite3_mprintf("\"%w\".\"%w\"", vt->schema, vt->ext_table);
	*idcol = sqlite3_mprintf("\"%w\"", vt->ext_rowid);
	*cols = sqlite3_mprintf("%s", "");
	for (int i = 0; i < vt->ncols; i++)
	{
		char	   *t = sqlite3_mprintf("%s%s\"%w\"", *cols, i ? ", " : "", vt->cols[i]);

		sqlite3_free(*cols);
		*cols = t;
	}
}

typedef enum SourceQuery
{
	SRC_ROW,							/* the columns of the document ?1 */
	SRC_IDS,							/* every document id */
	SRC_ID,								/* the id ?1, if there is such a document */
	SRC_ALL								/* id and columns of every document */
} SourceQuery;

static sqlite3_stmt *
source_stmt(SqVtab *vt, SourceQuery kind, int flags)
{
	sqlite3_stmt *q;
	char	   *t,
			   *id,
			   *cl;
	char	   *sql;
	int			rc;

	if (vt->content == SQ_CONTENT_NONE)
		sq_error_set(ERRCODE_FEATURE_NOT_SUPPORTED, "sift: table \"%s\" is contentless: it keeps no text to read", vt->name);
	source(vt, &t, &id, &cl);
	if (kind == SRC_ROW)
		sql = sqlite3_mprintf("SELECT %s FROM %s WHERE %s = ?1", cl, t, id);
	else if (kind == SRC_ID)
		sql = sqlite3_mprintf("SELECT %s FROM %s WHERE %s = ?1", id, t, id);
	else if (kind == SRC_IDS)
		sql = sqlite3_mprintf("SELECT %s FROM %s ORDER BY %s", id, t, id);
	else
		sql = sqlite3_mprintf("SELECT %s, %s FROM %s ORDER BY %s", id, cl, t, id);
	rc = sqlite3_prepare_v3(vt->db, sql, -1, flags, &q, NULL);
	sqlite3_free(sql);
	sqlite3_free(t);
	sqlite3_free(id);
	sqlite3_free(cl);
	if (rc != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
	return q;
}

/* The prepared SELECT of one document's columns, taken from the one the table keeps for reuse */
static sqlite3_stmt *
row_stmt(SqVtab *vt)
{
	sqlite3_stmt *q = vt->rowstmt;

	vt->rowstmt = NULL;
	return q ? q : source_stmt(vt, SRC_ROW, SQLITE_PREPARE_PERSISTENT);
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

/* The text the index holds for the document: the columns joined by the field separator */
static text *
fetch_text(SqVtab *vt, int64_t id)
{
	sqlite3_stmt *q = row_stmt(vt);
	StringInfoData buf;

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
	return cstring_to_text_with_len(buf.data, buf.len);
}

char *
sq_vt_doc_text(SqVtab *vt, int64_t rowid, int col, int *len)
{
	sqlite3_stmt *q;
	StringInfoData buf;

	if (vt->content == SQ_CONTENT_NONE)
		return NULL;
	q = row_stmt(vt);
	sqlite3_bind_int64(q, 1, rowid);
	if (sqlite3_step(q) != SQLITE_ROW)
	{
		row_stmt_give_back(vt, q);
		return NULL;
	}
	initStringInfo(&buf);
	for (int i = 0; i < vt->ncols; i++)
		if (col < 0 || col == i)
		{
			if (buf.len)
				appendStringInfoChar(&buf, ' ');
			appendBinaryStringInfo(&buf, sqlite3_column_text(q, i), sqlite3_column_bytes(q, i));
		}
	row_stmt_give_back(vt, q);
	*len = buf.len;
	return buf.data;
}

bool
sq_vt_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	SqVtab	   *vt = arg;

	if (want_doc)
		*doc = fetch_text(vt, sq_rowid(tid->blk, tid->off));
	return true;
}

/* ---------- analyzer and indexing ---------- */

SiftAnalyzer *
sq_vt_analyzer(SqVtab *vt)
{
	if (vt->analyzer == NULL)
	{
		SiftAnalyzerConfig cfg;

		sift_analyzer_config_from_options(&vt->opts.base, &cfg);
		vt->acx = AllocSetContextCreate(NULL, "sift analyzer", ALLOCSET_DEFAULT_SIZES);
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
	n = sift_analyze_doc(sq_vt_analyzer(vt), buf.data, buf.len, &terms, &dl);
	sq_inv_add(vt->pend, sq_key(rowid), terms, n, dl);
	sift_analyzer_reset(sq_vt_analyzer(vt));
	pfree(buf.data);
}

/* ---------- segments: flush, merge ---------- */

static int
fanin(const SqVtab *vt)
{
	return vt->automerge >= 2 ? vt->automerge : SIFT_MERGE_FANIN;
}

/* Merge steps by the tier policy, at most max of them; each is a bounded rewrite of a few segments */
static int
merge_steps(SqVtab *vt, int64_t max)
{
	uint64		sel[SIFT_MAX_SEGMENTS];
	int64_t		steps = 0;

	while (steps < max)
	{
		int			n = sq_merge_pick(vt->st, false, fanin(vt), sel);

		if (n == 0)
			break;
		sq_merge(vt->st, sel, n);
		steps++;
	}
	return steps;
}

/* Everything into one segment, a few of the smallest at a time */
static void
optimize(SqVtab *vt)
{
	uint64		sel[SIFT_MAX_SEGMENTS];
	int			at_once = 2 * fanin(vt);

	for (;;)
	{
		int			n = sq_merge_pick(vt->st, true, 2, sel);

		if (n == 0)
			break;
		sq_merge(vt->st, sel, Min(n, at_once));
		if (n <= at_once)
			break;
	}
}

void
sq_vt_flush(SqVtab *vt, bool automerge)
{
	if (!sq_inv_empty(vt->pend))
	{
		if (sq_store_dir(vt->st)->nsegs >= SIFT_MAX_SEGMENTS - 1 && merge_steps(vt, 1) == 0)
			optimize(vt);
		sq_inv_write(vt->pend, vt->st);
	}
	else
		sq_inv_clear(vt->pend);
	sq_store_flush_pages(vt->st);
	if (automerge && vt->automerge >= 2)
		merge_steps(vt, INT64_MAX);
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

/* ---------- module: create, connect ---------- */

static void
free_vtab(SqVtab *vt)
{
	SqVtab	  **pp = &vt->conn->tables;

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
	sqlite3_free(vt->ext_table);
	sqlite3_free(vt->ext_rowid);
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

/* The settings other connections may have changed with a command */
static void
load_settings(SqVtab *vt)
{
	vt->automerge = (int) sq_store_config_get(vt->st, "automerge", vt->automerge);
	vt->pending_postings = sq_store_config_get(vt->st, "pending-postings", vt->pending_postings);
}

static char *
fields_of(const SqVtab *vt)
{
	char	   *fields = sqlite3_mprintf("%s", "");

	for (int i = 0; i < vt->ncols; i++)
	{
		char	   *t = sqlite3_mprintf("%s%s%s", fields, i ? "," : "", vt->cols[i]);

		sqlite3_free(fields);
		fields = t;
	}
	return fields;
}

static char *
check_args(SqVtab *vt, int argc, const char *const *argv)
{
	char	   *err = NULL;

	vt->cols = sqlite3_malloc(argc * sizeof(char *));
	options_default(vt);
	for (int i = 3; i < argc && err == NULL; i++)
		if (!option_set(vt, argv[i], &err))
			vt->cols[vt->ncols++] = unquote(argv[i]);
	if (err == NULL && vt->ncols == 0)
		err = sqlite3_mprintf("sift: a table needs at least one column");
	if (err == NULL && vt->ncols > SIFT_MAX_FIELDS)
		err = sqlite3_mprintf("sift: at most %d columns", SIFT_MAX_FIELDS);
	if (err == NULL && vt->content == SQ_CONTENT_NONE && !vt->opts.base.positions)
		err = sqlite3_mprintf("sift: a contentless table needs positions=1 (phrases cannot be rechecked against text)");
	return err;
}

static void
create_content(SqVtab *vt)
{
	char	   *t = shadow(vt, "content"),
			   *cl = col_list(vt, ""),
			   *sql = sqlite3_mprintf("CREATE TABLE %s(id INTEGER PRIMARY KEY, %s)", t, cl);
	int			r = sqlite3_exec(vt->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	sqlite3_free(cl);
	if (r != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
}

static int
sqConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr, bool create)
{
	SqVtab	   *vt = sqlite3_malloc(sizeof(SqVtab));
	char	   *err;
	int			rc = SQLITE_OK;

	if (vt == NULL)
		return SQLITE_NOMEM;
	memset(vt, 0, sizeof(*vt));
	vt->conn = aux;
	vt->db = db;
	vt->schema = sqlite3_mprintf("%s", argv[1]);
	vt->name = sqlite3_mprintf("%s", argv[2]);
	if ((err = check_args(vt, argc, argv)) != NULL)
	{
		*pzErr = err;
		free_vtab(vt);
		return SQLITE_ERROR;
	}
	if (vt->ncols > 1)
	{
		/* queries can name a column: title:word; unqualified queries see the plain concatenation */
		char	   *fields = fields_of(vt);

		snprintf(vt->opts.fields, sizeof(vt->opts.fields), "%s", fields);
		sqlite3_free(fields);
	}
	vt->st = sq_store_new(db, vt->schema, vt->name, &vt->opts, vt->chunk_bytes, vt->cache_kib);
	vt->pend = sq_inv_new(vt->opts.base.positions);
	vt->next = vt->conn->tables;
	vt->conn->tables = vt;
	rc = declare(vt, pzErr);
	if (rc == SQLITE_OK)
	{
		SqStore    *prev;

		sq_mem_init();
		prev = sq_store_enter(vt->st);
		SQ_TRY();
		if (create)
		{
			if (vt->content == SQ_CONTENT_INTERNAL)
				create_content(vt);
			sq_store_create_tables(vt->st);
		}
		sq_store_refresh(vt->st, false);
		load_settings(vt);
		SQ_CATCH();
		*pzErr = sqlite3_mprintf("%s", sq_errmsg());
		rc = SQLITE_ERROR;
		SQ_END();
		sq_store_leave(prev);
	}
	if (rc != SQLITE_OK)
	{
		free_vtab(vt);
		return rc;
	}
	sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS);
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

static void
drop_content(SqVtab *vt)
{
	char	   *t = shadow(vt, "content"),
			   *sql = sqlite3_mprintf("DROP TABLE IF EXISTS %s", t);
	int			r = sqlite3_exec(vt->db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);
	sqlite3_free(t);
	if (r != SQLITE_OK)
		elog(ERROR, "sift: %s", sqlite3_errmsg(vt->db));
}

/* SQLite does not call xDisconnect after a successful xDestroy: the table is freed here */
static int
sqDestroy(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	MemoryContext old = sq_enter();
	SqStore    *prev = sq_store_enter(vt->st);
	int			rc = SQLITE_OK;

	sqlite3_finalize(vt->rowstmt);
	vt->rowstmt = NULL;
	SQ_TRY();
	sq_store_drop_tables(vt->st);
	if (vt->content == SQ_CONTENT_INTERNAL)
		drop_content(vt);
	SQ_CATCH();
	rc = fail(vt);
	SQ_END();
	sq_store_leave(prev);
	sq_leave(old);
	if (rc == SQLITE_OK)
		free_vtab(vt);
	return rc;
}

static int
sqRename(sqlite3_vtab *pVtab, const char *newname)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	static const char *const sfx[] = {"chunks", "segs", "meta", "config", "content"};
	int			rc = SQLITE_OK;

	sqlite3_finalize(vt->rowstmt);
	vt->rowstmt = NULL;
	sq_store_rename(vt->st, vt->name);		/* closes the store's statements and blob handles on the old names */
	for (int i = 0; i < 5 && rc == SQLITE_OK; i++)
	{
		char	   *sql;

		if (i == 4 && vt->content != SQ_CONTENT_INTERNAL)
			break;
		sql = sqlite3_mprintf("ALTER TABLE \"%w\".\"%w_%s\" RENAME TO \"%w_%s\"", vt->schema, vt->name, sfx[i], newname, sfx[i]);
		rc = sqlite3_exec(vt->db, sql, NULL, NULL, &vt->base.zErrMsg);
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
	return strcmp(name, "chunks") == 0 || strcmp(name, "segs") == 0 || strcmp(name, "meta") == 0 ||
		strcmp(name, "config") == 0 || strcmp(name, "content") == 0;
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
	sq_store_hold(vt->st, 1);
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
	c->qs = NULL;
	c->nqs = 0;
	c->hits = NULL;
	c->nhits = c->hitpos = 0;
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

	cursor_reset(c);
	c->vt->ncursors--;
	sq_store_hold(c->vt->st, -1);
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

	if (c->hits)
	{
		c->eof = c->hitpos >= c->nhits;
		if (!c->eof)
		{
			c->rowid = c->hits[c->hitpos].rowid;
			c->score = c->hits[c->hitpos++].score;
		}
		return;
	}
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

/* The rows the scan has to produce: LIMIT plus OFFSET, -1 for all */
static int64_t
ranked_limit(int idx, int *a, sqlite3_value **argv)
{
	int64_t		k = -1;

	if (idx & IDX_LIMIT)
	{
		k = sqlite3_value_int64(argv[(*a)++]);
		if (idx & IDX_OFFSET)
		{
			int64_t		skip = sqlite3_value_int64(argv[(*a)++]);

			k += Max(skip, 0);
		}
	}
	return k > 0 ? k : (k == 0 ? 1 : -1);
}

static void
start_match(SqCursor *c, int idx, sqlite3_value **argv)
{
	SqVtab	   *vt = c->vt;
	int			nmatch = idx >> IDX_NSHIFT,
				a = nmatch;

	SqQuery    *texts = palloc(nmatch * sizeof(SqQuery));

	c->state = sift_state_open(sq_store_rel(vt->st), c->cx);
	c->qs = palloc(nmatch * sizeof(SiftQuery *));
	c->nqs = nmatch;
	for (int i = 0; i < nmatch; i++)
	{
		const char *q = (const char *) sqlite3_value_text(argv[i]);

		texts[i] = (SqQuery) {q ? q : "", sqlite3_value_bytes(argv[i])};
		c->qs[i] = sift_state_query(c->state, texts[i].text, texts[i].len);
	}
	c->plan = sift_plan_bind(c->state, c->qs, nmatch, (idx & IDX_RANK) ? SIFT_BIND_SCORE : 0);
	if (idx & IDX_RANK)
	{
		int64_t		k = ranked_limit(idx, &a, argv);

		c->mode = SC_RANK;
		if (k > 0 && sq_par_available(vt, c->plan, texts, nmatch))
			c->hits = sq_par_rank(vt, texts, nmatch, k, &c->nhits);
		if (c->hits == NULL)
			c->ranker = sift_rank_begin(c->plan, k, sq_vt_verify, vt);
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
	c->rows = source_stmt(c->vt, (idx & IDX_ROWID) ? SRC_ID : SRC_IDS, 0);
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
		c->cx = AllocSetContextCreate(NULL, "sift scan", ALLOCSET_DEFAULT_SIZES);
		MemoryContextSwitchTo(c->cx);
		if (!vt->in_txn && sq_store_refresh(vt->st, true))
			load_settings(vt);
		if (idxNum & IDX_MATCH)
		{
			sq_vt_flush(vt, false);
			start_match(c, idxNum, argv);
		}
		else
			start_rows(c, idxNum, argv);
	}
	SQ_CATCH();
	cursor_reset(c);
	c->eof = true;
	rc = fail(vt);
	SQ_END();
	MemoryContextSwitchTo(call_);
	sq_store_leave(prev_);
	sq_leave(old_);
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

/* Steps the content row of the current document; false on an error, which is the SQLite error of ctx */
static bool
load_row(SqCursor *c, sqlite3_context *ctx)
{
	SqVtab	   *vt = c->vt;
	MemoryContext old = sq_enter();
	SqStore    *prev = sq_store_enter(vt->st);
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
	sq_store_leave(prev);
	sq_leave(old);
	if (failed)
		sqlite3_result_error(ctx, sq_errmsg(), -1);
	return !failed;
}

static int
sqColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
	SqCursor   *c = (SqCursor *) cur;
	SqVtab	   *vt = c->vt;

	if (i == vt->ncols)
	{
		sqlite3_result_pointer(ctx, c, "sift_cursor", NULL);
		return SQLITE_OK;
	}
	if (i == vt->ncols + 1)
	{
		if (c->mode == SC_RANK)
			sqlite3_result_double(ctx, -c->score);
		return SQLITE_OK;
	}
	if (vt->content == SQ_CONTENT_NONE)
		return SQLITE_OK;
	if (c->row == NULL && !load_row(c, ctx))
		return SQLITE_ERROR;
	if (c->row)
		sqlite3_result_value(ctx, sqlite3_column_value(c->row, i));
	return SQLITE_OK;
}

/* ---------- writes ---------- */

/* The internal content table: INSERT or UPDATE of one row, whose rowid is returned */
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

/* Reads every document of the content source again: the index is rebuilt from the text */
static void
rebuild(SqVtab *vt)
{
	sqlite3_stmt *q;
	SiftDirSnapshot dir;

	sq_inv_clear(vt->pend);
	sq_store_flush_pages(vt->st);
	dir = *sq_store_dir(vt->st);
	for (int i = 0; i < dir.nsegs; i++)
		sq_store_retire(vt->st, dir.segs[i].gen);
	q = source_stmt(vt, SRC_ALL, 0);
	while (sqlite3_step(q) == SQLITE_ROW)
	{
		sqlite3_value *cols[SIFT_MAX_FIELDS];

		for (int i = 0; i < vt->ncols; i++)
			cols[i] = sqlite3_column_value(q, i + 1);
		check_rowid(sqlite3_column_int64(q, 0));
		index_doc(vt, sqlite3_column_int64(q, 0), cols);
		if ((int64_t) sq_inv_postings(vt->pend) > vt->pending_postings)
			sq_vt_flush(vt, false);
	}
	sqlite3_finalize(q);
	sq_vt_flush(vt, true);
}

/* ---------- integrity check ---------- */

#define BAD(...) sq_error_set(ERRCODE_INDEX_CORRUPTED, "sift: integrity-check: " __VA_ARGS__)

static void
check_rows(SqVtab *vt, const SiftSegEntry *e)
{
	int64_t		lo,
				hi,
				rows,
				bytes,
				blocks = (int64_t) e->header_blk - e->reserved + 1,
				chunk = sq_store_chunk_bytes(vt->st);

	sq_store_seg_rows(vt->st, e, &lo, &hi);
	if (!sq_store_rows_between(vt->st, lo, hi, &rows, &bytes))
		BAD("cannot read the chunks of segment %llu", (unsigned long long) e->gen);
	if (rows != (blocks * BLCKSZ + chunk - 1) / chunk || bytes != blocks * BLCKSZ)
		BAD("segment %llu has %lld chunk rows of %lld bytes, the manifest says %lld blocks", (unsigned long long) e->gen,
			(long long) rows, (long long) bytes, (long long) blocks);
}

static uint64
check_segment(SqVtab *vt, const SiftSegEntry *e)
{
	const SqOptions *o = sq_store_options(vt->st);
	SiftScoreRef ref = {(float4) o->base.k1, (float4) o->base.b, 0};
	SiftSegment *seg;
	SiftSegStats stats;
	uint64		live;

	check_rows(vt, e);
	seg = sift_seg_open(sq_store_rel(vt->st), e, CurrentMemoryContext);
	if (seg->hdr->gen != e->gen || seg->hdr->ndocs != e->ndocs || seg->hdr->sum_dl != e->sum_dl)
		BAD("segment %llu holds %llu documents, the manifest %llu", (unsigned long long) e->gen,
			(unsigned long long) seg->hdr->ndocs, (unsigned long long) e->ndocs);
	sift_seg_walk(seg, &ref, true, &stats, NULL, NULL);
	live = sift_seg_live_docs(seg);
	sift_seg_close(seg);
	return live;
}

typedef struct DocCheck
{
	sqlite3_stmt *q;
	uint64		missing;
	int64_t		first;
} DocCheck;

static void
check_doc(void *arg, SiftKey key, uint16 dl)
{
	DocCheck   *d = arg;
	int64_t		id = sq_key_rowid(key);

	sqlite3_bind_int64(d->q, 1, id);
	if (sqlite3_step(d->q) != SQLITE_ROW && d->missing++ == 0)
		d->first = id;
	sqlite3_reset(d->q);
}

/* Every live document of the index is in the text, and the text has no others */
static void
check_content(SqVtab *vt, uint64 live)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);
	DocCheck	d = {0};
	sqlite3_stmt *c;
	int64_t		n = 0;

	d.q = source_stmt(vt, SRC_ID, 0);
	for (int i = 0; i < dir->nsegs; i++)
		sq_seg_live_docs(vt->st, &dir->segs[i], check_doc, &d);
	sqlite3_finalize(d.q);
	c = source_stmt(vt, SRC_IDS, 0);
	while (sqlite3_step(c) == SQLITE_ROW)
		n++;
	sqlite3_finalize(c);
	if (d.missing)
		BAD("%llu indexed documents are not in the text (first: rowid %lld)", (unsigned long long) d.missing, (long long) d.first);
	if (n != (int64_t) live)
		BAD("the text has %lld documents, the index %llu", (long long) n, (unsigned long long) live);
}

/* The manifest, every segment, the chunk rows and (deep) the text: what the engine reads must be what was written */
static void
integrity_check(SqVtab *vt, bool deep)
{
	const SiftDirSnapshot *dir = sq_store_dir(vt->st);
	uint64		live = 0;
	int64_t		rows = 0;

	sq_vt_flush(vt, false);
	for (int i = 0; i < dir->nsegs; i++)
	{
		int64_t		lo,
					hi,
					n,
					b;

		live += check_segment(vt, &dir->segs[i]);
		sq_store_seg_rows(vt->st, &dir->segs[i], &lo, &hi);
		sq_store_rows_between(vt->st, lo, hi, &n, &b);
		rows += n;
		if (dir->segs[i].gen >= dir->meta.next_gen || dir->segs[i].header_blk >= sq_store_hw(vt->st))
			BAD("segment %llu is past the high-water mark", (unsigned long long) dir->segs[i].gen);
	}
	if (sq_store_rows_total(vt->st) != rows)
		BAD("the chunk table has %lld rows, the segments account for %lld", (long long) sq_store_rows_total(vt->st),
			(long long) rows);
	if (deep && vt->content != SQ_CONTENT_NONE)
		check_content(vt, live);
}

/* ---------- commands ---------- */

static int64_t
arg_int(sqlite3_value *v, int64_t dflt)
{
	return sqlite3_value_type(v) == SQLITE_NULL ? dflt : sqlite3_value_int64(v);
}

static void
remove_by_command(SqVtab *vt, sqlite3_value *rowid)
{
	if (vt->content == SQ_CONTENT_INTERNAL)
		elog(ERROR, "sift: 'delete' is for tables with external or no content; use DELETE FROM");
	if (sqlite3_value_type(rowid) == SQLITE_NULL)
		elog(ERROR, "sift: 'delete' needs the rowid");
	index_remove(vt, sqlite3_value_int64(rowid));
}

static void
delete_all(SqVtab *vt)
{
	SiftDirSnapshot dir = *sq_store_dir(vt->st);

	sq_inv_clear(vt->pend);
	for (int i = 0; i < dir.nsegs; i++)
		sq_store_retire(vt->st, dir.segs[i].gen);
	if (vt->content == SQ_CONTENT_INTERNAL)
	{
		char	   *t = shadow(vt, "content"),
				   *sql = sqlite3_mprintf("DELETE FROM %s", t);

		sqlite3_exec(vt->db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
		sqlite3_free(t);
	}
}

static void
setting(SqVtab *vt, const char *cmd, sqlite3_value *arg)
{
	int64_t		n = arg_int(arg, -1);

	if (n < 0)
		elog(ERROR, "sift: '%s' needs a non-negative value in the rank column", cmd);
	if (strcmp(cmd, "cache-size") == 0)
	{
		vt->cache_kib = n;
		sq_store_set_cache(vt->st, vt->cache_kib);
	}
	else if (strcmp(cmd, "parallel") == 0)
		vt->parallel = n;
	else if (strcmp(cmd, "automerge") == 0)
	{
		vt->automerge = n;
		sq_store_config_set(vt->st, cmd, n);
	}
	else
	{
		vt->pending_postings = n;
		sq_store_config_set(vt->st, cmd, n);
	}
}

static void
command(SqVtab *vt, const char *cmd, sqlite3_value *arg, sqlite3_value *rowid)
{
	if (strcmp(cmd, "merge") == 0)
	{
		int64_t		steps = arg_int(arg, 0);

		sq_vt_flush(vt, false);
		merge_steps(vt, steps > 0 ? steps : INT64_MAX);
	}
	else if (strcmp(cmd, "optimize") == 0)
	{
		sq_vt_flush(vt, false);
		if (sq_store_dir(vt->st)->nsegs > 1 || any_dirty(vt))
			optimize(vt);
	}
	else if (strcmp(cmd, "flush") == 0)
		sq_vt_flush(vt, true);
	else if (strcmp(cmd, "rebuild") == 0)
		rebuild(vt);
	else if (strcmp(cmd, "integrity-check") == 0)
		integrity_check(vt, arg_int(arg, 0) != 0);
	else if (strcmp(cmd, "delete") == 0)
		remove_by_command(vt, rowid);
	else if (strcmp(cmd, "delete-all") == 0)
		delete_all(vt);
	else if (strcmp(cmd, "cache-size") == 0 || strcmp(cmd, "parallel") == 0 || strcmp(cmd, "automerge") == 0 ||
			 strcmp(cmd, "pending-postings") == 0)
		setting(vt, cmd, arg);
	else if (strcmp(cmd, "x-fault") == 0)
		sq_store_set_fault(vt->st, (int) (arg_int(arg, 0) / 1000000), arg_int(arg, 0) % 1000000);
	else
		elog(ERROR, "sift: unknown command \"%s\"", cmd);
}

static void
insert_doc(SqVtab *vt, sqlite3_int64 oldid, sqlite3_value *newid, sqlite3_value **cols, sqlite3_int64 *rowid)
{
	if (vt->content == SQ_CONTENT_INTERNAL)
		content_write(vt, oldid, newid, cols, rowid);
	else if (sqlite3_value_type(newid) == SQLITE_NULL)
		elog(ERROR, "sift: a table with external or no content needs a rowid");
	else
		*rowid = sqlite3_value_int64(newid);
	check_rowid(*rowid);
	index_doc(vt, *rowid, cols);
	if ((int64_t) sq_inv_postings(vt->pend) > vt->pending_postings)
		sq_vt_flush(vt, false);
}

/* INSERT OR REPLACE with a rowid that exists: the old document goes first */
static void
replace_existing(SqVtab *vt, sqlite3_value *newid)
{
	if (sqlite3_vtab_on_conflict(vt->db) != SQLITE_REPLACE || sqlite3_value_type(newid) == SQLITE_NULL)
		return;
	index_remove(vt, sqlite3_value_int64(newid));
	if (vt->content == SQ_CONTENT_INTERNAL)
		content_delete(vt, sqlite3_value_int64(newid));
}

static int
sqUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite3_int64 *pRowid)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		int			nc = vt->ncols;

		if (argc == 1)
		{
			index_remove(vt, sqlite3_value_int64(argv[0]));
			if (vt->content == SQ_CONTENT_INTERNAL)
				content_delete(vt, sqlite3_value_int64(argv[0]));
		}
		else if (sqlite3_value_type(argv[2 + nc]) != SQLITE_NULL)
			command(vt, (const char *) sqlite3_value_text(argv[2 + nc]), argv[3 + nc], argv[1]);
		else
		{
			sqlite3_int64 oldid = sqlite3_value_type(argv[0]) == SQLITE_NULL ? -1 : sqlite3_value_int64(argv[0]);

			if (oldid >= 0)
				index_remove(vt, oldid);
			else
				replace_existing(vt, argv[1]);
			if (sqlite3_value_type(argv[1]) != SQLITE_NULL)
				check_rowid(sqlite3_value_int64(argv[1]));
			insert_doc(vt, oldid, argv[1], argv + 2, pRowid);
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
		if (sq_store_refresh(vt->st, true))
			load_settings(vt);
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
		sq_vt_flush(vt, true);
	}
	TRY_END(vt)
}

static int
sqCommit(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;

	vt->in_txn = false;
	sq_store_commit(vt->st);
	return SQLITE_OK;
}

static int
sqRollback(sqlite3_vtab *pVtab)
{
	SqVtab	   *vt = (SqVtab *) pVtab;
	TRY_BEGIN(vt)
	{
		sq_inv_clear(vt->pend);
		sq_store_rollback(vt->st, true);
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
		sq_vt_flush(vt, false);
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
		sq_store_rollback(vt->st, false);
	}
	TRY_END(vt)
}

/* ---------- the module, and lookup by name for the SQL functions ---------- */

const sqlite3_module sq_sift_module = {
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
	.xFindFunction = sq_aux_find_function,
	.xRename = sqRename,
	.xSavepoint = sqSavepoint,
	.xRelease = sqRelease,
	.xRollbackTo = sqRollbackTo,
	.xShadowName = sqShadowName,
};

static SqVtab *
lookup_table(SqConn *conn, const char *name)
{
	for (SqVtab *vt = conn->tables; vt; vt = vt->next)
		if (sqlite3_stricmp(vt->name, name) == 0)
			return vt;
	return NULL;
}

/* The tables of a connection connect on first use: touching the table by name connects it */
SqVtab *
sq_find_table(SqConn *conn, const char *name)
{
	SqVtab	   *vt = name ? lookup_table(conn, name) : NULL;

	if (vt == NULL && name != NULL)
	{
		char	   *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" LIMIT 0", name);

		sqlite3_exec(conn->db, sql, NULL, NULL, NULL);
		sqlite3_free(sql);
		vt = lookup_table(conn, name);
	}
	return vt;
}
