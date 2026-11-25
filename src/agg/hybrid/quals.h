/*
 * agg/hybrid/quals.h - the structured quals of SiftHybridScan: which a sift_col index of format major 2 decides exactly (the planner's side), and how the ones that no index qual makes
 * (lower(col) LIKE 'p%', lower(col) = 'x', col ILIKE 'p%', array && and @>, cardinality(array) op k) become predicates of the scan once their right hand sides are known (the executor's).
 *
 * A qual is pushed with a Const or a Param (a prepared statement's, a LATERAL's, an uncorrelated sub-select's) or any other stable expression without a Var on its right hand side. The
 * right hand side is evaluated when the scan starts and again when a parameter changes (hybrid_rescan). What the store cannot decide for the value it got (a pattern with an upper case
 * letter or a byte above 0x7F, an @> with a NULL element) is left to the heap: the scan then does not claim its quals are exact, and a hit is checked against them.
 */
#ifndef SIFT_HYBRID_QUALS_H
#define SIFT_HYBRID_QUALS_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

#include "col/col.h"
#include "col/like/simd.h"

typedef enum SiftHybQualKind
{
	SHQ_STD,					/* an index qual: a comparison, = ANY, IS [NOT] NULL on a column; the access method makes its predicate */
	SHQ_IPREFIX,				/* lower(col) LIKE rhs, col ILIKE rhs: a prefix, or the whole string when rhs has no wildcard */
	SHQ_IEQ,					/* lower(col) = rhs */
	SHQ_ARR_OVERLAP,			/* col && rhs */
	SHQ_ARR_CONTAINS,			/* col @> rhs */
	SHQ_CARD,					/* cardinality(col) <op> rhs */
	SHQ_NE,						/* col <> rhs: a column the store compares by value, with a constant of the same kind */
	SHQ_RESIDUAL,				/* a clause of an aggregate's scan that the store does not decide: the heap does (never pushed by SiftHybridScan, whose rows keep it as a qual) */
	SHQ_EXISTS,					/* a tenant EXISTS of an aggregate's scan: the heap decides it unless the key sets of every SHQ_KEYSET were taken by the store */
	SHQ_KEYSET					/* (col = ANY (rhs)) IS NOT UNKNOWN: a key set that skips what it misses, true of every row (hybrid_semi_plan.c) */
} SiftHybQualKind;

/* What the planner found of a qual: its kind and the column of the store's index that it is on (0-based) */
typedef struct SiftHybQual
{
	SiftHybQualKind kind;
	int			col;
	bool		fold;			/* SHQ_IPREFIX: the pattern is folded to lower case as well (ILIKE) */
} SiftHybQual;

/* The types a store column is read and compared in */
extern bool sift_hybrid_type_usable(Oid typid);

/* The types a qual is tested in: those above, and a numeric, which its leaf compares as a scaled integer */
extern bool sift_hybrid_filter_type(Oid typid);

/*
 * Whether the store of index ix (open as indexrel) decides the clause exactly, with qual describing it; *norm gets the clause with its column on the left. A text column compares by bytes only
 * under a deterministic collation (ranges and case folding: the C collation); a float only with a constant that is no NaN; a numeric only with a constant its leaf holds exactly; an array operator only on the head of an array that keeps its image.
 */
extern bool sift_hybrid_qual_pushable(IndexOptInfo *ix, Relation indexrel, Expr *clause, SiftHybQual *qual, Expr **norm);

/* The executor's form of an extended qual */
typedef struct SiftHybExt
{
	SiftHybQual qual;
	int			leaf;			/* of the column in the store */
	Oid			collid;
	SiftArrKind akind;
	int			op;				/* SHQ_CARD: SiftColOp */
	ExprState  *rhs;
	/* the result of the last evaluation */
	bool		unsat;			/* no row can pass: the right hand side is NULL, or an empty array for && */
	bool		to_heap;		/* the store cannot decide it for this value: the heap does */
	int			npreds;
	SiftColPred pred[2];
	SiftColValue vals[2][2];
	SiftArrQuery *aq;
} SiftHybExt;

/* The qual is the normalized clause of sift_hybrid_qual_pushable; leaf is the leaf of its column in the store */
extern SiftHybExt *sift_hybrid_ext_init(PlanState *ps, const SiftHybQual *q, Expr *clause, int leaf);

/* Evaluates the right hand side and makes the predicates; the text they point to lives in cx until the next evaluation */
extern void sift_hybrid_ext_eval(SiftHybExt *e, ExprContext *econtext, MemoryContext cx);

#endif							/* SIFT_HYBRID_QUALS_H */
