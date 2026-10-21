/*
 * agg/gen_int.h - the generic aggregates, run through PostgreSQL's own functions (agg/gen.c)
 */
#ifndef SIFT_AGG_GEN_INT_H
#define SIFT_AGG_GEN_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/accum_int.h"

/*
 * The aggregate runs as ExecAgg runs it (advance_transition_function, finalize_aggregate): the state of each group is a Datum made
 * by the aggregate's transition function, called through fmgr with an AggState as its context (AggCheckCallContext, AggGetAggref,
 * AggRegisterCallback), in memory that lives as long as the scan; the final function runs once per group when the scan has ended.
 * DISTINCT: the (group, value) pairs go to a set, and the distinct values of a group go through the transition function in the
 * order of the type's btree, as PostgreSQL's sort gives them. WITHIN GROUP: the values of all groups go through one tuplesort
 * (memory bounded by work_mem) by group; each group's values are then given to the aggregate, which sorts them as it does in
 * ExecAgg, and the state is freed after the group's final function.
 */
#define GEN_INIT	1				/* the group's state is made */
#define GEN_NULL	2				/* transValueIsNull */
#define GEN_NOVAL	4				/* noTransValue: a strict transition function without initial value takes its first input as the state */
#define GEN_DONE	8				/* acc->r is the result */
#define GEN_RNULL	16				/* ... and it is NULL */

typedef struct GenSt
{
	const SiftAggGen *g;
	int			ai;				/* the aggregate's number: its table of joined values is aot[ai] */
	Aggref	   *aggref;
	ExprContext *cx;				/* the states live in its per-tuple memory */
	AggStatePerTransData pertrans;
	AggStatePerAggData peragg;
	FmgrInfo	transfn;
	FmgrInfo	finalfn;
	FmgrInfo   *cmp;				/* DISTINCT: the btree comparison and the equality of the argument's type */
	FmgrInfo   *eq;
	FunctionCallInfo tfc;
	FunctionCallInfo ffc;
	bool		strict;
	bool		has_final;
	int			nfinal;
	int			ndirect;
	Oid			transtype;
	int16		tlen;
	bool		tbyval;
	int16		rlen;
	bool		rbyval;
	Datum		initval;
	bool		initnull;
	ExprState  *argx[SIFT_AGG_MAX_GARGS];
	Datum		argk[SIFT_AGG_MAX_GARGS];		/* constant arguments */
	bool		argknull[SIFT_AGG_MAX_GARGS];
	Datum		dir[FUNC_MAX_ARGS];				/* direct arguments, evaluated once */
	bool		dirnull[FUNC_MAX_ARGS];
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
	uint32		nsid;				/* WITHIN GROUP, ORDER BY: groups numbered as their first value arrives (group indexes change as the table grows) */
	int			vcol[SIFT_AGG_MAX_GARGS];		/* ORDER BY: the column of the sort tuple that holds argument k after the group number, -1 for a constant */
	FmgrInfo	combinefn;				/* parallel: the partial states of the workers merge through these */
	FmgrInfo	serialfn;
	FmgrInfo	deserialfn;
	FunctionCallInfo cfc;
	FunctionCallInfo sfc;
	FunctionCallInfo dfc;
	bool		tmode;				/* the arguments go through the tuplesort of vcol tuples: ORDER BY, or DISTINCT once its hash set outgrew work_mem */
	bool		spilled;
	int			ndcol;				/* DISTINCT in the sort: the tuple columns that make two inputs equal, with their equality functions ... */
	int			dcol[SIFT_AGG_MAX_GARGS];
	FmgrInfo	deq[SIFT_AGG_MAX_GARGS];
	int16		dlen[SIFT_AGG_MAX_GARGS];
	bool		dbyval[SIFT_AGG_MAX_GARGS];
	Datum		prev[SIFT_AGG_MAX_GARGS];		/* ... and the previous input's values */
	bool		prevnull[SIFT_AGG_MAX_GARGS];
	MemoryContext pcx;
	SortSupportData *fsup;		/* DISTINCT ON (SiftAggGen.first): the comparison of each sort key of the call ... */
	int			nfk;
	int			fkey[SIFT_AGG_MAX_GARGS];	/* ... the argument it reads */
	bool		follower;		/* ... every DISTINCT ON aggregate of the query has the same sort keys, so the first one decides which rows win and the others take those */
	bool		iskey[SIFT_AGG_MAX_GARGS];
	int16		alen[SIFT_AGG_MAX_GARGS];	/* the arguments' types, to copy the values of the best row */
	bool		abyval[SIFT_AGG_MAX_GARGS];
} GenSt;

/* the best row of a group of a DISTINCT ON aggregate (the group's state until the scan ends): the values of the arguments, then their NULL flags */
typedef Datum GenBest;

#define GEN_BEST_SIZE(n)		((n) * (sizeof(Datum) + sizeof(bool)))
#define GEN_BEST_NULL(b, n, k)	(((bool *) ((b) + (n)))[k])

/* the leaves an argument reads, decoded for the chunk's tuples */
typedef struct GenIn
{
	const SiftSrcVec *vec[SIFT_AGG_MAX_GARGS][SIFT_AGG_MAX_GLEAVES];
	const uint32 *idx[SIFT_AGG_MAX_GARGS][SIFT_AGG_MAX_GLEAVES];
	const uint32 *cout[SIFT_AGG_MAX_GARGS];	/* an argument of a joined table: the chunk's combo rows and the combo's value ids */
	const uint32 *cval[SIFT_AGG_MAX_GARGS];
} GenIn;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* gen.c */
void gen_sort_begin(GenSt *st);
void gen_sort_put(GenSt *st, AggAcc *acc, uint32 gid, const Datum *args, const bool *nulls);
void gen_open(AggExec *ex);
void gen_close(AggExec *ex);
void gen_arg(AggExec *ex, GenSt *st, const GenIn *in, int j, int k, Datum *arg, bool *isnull);
void gen_advance(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const Datum *args, const bool *nulls);
void gen_state_put(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, StringInfo vb);
void gen_state_merge(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const char **cur);
void gen_final(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g);

#pragma GCC visibility pop

#endif
