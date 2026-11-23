/*
 * col/f2/f2_col_diff.h - what an UPDATE of a heap row changed of the entries of a sift_col index (spec 49 3, 4.2), found from the two versions of the row: the attributes of the index
 * whose value differs (a TOAST pointer is compared as the pointer, a document is not read), whether the row left or entered a partial index, which leaves of the changed attributes differ
 * (only those are shredded, old and new), and whether the change may be a patch of the column delta chain (SIFT_PT_COL_DELTA) instead of a tombstone and a new row.
 *
 * Equal here is "the same bytes": a value that is equal but not the same bytes is reported changed, which costs a patch the equal leaves of would have saved and never a wrong result.
 */
#ifndef SIFT_COL_F2_COL_DIFF_H
#define SIFT_COL_F2_COL_DIFF_H

#include "postgres.h"

#include "col/f2/rowbuf.h"

#define SIFT_F2_DIFF_MAX_LEAVES		9	/* the changed leaves of one record, and the slot of the address (SIFT_COL_DELTA_MAX_COLS_INDEX + 1) */

/* ---- the attributes of the index ---- */

typedef struct SiftF2AttDesc
{
	int16		attlen;
	bool		attbyval;
} SiftF2AttDesc;

/* The two varlena values are the same bytes: two TOAST pointers by the pointer (nothing is detoasted), two inline values by their bytes; one in the table's toast and one inline are not */
extern bool sift_datum_fast_equal(Datum a, Datum b);

/* The two values of the attribute are the same bytes, a NULL being the same as a NULL only */
extern bool sift_f2_att_equal(const SiftF2AttDesc *att, Datum oldv, bool oldnull, Datum newv, bool newnull);

/* The attributes (bit i: attribute i) whose value differs; at most 64 */
extern uint64 sift_f2_att_diff(const SiftF2AttDesc *atts, int natts, const Datum *oldv, const bool *oldnull, const Datum *newv, const bool *newnull);

/* What the update did to the row's membership of a partial index */
typedef enum SiftF2PredMove
{
	SIFT_PRED_NEITHER,			/* in neither version: not in the index, nothing to do */
	SIFT_PRED_ENTERED,			/* only the new version: the new entry is the one aminsert wrote, the old version had none */
	SIFT_PRED_EXITED,			/* only the old version: its entry ends, aminsert wrote none */
	SIFT_PRED_BOTH				/* in both: the columns are compared */
} SiftF2PredMove;

extern SiftF2PredMove sift_f2_pred_move(bool old_in, bool new_in);

/* ---- the leaves of the changed attributes ---- */

/*
 * The leaves, of those that want says belong to a changed attribute, whose cell differs between the old and the new version: leaf[] and cell[] hold the new cells, ascending, at most
 * SIFT_F2_DIFF_MAX_LEAVES. rejected: a change a patch cannot hold, which sends the update the way of a tombstone and a new row: more changed leaves than a record holds, a cell that keeps a
 * value its column could not hold (xp), a leaf of the elements of an array or the length of one. olde, newe: per leaf the cells of the elements, NULL for a leaf of the rows; parent: per
 * leaf the length leaf it is an element of, or -1.
 */
typedef struct SiftF2LeafDiff
{
	int			nchanged;
	bool		rejected;
	int16		leaf[SIFT_F2_DIFF_MAX_LEAVES];
	SiftCell	cell[SIFT_F2_DIFF_MAX_LEAVES];
} SiftF2LeafDiff;

extern void sift_f2_leaf_diff(const SiftCellKind *kinds, const int16 *parent, const bool *want, int ncols, const SiftCell *oldc, SiftCell *const *olde, const SiftCell *newc, SiftCell *const *newe,
							  SiftF2LeafDiff *out);

/* A cell the same as another: the same null and tag, and for a value the same integer, float bits or string bytes; a cell with a payload is the same only as one with the same payload */
extern bool sift_f2_cell_same(SiftCellKind kind, const SiftCell *a, const SiftCell *b);

/* ---- admission (spec 49 4.2) ---- */

typedef enum SiftF2Admit
{
	SIFT_F2A_OK = 0,
	SIFT_F2A_NOT_TRACKED,		/* track_changes is off: no trigger, no delta */
	SIFT_F2A_KEY_CHANGED,		/* a key_order leaf changed: the row moves to another place of the sort */
	SIFT_F2A_PREDICATE,			/* the row entered or left the partial index */
	SIFT_F2A_REJECTED,			/* a leaf that cannot be a patch (leaf diff) */
	SIFT_F2A_TOO_MANY_LEAVES,
	SIFT_F2A_TOO_WIDE,
	SIFT_F2A_PAYLOAD,
	SIFT_F2A_CHAIN_FULL
} SiftF2Admit;

typedef struct SiftF2AdmitIn
{
	bool		tracked;
	bool		key_changed;
	SiftF2PredMove pred;
	bool		rejected;		/* SiftF2LeafDiff.rejected */
	int			nchanged_leaves;
	int			total_leaves;
	uint32		payload_bytes;	/* the bytes of the columns of the record, the address excluded */
	uint32		chain_pages;	/* the pages of the partition's chain, and those its staged records will need */
	uint32		staged_pages;
} SiftF2AdmitIn;

extern SiftF2Admit sift_f2_col_delta_admit_index(const SiftF2AdmitIn *in);

#ifndef SIFT_S49_STANDALONE
#include "access/htup.h"
#include "catalog/index.h"
#include "executor/executor.h"

#include "col/f2/f2.h"

/* What the tuples of an update are read with: the index, its table, and for an index that is not of plain columns the executor state its entries are formed in */
typedef struct SiftF2DiffEnv
{
	Relation	heap;
	Relation	index;
	SiftF2Rel  *f;
	bool		direct;			/* the columns of the index are columns of the table: no predicate, no expression */
	IndexInfo  *info;
	EState	   *estate;
	TupleTableSlot *slot;
	ExprState  *pred;
	MemoryContext cx;			/* the strings of the cells of the result live here */
} SiftF2DiffEnv;

typedef struct SiftF2DiffResult
{
	SiftF2PredMove pred;
	bool		key_changed;
	int			nchanged_atts;
	SiftF2LeafDiff leaves;
} SiftF2DiffResult;

/* Compares the two versions of a row the index holds (an UPDATE that was not HOT); tid is the address of the new version, which the entries are shredded for */
extern void sift_f2_diff_update(const SiftF2DiffEnv *env, HeapTuple oldt, HeapTuple newt, SiftF2DiffResult *res);
#endif

#endif							/* SIFT_COL_F2_COL_DIFF_H */
