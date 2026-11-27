/*
 * agg/unnest/jsonb.h - CROSS JOIN LATERAL jsonb_to_recordset(col) AS alias(c1 t1, ..) over a jsonb column of the fact table whose array of objects the index keeps shredded (a group leaf for its
 * lengths and a leaf for each key of its objects), as the aggregate scan of an unnest (agg/unnest/plan.c, v6_unnest_run.c) takes it, and the quals of the element: the planning of the lateral
 * relation into a SiftV6XElem (an element column for each declared column), the WHERE clauses on the columns of the element, and the evaluation PostgreSQL's own jsonb_to_recordset gives of a row whose
 * document the leaves cannot hold (a document that is not an array of objects, or a value of another type than the column's), so that such a row answers as the statement says, errors included.
 */
#ifndef SIFT_AGG_V6_UNNEST_JSONB_H
#define SIFT_AGG_V6_UNNEST_JSONB_H

#include "postgres.h"

#include "fmgr.h"
#include "nodes/execnodes.h"
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
/* The Ext of the unnest, p->xseed: the leaves of the column of the index or the table the function takes apart, false when an index does not keep all of them as columns */
extern bool sift_v6_unnest_seed(V6Plan *p, RelOptInfo *elem_rel, const SiftV6ElemRte *rte, bool left);
/* The element stream e of the array of rte (its leaves, or the columns the function of a table gives) and the names of its recordset columns; false when the source reads no such array */
extern bool sift_v6_unnest_elem_seed(V6Plan *p, const SiftV6ElemRte *rte, bool left, SiftV6XElem *e, List **names);
/* The WHERE clauses of the lateral relation as the elem_where of p->xseed; false when the scan cannot decide one */
extern bool sift_v6_unnest_elem_quals(V6Plan *p);
/* A comparison, an IN list or a null test of a column of the element with constants, over the virtual attribute of the column; NULL when the clause is not one */
extern Expr *sift_v6_unnest_elem_qual(const V6Plan *p, Node *clause);
/* The column of the statement a virtual attribute of the element is: the vector the pairs of the unnest bring of it */
extern SiftV6Col sift_v6_elem_vatt_col(int attno, Oid typid);

/* The leaf holds the values of a vector of type typid (text, an integer, a boolean or a float) as they are stored */
extern bool sift_v6_leaf_holds(Oid typid, int32 typmod, const SiftColDef *d);
/* The group leaf of the list of attribute att (the document itself, or its key `key`): its lengths, whose children are its elements or the keys of its objects (-1: none) */
extern int	sift_v6_unnest_group_of(const SiftV6Leaves *r, int att, const char *key);

/*
 * agg/unnest/array_elem.c, SV6E_ELEMS_TEXT: jsonb_array_elements_text(doc) or (doc -> 'key') over a list of strings the index keeps shredded (`doc.key[] text`: a group leaf of its lengths and a
 * text leaf of its elements). rte: the function of a lateral relation. seed: the group and the element leaf of attribute att, the one column `value`. fn_oid: the PostgreSQL function the rows the
 * leaves cannot hold (a document that is no list of strings) are read with.
 */
extern bool sift_v6_elems_rte(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out);
extern bool sift_v6_elems_seed(const SiftV6Leaves *r, SiftV6XElem *e, const char *key, int att, List **names);
/* the text leaf of the elements of group (a list of scalars: the child the list itself names; an array of objects has one child per key instead), -1: none */
extern int	sift_v6_elems_leaf(const SiftV6Leaves *r, int att, int group);
extern Oid	sift_v6_elem_fn_oid(SiftV6ElemFn fn);

/*
 * agg/unnest/members.c, SV6E_MEMBERS: jsonb_array_elements(doc) or (doc -> 'key') AS e over a list of objects the index keeps shredded, read by `e ->> 'k'` and `(e ->> 'k')::type`. rte: the function
 * of a lateral relation. seed: the group of attribute att, with no column yet. column: e is such a read, as the column of the element the leaf of its member is (added at its first read). late: column
 * slot of the element is computed after the quals of the element (the cast of a member, the value of jsonb_each), which test it among the clauses of the pairs only. member_text: elem ->> name.
 */
extern bool sift_v6_members_rte(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out);
extern bool sift_v6_members_seed(const SiftV6Leaves *r, SiftV6XElem *e, const char *key, int att);
extern bool sift_v6_members_column(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_elem_late(const SiftV6XElem *e, int slot);
extern Datum sift_v6_member_text(Datum elem, text *name, bool *isnull);
/* agg/unnest/table_elems.c: the elements of rte over a column of the sift_table of p that no declared leaves hold, which the scan takes apart from each row; elem_type: a column of them may have the type t */
extern bool sift_v6_table_elems_seed(const V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, List **names);
extern bool sift_v6_table_elem_type(Oid t);
/* the leaf of the key `name` of the objects of the group of attribute att (-1: none) */
extern int	sift_v6_unnest_member_leaf(const SiftV6Leaves *r, int att, int group, const char *name);

/* the column of the elements of rte that is the member `name` of type `type` (its kind when kind): its place, added when the tests have none yet; -1 when there is no room */
extern int	sift_v6_unnest_rte_col(SiftV6ElemRte *rte, const char *name, Oid type, bool kind);
/* the array of a row is `array` (a column of the table, an expression of it the index holds as a column, or the key of such a document): its place in rte */
extern void sift_v6_unnest_array_rte(const Expr *array, SiftV6ElemRte *out);
/*
 * the type of the member `name` of the objects of the array rte names (NULL: of the elements of a list of scalars), as the leaf of the index holds it (text, an integer or a boolean) and whether
 * it is an astext leaf; InvalidOid when the index has no such leaf or holds it otherwise
 */
extern Oid	sift_v6_unnest_member_type(const V6Plan *p, const SiftV6ElemRte *rte, const char *name, bool *astext);
/*
 * The leaf that says which rows have no document when the array of rte is a key of it (a jsonpath of such a row is NULL, of a document without the key false): the presence leaf of the column,
 * -1 when the array is the column itself; false when the index has no such leaf.
 */
extern bool sift_v6_unnest_doc_leaf(const V6Plan *p, const SiftV6ElemRte *rte, int *leaf);

/* doc ->> 'key' = 'constant' of a shredded text key of a jsonb column of the fact table, as a comparison of the virtual attribute of the key's leaf (agg/qual/jsonb.c); NULL when it is not one */
extern Expr *sift_v6_json_text_qual(const SiftV6Rel *rel, const OpExpr *op);
/* no number, boolean or document has s as its `->>` text */
extern bool sift_v6_json_text_unambiguous(const char *s);

/* The clause of the quals of the element, and for each vector it reads the column of the element it is */
extern bool sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec);

/* The evaluation of jsonb_to_recordset on the document of a row, for the rows the leaves cannot hold */
typedef struct SiftV6RecFn
{
	FmgrInfo	flinfo;
	ReturnSetInfo rsinfo;
	ExprContext *econtext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	int			ncols;
	Oid			type[SIFT_V6_XELEM_COLS];
	SiftV6ElemFn fn;
	SiftV6ValueRead vread;		/* jsonb_each, jsonb_each_text: the members come with the value as the function gives it, which values() reads as the statement does */
	FmgrInfo	vfn;			/* the cast of jsonb_each's jsonb to the type of the value, or its #>> '{}' */
	FmgrInfo	vin[SIFT_V6_XELEM_COLS];	/* the input function of the type of a column computed late: the value of jsonb_each, the cast of a member of SV6E_MEMBERS */
	Oid			vio[SIFT_V6_XELEM_COLS];
	text	   *member[SIFT_V6_XELEM_COLS];	/* SV6E_MEMBERS: the key each column reads of an element */
	Datum	   *val;			/* the value of jsonb_each, the element of SV6E_MEMBERS */
	bool	   *val_null;
	Datum	   *key;			/* the key of jsonb_each */
} SiftV6RecFn;

extern SiftV6RecFn *sift_v6_recfn_new(MemoryContext cx, const SiftV6XElem *e, const List *names);
/* The elements of doc, as the vectors ev[0..ncols) (data, valid) allocated in cx, which also holds what the function leaves of the call; returns how many */
extern uint32 sift_v6_recfn_call(SiftV6RecFn *f, Datum doc, MemoryContext cx, SiftVec *ev);
/* The same a chunk at a time: open calls the function on doc, its rows in a tuplestore of cx (spilled past work_mem); next gives the following max of them as ev in cx, fewer at the end */
extern void sift_v6_recfn_open(SiftV6RecFn *f, Datum doc, MemoryContext cx);
extern uint32 sift_v6_recfn_next(SiftV6RecFn *f, MemoryContext cx, SiftVec *ev, uint32 max);
/*
 * jsonb_each and jsonb_each_text: the value column of the n members of the last call or chunk, for the members of keep (NULL: all), which are those that reach the statement's expressions. The cast of a
 * member that raises is the error of the pair of that member, which a join may drop: it is not raised here but left in late[k] (n entries, zeroed by the caller; the member has no value), for the
 * caller to raise when the pair reaches an expression of the statement that reads the value (agg/unnest/late.h). Any other error (a cancel, out of memory) is raised.
 */
extern void sift_v6_recfn_values(SiftV6RecFn *f, const uint64 *keep, MemoryContext cx, SiftVec *ev, uint32 n, ErrorData **late);
extern void sift_v6_recfn_free(SiftV6RecFn *f);

#endif							/* SIFT_AGG_V6_UNNEST_JSONB_H */
