/*
 * col/v1_engine/store_ops_unnest.h - the state of one pass of the aggregate scan of a LATERAL unnest over Format 1 (v1_store_ops_unnest.c), shared with the emission of its pairs
 * (v1_store_ops_unnest_emit.c), the members of a map read by key (v1_store_ops_unnest_keys.c) and the tests of the elements of a row (col/v1_engine/store_ops_unnest_test.c).
 */
#ifndef SIFT_COL_V1_ENGINE_V1_STORE_OPS_UNNEST_H
#define SIFT_COL_V1_ENGINE_V1_STORE_OPS_UNNEST_H

#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "agg/datum.h"
#include "agg/expr/str.h"
#include "agg/filter/pg.h"
#include "agg/gather.h"
#include "agg/expr/pg.h"
#include "agg/pipe.h"
#include "agg/plan/rewrite.h"
#include "agg/star/run.h"
#include "agg/unnest/late.h"
#include "agg/unnest/unnest.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/test.h"
#include "agg/vexpr.h"
#include "agg/vexpr/str.h"
#include "col/v1/store_ops.h"
#include "core/errctx.h"
#include "core/memtrack.h"

StaticAssertDecl(SIFT_V1_MAX_ELEM_COLS == SIFT_V6_XELEM_COLS, "the element columns of a source are those of the unnest");

#define OUT_SLOTS	(SIFT_V6_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE + 1 + SIFT_V6_XTESTS)
#define MONTH_BITS	12

typedef struct UnCol
{
	int			attno;			/* of the source: the attribute of the index, or SIFT_V6_JLEAF_VATT + the leaf of a key of a document */
	Oid			typid;
	int			scale;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: a date is asked of it once while it holds its slot */
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
	SiftV6Pipe *pipe;
	SiftV6Source *src;
	SiftBatchSel bs;
	SiftBatchSel obs;			/* the pairs of a call of emit_pairs */
	int			ncols;
	UnCol		cols[SIFT_V6_MAX_COLS];
	UnArith		arith[SIFT_V6_MAX_DERIVE];
	SiftV6PgExpr *pg[SIFT_V6_MAX_DERIVE];	/* an SVD_PG_EXPR derivation: its evaluator, the vector of the pairs each of its inputs is, and its result */
	int			pgslot[SIFT_V6_MAX_DERIVE][SIFT_V6_MAX_COLS];
	SiftVxOut	pgout[SIFT_V6_MAX_DERIVE];
	int			nel;			/* the columns of the element */
	int			row_slot;		/* the slot of the number of the row of each pair, for an EXISTS over the elements (-1: none) */
	SiftVecType etype[SIFT_V6_XELEM_COLS];
	bool		eused[SIFT_V6_XELEM_COLS];	/* the statement reads the column; the others are read for the values their leaf cannot hold */
	AttrNumber	heap_att;		/* the array column of the table */
	bool		has_elems;		/* the leaves of the index hold the elements of the array: the rows they cannot hold are read from the heap */
	SiftV1ElemSpec espec;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftV6RecFn *rec;
	bool		keyed;			/* SV6E_KEYS: a row of the table is one pair, the members its keys name */
	int			lcol[SIFT_V6_XELEM_COLS];	/* keyed: the column of the batch that is the leaf of a column that is no member of the map (-1: a member) */
	uint32	   *kidx[SIFT_V6_XELEM_COLS];	/* keyed: the member of the map each column of the pair has (SIFT_GATHER_NONE: none) */
	uint32	   *elem_of[SIFT_V6_XELEM_COLS];	/* keyed: the member each key names in each row of the batch */
	ExprState  *cexpr[SIFT_V6_XELEM_COLS];	/* keyed: the expression each column is, with which a row of the heap is given its value */
	SiftClause	pwhere;
	bool		has_pwhere;
	int			pcol[SIFT_V6_MAX_FCOLS];
	int			npvec;
	SiftVec    *pvp[SIFT_V6_MAX_FCOLS];
	EState	   *estate;			/* the expressions of a row read from the heap, over a virtual row of the columns they read */
	ExprContext *econtext;
	TupleTableSlot *heap_slot;
	List	   *heap_atts;		/* the columns of the heap row those expressions read */
	AttrNumber	hatt[MaxHeapAttributeNumber];
	Datum		hval[MaxHeapAttributeNumber];
	bool		hnull[MaxHeapAttributeNumber];
	ExprState  *arr;			/* the array of a row of the heap when it is an expression of its columns (SiftV6Ext.elem_array), else NULL: the column heap_att */
	SiftV6LateProbe *probe;		/* the expressions that read a member whose cast raised (SiftV6Ext.late_probe) */
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads (-1: not one of the table) */
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
	SiftVec    *out[OUT_SLOTS];
	uint64	   *out_valid[OUT_SLOTS];
	void	   *out_data[OUT_SLOTS];
	SiftVec		outv[OUT_SLOTS];
	SiftVec		fv[SIFT_V6_MAX_COLS];	/* the columns of the rows of a batch, those that are months computed */
	int64	   *mdata[SIFT_V6_MAX_COLS];
	uint64	   *mvalid[SIFT_V6_MAX_COLS];
	int32	   *month_key;
	int32	   *month_val;
	Datum		month_unit;
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the array of the row being taken apart */
} UnRun;

/* the vector of the pipe the flag of test k is: after the derivations */
static inline int
flag_slot(const UnRun *u, int k)
{
	return u->ncols + u->nel + u->spec->nderive + k;
}

/* the column of the heap row that is attribute att of the index (of a table: its own column) */
static inline AttrNumber
un_heap_column(const UnRun *u, int att)
{
	return u->spec->is_table ? (AttrNumber) att : u->index->rd_index->indkey.values[att - 1];
}

static inline SiftVecType
un_vec_type(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

/* the column of the batches a column of the statement is read from */
extern int	sift_v1_unnest_col_index(UnRun *u, const SiftV6Col *c, bool month);
/* the pairs of (row, element) into the vectors of the pairs, and on to the groups: pidx[i] names the row of fact and eidx[i] the element of elems of pair i (SIFT_GATHER_NONE: a NULL one) */
extern void sift_v1_unnest_emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m);
/* the pairs of one row whose elements are in ev: those the bitmap keeps (NULL: all), and for a left join a NULL element when it has none */
extern void sift_v1_unnest_emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row, ErrorData *const *late);
/* the pairs of m rows that are one pair each, the members of a map already gathered */
extern void sift_v1_unnest_run_pairs(UnRun *u, uint32 m);
extern void sift_v1_unnest_heap_open(UnRun *u, Relation heap);
extern void sift_v1_unnest_heap_close(UnRun *u);
/* the elements of the array of the heap row loaded, as columns: how many */
extern uint32 sift_v1_unnest_heap_elems(UnRun *u, SiftVec *ev);
/* the row at tag of the heap, as the virtual row the expressions of the run read; its columns are those registered with sift_v1_unnest_heap_expr and sift_v1_unnest_heap_att */
extern void sift_v1_unnest_heap_load(UnRun *u, uint64 tag);
extern ExprState *sift_v1_unnest_heap_expr(UnRun *u, Expr *e);
extern void sift_v1_unnest_heap_att(UnRun *u, AttrNumber att);
/* a row the leaves cannot hold: the array of its heap row taken apart and its pairs */
extern void sift_v1_unnest_emit_heap_row(UnRun *u, const SiftV6Batch *b, uint32 row);
/* the rows of the batch the map's leaves hold: one pair each; the rows they cannot hold are read from the heap */
extern void sift_v1_unnest_run_keyed(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb);
/* the tests of the rows of the batch: the rows they keep are one pair each, those the leaves cannot hold are the heap's */
extern void sift_v1_unnest_run_tests(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb);
extern void sift_v1_unnest_setup_tests(UnRun *u);

#endif							/* SIFT_COL_V1_ENGINE_V1_STORE_OPS_UNNEST_H */
