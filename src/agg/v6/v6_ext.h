/*
 * agg/v6/v6_ext.h - what the aggregate scan of SiftAggV6 reads beyond the columns of one table: the small relations of a star join (up to SIFT_STAR_MAX_DIMS dimensions, each an inner or left
 * equi join of the fact table, read whole from the plan of the dimension into the tables of agg/v6/v6_star.h) and the array an unnest of the index takes apart (the elements of the array
 * column's leaves, without the rows being built). The planner makes a SiftV6Ext, plan.c stores it with the spec (v6_spec_pack.c), and exec.c hands it to the scan.
 */
#ifndef SIFT_AGG_V6_EXT_H
#define SIFT_AGG_V6_EXT_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_star.h"

#define SIFT_V6_XELEM_COLS	16		/* the columns of an element: at most SIFT_F2_PULL_MAX_CHILD */

/* A dimension of a star join: the fact attributes and the columns of the plan of the dimension its keys and payload are */
typedef struct SiftV6XDim
{
	SiftV6JoinMode mode;		/* SV6J_INNER_EQUI or SV6J_LEFT_EQUI */
	int			nkeys;
	int			fatt[SIFT_STAR_MAX_KEYS];	/* the attribute of the index each key is of the fact table */
	Oid			ftype[SIFT_STAR_MAX_KEYS];
	Oid			ktype[SIFT_STAR_MAX_KEYS];	/* the type of the key column of the dimension */
	AttrNumber	kcol[SIFT_STAR_MAX_KEYS];	/* the column of the dimension: its attribute while planned, then its position in the tuples of its plan */
	int			npay;
	AttrNumber	pcol[SIFT_STAR_MAX_PAY];
	Oid			ptype[SIFT_STAR_MAX_PAY];
} SiftV6XDim;

/* The function of the lateral relation: unnest of an array column, or jsonb_to_recordset of a jsonb one */
typedef enum SiftV6ElemFn
{
	SV6E_UNNEST = 0,
	SV6E_RECORDSET
} SiftV6ElemFn;

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
	bool		left;			/* LEFT JOIN LATERAL ... ON true: a row with no element is kept with a NULL one */
	bool		semi;			/* EXISTS over the elements: a row once, when an element passes elem_where, and no element */
	int			ncols;
	int			col_leaf[SIFT_V6_XELEM_COLS];
	Oid			col_type[SIFT_V6_XELEM_COLS];
} SiftV6XElem;

typedef struct SiftV6Ext
{
	int			ndims;
	SiftV6XDim	dim[SIFT_STAR_MAX_DIMS];
	bool		unnest;
	SiftV6XElem elem;
	List	   *where;			/* unnest: the quals of the fact table over INDEX_VAR attributes, which the scan evaluates on its batches (it has no index keys) */
	List	   *elem_where;		/* unnest: the quals of the element over the virtual attributes of its columns (v6_unnest_jsonb.h), which the scan evaluates on the elements of a batch */
	List	   *elem_names;		/* recordset: the name of each column, as String nodes */
	List	   *heap_where;		/* unnest: the quals of the fact table as the statement has them, which a row read from the heap is tested with (the batches of the index decide the others) */
} SiftV6Ext;

extern SiftV6Ext *sift_v6_ext_new(void);
/* The Ext of a spec as a list: [ints, where, elem_where, elem_names, heap_where]; NIL for none */
extern List *sift_v6_ext_pack(const SiftV6Ext *x);
extern SiftV6Ext *sift_v6_ext_unpack(const List *l);
/* The positions of the key and payload columns of every dimension in the output of its plan (custom_plans of the node), from the attributes the planner chose */
extern void sift_v6_ext_resolve(SiftV6Ext *x, const List *custom_plans);

#endif							/* SIFT_AGG_V6_EXT_H */
