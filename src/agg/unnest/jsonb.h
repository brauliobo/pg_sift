/*
 * agg/unnest/jsonb.h - CROSS JOIN LATERAL jsonb_to_recordset(col) AS alias(c1 t1, ..) over a jsonb column of the fact table whose array of objects the index keeps shredded (a group leaf for its
 * lengths and a leaf for each key of its objects), as the aggregate scan of an unnest (agg/unnest/plan.c, v6_unnest_run.c) takes it, and the quals of the element: the planning of the lateral
 * relation into a SiftV6XElem (an element column for each declared column), the WHERE clauses on the columns of the element, and the evaluation PostgreSQL's own jsonb_to_recordset gives of a row whose
 * document the leaves cannot hold (a document that is not an array of objects, or a value of another type than the column's), so that such a row answers as the statement says, errors included.
 */
#ifndef SIFT_AGG_V6_UNNEST_JSONB_H
#define SIFT_AGG_V6_UNNEST_JSONB_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/plan/int.h"
#include "agg/ext.h"
#include "agg/filter/pg.h"

/* The virtual attribute of the quals the scan evaluates on its batches (an INDEX_VAR varattno above every column of an index): column c of the element (the leaf of a jsonb key of the fact table is SIFT_V6_JTEXT_VATT, agg/engine.h) */
#define SIFT_V6_ELEM_VATT	(1 << 13)

/* The lateral function scan of the statement: its function, the column of the fact table it takes, and what it declares */
typedef struct SiftV6ElemRte
{
	SiftV6ElemFn fn;
	const Expr *array;			/* the array of a row: a column of the fact table, an expression of it that the index holds as a column, or the key of such a document (base -> 'key') */
	const Expr *base;			/* the column or index expression that holds the array, array itself or the document it is a key of */
	const char *key;			/* the key of the document base that holds the array; NULL when base is the array */
	Oid			elem;			/* unnest: the type of the element */
	List	   *names;			/* jsonb_to_recordset: the String of each declared column; SV6E_PATH: of each member the tests read */
	List	   *types;			/* and its type, as Oid cells */
	List	   *kinds;			/* SV6E_PATH: whether the column is the kind of the member (SIFT_ELEM_KIND_*) and not its value, as int cells */
} SiftV6ElemRte;

/* elem_rel is the relation of unnest(col) or jsonb_to_recordset(col) of a column of fact; false for any other relation, or a function this scan cannot take apart */
extern bool sift_v6_unnest_rte(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *elem_rel, SiftV6ElemRte *out);
/* the same for a relation that other relations join (a lookup keyed by its columns): the join clauses are the caller's to read */
extern bool sift_v6_unnest_rte_joined(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *elem_rel, SiftV6ElemRte *out);
/* the function of a lateral function scan, over the column v: unnest of an array, or jsonb_to_recordset of a jsonb one with the columns it declares */
extern bool sift_v6_unnest_rte_function(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out);
/* e is date_trunc('month', c)::date of a date column c of the fact table in a statement that unnests: k is c */
extern bool sift_v6_unnest_month_key(V6Plan *p, const Expr *e, SiftV6Col *k);
/* rte is the function scan of the sub-select of the EXISTS sp, which takes an array of the fact table (an expression of the parameters sp passes); false for any other. out->array is that array in the terms of the fact table */
extern bool sift_v6_unnest_exists_rte(const RangeTblEntry *rte, const SubPlan *sp, SiftV6ElemRte *out);
/* e reads the fact table alone: one of its columns or an expression of them */
extern bool sift_v6_unnest_array_ok(PlannerInfo *root, const RelOptInfo *fact, const Expr *e);
/* The Ext of the unnest, p->xseed: the leaves of the column of the index the function takes apart, false when the index does not keep all of them as columns */
extern bool sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left);
/* The WHERE clauses of the lateral relation as the elem_where of p->xseed; false when the scan cannot decide one */
extern bool sift_v6_unnest_elem_quals(V6Plan *p);
/* A comparison, an IN list or a null test of a column of the element with constants, over the virtual attribute of the column; NULL when the clause is not one */
extern Expr *sift_v6_unnest_elem_qual(const V6Plan *p, Node *clause);
/* The column of the statement a virtual attribute of the element is: the vector the pairs of the unnest bring of it */
extern SiftV6Col sift_v6_elem_vatt_col(int attno, Oid typid);

/* The leaf holds the values of a vector of type typid (text, an integer, a boolean or a float) as they are stored */
extern bool sift_v6_leaf_holds(Oid typid, int32 typmod, const SiftColDef *d);
/* The group leaf of the list of attribute att (the document itself, or its key `key`): its lengths, whose children are its elements or the keys of its objects (-1: none) */
extern int	sift_v6_unnest_group_of(const SiftColRel *r, int att, const char *key);

/*
 * agg/unnest/array_elem.c, SV6E_ELEMS_TEXT: jsonb_array_elements_text(doc) or (doc -> 'key') over a list of strings the index keeps shredded (`doc.key[] text`: a group leaf of its lengths and a
 * text leaf of its elements). rte: the function of a lateral relation. seed: the group and the element leaf of attribute att, the one column `value`. fn_oid: the PostgreSQL function the rows the
 * leaves cannot hold (a document that is no list of strings) are read with.
 */
extern bool sift_v6_elems_rte(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out);
extern bool sift_v6_elems_seed(const SiftColRel *r, SiftV6XElem *e, const char *key, int att, List **names);
extern Oid	sift_v6_elem_fn_oid(SiftV6ElemFn fn);

/* The leaves of the map `col.* type` of attribute att: its lengths, the keys of its members and their values; false when att holds no such map, or the index keeps the members in no leaf a column can read */
extern bool sift_v6_map_leaves(const SiftColRel *r, int att, int *group, int *key, int *value);

/*
 * agg/mapkey.c, SV6E_KEYS: the members of a map the statement reads by a constant key, with no lateral relation. paths: the Ext of p->xseed is made when the statement reads one, and the path added when
 * the statement compiles against it (true: the statement was one of them, whether a path was added or not). column: e is such a read, as the column of the scan that gives its member. on: p->xseed is one.
 */
extern bool sift_v6_mapkey_paths(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);
extern bool sift_v6_mapkey_column(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_mapkey_on(const V6Plan *p);
/* the WHERE clause ri over the members the statement reads, as a qual the scan evaluates on its rows (xseed->pair_where); false when it is not one */
extern bool sift_v6_mapkey_pair_qual(V6Plan *p, RestrictInfo *ri);
/* the column of the elements of rte that is the member `name` of type `type` (its kind when kind): its place, added when the tests have none yet; -1 when there is no room */
extern int	sift_v6_unnest_rte_col(SiftV6ElemRte *rte, const char *name, Oid type, bool kind);
/* the array of a row is `array` (a column of the table, an expression of it the index holds as a column, or the key of such a document): its place in rte */
extern void sift_v6_unnest_array_rte(const Expr *array, SiftV6ElemRte *out);
/* the type of the member `name` of the objects of the array rte names, as the leaf of the index holds it (text, an integer or a boolean); InvalidOid when the index has no such leaf or holds it otherwise */
extern Oid	sift_v6_unnest_member_type(const V6Plan *p, const SiftV6ElemRte *rte, const char *name);

/* doc ->> 'key' = 'constant' of a shredded text key of a jsonb column of the fact table, as a comparison of the virtual attribute of the key's leaf (agg/qual/jsonb.c); NULL when it is not one */
extern Expr *sift_v6_json_text_qual(const SiftV6Rel *rel, const OpExpr *op);

/* The clause of the quals of the element, and for each vector it reads the column of the element it is */
extern bool sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec);

/* The evaluation of jsonb_to_recordset on the document of a row, for the rows the leaves cannot hold */
typedef struct SiftV6RecFn SiftV6RecFn;

extern SiftV6RecFn *sift_v6_recfn_new(MemoryContext cx, const SiftV6Ext *x);
/* The elements of doc, as the vectors ev[0..ncols) (data, valid) allocated in cx, which also holds what the function leaves of the call; returns how many */
extern uint32 sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev);
/*
 * jsonb_each and jsonb_each_text: the value column of the n members of the last call, for the members of keep (NULL: all), which are those that reach the statement's expressions. The cast of a
 * member that raises is the error of the pair of that member, which a join may drop: it is not raised here but left in late[k] (n entries, zeroed by the caller; the member has no value), for the
 * caller to raise when the pair reaches the groups.
 */
extern void sift_v6_recfn_values(SiftV6RecFn *f, const uint64 *keep, MemoryContext cx, SiftVec *ev, uint32 n, ErrorData **late);
extern void sift_v6_recfn_free(SiftV6RecFn *f);

#endif							/* SIFT_AGG_V6_UNNEST_JSONB_H */
