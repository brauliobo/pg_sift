/*
 * vtab.h - the state of a virtual table and of its cursors, shared by vtab.c (the module, with content.c, cursor.c and write.c:
 * see vtab_int.h), auxfn.c (snippets, facets) and pscan.c (parallel scans).
 */
#ifndef SQVTAB_H
#define SQVTAB_H

#include "api.h"

#include "bm25/eval.h"

typedef enum SqContent
{
	SQ_CONTENT_INTERNAL,				/* <t>_content holds the text (the default) */
	SQ_CONTENT_EXTERNAL,				/* content='tbl': the text is read from the user's table */
	SQ_CONTENT_NONE						/* content='': only the index */
} SqContent;

typedef struct SqVtab SqVtab;

/* What one SQLite connection knows of its sift tables; SQLite's connection mutex protects it */
typedef struct SqConn
{
	sqlite3    *db;
	SqVtab	   *tables;
	struct SqConn *next_loaded;
} SqConn;

struct SqVtab
{
	sqlite3_vtab base;
	SqConn	   *conn;
	sqlite3    *db;
	char	   *schema;
	char	   *name;
	int			ncols;
	char	  **cols;
	SqStore    *st;
	SqOptions	opts;
	SqContent	content;
	char	   *ext_table;				/* content='tbl' */
	char	   *ext_rowid;				/* content_rowid, "rowid" by default */
	int			chunk_bytes;
	int			cache_kib;
	int			automerge;				/* segments of a tier that trigger a merge after a flush, 0 = never */
	int64_t		pending_postings;		/* the buffer is written as a segment above this */
	int			parallel;				/* threads of a scan, below 2 = serial */
	int64_t		par_scans;				/* scans that ran in parallel */
	MemoryContext acx;					/* analyzer */
	SiftAnalyzer *analyzer;
	SqInv	   *pend;					/* documents not yet in a segment */
	int			ncursors;
	bool		in_txn;
	sqlite3_stmt *rowstmt;				/* reusable SELECT of one content row */
	SqVtab	   *next;
};

typedef struct SqHit
{
	int64_t		rowid;
	float4		score;
} SqHit;

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
	SiftQuery **qs;						/* the analyzed MATCH queries */
	int			nqs;
	SiftPageMatch pm;
	uint64		left[SIFT_OFFMASK_WORDS];	/* unreported offsets of pm */
	bool		have_pm;
	SqHit	   *hits;					/* a ranked scan that ran on worker threads: its result */
	int64_t		nhits,
				hitpos;
	int64_t		rowid;
	double		score;
	sqlite3_stmt *rows;
	sqlite3_stmt *row;					/* content row of the current rowid */
	bool		eof;
} SqCursor;

extern const sqlite3_module sq_sift_module;

/* The call context every entry point works in, and the error to SQLite's */
extern MemoryContext sq_enter(void);
extern void sq_leave(MemoryContext old);

extern bool sq_vt_verify(void *arg, ItemPointer tid, bool want_doc, text **doc);
extern SqVtab *sq_find_table(SqConn *conn, const char *name);
extern void sq_vt_flush(SqVtab *vt, bool automerge);
extern SiftAnalyzer *sq_vt_analyzer(SqVtab *vt);

/* The text of column col (-1: all columns joined by a space) of the document, NULL when the table keeps none */
extern char *sq_vt_doc_text(SqVtab *vt, int64_t rowid, int col, int *len);

/* auxfn.c */
extern int	sq_aux_init(sqlite3 *db, SqConn *conn);
extern int	sq_aux_find_function(sqlite3_vtab *pVtab, int nArg, const char *zName,
								 void (**pxFunc) (sqlite3_context *, int, sqlite3_value **), void **ppArg);

/* pscan.c */
typedef struct SqQuery
{
	const char *text;
	int			len;
} SqQuery;

extern int	sq_par_available(SqVtab *vt, SiftPlan *plan, const SqQuery *q, int nq);
extern SqHit *sq_par_rank(SqVtab *vt, const SqQuery *q, int nq, int64_t k, int64_t *nhits);
extern bool sq_par_count_pays(const SiftQuery *q);
extern int64_t sq_par_count(SqVtab *vt, const SqQuery *q, int nq);

#endif							/* SQVTAB_H */
