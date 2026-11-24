/*
 * agg/vexpr.h - expressions over the 2048-row batch (agg/vec.h) without a call of the executor per row: an expression tree of columns, constants, the
 * operators + - * / and unary minus and abs, casts, comparisons, AND OR NOT, IS [NOT] NULL, COALESCE, CASE WHEN (of numbers or of booleans), boolean inputs (a
 * boolean column, or jsonb_exists(doc, key) as the key presence of the document column and its validity), date_trunc and EXTRACT of a timestamp or a date compiles
 * to a program of at most SIFT_VEXPR_MAX_OPS operations on SIFT_VEXPR_MAX_REGS value registers and as many boolean slots, and one call of sift_vexpr_eval runs it
 * over a batch.
 *
 * What PostgreSQL gives for one row it gives here for every row, bit for bit: int2/int4/int8, numeric (of at most 38 digits, as scaled integers: SVEC_NUM64 up
 * to what an int64 holds, SVEC_NUM128 above) and float8 arithmetic with its errors (integer out of range, bigint out of range, division by zero, value out of
 * range: overflow and underflow of float8, timestamp out of range), NULL as the unknown of three-valued logic (a boolean is two bitmaps, the rows that are true
 * and the rows that are known, so NOT of an unknown is unknown), float8 compares in its order (NaN equals NaN and is above +Infinity, -0 equals 0). Operands of
 * two types meet in the type PostgreSQL gives the operator: int4 and int8 in int8, an integer and a numeric in numeric (scales are brought to the larger, a
 * product has the sum of the scales), a number and a float8 in float8 (a numeric of at most 15 digits); the compiler adds the casts, and keeps a numeric in
 * int64 only while the bound of its digits proves that no operation can leave it, else in 128 bits. An operation runs on the rows that PostgreSQL would run it on
 * and no others: every operation has the batch rows that are live for it (act), an error can come only from such a row, the right side of AND is live where the
 * left is not false, of OR where it is not true, the second argument of COALESCE where the first is NULL, a branch of CASE where its condition is the first true
 * one; an operation whose rows are all dead is jumped over. The error raised for a batch is the one of the first row that has any (of the first operation of the
 * program for that row), as PostgreSQL's row by row evaluation raises, whichever operation of the program meets an error of a later row first.
 *
 * Calendar: date_trunc of a timestamp to second, minute, hour, day or week is a fixed period (a multiply by the reciprocal of the period and a correction), to month,
 * quarter or year and EXTRACT of year, quarter, month, day, dow, isodow, doy, hour, minute or epoch go through the days of the proleptic Gregorian calendar (twelve
 * integer operations a date, no branch and no table). A timestamptz has the zone of the node (core/zoneoff.h, the pieces of constant offset of the session zone):
 * every row reads its own piece, and date_trunc to a day or more puts the instant of the truncated local time in the piece of that time, as PostgreSQL does (a month
 * that starts in standard time ends a row of summer time in the offset of the month's first midnight). +-infinity is kept by date_trunc, gives NULL to EXTRACT but year and epoch, and those two raise 0A000 for it: a scan sends a
 * chunk whose zone holds an infinity to the executor. EXTRACT is a numeric as in PostgreSQL: scale 0, and for the epoch of a timestamp scale 6.
 *
 * Registers are views: a column is read in place (the register aliases the vector of the input, nothing is copied), an operation writes a buffer of the
 * register file. A register whose validity is NULL (all_valid) has every live row valid.
 *
 * compile returns false for what the machine does not take (a type that is none of the above, a numeric divided, a numeric of more than 38 digits, a tree deeper
 * than the registers), and the caller keeps the executor's evaluation for that expression.
 */
#ifndef SIFT_AGG_VEXPR_H
#define SIFT_AGG_VEXPR_H

#include "agg/kern6.h"

#define SIFT_VEXPR_MAX_REGS		16
#define SIFT_VEXPR_MAX_MASKS	16
#define SIFT_VEXPR_MAX_OPS		64
#define SIFT_VEXPR_MAX_ARGS		17

typedef enum SiftVexprKind
{
	SVX_COL,					/* column aux of the input, of type (a numeric: scale and prec) */
	SVX_CONST,					/* i64 (int4, int8, a numeric of scale) or f64 (float8) of type */
	SVX_NULL,					/* NULL of type (a numeric: scale) */
	SVX_CAST,					/* args[0] to type: int4 to int8, float8, numeric or int8 to float8, numeric, a numeric to float8 */
	SVX_ADD,
	SVX_SUB,
	SVX_MUL,
	SVX_DIV,
	SVX_NEG,
	SVX_ABS,
	SVX_CMP,					/* aux = SiftCmpOp, of two arguments of one type */
	SVX_AND,
	SVX_OR,
	SVX_NOT,
	SVX_IS_NULL,
	SVX_IS_NOT_NULL,
	SVX_MASK_IN,				/* the boolean input aux: a boolean column, or jsonb_exists(doc, key) */
	SVX_BOOL_CONST,				/* flag */
	SVX_COALESCE,				/* two or more arguments of one type */
	SVX_CASE,					/* aux = the WHENs: args = cond, then, ..., and an else when flag */
	SVX_DATE_TRUNC,				/* aux = SiftVxUnit of args[0], a timestamp (type int64); zone: set for a timestamptz, the zone its instants are read in */
	SVX_EXTRACT					/* aux = SiftVxField of args[0], a timestamp or timestamptz (int64) or a date (int32); zone as of date_trunc */
} SiftVexprKind;

typedef enum SiftVxUnit
{
	SVU_SECOND,
	SVU_MINUTE,
	SVU_HOUR,
	SVU_DAY,
	SVU_WEEK,
	SVU_MONTH,
	SVU_QUARTER,
	SVU_YEAR
} SiftVxUnit;

typedef enum SiftVxField
{
	SVF_YEAR,
	SVF_QUARTER,
	SVF_MONTH,
	SVF_DAY,
	SVF_DOW,
	SVF_ISODOW,
	SVF_DOY,
	SVF_HOUR,
	SVF_MINUTE,
	SVF_EPOCH
} SiftVxField;

typedef struct SiftVexprNode
{
	SiftVexprKind kind;
	SiftVecType type;			/* of the value: SVEC_BOOL for a boolean (it is told of a COALESCE or a CASE by it) */
	int16		scale;			/* a numeric: the digits after the point */
	uint8		prec;			/* a numeric column: the most digits of a value (0: 18 for SVEC_NUM64, 38 for SVEC_NUM128) */
	int32		aux;
	int64		i64;
	double		f64;
	bool		flag;
	struct ZoneOff *zone;
	uint8		nargs;
	const struct SiftVexprNode *args[SIFT_VEXPR_MAX_ARGS];
} SiftVexprNode;

typedef enum SiftVecOpCode
{
	VOP_LOAD_COL,
	VOP_LOAD_CONST,
	VOP_LOAD_NULL,
	VOP_CAST,
	VOP_ADD,
	VOP_SUB,
	VOP_MUL,
	VOP_DIV,
	VOP_NEG,
	VOP_ABS,
	VOP_CMP,
	VOP_IS_NULL,
	VOP_MASK_AND,
	VOP_MASK_OR,
	VOP_MASK_NOT,
	VOP_MASK_IN,
	VOP_MASK_CONST,
	VOP_INIT_NULL,				/* dst has no valid row (the start of a CASE) */
	VOP_COPY,					/* dst = a, in a buffer of dst (a column is read in place until something needs to write it) */
	VOP_BLEND,					/* the rows of mask c: dst takes a with its validity */
	VOP_ACT_COPY,				/* live rows of slot dst = those of slot a */
	VOP_ACT_AND,				/* dst = a and the rows of slot b that are true */
	VOP_ACT_ANDN,				/* dst = a and not the rows of slot b that are true */
	VOP_ACT_NOTFALSE,			/* dst = a and the rows of slot b that are not false (true or unknown) */
	VOP_ACT_NOTVALID,			/* dst = a and the rows where register b is NULL */
	VOP_SKIP_IF_EMPTY,			/* when the live rows of slot a are none, the next aux operations are jumped over */
	VOP_RESCALE,				/* a numeric times 10^aux, in type */
	VOP_DATE_TRUNC,				/* aux = SiftVxUnit, zone = that of a timestamptz */
	VOP_EXTRACT,				/* aux = SiftVxField, zone = that of a timestamptz, type = the input (int32 a date, int64 a timestamp), type_to numeric of scale */
	VOP_MASK_BLEND,				/* the rows of slot c take the true and known bits of slot a: a boolean CASE and COALESCE */
	VOP_MASK_UNKNOWN,			/* slot dst has no known row (the start of a boolean CASE) */
	VOP_MASK_ISNULL,			/* slot dst: the rows where boolean slot a is unknown are true, aux = 1 for IS NOT NULL */
	VOP_ACT_UNKNOWN				/* dst = a and the rows where boolean slot b is unknown */
} SiftVecOpCode;

#define VOP_FLAG_CONST_B	1	/* the second operand is imm_i64 / imm_f64 */

typedef struct SiftVecOp
{
	SiftVecOpCode op;
	SiftVecType type;			/* of the operands (the target of a CAST is type_to) */
	SiftVecType type_to;
	uint8		dst;
	uint8		a;
	uint8		b;
	uint8		c;
	uint8		act;			/* the slot of the rows that are live for this operation */
	uint16		flags;
	int16		scale;			/* of a numeric result, or the source of a cast of a numeric to float8 */
	int32		aux;
	int64		imm_i64;
	double		imm_f64;
	struct ZoneOff *zone;
} SiftVecOp;

typedef struct SiftVexprProg
{
	SiftVecOp	ops[SIFT_VEXPR_MAX_OPS];
	uint16		n_ops;
	uint8		n_regs;
	uint8		n_masks;
	bool		out_is_bool;
	bool		wide;			/* a register holds 128-bit values: the register file is made for them at the first run */
	uint8		out;			/* the register of the value, or the slot of the boolean */
	SiftVecType out_type;
	int16		out_scale;
} SiftVexprProg;

/* The inputs of a program: columns by index, and the booleans (bits = the rows that are true where valid; valid NULL = all valid) */
typedef struct SiftVexprMaskIn
{
	const uint64 *bits;
	const uint64 *valid;
} SiftVexprMaskIn;

typedef struct SiftVexprIn
{
	const SiftVec *cols;
	const SiftVexprMaskIn *masks;
} SiftVexprIn;

typedef struct SiftVexprCtx SiftVexprCtx;

/* What a program gave: a value register, or a boolean as the rows that are true and the rows that are known (the same bits outside the live rows mean nothing) */
typedef struct SiftVexprOut
{
	bool		is_bool;
	const SiftVec *vec;
	const uint64 *true_bits;
	const uint64 *known_bits;
} SiftVexprOut;

/* false when the tree is not one the machine runs */
extern bool sift_vexpr_compile(const SiftVexprNode *root, SiftVexprProg *out);

/* The register file (SIFT_VEXPR_MAX_REGS * 16 KB and the slots), allocated once per scan, in cxt */
extern SiftVexprCtx *sift_vexpr_ctx_new(MemoryContext cxt);
extern void sift_vexpr_ctx_free(SiftVexprCtx *ctx);

/* Runs prog over the n <= SIFT_VEC_LEN rows of the batch, those of active (the bits >= n zero); raises what PostgreSQL raises for the first row that has an error */
extern void sift_vexpr_eval(SiftVexprCtx *ctx, const SiftVexprProg *prog, const SiftVexprIn *in, const uint64 *active, uint16 n, SiftVexprOut *out);

/* active &= the rows for which the boolean program is true (an unknown row is not); returns the rows left */
extern uint16 sift_vexpr_filter(SiftVexprCtx *ctx, const SiftVexprProg *prog, const SiftVexprIn *in, uint64 *active, uint16 n);

#endif							/* SIFT_AGG_VEXPR_H */
