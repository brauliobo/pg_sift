/*
 * sift_aggspec.h - the plan of a SiftAgg: what the planner proved about a query and the executor runs.
 *
 * The planner (sift_aggplan.c) resolves every column, jsonb path and array element the query uses to a leaf of the column
 * source (sift_colsrc.h) and builds a SiftAggSpec; the spec travels in custom_private as a flat List (sift_aggspec_pack /
 * sift_aggspec_unpack: Integer, String and Node items in a fixed order) so the plan can be copied, cached and sent to
 * parallel workers like any other plan. Nothing in a spec points to the planner's data.
 *
 * Levels: a query without a set-returning function runs at row level (one unit row = one tuple). With one (jsonb_to_recordset,
 * jsonb_array_elements, jsonb_each over an array or object leaf) it runs at element level: one tuple per element of the group
 * leaf `group`, the row level columns of its parent row being available to every tuple.
 */
#ifndef SIFT_AGGSPEC_H
#define SIFT_AGGSPEC_H

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "sift_aggkern.h"
#include "sift_colsrc.h"

#define SIFT_AGG_MAX_KEYS	4
#define SIFT_AGG_MAX_VX		16		/* operations of a float8 aggregate argument */

typedef enum SiftAggPredKind
{
	SAP_TRUE,
	SAP_FALSE,
	SAP_AND,
	SAP_OR,
	SAP_CMP,					/* leaf op operand: kernels on I32/I64/F64 leaves */
	SAP_IN,						/* leaf IN / NOT IN (array operand): I32 leaves */
	SAP_NULL,					/* leaf IS [NOT] NULL */
	SAP_LUT						/* boolean expression of one leaf, evaluated by the executor on every distinct value */
} SiftAggPredKind;

typedef struct SiftAggPred
{
	SiftAggPredKind kind;
	int			id;				/* preorder number, the index of the node's executor state */
	int			leaf;			/* SAP_CMP, SAP_IN, SAP_NULL, SAP_LUT */
	int			op;				/* SAP_CMP: SiftCmpOp (leaf op operand); SAP_IN, SAP_NULL: 1 = NOT IN / IS NOT NULL */
	Node	   *expr;			/* SAP_CMP: operand (no Vars); SAP_IN: array operand; SAP_LUT: bool expression of Param 0 (PARAM_EXEC) */
	Oid			argtype;		/* SAP_CMP, SAP_IN: type of the operand (element type for IN) */
	int			level;			/* 0 row, 1 element: of the leaves the node uses (AND/OR: the highest of the children) */
	int			nchild;
	struct SiftAggPred **child;
	char	   *desc;			/* EXPLAIN */
} SiftAggPred;

typedef enum SiftAggKeyKind
{
	SAK_LEAF,					/* the leaf's value */
	SAK_LUT						/* expr(leaf value), evaluated once per distinct value */
} SiftAggKeyKind;

typedef struct SiftAggKey
{
	SiftAggKeyKind kind;
	int			leaf;
	Node	   *expr;			/* SAK_LUT: expression of Param 0 */
	Oid			typid;			/* of the key as the query outputs it */
	int32		typmod;
	Oid			collid;
	Oid			eqop;			/* the grouping equality operator */
	char	   *desc;
} SiftAggKey;

typedef enum SiftAggKind
{
	SAA_COUNT_STAR,
	SAA_COUNT,					/* count(leaf) */
	SAA_COUNT_ROWS,				/* count(DISTINCT x) where x is unique per row: distinct parent rows at element level */
	SAA_COUNT_DISTINCT,			/* count(DISTINCT leaf) */
	SAA_SUM_INT,				/* sum of an I32 leaf (int2, int4): int8 */
	SAA_SUM_F64,				/* sum of a F64 leaf: float8 (float4 for a float4 result) */
	SAA_AVG_INT,				/* avg of an I32 leaf: numeric */
	SAA_AVG_F64,				/* avg of a F64 leaf: float8 */
	SAA_MIN,
	SAA_MAX
} SiftAggKind;

/* One step of the postfix program of a float8 aggregate argument (+ - * of leaves and constants, evaluated in double) */
typedef enum SiftAggVxOp
{
	SVX_LEAF,					/* push leaf (an I32 or F64 leaf, widened to double) */
	SVX_CONST,					/* push c */
	SVX_ADD,
	SVX_SUB,
	SVX_MUL,
	SVX_NEG
} SiftAggVxOp;

typedef struct SiftAggVx
{
	SiftAggVxOp op;
	int			leaf;
	double		c;
} SiftAggVx;

typedef struct SiftAggAgg
{
	SiftAggKind kind;
	int			leaf;			/* -1: none */
	int			nvx;			/* SUM_F64 / AVG_F64: the argument is this program instead of a leaf */
	SiftAggVx  *vx;
	SiftAggPred *filter;		/* NULL: none */
	Oid			restype;		/* result type of the Aggref */
	Oid			argtype;		/* type of the leaf's SQL values (output type of min/max) */
	Oid			collid;			/* input collation: count(DISTINCT) of dictionary values */
	char	   *desc;
} SiftAggAgg;

typedef struct SiftAggSpec
{
	Oid			indexoid;		/* the column index; InvalidOid: the table itself */
	Oid			heapoid;
	int			group;			/* leaf id of the array/object leaf the query unnests, -1: row level */
	int			nleaves;		/* the leaves the spec reads, ascending */
	int		   *leaves;
	int			npred;			/* conjuncts, ANDed */
	SiftAggPred **pred;
	int			nkeys;
	SiftAggKey *key;
	int			naggs;
	SiftAggAgg *agg;
	int			npreds_total;	/* number of SiftAggPred nodes (ids 0..n-1) */
	double		est_groups;
	char	   *desc_quals;		/* EXPLAIN: the conjuncts, as text */
} SiftAggSpec;

extern List *sift_aggspec_pack(const SiftAggSpec *spec);
extern SiftAggSpec *sift_aggspec_unpack(const List *items);

#endif							/* SIFT_AGGSPEC_H */
