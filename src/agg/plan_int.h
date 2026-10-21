/*
 * agg/plan_int.h - shared between the planner files (agg/plan.c, agg/plan_*.c).
 */
#ifndef SIFT_AGG_PLAN_INT_H
#define SIFT_AGG_PLAN_INT_H

#include "nodes/extensible.h"

#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/spec.h"

typedef enum FuncKind
{
	FK_NONE,
	FK_RECORDSET,				/* jsonb_to_recordset(array) AS t(f type, ...) */
	FK_ELEMENTS,				/* jsonb_array_elements(array) */
	FK_ELEMENTS_TEXT,			/* jsonb_array_elements_text(array of strings): one text column, the element */
	FK_EACH,					/* jsonb_each(object) */
	FK_EACH_TEXT				/* jsonb_each_text(object) */
} FuncKind;

/* a table joined by equality to leaves (SiftAggJoin) and the combos that evaluate expressions of leaves and its columns (SiftAggCombo) */
typedef struct JoinCtx
{
	Index		rti;
	SiftJoinKind kind;
	List	   *clauses;		/* the ON clause (all of the WHERE quals that read the table of an inner join) */
	SiftAggJoin j;
	Node	   *kexpr[SIFT_AGG_MAX_JKEYS];	/* the key expressions, in terms of the query's Vars */
	int			combo;
	Path	   *cpath;			/* a right side that is a plan (sub-select, VALUES, function): the path of its rows, NULL for a table */
	List	   *args;			/* parent expressions passed to a correlated native child */
	PlannerInfo *subroot;		/* the query level its native plan was made in */
	List	   *cvars;			/* its columns the spec reads (Var, one per column): what the path outputs */
} JoinCtx;

typedef struct SlotReq
{
	Node	   *expr;
	bool		is_key;
	int			key;
	int			agg;
	bool		direct;			/* the value itself (SiftAggSlot.direct) */
	bool		late;			/* a boolean slot whose expression may raise an error (SiftAggPred.late) */
	bool		lowered;		/* expr is over Params already (lower_scoped): finish_combos leaves it */
} SlotReq;

typedef struct ComboCtx
{
	int			join;			/* index in AggCtx.joins, -1 none */
	int			nl;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	int			refcombo[SIFT_AGG_MAX_CLEAVES];
	int			refcol[SIFT_AGG_MAX_CLEAVES];
	bool		typed[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	List	   *slots;			/* SlotReq */
} ComboCtx;

/* a parameter of the EXISTS sub-plan being compiled and the leaf of the outer row it stands for */
typedef struct OuterParam
{
	int			id;
	int			leaf;
} OuterParam;

typedef struct AggCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Index		relid;
	Oid			heapoid;
	Oid			indexoid;
	SiftSrcLayout *lay;
	int			nparts;			/* a partitioned table: its live partitions, which the scan reads one after the other (indexoid is the first's) */
	Oid		   *partheap;
	Oid		   *partindex;
	double		part_pages;		/* ... and the pages of their heaps */
	List	   *attexpr;		/* per attribute of the source: its expression with varno = relid (NULL: none) */
	List	   *attalias;		/* per attribute: another expression that denotes it (NULL: none or no list) */
	Index		func_rti;
	FuncKind	func_kind;
	RangeTblFunction *func;
	const char *func_key;		/* LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k': r.value is the member k of each row, no unnest */
	int			func_obj;		/* ... and the object leaf of doc */
	Bitmapset  *func_ovf;		/* ... and the leaves that make the scan unfit when a unit holds what they cannot store (a document that is no object raises in jsonb_each) */
	int			group;			/* leaf of the unnested array/object, -1 */
	Bitmapset  *used;			/* leaves the spec reads */
	Bitmapset  *read;			/* leaves whose values it reads, not only their JSON types */
	Bitmapset  *jcast;			/* leaves read through the cast function of jsonb to a number or boolean (before PostgreSQL 18 it raises for a JSON null) */
	List	   *dpctx;			/* deparse context of expr_text */
	bool		other_group;	/* a leaf of an array the query does not unnest is read */
	int			rec_pending;	/* 1 + the leaf func_var_leaf just resolved from a column of jsonb_to_recordset (0: none), which the use_leaf that follows takes */
	Bitmapset  *rec_ok;			/* leaves read as a column of jsonb_to_recordset ... */
	Bitmapset  *rec_bad;		/* ... and the leaves read in any other way */
	int			cast_pending;	/* 1 + the leaf match_leaf just resolved from the cast of a jsonb_each member's jsonb value to a number (0: none), which the use_leaf that follows takes */
	Bitmapset  *cast_ok;		/* leaves read through such a cast ... */
	Bitmapset  *cast_bad;		/* ... and the leaves read in any other way */
	int			npred;			/* ids handed out to predicate nodes */
	List	   *joins;			/* JoinCtx */
	Bitmapset  *nulling;		/* the outer joins whose nullable side is the scanned table: its Vars carry them in varnullingrels */
	List	   *combos;			/* ComboCtx */
	int			nvleaf;			/* SiftAggSpec.vleaf: leaves c->lay has after the source's */
	SiftAggVLeaf vleaf[SIFT_AGG_MAX_VLEAVES];
	List	   *rtable;			/* the range table expr_text resolves Vars in; NIL: the query's */
	Bitmapset  *elem_used;		/* leaves read inside an existential predicate (SAP_ANY_ELEM) */
	List	   *oparams;		/* OuterParam of the EXISTS sub-plan whose test is being compiled */
	double		elem_work;		/* element predicate evaluations per row of those, for the cost */
	SiftSrcStats stats;
	SiftSrcPreagg pa;			/* what the units of the index store besides their rows */
	SiftSearchPlan *search;		/* the BM25 conjuncts of the WHERE taken out: the scan reads the rows they match (agg/search.h) */
	double		topn_frac;		/* top-N: the fraction of the table the scan reads before the n-th best row cannot be beaten */
	int			nwide;			/* the conjuncts on wide leaves (SAP_WIDE) of the spec */
	int			nsubplan;		/* the hashed sub-plans its lookup tables evaluate: the leader runs them (a worker has no plan state to start them in) */
	Cost		subplan_cost;	/* their start-up: the sub-selects run once, as in the normal plan */
	bool		joinless;		/* the caller takes no join: the sub-selects are not planned */
	int			nsiblings;		/* the plans and the unnest joined to the scanned table: the order PostgreSQL runs them in is its own */
	List	   *planned;		/* the correlated children planned for the query (JoinCtx, by rti): the other scanned table of a flipped join reuses them */
} AggCtx;

/* Planner-only paths of correlated children are planned in their own query level. */
typedef struct SiftAggPath
{
	CustomPath path;
	List       *correlated;		/* the native path of each correlated child, NULL for the other joins */
	List       *subroots;		/* and the query level it belongs to */
} SiftAggPath;

typedef struct JsonRef
{
	int			att;			/* attribute holding the jsonb root; 0: a column of the unnested function */
	int			fcol;			/* that column (1-based) */
	List	   *path;
	bool		as_text;
	Oid			cast_to;
} JsonRef;

typedef struct OuterRef
{
	PlannerInfo *root;			/* the query the examined RTE belongs to */
	int			depth;			/* the query levels below that RTE the walk is in */
	int			skip;			/* the levels above the RTE's query whose references do not count: 0 counts the query's own */
} OuterRef;

typedef enum TypeClass
{
	TC_NONE,
	TC_INT,
	TC_DATE,
	TC_TS,
	TC_TSTZ,
	TC_FLOAT,
	TC_BOOL,
	TC_NUMERIC
} TypeClass;

typedef struct ComboWalk
{
	AggCtx	   *c;
	JoinCtx    *jc;
	List	   *vars;			/* Vars of the joined table */
	int			nl;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	bool		null_input;
	bool		bad;
	List	   *subparams;		/* the output Params of the hashed sub-plans met (as LeafWalk) */
} ComboWalk;

#define AGG_GEN_ROW			12.0	/* a transition function call through fmgr, and the Datum of a leaf (the function's own procost comes on top) */
#define AGG_GEN_DISTINCT	10.0	/* the (group, value) pair of a DISTINCT aggregate: a hash probe */
#define AGG_GEN_SORTED		30.0	/* a row of an aggregate with ORDER BY: the tuplesort put, and its share of the sort and the read back */
#define AGG_GEN_FIRST		1.0		/* a row of a DISTINCT ON column: the compare of its sort keys with the best row of the group, the value read when it wins */
#define AGG_GEN_FIRST_KEY	1.2		/* ... and each sort key's compare */
#define AGG_GEN_FOLLOW		0.3		/* a row of the other DISTINCT ON columns: the answer of the first one */
#define AGG_GEN_HYP			200.0	/* a row of a hypothetical-set aggregate: sorted by the scan for its groups, then again by the aggregate (no gain over the normal plan) */
#define AGG_GEN_ORDERED		24.0	/* a value of an ordered-set aggregate: the tuplesort put */
#define AGG_GEN_GROUP		150.0	/* a group's final function and state, or the sort of an ordered-set group */

typedef struct LeftJoin
{
	Index		rti;			/* the right side, a table */
	List	   *quals;			/* its ON clause */
	SiftJoinKind kind;
} LeftJoin;

typedef struct FromScan
{
	List	   *rtis;			/* range table indexes, in FROM order */
	List	   *quals;
	List	   *scan_quals;		/* conditions inside the nullable scanned input, before preservation */
	List	   *lefts;			/* LeftJoin */
	bool		ok;
	bool		flip;			/* the scanned table is the right side of a LEFT JOIN (its other side is preserved) */
	bool		flippable;		/* a LEFT or FULL JOIN of two tables was met: the scan may be the other one */
	Bitmapset  *ojrels;			/* the outer joins that preserve the other side */
} FromScan;

/* Aggrefs of the output, each once; false if a column is used outside the keys and aggregates */
typedef struct OutWalk
{
	List	   *keys;			/* group expressions */
	List	   *aggs;
	bool		bad;
} OutWalk;

#define AGG_PREAGG_EXACT	0.98	/* the share of exact units from which the metadata answers the query: no workers, they cost more than the answer */

extern const CustomPathMethods agg_path_methods;

extern SiftSrcLeaf *leaf_of(AggCtx *c, int id);
extern int leaf_level(AggCtx *c, int id);
extern const char *leaf_name(AggCtx *c, int id);
extern bool null_input_param(Node *n);
extern Node *null_guarded_arm(Node *n);
extern bool has_exec_param_walker(Node *node, void *context);
extern bool pseudo_const(Node *n);
#define AGG_JOIN_ROW		4.0		/* a row of a joined table, read when the scan starts */
#define AGG_IDX_ROW			12.0	/* a row of the joined table read through an index: the btree descent and the heap fetch */

extern Node *strip_relabel(Node *n);
extern Oid func_of(Node *n);
extern List *func_args(Node *n);
extern bool paths_equal(List *a, List *b);
extern int attr_of_expr(AggCtx *c, Node *e);
extern int child_leaf(AggCtx *c, int group, const char *name, Oid typid);
extern bool path_keys(Node *n, List **keys);
extern bool json_access(AggCtx *c, Node *e, JsonRef *r);
extern Oid json_cast_target(Oid fid);
extern int match_leaf(AggCtx *c, Node *e);
extern Node *normalize_expr(AggCtx *c, Node *n);

extern bool outer_ref_walker(Node *n, OuterRef *o);
extern bool cte_owner_correlated(PlannerInfo *owner);
extern bool lut_subplan_ok(AggCtx *c, SubPlan *sp);
extern bool array_initplan_ok(AggCtx *c, Param *prm);
extern bool single_leaf(AggCtx *c, Node *e, int *leaf, Oid *ptype, int32 *ptypmod, Oid *pcoll);
extern void hash_param_saop(ScalarArrayOpExpr *sa);
extern Node *substitute_leaf(AggCtx *c, Node *e, Oid ptype, int32 ptypmod, Oid pcoll);
extern bool lut_leaf_ok(AggCtx *c, int leaf);
extern bool combo_leaf_ok(AggCtx *c, int leaf);
extern bool raising_walker(Node *n, void *context);
extern bool leaf_stats_range(AggCtx *c, int leaf, int64 *lo, int64 *hi);
extern bool lut_expr_ok(AggCtx *c, int leaf, Node *expr);
extern double key_groups(AggCtx *c, const SiftAggKey *k);
extern const char *bucket_text(int secs);
extern int bucket_key_secs(AggCtx *c, int leaf, Node *expr);
extern int leaf_digits(Oid typid, int32 typmod);
extern bool nx_addsub(SiftAggNum *o, SiftAggNxOp op, int *scale, int *dig, int s2, int d2);
extern bool nx_walk(AggCtx *c, Node *n, SiftAggNum *o, int *scale, int *dig);
extern bool compile_nx(AggCtx *c, Node *e, int maxdig, SiftAggNum *o);
extern int nx_level(AggCtx *c, const SiftAggNum *o);
extern bool nx_raises(const SiftAggNum *o);

extern TypeClass type_class(Oid t);
extern int builtin_cmp(Oid opno, Oid *lt, Oid *rt);
extern int commute(int op);
extern SiftAggPred *new_pred(AggCtx *c, SiftAggPredKind kind);
extern char *expr_text(AggCtx *c, Node *n);
extern const char *cmp_text(int op);
extern void use_leaf(AggCtx *c, int leaf);
extern void use_matched_leaf(AggCtx *c, int leaf);
extern SiftAggPred *make_leaf_pred(AggCtx *c, SiftAggPredKind kind, int leaf);
extern SiftAggPred *new_junction(AggCtx *c, SiftAggPredKind kind, int nchild);
extern void junction_done(SiftAggPred *p);
extern SiftAggPred *compile_junction(AggCtx *c, SiftAggPredKind kind, List *args);
extern SiftAggPred *compile_pred(AggCtx *c, Node *n);

extern int typeof_leaf(AggCtx *c, Node *e);
extern bool source_can_types(AggCtx *c);
extern SiftAggPred *compile_typeof(AggCtx *c, Node *n);
extern SiftAggPred *compile_wide(AggCtx *c, Node *n);
extern SiftAggPred *compile_tid(AggCtx *c, Node *n);
extern bool types_guard_reads(AggCtx *c);
extern void typeof_reads(AggCtx *c, SiftAggSpec *s);

extern JoinCtx *join_of_var(AggCtx *c, Node *n);
extern int combo_leaf(ComboCtx *cb, int id, bool typed, Oid ptype, int32 ptypmod, Oid pcoll);
extern ComboCtx *new_combo(AggCtx *c, int join);
extern int combo_index(AggCtx *c, ComboCtx *cb);
extern int combo_reference(AggCtx *c, ComboCtx *cb, JoinCtx *jc, Var *v);
extern bool join_clause(AggCtx *c, JoinCtx *jc, Node *q);
extern bool combo_walker(Node *n, ComboWalk *w);
extern bool join_expr_clause(AggCtx *c, JoinCtx *jc, Node *q);
extern bool combo_slot_in(AggCtx *c, JoinCtx *force, Node *expr, bool is_key, bool after_joins, int *ci, int *si);
extern bool combo_slot(AggCtx *c, Node *expr, bool is_key, int *ci, int *si);
extern Node *lower_scoped(AggCtx *c, int ci, Node *expr);
extern void finish_combos(AggCtx *c, SiftAggSpec *s);
extern char *combo_text(AggCtx *c, int ci, Node *expr);
extern SiftAggPred *compile_combo_pred_at(AggCtx *c, Node *n, bool after_joins);
extern SiftAggPred *compile_combo_pred(AggCtx *c, Node *n);

extern SlotReq *slot_req(AggCtx *c, int combo, int slot);
extern void combo_set_key(AggCtx *c, SiftAggKey *k, int key);
extern bool compile_key(AggCtx *c, SortGroupClause *sgc, Node *expr, SiftAggKey *k);
extern bool unique_row_column(AggCtx *c, Node *arg);
extern void share_ordered(SiftAggSpec *s);
extern bool compile_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai);

extern bool arg_joined(AggCtx *c, Node *arg);
extern bool compile_gen(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai);

extern void scan_from(Node *n, FromScan *f);
extern FuncKind func_kind_of(RangeTblFunction *f);
extern SiftSrcLeafKind func_leaf_kind(FuncKind k);
extern int group_leaf_below(AggCtx *c, Node *arg, List *keys, SiftSrcLeafKind kind);
extern int group_leaf_of(AggCtx *c, Node *arg, SiftSrcLeafKind kind);
extern bool find_source(AggCtx *c, IndexOptInfo **ixout);
extern void build_attexprs(AggCtx *c);
extern void finish_leaves(AggCtx *c, SiftAggSpec *s);
extern SiftSrcRecheck spec_recheck(AggCtx *c, const SiftAggSpec *s, int m);

extern SiftAggPred *compile_existential(AggCtx *c, Node *n, Node **target);
extern SiftAggPred *compile_exists_post(AggCtx *c, SubPlan *sp);
extern bool scoped_child(AggCtx *c, Query *sub, Index offset, JoinCtx *jc, List **args);
extern bool target_not_null(AggCtx *c, Node *target);
extern void record_deps(PlannerInfo *root, Node *n);
extern void pred_deps(PlannerInfo *root, SiftAggPred *p);
extern bool out_walker(Node *n, OutWalk *w);

extern double elems_of(AggCtx *c, int grp);
extern double preagg_share(AggCtx *c, SiftAggSpec *s);
extern bool extern_param_walker(Node *node, void *context);
extern double rel_pages(AggCtx *c);
extern void agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out);

extern bool unsupported_clause(AggCtx *c, Node *n);
extern bool add_join(AggCtx *c, FromScan *fs, Index rti);
extern bool preserved_join(AggCtx *c);
extern bool setup_joins(AggCtx *c, List **quals, List **post, List **scan_quals, List **scan_post);
extern void add_agg_path(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target,
						 List *scan_tlist, int nworkers);
extern bool child_paths(AggCtx *c, SiftAggSpec *s);
extern Node *sift_agg_child_sig(Node *n, List *params);

extern bool first_aggs(AggCtx *c, PathTarget *target, List *keys, List *more, List **firsts);
extern void first_mode(SiftAggAgg *out, int nsort, bool leader);
extern bool first_fits(SiftAggSpec *s);
extern bool distinct_on_more(AggCtx *c, Query *parse, List **more);
extern void topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);

extern void sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
							   GroupPathExtraData *gextra);

extern bool agg_base(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs);
extern SiftAggSpec *agg_spec_new(AggCtx *c);
extern bool agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs);
extern bool spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys);
extern void agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
					   PathTarget *target, List *keys, List *scan_tlist);
extern bool compile_grouping(Query *parse, GroupingFunc *g, SiftAggAgg *out);
extern void add_sets(Query *parse, SiftAggSpec *s, List **scan_tlist);

#endif							/* SIFT_AGG_PLAN_INT_H */
