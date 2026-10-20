/*
 * vtab_int.h - what the pieces of the SQLite virtual table module share: vtab.c (create, connect, transactions, the module), content.c
 * (where the text of a document comes from), cursor.c (best index, scans) and write.c (indexing, flush, merge, xUpdate, the commands).
 */
#ifndef SIFT_VTAB_INT_H
#define SIFT_VTAB_INT_H

#include "vtab.h"

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

typedef enum SourceQuery
{
	SRC_ROW,							/* the columns of the document ?1 */
	SRC_IDS,							/* every document id */
	SRC_ID,								/* the id ?1, if there is such a document */
	SRC_ALL								/* id and columns of every document */
} SourceQuery;

/* vtab.c */
extern SIFT_INTERNAL int fail(SqVtab *vt);
extern SIFT_INTERNAL void load_settings(SqVtab *vt);

/* content.c */
extern SIFT_INTERNAL char *shadow(const SqVtab *vt, const char *suffix);
extern SIFT_INTERNAL char *col_list(const SqVtab *vt, const char *prefix);
extern SIFT_INTERNAL sqlite3_stmt *source_stmt(SqVtab *vt, SourceQuery kind, int flags);
extern SIFT_INTERNAL text *fetch_text(SqVtab *vt, int64_t id);

/* cursor.c: the cursor half of sq_sift_module */
extern SIFT_INTERNAL int sqBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *info);
extern SIFT_INTERNAL int sqOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor);
extern SIFT_INTERNAL int sqClose(sqlite3_vtab_cursor *cur);
extern SIFT_INTERNAL int sqEof(sqlite3_vtab_cursor *cur);
extern SIFT_INTERNAL int sqFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv);
extern SIFT_INTERNAL int sqNext(sqlite3_vtab_cursor *cur);
extern SIFT_INTERNAL int sqRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid);
extern SIFT_INTERNAL int sqColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i);

/* write.c */
extern SIFT_INTERNAL int sqUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite3_int64 *pRowid);

SQLITE_EXTENSION_INIT3

/* The prepared SELECT of one document's columns, taken from the one the table keeps for reuse */
static inline sqlite3_stmt *
row_stmt(SqVtab *vt)
{
	sqlite3_stmt *q = vt->rowstmt;

	vt->rowstmt = NULL;
	return q ? q : source_stmt(vt, SRC_ROW, SQLITE_PREPARE_PERSISTENT);
}

static inline void
row_stmt_give_back(SqVtab *vt, sqlite3_stmt *q)
{
	sqlite3_reset(q);
	if (vt->rowstmt)
		sqlite3_finalize(q);
	else
		vt->rowstmt = q;
}

#endif							/* SIFT_VTAB_INT_H */
