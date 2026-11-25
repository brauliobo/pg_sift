/*
 * agg/v6/v6_unnest_jsonb.h - CROSS JOIN LATERAL jsonb_to_recordset(col) AS alias(c1 t1, ..) over a jsonb column of the fact table whose array of objects the index keeps shredded (a group leaf for its
 * lengths and a leaf for each key of its objects), as the aggregate scan of an unnest (agg/v6/v6_unnest_plan.c, v6_unnest_run.c) takes it, and the quals of the element: the planning of the lateral
 * relation into a SiftV6XElem (an element column for each declared column), the WHERE clauses on the columns of the element, and the evaluation PostgreSQL's own jsonb_to_recordset gives of a row whose
 * document the leaves cannot hold (a document that is not an array of objects, or a value of another type than the column's), so that such a row answers as the statement says, errors included.
 */
#ifndef SIFT_AGG_V6_UNNEST_JSONB_H
#define SIFT_AGG_V6_UNNEST_JSONB_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_filter_pg.h"

/* The virtual attributes of the quals the scan evaluates on its batches (an INDEX_VAR varattno above every column of an index): column c of the element, and leaf l of a jsonb key of the fact table */
#define SIFT_V6_ELEM_VATT	(1 << 13)
#define SIFT_V6_JLEAF_VATT	(1 << 14)

/* The lateral function scan of the statement: its function, the column of the fact table it takes, and what it declares */
typedef struct SiftV6ElemRte
{
	SiftV6ElemFn fn;
	const Var  *array;
	Oid			elem;			/* unnest: the type of the element */
	List	   *names;			/* jsonb_to_recordset: the String of each declared column */
	List	   *types;			/* and its type, as Oid cells */
} SiftV6ElemRte;

/* elem_rel is the relation of unnest(col) or jsonb_to_recordset(col) of a column of fact; false for any other relation, or a function this scan cannot take apart */
extern bool sift_v6_unnest_rte(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *elem_rel, SiftV6ElemRte *out);
/* the same for a relation that other relations join (a lookup keyed by its columns): the join clauses are the caller's to read */
extern bool sift_v6_unnest_rte_joined(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *elem_rel, SiftV6ElemRte *out);
/* e is date_trunc('month', c)::date of a date column c of the fact table in a statement that unnests: k is c */
extern bool sift_v6_unnest_month_key(V6Plan *p, const Expr *e, SiftV6Col *k);
/* The Ext of the unnest, p->xseed: the leaves of the column of the index the function takes apart, false when the index does not keep all of them as columns */
extern bool sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left);
/* The WHERE clauses of the lateral relation as the elem_where of p->xseed; false when the scan cannot decide one */
extern bool sift_v6_unnest_elem_quals(V6Plan *p);

/* doc ->> 'key' = 'constant' of a shredded text key of a jsonb column of the fact table, as a comparison of the virtual attribute of the key's leaf (agg/v6/jsonb_qual.c); NULL when it is not one */
extern Expr *sift_v6_json_text_qual(const SiftV6Rel *rel, const OpExpr *op);

/* The clause of the quals of the element, and for each vector it reads the column of the element it is */
extern bool sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec);

/* The evaluation of jsonb_to_recordset on the document of a row, for the rows the leaves cannot hold */
typedef struct SiftV6RecFn SiftV6RecFn;

extern SiftV6RecFn *sift_v6_recfn_new(MemoryContext cx, const SiftV6Ext *x);
/* The elements of doc, as the vectors ev[0..ncols) (data, valid) allocated in cx; returns how many */
extern uint32 sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev);

#endif							/* SIFT_AGG_V6_UNNEST_JSONB_H */
