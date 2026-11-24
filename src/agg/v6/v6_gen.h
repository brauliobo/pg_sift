/*
 * agg/v6/v6_gen.h - SV6_GEN_PG and SV6_OSA in the groups of SiftAggV6: any PostgreSQL aggregate, run through its own transition, combine and final functions.
 *
 * The aggregate runs as ExecAgg runs it: the state of a group is a Datum of the transition type, made by aggtransfn through fmgr with an AggState as its context
 * (AggCheckCallContext, AggGetAggref, AggRegisterCallback), in memory that lives as long as the scan. A state is the 32 bytes of a V6Acc: n the flags below, v0 the state
 * (the result once V6GEN_DONE), v1 the number of the group's sort (+ 1, 0: none yet), aux the inputs the sort holds for the group. DISTINCT, ORDER BY and WITHIN GROUP aggregates
 * put their inputs through one tuplesort (v6_gen_sort.h), and the states of their groups are made from it, group by group, when the scan ends. The groups of a statement with
 * such an aggregate never spill: the statement is planned only when the groups the planner expects fit the memory of the group table, and stops when they do not.
 */
#ifndef SIFT_AGG_V6_GEN_H
#define SIFT_AGG_V6_GEN_H

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"

#include "agg/v6/group_int.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_acc.h"

#define V6GEN_INIT		1		/* the group's state is made */
#define V6GEN_NULL		2		/* transValueIsNull */
#define V6GEN_NOVAL		4		/* noTransValue: a strict transition function without an initial value takes its first input as the state */
#define V6GEN_DONE		8		/* v0 is the result */
#define V6GEN_RNULL		16		/* ... and it is NULL */

/* the columns one aggregate reads: arg, arg2 and the xcols of SiftV6Agg */
#define V6GEN_MAX_COLS	(SIFT_V6_MAX_DCOLS + 1)

/* a column the aggregate reads: the attribute of the table it is, the vector of the batch it comes in and what the vector holds */
typedef struct V6GenCol
{
	int			att;
	int			vec;
	Oid			typid;
	int			scale;
} V6GenCol;

/* an argument of the call: a column of the batch (col: its number), an expression of columns (expr: it reads the columns of var) or a constant evaluated when the group starts */
typedef struct V6GenArg
{
	bool		varies;
	int			col;
	ExprState  *expr;
	int			nvars;
	int		   *var;
	Oid			typid;
	bool		knull;
	Datum		k;
} V6GenArg;

struct V6GenSort;

typedef struct V6GenAgg
{
	int			j;				/* the aggregate of the spec */
	const SiftV6Agg *spec;
	Aggref	   *ref;
	int			nargs;			/* the Aggref's args, the sort-only (resjunk) ones included */
	int			ntrans;			/* ... the ones the transition function takes: the first ntrans */
	V6GenArg   *arg;
	int			ncols;
	V6GenCol	col[V6GEN_MAX_COLS];
	bool		ordered;		/* WITHIN GROUP */
	bool		hyp;			/* hypothetical-set: the arguments are the sort columns and every row counts, NULLs included */
	bool		distinct;
	bool		sorted;			/* the inputs of a group go through the tuplesort first: DISTINCT, ORDER BY, WITHIN GROUP */
	ExprContext *cx;			/* the states live in its per-tuple memory */
	AggStatePerTransData pertrans;
	AggStatePerAggData peragg;
	FmgrInfo	transfn;
	FmgrInfo	finalfn;
	FmgrInfo	combinefn;
	FunctionCallInfo tfc;
	FunctionCallInfo ffc;
	FunctionCallInfo cfc;
	FmgrInfo	serialfn;		/* a partial state of a parallel participant: carried by the serial function, or as a datum when the state is no internal one */
	FmgrInfo	deserialfn;
	FunctionCallInfo sfc;
	FunctionCallInfo dfc;
	bool		strict;
	bool		has_final;
	bool		has_combine;
	int			nfinal;
	int			ndirect;
	Oid			transtype;
	int16		tlen;
	bool		tbyval;
	int16		rlen;
	bool		rbyval;
	Datum		initval;
	bool		initnull;
	Datum	   *dir;			/* the direct arguments, evaluated once */
	bool	   *dirnull;
	struct V6GenSort *sort;
	uint32		nsid;			/* the groups numbered in the sort, as their first input arrives */
} V6GenAgg;

typedef struct V6Gen
{
	const SiftV6Spec *spec;
	MemoryContext cx;
	AggState   *as;				/* the AggState the functions are called with */
	TupleTableSlot *slot;		/* the row the expressions of the arguments read: the attributes of the table by number, the columns they use set */
	int			natts;
	ExprContext *tmp;			/* the argument values of a batch, the transition functions' temporary memory */
	MemoryContext res;			/* the results of the sorted aggregates, until the scan's end */
	V6GenAgg  **agg;			/* per aggregate of the spec: NULL for the ones the kernels run */
	uint64		limit;			/* the bytes the states may hold */
	bool		sorted;
} V6Gen;

/* planner side (v6_gen.c): the generic aggregate of the spec, its argument columns in order, and the Aggref the groups run, whose columns are attributes 1.. of p->gen_vars; NULL when the scan cannot run it */
struct V6Plan;
extern Aggref *sift_v6_gen_compile(struct V6Plan *p, Aggref *a, SiftV6Agg *out);
/* the groups the planner expects fit the group table, for a spec whose aggregates never spill */
extern bool sift_v6_gen_fits(const SiftV6Spec *spec, double groups);
extern bool sift_v6_spec_has_gen(const SiftV6Spec *spec);

/* executor side (v6_gen.c) */
extern V6Gen *v6_gen_new(const SiftV6Spec *spec, MemoryContext cx, uint64 limit);
extern void v6_gen_free(V6Gen *gen);
/* the argument values of the rows of a batch live until the next one starts */
extern void v6_gen_batch_begin(V6Gen *gen);
/* aggregate j of a group takes row r of the batch whose vectors are argvec: the group's state is acc */
extern void v6_gen_fold(V6Gen *gen, int j, V6Acc *acc, SiftVec *const *argvec, uint16 r);
/* the state s of another table joins the state d of the same group */
extern void v6_gen_combine(V6Gen *gen, int j, V6Acc *d, const V6Acc *s);
/* the result of the group of state acc, in the current memory context */
extern Datum v6_gen_datum(V6Gen *gen, int j, V6Acc *acc, bool *isnull);
/* the scan has ended: the inputs of the sorted aggregates go through the transition functions, group by group, and each group has its result */
extern void v6_gen_finish(V6Gen *gen, SiftV6Group *g);
/* the group table of a statement that cannot spill holds more than its memory: the statement stops */
extern void v6_gen_no_spill(const V6Gen *gen);
/* the partial state of group j as the bytes that follow the accumulators of its record in a parallel run (none when it has no value), and a state that came that way joining dst */
extern void v6_gen_state_put(V6Gen *gen, int j, const V6Acc *acc, StringInfo buf);
extern const char *v6_gen_state_take(V6Gen *gen, int j, V6Acc *dst, const V6Acc *src, const char *tail);
/* every state of every group is dropped: the groups went elsewhere */
extern void v6_gen_release(V6Gen *gen);
/* a participant of a parallel run holds what it holds until it hands its groups over */
extern void v6_gen_unbound(V6Gen *gen);
/* the aggregates of the spec can run in workers: none sorts its inputs, each has a combine function and, for an internal state, serial and deserial functions, and all are parallel safe */
extern bool sift_v6_gen_parallel_ok(const SiftV6Spec *spec);
extern uint64 v6_gen_limit(const V6Gen *gen);

/* the AggState the functions are called with names the aggregate (or its final function) they run for */
extern void v6_gen_call_context(V6Gen *gen, V6GenAgg *ga, bool final);
extern void v6_gen_advance(V6Gen *gen, V6GenAgg *ga, V6Acc *acc, const Datum *args, const bool *nulls);
extern void v6_gen_final(V6Gen *gen, V6GenAgg *ga, V6Acc *acc);
extern void v6_gen_args_of(V6Gen *gen, V6GenAgg *ga, SiftVec *const *argvec, uint16 r, Datum *args, bool *nulls);

#endif							/* SIFT_AGG_V6_GEN_H */
