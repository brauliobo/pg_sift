/*
 * agg/v6/v6_ext.h - what the aggregate scan of SiftAggV6 reads beyond the columns of one table: the small relations of a star join (up to SIFT_STAR_MAX_DIMS dimensions, each an inner or left
 * equi join of the fact table, read whole from the plan of the dimension into the tables of agg/v6/v6_star.h) and the array an unnest of the index takes apart (the elements of the array
 * column's leaves, without the rows being built). The planner makes a SiftV6Ext, plan.c stores it with the spec (v6_spec_pack.c), and exec.c hands it to the scan.
 */
#ifndef SIFT_AGG_V6_EXT_H
#define SIFT_AGG_V6_EXT_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_star.h"

#define SIFT_V6_XELEM_COLS	16		/* the columns of an element: at most SIFT_F2_PULL_MAX_CHILD */
#define SIFT_V6_XTESTS		16		/* the tests of the elements of an array a statement makes of a row (EXISTS, jsonpath): each is a boolean of the row */
#define SIFT_V6_XCMPS		4		/* the comparisons of an element with a column of its row one test has */

/* A dimension of a star join: the fact attributes and the columns of the plan of the dimension its keys and payload are */
typedef struct SiftV6XDim
{
	SiftV6JoinMode mode;
	int			nkeys;
	int			fatt[SIFT_STAR_MAX_KEYS];	/* the attribute of the index each key is of the fact table (fsrc SV6SRC_ATTR) */
	Oid			ftype[SIFT_STAR_MAX_KEYS];
	SiftV6Src	fsrc[SIFT_STAR_MAX_KEYS];	/* where the probing side has the key: a column of the index, the element of the unnest (fslot: its column) or a payload column of an earlier dimension (fdim, fslot) */
	int			fdim[SIFT_STAR_MAX_KEYS];
	int			fslot[SIFT_STAR_MAX_KEYS];
	int			kx[SIFT_STAR_MAX_KEYS];		/* a key the probing side computes: 1 + its place in SiftV6Ext.kexpr, the other fields of it naming no column (0: the column they name) */
	Oid			ktype[SIFT_STAR_MAX_KEYS];	/* the type of the key column of the dimension */
	AttrNumber	kcol[SIFT_STAR_MAX_KEYS];	/* the column of the dimension: its attribute while planned, then its position in the tuples of its plan */
	Index		kvarno[SIFT_STAR_MAX_KEYS];	/* the relation of the column when the dimension is a join of several (0: the attribute is the one of the only relation) */
	int			npay;
	AttrNumber	pcol[SIFT_STAR_MAX_PAY];
	Oid			ptype[SIFT_STAR_MAX_PAY];
} SiftV6XDim;

/* The function of the lateral relation: unnest of an array column, or jsonb_to_recordset, jsonb_each or jsonb_each_text of a jsonb one; SV6E_KEYS has no relation: a map read by constant keys, one row for each row of the table; SV6E_PATH: no lateral relation, the tests are over the keys of the objects of a jsonb array (jsonpath, @>) */
typedef enum SiftV6ElemFn
{
	SV6E_UNNEST = 0,
	SV6E_RECORDSET,
	SV6E_PATH,
	SV6E_KEYS,
	SV6E_EACH,
	SV6E_EACH_TEXT
} SiftV6ElemFn;

/* How the statement reads the value of a member of jsonb_each / jsonb_each_text: through its text, (value #>> '{}')::T or value::T of the text, or as jsonb_each's jsonb cast to T: a string or a boolean raises for the cast and not for the text */
typedef enum SiftV6ValueRead
{
	SV6V_NONE = 0,
	SV6V_TEXT,
	SV6V_CAST
} SiftV6ValueRead;

/*
 * The array of an unnest: the attribute of the index, the leaf of its lengths, and one leaf for each column of its elements (the one of unnest, the declared columns of a recordset, all of them
 * read so that a value the leaf cannot hold is known to the scan whichever column the statement names).
 */
typedef struct SiftV6XElem
{
	SiftV6ElemFn fn;
	int			attno;
	Oid			type;			/* unnest: the type of the element */
	int			group_leaf;
	char	   *key;			/* recordset: the key of the document whose array it takes apart (jsonb_to_recordset(doc -> 'key')); NULL for the document itself */
	bool		left;			/* LEFT JOIN LATERAL ... ON true: a row with no element is kept with a NULL one */
	bool		semi;			/* EXISTS over the elements: a row once, when an element passes elem_where, and no element */
	SiftV6ValueRead vread;		/* jsonb_each, jsonb_each_text: how the statement reads column 1, the value of the member (column 0 is its key) */
	int			ncols;
	int			col_leaf[SIFT_V6_XELEM_COLS];
	Oid			col_type[SIFT_V6_XELEM_COLS];
	int			col_att[SIFT_V6_XELEM_COLS];	/* SV6E_KEYS: the attribute of the index each column reads */
	int			key_leaf;		/* SV6E_KEYS: the leaves of the keys and of the values of the map (-1: none); a column is the value of the key elem_names names, of the type col_type, or the leaf col_leaf (>= 0) of a key of a document */
	int			val_leaf;
	bool		col_kind[SIFT_V6_XELEM_COLS];	/* the column is the kind of the value the leaf has in the element (SIFT_ELEM_KIND_*, agg/vec.h), not the value */
} SiftV6XElem;

/* An element compared with a column of its row: the SiftCmpOp (agg/kern6.h) of the element column against the column outer of the index, both integers or both float8 */
typedef struct SiftV6XCmp
{
	int			op;
	int			elem;
	int			outer;
	Oid			outer_type;
} SiftV6XCmp;

/*
 * A test of the elements of the array of a row, a boolean of the row: some element passes the quals (test_quals[i]: over the element columns, v6_unnest_jsonb.h) and the comparisons with the columns of the row.
 * A row whose document the leaves cannot hold is decided from the heap. need: the rows the statement keeps (0 all: a FILTER reads the flag as the virtual attribute SIFT_V6_XFLAG_VATT + the test;
 * 1 those where it is true; 2 those where it is false). nullable: a jsonpath is NULL for a row with no document, an EXISTS is false.
 */
typedef struct SiftV6XTest
{
	int			need;
	bool		nullable;
	int			ncmps;
	SiftV6XCmp	cmp[SIFT_V6_XCMPS];
} SiftV6XTest;

#define SIFT_V6_RESID_COLS	16

typedef struct SiftV6Ext
{
	int			ndims;
	SiftV6XDim	dim[SIFT_STAR_MAX_DIMS];
	List	   *kexpr;			/* the probing sides of the keys that are expressions (lower(i.category), o.k + i.n - 3), over the virtual attributes 1.. of rcol like resid: PostgreSQL's evaluator gives their vectors before the dimension is probed */
	List	   *resid;			/* the conditions of the join that no key says (an inequality, a WHERE over the columns of a left join), over the virtual attributes 1.. of rcol (v6_join_lookup.h); a pair that fails one leaves the join */
	int			nrcol;
	SiftV6Col	rcol[SIFT_V6_RESID_COLS];
	bool		unnest;
	SiftV6XElem elem;
	List	   *where;			/* unnest: the quals of the fact table over INDEX_VAR attributes, which the scan evaluates on its batches (it has no index keys) */
	List	   *elem_where;		/* unnest: the quals of the element over the virtual attributes of its columns (v6_unnest_jsonb.h), which the scan evaluates on the elements of a batch */
	int			ntests;			/* tests of the row's elements: the scan gives each row once, with a flag for each test (the attribute SIFT_V6_XFLAG_VATT + i of a FILTER) */
	SiftV6XTest test[SIFT_V6_XTESTS];
	List	   *test_quals;		/* of each test: its quals over the element columns, and with them the expression a row the leaves cannot hold is decided by (a jsonpath; NIL for an EXISTS, decided by the function) */
	List	   *elem_names;		/* recordset: the name of each column, as String nodes */
	List	   *elem_array;		/* unnest: the array of a row as an expression of the table's columns when it is not the column of elem.attno itself (doc -> 'key', or an expression of the index), which the rows read from the heap evaluate; NIL otherwise */
	List	   *heap_where;		/* unnest: the quals of the fact table as the statement has them, which a row read from the heap is tested with (the batches of the index decide the others) */
	List	   *pair_where;		/* SV6E_KEYS: the quals over the values of the keys (the virtual attributes of v6_unnest_jsonb.h), which the scan evaluates on the rows it gives to the groups */
	List	   *col_expr;		/* SV6E_KEYS: the expression of the statement each column is, which a row read from the heap is given the value of */
} SiftV6Ext;

extern SiftV6Ext *sift_v6_ext_new(void);
/* SV6E_KEYS, EXPLAIN: the members of documents the columns of the scan read, as `attname['key'] type, ..` (index_desc: the attributes of the index) */
extern char *sift_v6_mapkey_text(const SiftV6Ext *x, TupleDesc index_desc);
/* The column the probing side has key k of dimension d in: what the scan or the unnest maps to a slot of its pipe */
extern SiftV6Col sift_v6_xdim_key_col(const SiftV6XDim *xd, int k);

/* the key is a column of the fact side (otherwise an expression the pipe evaluates) */
static inline bool
sift_v6_xdim_key_is_col(const SiftV6XDim *xd, int k)
{
	return xd->kx[k] == 0;
}
/* The Ext of a spec as a list: [ints, where, elem_where, elem_names, heap_where, resid, kexpr]; NIL for none */
extern List *sift_v6_ext_pack(const SiftV6Ext *x);
extern SiftV6Ext *sift_v6_ext_unpack(const List *l);
/* The positions of the key and payload columns of every dimension in the output of its plan (custom_plans of the node), from the attributes the planner chose */
extern void sift_v6_ext_resolve(SiftV6Ext *x, const List *custom_plans);

#endif							/* SIFT_AGG_V6_EXT_H */
