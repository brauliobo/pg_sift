/*
 * content.c - where the text of a document comes from: the table's own <t>_content, the user's table, or none.
 */
#include "vtab_int.h"

SQLITE_EXTENSION_INIT3

/* ---------- content: where the text of a document comes from ---------- */

char *
shadow(const SqVtab *vt, const char *suffix)
{
	return sqlite3_mprintf("\"%w\".\"%w_%s\"", vt->schema, vt->name, suffix);
}

char *
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

sqlite3_stmt *
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

/* The text the index holds for the document: the columns joined by the field separator */
text *
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
