/*
 * col/f2/agg/unnest/int.h - the state of one pass of the aggregate scan of a LATERAL unnest (f2_v6_unnest_run.c), shared with the emission of its pairs (f2_v6_unnest_emit.c).
 */
#ifndef SIFT_COL_F2_V6_UNNEST_INT_H
#define SIFT_COL_F2_V6_UNNEST_INT_H

#include "postgres.h"

#include "access/tableam.h"
#include "executor/executor.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "col/f2/agg/unnest/par.h"
#include "agg/filter/pg.h"
#include "agg/datum.h"
#include "agg/expr/str.h"
#include "agg/gather.h"
#include "agg/expr/pg.h"
#include "agg/plan/rewrite.h"
#include "agg/pipe.h"
#include "agg/qual/bqual.h"
#include "agg/unnest/late.h"
#include "agg/star/run.h"
#include "agg/unnest/unnest.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/pairs.h"
#include "core/memtrack.h"
#include "col/f2/agg/unnest/run.h"
#include "agg/vet.h"
#include "agg/vexpr/str.h"
#include "col/am.h"
#include "col/f2/sink/ext.h"
#include "col/f2/agg/map_preagg.h"
#include "agg/unnest/test.h"
#include "core/errctx.h"


#define OUT_SLOTS	(SIFT_F2_PULL_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE + 1 + SIFT_V6_XTESTS)
#define MONTH_BITS	12

StaticAssertDecl(SIFT_V6_XELEM_COLS <= SIFT_F2_PULL_MAX_CHILD, "an element column is a child leaf of the pull");

typedef struct UnCol
{
	int			leaf;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: a date is asked of it once while it holds its slot */
	bool		json;			/* the leaf of a key of a jsonb document: a row that holds a value of another JSON type than the leaf's has it as the payload of an exception */
	Oid			typid;			/* json: the type the statement reads the value as (the `->>` text or its cast) */
	bool		jint;			/* json: SiftV6Col.jint, the integer cast of the text of an integer leaf */
	bool		read;			/* the statement reads the column itself; it may be only the source of a test of the elements */
	bool		jsonb_cast;		/* json: SiftV6Col.jsonb_cast, the cast of the jsonb of the value */
	AttrNumber	heap_att;
	const char *key;
	int			keylen;
	bool		member;			/* a member of a document read by a path (SiftV6Col.jmember), converted as the statement does */
	int			map;			/* member: 1 + the map among those of the pull whose member it is, the leaf of the column its lengths; 0: a path the index declares, the leaf of its values */
	List	   *path;			/* member: the keys from the document of heap_att to the member, key the last */
	bool		length;			/* member: jsonb_array_length of the list whose group is the leaf (path: the keys to the list) */
	int			mask;			/* 1 + the key-presence mask of the pull (SiftF2PullMask) the column is: whether the document has the key of the leaf, 1 or 0; 0: a column of values */
} UnCol;

/* a map of a document whose members are columns of the rows: one more group of the pull, after the arrays of the streams */
typedef struct UnMap
{
	int			group;
	int			key;
	int			value;
} UnMap;

/* an arithmetic of the statement (SVD_ARITH): the vector of the pairs each of its columns is, and whether the column is a float8 */
typedef struct UnArith
{
	int			nin;
	int			in[SIFT_V6_MAX_CASE + 1];
	bool		f64[SIFT_V6_MAX_CASE + 1];
} UnArith;

/* An array of the rows whose elements the run reads (a stream of SiftV6Ext): the element columns of the pull, and how a row read from the heap is taken apart */
typedef struct UnStream
{
	const SiftV6XElem *el;
	int			nchild;			/* the element columns of the pull and how each is decoded */
	SiftVecType ctype[SIFT_V6_XELEM_COLS];
	SiftVecType etype[SIFT_V6_XELEM_COLS];	/* the columns of the element as the pairs and the tests read them */
	AttrNumber	heap_att;		/* the array column of the table */
	ExprState  *arr;			/* the array of a row of the heap when it is an expression (SiftV6Ext.elem_array), else NULL: the column heap_att */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftV6RecFn *rec;
} UnStream;

/* the leaves of the host of the pull, an index or a sift_table: their definitions, the first leaf of each attribute of the host and the leaves of the top-level keys of its documents */
typedef struct UnLeaves
{
	const SiftColSchema *schema;
	const int  *first;
	const bool *key_leaf;
} UnLeaves;

typedef struct UnRun
{
	const SiftV6Spec *spec;
	const SiftV6Ext *x;
	Relation	host;			/* the index, or the sift_table */
	bool		table;
	SiftV6ScanStats *stats;
	SiftV6Vet	vet;
	SiftV6Pipe *pipe;
	SiftBatchSel bs;
	SiftBatchSel obs;			/* the pairs of a call of emit_pairs */
	int			ncols;
	UnCol		cols[SIFT_F2_PULL_MAX_COLS];
	int			nmasks;			/* the columns that are key-presence masks (UnCol.mask) */
	UnArith		arith[SIFT_V6_MAX_DERIVE];
	SiftV6PgExpr *pg[SIFT_V6_MAX_DERIVE];	/* an SVD_PG_EXPR derivation: its evaluator, the vector of the pairs each of its inputs is, and its result */
	int			pgslot[SIFT_V6_MAX_DERIVE][SIFT_V6_MAX_COLS];
	SiftVxOut	pgout[SIFT_V6_MAX_DERIVE];
	int			nel;			/* the columns of the element of the pairs (stream 0) */
	int			row_slot;		/* the slot of the number of the row of each pair, for an EXISTS over the elements (-1: none) */
	bool		eused[SIFT_V6_XELEM_COLS];	/* the statement reads the column; the others are read for the values their leaf cannot hold */
	int			nst;
	UnStream	st[SIFT_V6_XSTREAMS];	/* st[0]: the array of the pairs, or the first one the tests read */
	int			nmaps;
	UnMap		map[SIFT_F2_PULL_MAX_GROUPS];
	EState	   *estate;			/* the quals of the table as a row of the heap is tested with them */
	ExprContext *econtext;
	ExprState  *hqual;
	SiftV6LateProbe *probe;		/* the expressions that read a member whose cast raised (SiftV6Ext.late_probe) */
	SiftClause	where;
	bool		has_where;
	bool		where_preds;	/* every qual of the table is a predicate of the pull: a granule whose zones say every row passes them needs none */
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads */
	SiftV6ElemEval *eev;		/* the quals of the element of a lateral unnest */
	SiftV6Bq   *bq;				/* the quals of the table no scan key says (SiftV6Spec.vqual), NULL: none */
	int			nbq;
	int			bqcol[SIFT_V6_MAX_COLS];	/* the column of the batch each input of bq is */
	SiftVec		bqv[SIFT_V6_MAX_COLS];
	SiftV6RowTests *tests;		/* the tests of the elements of a row (SiftV6Ext.test), NULL without any */
	int			ntests;
	int			outer_slot[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* the column of the batch each comparison of a test is with */
	AttrNumber	outer_att[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* and its column of the heap */
	int			doc_slot[SIFT_V6_XTESTS];	/* the column of the batch that is the presence of the document of a test (SiftV6XTest.doc_leaf), -1: none */
	ExprState  *heap_test[SIFT_V6_XTESTS];	/* a jsonpath test: the call a row the leaves cannot hold is decided by */
	bool		flag_used[SIFT_V6_XTESTS];	/* a FILTER of the statement reads the flag of the test */
	uint64	   *flag_valid[SIFT_V6_XTESTS];
	int64	   *flag_data[SIFT_V6_XTESTS];
	SiftVec		flagv[SIFT_V6_XTESTS];	/* the flag of each row of the batch */
	uint64		held[SIFT_VEC_WORDS];	/* the rows of the batch the tests are decided for (those the layout holds) */
	uint64		yes[SIFT_V6_XTESTS][SIFT_VEC_WORDS];
	uint64		known[SIFT_V6_XTESTS][SIFT_VEC_WORDS];
	SiftVec	   *out[OUT_SLOTS];
	uint64	   *out_valid[OUT_SLOTS];
	void	   *out_data[OUT_SLOTS];
	SiftVec		outv[OUT_SLOTS];
	SiftVec		fv[SIFT_F2_PULL_MAX_COLS];	/* the columns of the rows of a batch, those that are months computed or keys of a document with the values of their exceptions */
	int64	   *mdata[SIFT_F2_PULL_MAX_COLS];
	uint64	   *mvalid[SIFT_F2_PULL_MAX_COLS];
	void	   *jdata[SIFT_F2_PULL_MAX_COLS];
	uint64	   *jvalid[SIFT_F2_PULL_MAX_COLS];
	SiftF2Pull *pull;
	SiftF2PullSpec ps;
	int			pcol[SIFT_F2_PULL_MAX_COLS];	/* the column of the pull each column of the run is: itself, or the column of the shared pull of fused arms (arms.c) */
	Snapshot	snap;
	SiftUnPairs *pairs;			/* the plain unnest the compute threads fold: the pairs of the leader's own folds (par_plain) */
	uint32		pulled;			/* the batches of the index the pass has read */
	uint32		seen;			/* the batches of the first pass (0 in it): those of the batches again that it read have their rows of the heap and their counts in the groups already */
	int32	   *month_key;
	int32	   *month_val;
	Datum		month_unit;
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the columns of the rows of a batch */
	SiftMemTracker rmt;			/* a row read from the heap, taken apart (agg_unnest_heap_row): its array or document and the rows of its function, reset after each row */
	MemoryContext ccx;			/* a chunk of the elements of that row, at most SIFT_VEC_LEN, reset after each */
} UnRun;

/* the attribute of the heap a column of the host is: of the index, or the table's own */
static inline AttrNumber
heap_attno(const UnRun *u, int att)
{
	return u->table ? att : u->host->rd_index->indkey.values[att - 1];
}

/* the vector of the pipe the flag of test k is: after the derivations */
static inline int
flag_slot(const UnRun *u, int k)
{
	return u->ncols + u->nel + u->spec->nderive + k;
}

/* run.c: the run of spec over its own pull (ps the leaves it reads, pull not begun), one batch of the pull, its end */
extern void sift_f2_unnest_open(UnRun *u, const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats,
								const SiftV6StarRun *star);
extern void sift_f2_unnest_batch(UnRun *u, const SiftF2ExtBatch *b);
extern void sift_f2_unnest_close(UnRun *u);
extern void sift_f2_unnest_emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m);
/* rows.c: the rows of the batch in the TID range whose heap row the snapshot sees; then those the batch quals keep (u->bq), on the columns as the statement reads them (u->fv) */
extern uint16 sift_f2_unnest_vet_rows(UnRun *u, const SiftF2ExtBatch *b);
extern uint16 sift_f2_unnest_restrict_quals(UnRun *u, const SiftF2ExtBatch *b);
/* the batch quals of the spec (SiftV6Spec.vqual), over the columns of the batch the pull decodes */
extern void sift_f2_unnest_setup_bq(UnRun *u, const UnLeaves *r);
/* run.c: the column of the batch that is the column c of the table (its month when month), added to those the pull decodes when it is not one of them */
extern int	sift_f2_unnest_col_index(UnRun *u, const UnLeaves *r, const SiftV6Col *c, bool month);
extern void sift_f2_unnest_emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row);
/* member.c: the column of the batch that is the member c reads by a path, and its values on the rows the statement keeps */
extern int	sift_f2_unnest_member_col(UnRun *u, const UnLeaves *r, const SiftV6Col *c);
/* the members of the rows of the batch the layout holds; a row whose array it cannot hold is the heap's, which the quals of the table decide before its members are cast */
extern void sift_f2_unnest_member_column(UnRun *u, int i, const SiftF2ExtBatch *b, const SiftBatchSel *bs);
/* the members of row r from the heap row the vet holds, once the quals of the table keep it */
extern void sift_f2_unnest_member_heap_row(UnRun *u, uint32 r);
extern void sift_f2_unnest_emit_test_heap_row(UnRun *u, uint64 tid, uint32 row);
/*
 * the quals of the table on columns of an index, and the keys of the dimensions of a star join (star), as predicates of their leaves: the pull reads no granule whose zones they leave out (the
 * batches are filtered by the quals again) (where.c); true when every qual is one of them
 */
extern bool sift_f2_unnest_where_preds(UnRun *u, const UnLeaves *r, ScanKey star, int nstar, SiftF2PullSpec *ps);

#endif							/* SIFT_COL_F2_V6_UNNEST_INT_H */
