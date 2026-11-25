/*
 * agg/v6/v6_plan_rewrite.h - what the planner of SiftAggV6 does to an aggregate query before it compiles it (CB-4):
 *   SUM(x + C) of an int2 column x and a constant C is SUM(x) + C * COUNT(x) (NULL where COUNT(x) is 0): ninety such sums are two accumulators;
 *   a key that is the first key plus or minus a constant (ClientIP - 1 next to ClientIP) is a function of it: the scan groups by the first and the plan's projection makes the others;
 *   DATE_TRUNC('minute', t) of a timestamp is the date_bin of a fixed stride (the bin the scan has);
 *   length(text), extract(minute | hour FROM timestamp), the host regexp_replace of an URL and CASE WHEN column = constant .. THEN text ELSE constant END are the keys and arguments the scan computes (SiftV6Derive).
 */
#ifndef SIFT_AGG_V6_PLAN_REWRITE_H
#define SIFT_AGG_V6_PLAN_REWRITE_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/v6.h"

typedef struct SiftV6Resolver
{
	/* e is a column of the index the scan reads: its SiftV6Col (the planner's column_of) */
	bool		(*column) (void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out);
	void	   *ctx;
	bool		arith_ok;		/* the scan computes SVD_ARITH (the batches of an unnest do) */
	Index		elem_relid;		/* the relation of the elements of an unnest, whose columns an expression may read beside the table's (0: none) */
} SiftV6Resolver;

/*
 * The postfix of an SVD_ARITH: one byte a token, packed from cval[0] on. A column is the source (0) or a condition column (1 + its index); it is a float8, or an integer that PostgreSQL casts
 * to float8 in its place. At most one constant, aux as the bits of the double.
 */
#define SIFT_ARITH_COL		0x10	/* + the column */
#define SIFT_ARITH_CONST	0x20
#define SIFT_ARITH_ADD		0x30
#define SIFT_ARITH_SUB		0x31
#define SIFT_ARITH_MUL		0x32
#define SIFT_ARITH_MAX_TOKENS	32
#define SIFT_ARITH_MAX_STACK	8

/* a copy of expr in which every SUM(x + C) is the expression over SUM(x) and COUNT(x); the same tree gives the same copy, so that the plan can make it again */
extern Node *sift_v6_collapse_offset_sums(Node *expr);

/* e is the first key plus delta, a column of type *type (int4 or int8): the first key's Var (not copied) */
extern const Var *sift_v6_affine_of(const Expr *e, int64 *delta, Oid *type);
/* the range of x for which x + delta stays in the type: its bounds, tightened by the delta of one more derived key */
extern void sift_v6_affine_range(Oid type, int64 delta, int64 *lo, int64 *hi);

/* e is date_trunc(unit, timestamp column) of a unit of fixed length: its source column and the bin (key: the index the scan gives it) */
extern bool sift_v6_date_trunc_bin(const SiftV6Resolver *rs, const Expr *e, SiftV6Col *src, int64 *stride_us);

/* e is a key or an argument the scan computes: its derivation (src and the conditions are columns of the index) and the type of the vector; key: a key (else an argument) */
extern bool sift_v6_derive_of(const SiftV6Resolver *rs, const Expr *e, bool key, SiftV6Derive *d, Oid *restype);

/* The path of the grouped paths of input_rel with the order of ORDER BY .. LIMIT (v6_group_topk.h), added to output_rel (UPPERREL_ORDERED) */
/* LIMIT + OFFSET of the query when they are constants and the statement is ordered (0: otherwise) */
extern uint32 sift_v6_topk_of(const Query *q);
extern uint32 sift_v6_rows_k_of(const Query *q);
/* The sort columns of the query as positions of the scan tuple (spec topk_item); false when one is not a key or an aggregate of the node */
extern bool sift_v6_topk_items(const Query *q, const List *scan_tlist, SiftV6Spec *s);
#define SIFT_LOCK_MAX_K	4096
/* the K of GROUP BY .. LIMIT K with no ORDER BY, HAVING or DISTINCT (0: the query is not one, or K is over SIFT_LOCK_MAX_K) */
extern uint32 sift_v6_limit_of(const Query *q);
extern void sift_v6_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);

/* The derivations, the derived key and argument columns and the affine range of a spec as ints after those of sift_v6_spec_pack, and back from position at */
extern List *sift_v6_derive_pack(List *ints, const SiftV6Spec *s);
extern void sift_v6_derive_unpack(SiftV6Spec *s, const List *ints, int at);

#endif							/* SIFT_AGG_V6_PLAN_REWRITE_H */
