/*
 * vtab.c - the SQLite virtual table module "sift":
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
#include "vtab_int.h"

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

int
fail(SqVtab *vt)
{
	int			code = sq_errcode();

	sqlite3_free(vt->base.zErrMsg);
	vt->base.zErrMsg = sqlite3_mprintf("%s", sq_errmsg());
	sq_store_recover(vt->st);
	return code == ERRCODE_INDEX_CORRUPTED ? SQLITE_CORRUPT_VTAB : SQLITE_ERROR;
}

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
void
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
