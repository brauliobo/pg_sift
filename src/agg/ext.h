/*
 * agg/ext.h - what the aggregate scan of SiftAggV6 reads beyond the columns of one table: the small relations of a star join (up to SIFT_STAR_MAX_DIMS dimensions, each an inner or left
 * equi join of the fact table, read whole from the plan of the dimension into the tables of agg/star/star.h) and the array an unnest of the index takes apart (the elements of the array
 * column's leaves, without the rows being built). The planner makes a SiftV6Ext, plan.c stores it with the spec (v6_spec_pack.c), and exec.c hands it to the scan.
 */
#ifndef SIFT_AGG_V6_EXT_H
#define SIFT_AGG_V6_EXT_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/engine.h"
#include "agg/star/star.h"

#define SIFT_V6_XELEM_COLS	16		/* the columns of an element: at most SIFT_F2_PULL_MAX_CHILD */
#define SIFT_V6_XTESTS		16		/* the tests of the elements of an array a statement makes of a row (EXISTS, jsonpath): each is a boolean of the row */
#define SIFT_V6_XSTREAMS	4		/* the arrays of a row whose elements one statement reads: the one its pairs come from and those its tests read */

/* the column is the flag of a test (the virtual attribute SIFT_V6_XFLAG_VATT + the test a FILTER reads it by), no column of the table */
static inline bool
sift_v6_col_is_flag(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->attno >= SIFT_V6_XFLAG_VATT && c->attno < SIFT_V6_XFLAG_VATT + SIFT_V6_XTESTS;
}
#define SIFT_V6_XCMPS		4		/* the comparisons of an element with a column of its row one test has */
#define SIFT_V6_XPARTS		8		/* the parts of one test, each passed by some element of the row (the objects of the pattern of a containment) */

/* A dimension of a star join: the fact attributes and the columns of the plan of the dimension its keys and payload are */
/* the dim of the columns of SiftV6Ext.jexpr: a stage after the dimensions, whose payload the pipe computes */
#define SIFT_V6_JOINED_DIM	SIFT_STAR_MAX_DIMS

typedef struct SiftV6XDim
{
	SiftV6JoinMode mode;
	bool		first_row;		/* a lateral sub-select cut to its first row (agg/lateral_plan.h) */
	int			nkeys;
	int			fatt[SIFT_STAR_MAX_KEYS];	/* the attribute of the index each key is of the fact table (fsrc SV6SRC_ATTR) */
	Oid			ftype[SIFT_STAR_MAX_KEYS];
	SiftV6Src	fsrc[SIFT_STAR_MAX_KEYS];	/* where the probing side has the key: a column of the index, the element of the unnest (fslot: its column) or a payload column of an earlier dimension (fdim, fslot) */
	int			fdim[SIFT_STAR_MAX_KEYS];
	int			fslot[SIFT_STAR_MAX_KEYS];
	int			kx[SIFT_STAR_MAX_KEYS];		/* a key the probing side computes: 1 + its place in SiftV6Ext.kexpr, the other fields of it naming no column (0: the column they name) */
	int			kres[SIFT_STAR_MAX_KEYS];	/* a key the plan of the dimension computes from its columns: its position in the tuples of that plan (0: the column kcol names) */
	Oid			ktype[SIFT_STAR_MAX_KEYS];	/* the type of the key column of the dimension */
	AttrNumber	kcol[SIFT_STAR_MAX_KEYS];	/* the column of the dimension: its attribute while planned, then its position in the tuples of its plan */
	Index		kvarno[SIFT_STAR_MAX_KEYS];	/* the relation of the column when the dimension is a join of several (0: the attribute is the one of the only relation) */
	int			npay;
	AttrNumber	pcol[SIFT_STAR_MAX_PAY];
	Oid			ptype[SIFT_STAR_MAX_PAY];
	double		span;			/* the planner's: the values the statistics say the one integer key of a dimension of unique keys spans, which the table of its dense form is as wide as (0: none) */
	Index		pvarno[SIFT_STAR_MAX_PAY];	/* as kvarno, of each payload column */
} SiftV6XDim;

/* The function of the lateral relation: unnest of an array column, or jsonb_to_recordset, jsonb_array_elements_text, jsonb_array_elements of objects read by their members (SV6E_MEMBERS), jsonb_each or
 * jsonb_each_text of a jsonb one; SV6E_PATH: no lateral relation, the tests are over the keys of the objects of a jsonb array (jsonpath, @>) */
typedef enum SiftV6ElemFn
{
	SV6E_UNNEST = 0,
	SV6E_RECORDSET,
	SV6E_PATH,
	SV6E_ELEMS_TEXT,
	SV6E_MEMBERS,
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
	bool		col_kind[SIFT_V6_XELEM_COLS];	/* the column is the kind of the value the leaf has in the element (SIFT_ELEM_KIND_*, agg/vec.h), not the value */
	Oid			col_valid_type[SIFT_V6_XELEM_COLS];	/* the text of a member the statement reads the NULLs of only (`e ->> 'k' IS NULL`, count): the type its typed leaf is read as, whose
													 * validity the column has with no text (a row read from the heap has the text); InvalidOid: a column of values */
} SiftV6XElem;

/*
 * An element compared with a column of its row: the SiftCmpOp (agg/kern/kern.h) of the element column against the column outer of the index, both integers or both float8. group 0: a conjunct of the
 * test; g: an arm of its disjunction g, whose other arms are the quals of the element of test_quals (the rows of a disjunction are the union of the rows of its arms)
 */
typedef struct SiftV6XCmp
{
	int			op;
	int			elem;
	int			outer;
	Oid			outer_type;
	int			group;
} SiftV6XCmp;

/*
 * A test of the elements of the array of a row, a boolean of the row: for each of its parts some element passes the quals of the part (test_quals[i]: the parts, each a list of quals over the
 * element columns, v6_unnest_jsonb.h) and the comparisons with the columns of the row.
 * A row whose document the leaves cannot hold is decided from the heap. need: the rows the statement keeps (0 all: a FILTER reads the flag as the virtual attribute SIFT_V6_XFLAG_VATT + the test;
 * 1 those where it is true; 2 those where it is false). nullable: a jsonpath is NULL for a row with no document, an EXISTS is false; doc_leaf: the presence leaf of the document whose key the array
 * of a jsonpath is, which says the rows with no document (a document without the key has no array either; -1: a NULL array is a NULL document).
 */
typedef struct SiftV6XTest
{
	int			need;
	bool		nullable;
	int			stream;			/* the array whose elements the test reads (sift_v6_ext_stream) */
	int			doc_leaf;
	int			ncmps;
	SiftV6XCmp	cmp[SIFT_V6_XCMPS];
} SiftV6XTest;

#define SIFT_V6_RESID_COLS	16

typedef struct SiftV6Ext
{
	int			ndims;
	SiftV6XDim	dim[SIFT_STAR_MAX_DIMS];
	List	   *kexpr;			/* the probing sides of the keys that are expressions (lower(i.category), o.k + i.n - 3), over the virtual attributes 1.. of rcol like resid: PostgreSQL's evaluator gives their vectors before the dimension is probed */
	List	   *jexpr;			/* the keys and arguments of the joined rows (coalesce(m.tag, '-'), s.kind || o.source), over the same attributes: PostgreSQL's evaluator gives their vectors after the
								 * last dimension, the columns of SIFT_V6_JOINED_DIM */
	List	   *resid;			/* the conditions of the join that no key says (an inequality, a WHERE over the columns of a left join), over the virtual attributes 1.. of rcol (v6_join_lookup.h); a pair that fails one leaves the join */
	List	   *on[SIFT_STAR_MAX_DIMS];	/* the conditions of the ON clause of dimension d that no key says, over the same virtual attributes: a pair that fails one is no match (a fact row of a left or full join is kept without the dimension row, a row of a right or full join's dimension stays unmatched) */
	int			nrcol;
	SiftV6Col	rcol[SIFT_V6_RESID_COLS];
	bool		unnest;
	bool		row_tests;		/* the statement has no lateral relation: each row is one pair, and its tests are its flags */
	SiftV6XElem elem;			/* stream 0: the array the lateral relation takes apart, or the first one the tests read */
	int			nstreams;		/* the other arrays the tests read, streams[0 .. nstreams): the scan reads elements of 1 + nstreams arrays */
	SiftV6XElem streams[SIFT_V6_XSTREAMS - 1];
	List	   *where;			/* unnest: the quals of the fact table over INDEX_VAR attributes, which the scan evaluates on its batches (it has no index keys) */
	List	   *elem_where;		/* unnest: the quals of the element in the order the executor of the statement runs them (agg/unnest/test.h): a comparison the scan decides, over the virtual attributes of its
								 * columns (v6_unnest_jsonb.h), or a clause PostgreSQL evaluates as (the clause over the inputs of SIFT_V6_PGX_RELID, the slot and type of each input) */
	int			ntests;			/* tests of the row's elements: the scan gives each row once, with a flag for each test (the attribute SIFT_V6_XFLAG_VATT + i of a FILTER) */
	SiftV6XTest test[SIFT_V6_XTESTS];
	List	   *test_quals;		/* of each test: its parts over the element columns, the expression a row the leaves cannot hold is decided by (a jsonpath; NIL for an EXISTS, decided by the function),
								 * and its disjunctions: of each, the arms over the element columns that are no comparison with the row (SiftV6XCmp.group) */
	List	   *elem_names;		/* recordset: the name of each column, as String nodes */
	List	   *elem_array;		/* unnest: the array of a row as an expression of the table's columns when it is not the column of elem.attno itself (doc -> 'key', or an expression of the index), which the rows read from the heap evaluate; NIL otherwise */
	List	   *stream_names;	/* of each stream after the first: the elem_names of its array */
	List	   *stream_arrays;	/* of each stream after the first: its array as an expression of the table's columns, which the rows read from the heap evaluate */
	List	   *heap_where;		/* unnest: the quals of the fact table as the statement has them, which a row read from the heap is tested with (the batches of the index decide the others) */
	List	   *late_probe;		/* unnest: the keys and aggregates that read a column of the element computed late, for a member whose cast raised (agg/unnest/late.h) */
} SiftV6Ext;

extern SiftV6Ext *sift_v6_ext_new(void);

/* The array s of the elements the scan reads, with the names of its recordset columns and, when it is no column of the table itself, its expression (NULL: the column of attno) */
static inline const SiftV6XElem *
sift_v6_ext_stream(const SiftV6Ext *x, int s)
{
	return s == 0 ? &x->elem : &x->streams[s - 1];
}

static inline const List *
sift_v6_ext_stream_names(const SiftV6Ext *x, int s)
{
	return s == 0 ? x->elem_names : list_nth(x->stream_names, s - 1);
}

static inline const Expr *
sift_v6_ext_stream_array(const SiftV6Ext *x, int s)
{
	return s == 0 ? (x->elem_array != NIL ? linitial(x->elem_array) : NULL) : list_nth(x->stream_arrays, s - 1);
}
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
