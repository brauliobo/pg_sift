/*
 * col/f2/f2_v6_unnest_int.h - the state of one pass of the aggregate scan of a LATERAL unnest (f2_v6_unnest_run.c), shared with the emission of its pairs (f2_v6_unnest_emit.c).
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
#include "col/f2/f2_v6_unnest_par.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_expr_str.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_star_run.h"
#include "agg/v6/v6_unnest.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "agg/v6/v6_unnest_pairs.h"
#include "col/f2/f2_v6_unnest_run.h"
#include "agg/v6/v6_vet.h"
#include "agg/vexpr_str.h"
#include "col/am.h"
#include "col/f2/f2_sink_ext.h"
#include "col/f2/f2_v6_map_preagg.h"
#include "agg/v6/v6_unnest_test.h"
#include "core/errctx.h"


#define OUT_SLOTS	(SIFT_F2_PULL_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE + 1 + SIFT_V6_XTESTS)
#define MONTH_SLOTS	4096

StaticAssertDecl(SIFT_V6_XELEM_COLS <= SIFT_F2_PULL_MAX_CHILD, "an element column is a child leaf of the pull");

typedef struct UnCol
{
	int			leaf;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: each distinct date is asked of it once */
	bool		json;			/* the leaf of a key of a jsonb document: a row that holds a value of another JSON type than the leaf's has it as the payload of an exception */
	bool		read;			/* the statement reads the column itself; it may be only the source of a member (keyed) */
	AttrNumber	heap_att;
	const char *key;
	int			keylen;
} UnCol;

/* an arithmetic of the statement (SVD_ARITH): the vector of the pairs each of its columns is, and whether the column is a float8 */
typedef struct UnArith
{
	int			nin;
	int			in[SIFT_V6_MAX_CASE + 1];
	bool		f64[SIFT_V6_MAX_CASE + 1];
} UnArith;

typedef struct UnRun
{
	const SiftV6Spec *spec;
	const SiftV6Ext *x;
	Relation	index;
	SiftV6ScanStats *stats;
	SiftV6Vet	vet;
	SiftV6Pipe *pipe;
	SiftBatchSel bs;
	SiftBatchSel obs;			/* the pairs of a call of emit_pairs */
	int			ncols;
	UnCol		cols[SIFT_F2_PULL_MAX_COLS];
	UnArith		arith[SIFT_V6_MAX_DERIVE];
	SiftV6PgExpr *pg[SIFT_V6_MAX_DERIVE];	/* an SVD_PG_EXPR derivation: its evaluator, the vector of the pairs each of its inputs is, and its result */
	int			pgslot[SIFT_V6_MAX_DERIVE][SIFT_V6_MAX_COLS];
	SiftVxOut	pgout[SIFT_V6_MAX_DERIVE];
	int			nel;			/* the columns of the element */
	int			row_slot;		/* the slot of the number of the row of each pair, for an EXISTS over the elements (-1: none) */
	SiftVecType etype[SIFT_V6_XELEM_COLS];
	bool		eused[SIFT_V6_XELEM_COLS];	/* the statement reads the column; the others are read for the values their leaf cannot hold */
	AttrNumber	heap_att;		/* the array column of the table */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftV6RecFn *rec;
	bool		keyed;			/* SV6E_KEYS: a row of the table is one pair, the members its keys name */
	int			nchild;			/* the element columns of the pull and how each is decoded: those of the statement, or the keys and the values of the map when keyed */
	SiftVecType ctype[SIFT_V6_XELEM_COLS];
	FmgrInfo	jtext;			/* jsonb_object_field_text: the text of the key of a document read from the heap */
	ExprState  *cexpr[SIFT_V6_XELEM_COLS];	/* keyed: the expression each column is, with which a row of the heap is given its value */
	uint32	   *kidx[SIFT_V6_XELEM_COLS];	/* keyed: the member of the map each column of the pair has (SIFT_GATHER_NONE: none) */
	int			lcol[SIFT_V6_XELEM_COLS];	/* keyed: the column of the batch that is the leaf of a column that is no member of the map (-1: a member) */
	uint32	   *elem_of[SIFT_V6_XELEM_COLS];	/* keyed: the member each key names in each row of the batch */
	SiftClause	pwhere;
	bool		has_pwhere;
	int			pcol[SIFT_V6_MAX_FCOLS];
	int			npvec;
	SiftVec    *pvp[SIFT_V6_MAX_FCOLS];
	EState	   *estate;			/* the quals of the table as a row of the heap is tested with them */
	ExprContext *econtext;
	ExprState  *hqual;
	ExprState  *arr;			/* the array of a row of the heap when it is an expression (SiftV6Ext.elem_array), else NULL: the column heap_att */
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads */
	SiftV6ElemEval *eev;		/* the quals of the element of a lateral unnest */
	SiftV6RowTests *tests;		/* the tests of the elements of a row (SiftV6Ext.test), NULL without any */
	int			ntests;
	int			outer_slot[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* the column of the batch each comparison of a test is with */
	AttrNumber	outer_att[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* and its column of the heap */
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
	Snapshot	snap;
	SiftUnPairs *pairs;			/* the plain unnest the compute threads fold: the pairs of the leader's own folds (par_plain) */
	uint32		pulled;			/* the batches of the index the pass has read */
	uint32		seen;			/* the batches of the first pass (0 in it): those of the batches again that it read have their rows of the heap and their counts in the groups already */
	int32	   *month_key;
	int32	   *month_val;
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the arrays read from the heap for a batch */
	int			(*attr_slot) (void *arg, const SiftV6Col *c);	/* a table: the vector of the scan that holds a column of the rows, which the pairs gather from */
	void	   *attr_arg;
	SiftVecType ftype[SIFT_F2_PULL_MAX_COLS];	/* a table: the type of each vector of the rows of its batch */
} UnRun;

/* the vector of the pipe the flag of test k is: after the derivations */
static inline int
flag_slot(const UnRun *u, int k)
{
	return u->ncols + u->nel + u->spec->nderive + k;
}

extern void sift_f2_unnest_emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m);
extern void sift_f2_unnest_run_keyed(UnRun *u, const SiftF2ExtBatch *b, const SiftVec *vals, uint64 *fallback);
extern void sift_f2_unnest_emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row);
extern void sift_f2_unnest_emit_test_heap_row(UnRun *u, uint64 tid, uint32 row);
extern void sift_f2_unnest_emit_heap_keys(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row);

#endif							/* SIFT_COL_F2_V6_UNNEST_INT_H */
