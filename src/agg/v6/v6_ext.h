/*
 * agg/v6/v6_ext.h - what the aggregate scan of SiftAggV6 reads beyond the columns of one table: the small relations of a star join (up to SIFT_STAR_MAX_DIMS dimensions, each an inner or left
 * equi join of the fact table, read whole from the plan of the dimension into the tables of agg/v6/v6_star.h) and the array an unnest of the index takes apart (the elements of the array
 * column's leaf, without the rows being built). The planner makes a SiftV6Ext, plan.c stores it with the spec (v6_spec_pack.c), and exec.c hands it to the scan.
 */
#ifndef SIFT_AGG_V6_EXT_H
#define SIFT_AGG_V6_EXT_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_star.h"

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

/* The array of an unnest: the attribute of the index, its element type, and the leaves of its lengths and elements */
typedef struct SiftV6XElem
{
	int			attno;
	Oid			type;
	int			group_leaf;
	int			elem_leaf;
	bool		left;			/* LEFT JOIN LATERAL ... ON true: a row with no element is kept with a NULL one */
} SiftV6XElem;

typedef struct SiftV6Ext
{
	int			ndims;
	SiftV6XDim	dim[SIFT_STAR_MAX_DIMS];
	bool		unnest;
	SiftV6XElem elem;
	List	   *where;			/* unnest: the quals of the fact table over INDEX_VAR attributes, which the scan evaluates on its batches (it has no index keys) */
} SiftV6Ext;

extern SiftV6Ext *sift_v6_ext_new(void);
/* The Ext of a spec as a list: [ints, where]; NIL for none */
extern List *sift_v6_ext_pack(const SiftV6Ext *x);
extern SiftV6Ext *sift_v6_ext_unpack(const List *l);
/* The positions of the key and payload columns of every dimension in the output of its plan (custom_plans of the node), from the attributes the planner chose */
extern void sift_v6_ext_resolve(SiftV6Ext *x, const List *custom_plans);

#endif							/* SIFT_AGG_V6_EXT_H */
