/*
 * agg/engine.h - SiftAggV6, the aggregate scan of an index of format major 2: the rows the scan of col/f2/scan/scan.c accepts (its pruning, its filters on the compressed
 * chunks, its deletion vectors and pending deletes) are vetted against the heap in batches, decoded to the vectors of agg/vec.h and folded into the groups of agg/ht/ht.h
 * by the kernels of agg/kern/kern.h; one tuple leaves for each group. The planner side adds the path for a single table whose sift_col index of format major 2 holds every
 * column the query reads.
 */
#ifndef SIFT_AGG_V6_H
#define SIFT_AGG_V6_H

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "access/skey.h"
#include "executor/executor.h"
#include "utils/rel.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/vclass.h"
#include "agg/vec.h"
#include "agg/win/datebin.h"
#include "core/tidbridge.h"
#include "col/am.h"
#include "core/store/ops.h"

typedef enum SiftV6Kind
{
	SV6_COUNT_STAR,
	SV6_COUNT,
	SV6_SUM,
	SV6_AVG,
	SV6_MIN,
	SV6_MAX,
	SV6_COUNT_DISTINCT,
	SV6_GROUPING,				/* GROUPING(keys): arg.attno is the index of its SiftV6Func */
	SV6_ARG_MAX,				/* sift.arg_max(val, cmp): arg is val, arg2 is cmp */
	SV6_ARG_MIN,
	SV6_STDDEV_POP,				/* of a float column: the state of PostgreSQL's float8_accum (group_ext.h) */
	SV6_STDDEV_SAMP,
	SV6_VAR_POP,
	SV6_VAR_SAMP,
	SV6_BOOL_OR,
	SV6_BOOL_AND,
	SV6_PCT_CONT,				/* percentile_cont(frac) WITHIN GROUP (ORDER BY float8 column): the values of the group in the arena of its text states (v6_pct.h) */
	SV6_GEN_PG,					/* generic PostgreSQL aggregate (aggtransfn/aggcombinefn/aggfinalfn + optional DISTINCT/ORDER BY tuplesort) */
	SV6_OSA,					/* ordered-set / hypothetical-set aggregate (WITHIN GROUP: mode, percentile_disc/cont, rank/dense_rank/...) */
	SV6_SUM_DISTINCT,			/* sum(DISTINCT x) of an integer, numeric or float column: each distinct value of a group joins the sum of SV6_SUM once (group_dist.c) */
	SV6_AVG_DISTINCT,
	SV6_COUNT_ROWS				/* COUNT(DISTINCT x) of pairs of an unnest, x the key of a row of the scanned table: the rows of a group, each counted at its first pair (group/ext.c) */
} SiftV6Kind;

/* the aggregates whose values go through the exact set of v6_distinct.h */
static inline bool
sift_v6_kind_distinct(SiftV6Kind kind)
{
	return kind == SV6_COUNT_DISTINCT || kind == SV6_SUM_DISTINCT || kind == SV6_AVG_DISTINCT;
}

/* the ones that add the distinct values: the exact set hands each value to the group's sum (group_dist.c) instead of counting it */
static inline bool
sift_v6_kind_valued(SiftV6Kind kind)
{
	return kind == SV6_SUM_DISTINCT || kind == SV6_AVG_DISTINCT;
}

/* the aggregate whose state and result a DISTINCT sum or average has */
static inline SiftV6Kind
sift_v6_kind_base(SiftV6Kind kind)
{
	return kind == SV6_SUM_DISTINCT ? SV6_SUM : kind == SV6_AVG_DISTINCT ? SV6_AVG : kind;
}

#define SIFT_V6_MAX_KEYS	32
#define SIFT_V6_MAX_DCOLS	8		/* the columns of a COUNT(DISTINCT (a, b, ..)) */
#define SIFT_V6_MAX_FILTERS	64		/* the distinct FILTER (WHERE ..) clauses of a statement: one bit of a row's mask each (v6_filter_pivot.h) */
#define SIFT_V6_MAX_FCOLS	128		/* the columns they read */

static inline bool
sift_v6_kind_reads_column(SiftV6Kind kind)
{
	return kind != SV6_COUNT_STAR && kind != SV6_GROUPING;
}

static inline SiftV6Class
sift_v6_class(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return SV6C_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:		/* a numeric of the INT class: its scaled integer; an argument of an aggregate only */
			return SV6C_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return SV6C_F64;
		case BOOLOID:
			return SV6C_BOOL;
		case TEXTOID:
		case VARCHAROID:
			return SV6C_TEXT;
		default:
			return SV6C_NONE;
	}
}

/* the class of a column in the batches of an unnest: a char(n) is read as the strings of its dictionary */
static inline SiftV6Class
sift_v6_unnest_class(Oid typid)
{
	return typid == BPCHAROID ? SV6C_TEXT : sift_v6_class(typid);
}

/* SiftV6Col.scale of a column the scan reads as datums: a type the native vectors do not hold, or a numeric whose NaN, infinities and digit count are values of it instead of scaled integers */
#define SIFT_V6_SCALE_DATUM	(-1)

/* Where the values of a column of the scan come from */
typedef enum SiftV6Src
{
	SV6SRC_ATTR = 0,			/* a leaf of the index: the first one of attno, or the one jleaf names */
	SV6SRC_DIM,					/* column slot of the dimension dim of a star join */
	SV6SRC_ELEM					/* the element of the unnested array of the index */
} SiftV6Src;

/* The virtual attribute of the boolean flag of test i of the elements of a row's array (SiftV6Ext.test, v6_ext.h), which a FILTER reads */
#define SIFT_V6_XFLAG_VATT		(3 << 12)

/* The ints each input of an SiftV6Spec.pgexpr item has after its result type: its attribute, type, scale, source and slot (agg/expr/pg.c) */
#define SIFT_V6_PGX_INTS		5

typedef struct SiftV6Col
{
	int			attno;			/* of the index, 1-based; of the table, when the spec is_table */
	Oid			typid;
	int			scale;			/* numeric: the digits after the point of the scaled integer the leaf holds; 0 for any other type */
	int			jleaf;			/* 1 + the leaf of a key of the jsonb attribute whose `->>` text (typid text) or float8 cast the column is; 0: the first leaf of attno */
	int			jmember;		/* 1 + the entry of SiftV6Spec.jmembers of what the column reads by a path of a document: a member of a map (jleaf the leaf of the values of the map, found in each row by its key), or the cast of the jsonb of a path the index declares (jleaf its leaf); 0: none */
	bool		jtype;			/* with jleaf: the column is jsonb_typeof(doc -> 'key') (typid text), the type each row's chunk keeps of the key, not the value of it */
	bool		jsonb_cast;		/* with jleaf: the column is (doc -> 'key')::float8, which raises for a value that is no number where the `->>` text is converted */
	bool		jint;			/* with jleaf: the column is (doc ->> 'key')::int2, int4 or int8 of an integer leaf, which raises for a text that is no integer */
	SiftV6Src	src;
	int			dim;			/* SV6SRC_DIM: the dimension */
	int			slot;			/* SV6SRC_DIM: the payload column of the dimension */
	int			derive;			/* 1 + the number of its entry in SiftV6Spec.derive when the key or argument is computed from the column (attno is then the source) */
	Oid			collid;			/* a key: the collation its type is grouped under (the interner of a generic key, v6_datum.h) */
} SiftV6Col;

/* the two columns are one column of the scan: the same read of the same source */
static inline bool
sift_v6_col_same(const SiftV6Col *a, const SiftV6Col *b)
{
	return a->attno == b->attno && a->typid == b->typid && a->jleaf == b->jleaf && a->jmember == b->jmember && a->jtype == b->jtype && a->jsonb_cast == b->jsonb_cast &&
		a->jint == b->jint && a->src == b->src && a->dim == b->dim && a->slot == b->slot && a->derive == b->derive;
}

/* How the scan decodes the column: SV6C_DATUM for a column the planner marked to be read as datums (v6_datum.h) and for a whole jsonb document */
static inline SiftV6Class
sift_v6_col_class(const SiftV6Col *c)
{
	return c->scale == SIFT_V6_SCALE_DATUM || c->typid == JSONBOID ? SV6C_DATUM : sift_v6_class(c->typid);
}

struct SiftV6Ext;

typedef struct SiftV6Agg
{
	SiftV6Kind	kind;
	SiftV6Col	arg;			/* attno 0 for count(*) */
	SiftV6Col	arg2;			/* sift.arg_max, sift.arg_min: the column the order is of; attno 0 for the others */
	int			nxcols;			/* COUNT(DISTINCT (a, b, ..)): the columns after arg, which is the first */
	SiftV6Col	xcol[SIFT_V6_MAX_DCOLS - 1];
	int			fclause;		/* FILTER (WHERE ..): 1 + the number of its clause in SiftV6Spec.filter, 0 for none */
	Oid			restype;
	double		frac;			/* SV6_PCT_CONT: the fraction */
} SiftV6Agg;

/* a generic PostgreSQL aggregate or an ordered-set one (v6_gen.h): it reads up to nine columns, one per argument that is not a constant, and folds a NaN as PostgreSQL's own function does */
static inline bool
v6g_is_gen(const SiftV6Agg *a)
{
	return a->kind == SV6_GEN_PG || a->kind == SV6_OSA;
}

/* A numeric NaN is not NULL: these aggregates fold it. It counts, and makes a sum or an average NaN, a maximum NaN and a minimum NaN when no other row has a value, and is one more distinct value */
static inline bool
sift_v6_agg_takes_nan(const SiftV6Agg *a)
{
	return a->kind == SV6_COUNT || a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX || sift_v6_kind_valued(a->kind) ||
		(a->kind == SV6_COUNT_DISTINCT && a->nxcols == 0);
}

/* The keys (indexes into SiftV6Spec.key) the arguments of a GROUPING() name */
typedef struct SiftV6Func
{
	int			nargs;
	int			key[SIFT_V6_MAX_KEYS];
} SiftV6Func;

/* A key that is date_bin(stride, column, origin) of a timestamp or timestamptz column: the key's column is the source, and the scan bins it (v6_datebin_win.h) */
typedef struct SiftV6Bin
{
	int			key;			/* the index into SiftV6Spec.key */
	int64		stride_us;
	int64		origin;
	SiftDateBinStatus status;	/* the error of PostgreSQL the stride or the origin raises for every finite source (a stride with months): the scan raises it for the first such row */
} SiftV6Bin;

/* A key or an argument computed from columns of the row (agg/expr/str.h): the vector of the batch it is is made after the columns are decoded */
typedef enum SiftV6DeriveKind
{
	SVD_NONE = 0,
	SVD_STRLEN,					/* length(text) of src: the characters (the bytes in a single-byte encoding), as the type of the key or argument says */
	SVD_URL_HOST,				/* regexp_replace(src, '^https?://(?:www\\.)?([^/]+)/.*$', '\\1') */
	SVD_EXTRACT,				/* extract(minute or hour FROM src), src a timestamp; aux is the unit in microseconds */
	SVD_CASE_STR,				/* CASE WHEN cond[0] = cval[0] AND .. THEN src ELSE the constant END, text */
	SVD_ARITH,					/* + - * of float8 columns (src, cond[0..]) and a constant (aux): the postfix tokens in cval, v6_plan_rewrite.h; the batches of an unnest only */
	SVD_NUM_EXPR,				/* the exact scaled-integer product of up to three affine factors of numeric columns (agg/expr/num.h) */
	SVD_PG_EXPR					/* arbitrary PostgreSQL Expr evaluated over active rows via ExecEvalExpr with per-batch ResetExprContext */
} SiftV6DeriveKind;

#define SIFT_V6_MAX_DERIVE	64
#define SIFT_V6_MAX_CASE	4
#define SIFT_V6_ELSE_BYTES	12

typedef struct SiftV6Derive
{
	SiftV6DeriveKind kind;
	SiftV6Col	src;
	int64		aux;
	int			ncond;
	SiftV6Col	cond[SIFT_V6_MAX_CASE];
	int64		cval[SIFT_V6_MAX_CASE];
	int			else_len;
	uint8		else_bytes[SIFT_V6_ELSE_BYTES];
	bool		utf8;			/* length counts characters of a UTF-8 string */
} SiftV6Derive;

/* ORDER BY .. LIMIT of the groups: the sort column (its position in the tuple of the node), the ordering operator, its collation and where it puts the NULLs */
typedef struct SiftV6TopKItem
{
	int			pos;
	Oid			sortop;
	Oid			collid;
	bool		nulls_first;
} SiftV6TopKItem;

#define SIFT_V6_MAX_TOPK_ITEMS	12

/* A column a row Top-N gives: the heap row of the winner (the plan's tlist names its attribute), a key its heap of winners carries (a column of a dimension), or an aggregate of the whole set */
typedef enum SiftV6RowFrom
{
	SV6OUT_HEAP,
	SV6OUT_KEY,					/* at: the key */
	SV6OUT_TOTAL				/* at: the aggregate */
} SiftV6RowFrom;

typedef struct SiftV6RowOut
{
	SiftV6RowFrom from;
	int			at;
} SiftV6RowOut;

/* the end of the keys of a TID range: no block is as large as this */
#define SIFT_V6_TID_END	(UINT64CONST(1) << 48)
/* The ctid bounds by constant TIDs among the RestrictInfos clauses of the base relation relid as one range [*lo, *hi) of keys (block << 16 | offset), the other clauses in *rest; false: no bound
 * (agg/plan/tid.c) */
extern bool sift_v6_tid_bounds(Index relid, const List *clauses, uint64 *lo, uint64 *hi, List **rest);

typedef struct SiftV6Spec
{
	Oid			relid;			/* what the scan reads: the sift_col index, or the sift_table when is_table */
	bool		is_table;
	int			nparts;			/* a partitioned table: the index of each live partition, read into the one set of groups (indexoid is the first); 0 for one index */
	Oid		   *part_index;
	int			part_ncols;		/* the attributes of the partitioned table, and the attribute of each in every partition (part_attno[partition * part_ncols + attno - 1], 0 for a dropped one) */
	AttrNumber *part_attno;
	bool	   *part_is_table;	/* of each partition: its sift_table is read, not an index of it (the other fields of a partition are those of part_index) */
	struct PartitionPruneInfo *prune;	/* a generic plan: PostgreSQL's steps that prune the partitions on the values of the execution (agg/part_prune.c); NULL without them */
	List	   *prune_rels;		/* the partitioned table of each PartitionedRelPruneInfo of prune, in order */
	Expr	   *prune_array;	/* the array of `key = ANY (array)` whose elements the steps take one by one (agg/part_prune.c); NULL without one */
	int			parts_removed;	/* the partitions the steps ruled out when the scan began */
	bool		parts_none;		/* they ruled out every partition: the first one stands in as the source, and its quals pass no row */
	int			nkeys;
	SiftV6Col  *key;
	int			naggs;
	SiftV6Agg  *agg;
	int			nsets;			/* GROUPING SETS, ROLLUP, CUBE: the sets as masks over the keys; 0 for a plain GROUP BY */
	uint32	   *set_mask;
	int			nfuncs;
	SiftV6Func *func;
	List	   *indexqual;		/* the quals of the scan as an index scan has them: clauses on INDEX_VAR attributes */
	int			nfilters;
	List	   *filter;			/* of each distinct FILTER clause, the list of its quals in the form of indexqual */
	int			nfcols;
	SiftV6Col	fcol[SIFT_V6_MAX_FCOLS];	/* the columns the clauses read, after the columns of the aggregates in the vectors of a batch */
	int			nbins;
	SiftV6Bin	bin[SIFT_V6_MAX_KEYS];
	List	   *jmembers;		/* of each member of a document the scan reads by a path (SiftV6Col.jmember): the attribute of the document, the leaf of the values and the type the member is read as (Integer nodes), then the keys from the document (String nodes) */
	List	   *jsonqual;		/* the quals on the keys of jsonb documents the clauses of an operator class cannot say (agg/qual/jsonb.c) */
	List	   *vqual;			/* the quals no scan key says (a column against a column, OR, a numeric against a constant), over the columns the scan decodes (agg/qual/bqual.h): they narrow every batch */
	struct SiftV6Ext *ext;		/* the dimensions of a star join or the array an unnest takes apart (v6_ext.h); NULL for the scan of the columns of one table */
	Oid			text_indexoid;	/* a sift index whose search keeps the rows: its hits go to the scan as bridges (v6_text.c); InvalidOid without one */
	List	   *text_queries;	/* the search: an OR of arms, each the list of the queries (text Consts or $n) it ANDs */
	int			par_strategy;	/* SiftV6ParStrategy of the plan (0: serial) */
	int			par_workers;
	int			nderive;
	SiftV6Derive derive[SIFT_V6_MAX_DERIVE];
	bool		affine;			/* keys elided because they are the first key plus a constant: a first key outside [aff_lo, aff_hi] overflows one of them, an error as it is in PostgreSQL */
	Oid			aff_type;
	int64		aff_lo;
	int64		aff_hi;
	char	   *topn_desc;		/* EXPLAIN of a row Top-N: its sort keys */
	char	   *agg_desc;		/* its aggregates of the whole set (NULL: none) */
	const char *serial_because;	/* EXPLAIN VERBOSE: the rule that keeps the scan in one process while the session allows workers (NULL: none) */
	List	   *gen_written;	/* the planner's: the Aggrefs of genref as the statement writes them, which a reason names (not packed) */
	int			nout;			/* a row Top-N: the columns the node gives, in the order of its scan tlist */
	SiftV6RowOut *out;
	bool		rows;			/* ORDER BY .. LIMIT of the rows of one table, not of groups: the keys are the sort columns (then the columns of the dimensions the rows carry), topk the K, and the node gives the rows it fetches from the heap (v6_rows_topn.h); the aggregates are those of the whole set the rows carry */
	uint32		groups_est;		/* the groups the planner expects (0: unknown): the group table is sized for them at the start */
	uint32		topk;			/* LIMIT + OFFSET of the query when the node gives the best groups in order (0: all groups, unordered) */
	List	   *topk_count;		/* a row Top-N whose LIMIT and OFFSET an execution gives (expressions of parameters): the two, whose sum is its K (topk is the planner's guess); NIL: topk is the K */
	Node	   *skip;			/* a row Top-N under a Limit that has no OFFSET: the OFFSET of the statement, whose rows the node passes over itself (NULL: none) */
	int			ntopk;
	SiftV6TopKItem topk_item[SIFT_V6_MAX_TOPK_ITEMS];
	bool		topk_ties;		/* the best K and every row that ties the K-th on the sort items: the sort keys past them, which the node does not read, are sorted above it */
	bool		having;			/* the node runs a HAVING qual on the groups: the K best are the best that pass it, not the best of all groups */
	List	   *pgexpr;			/* the PostgreSQL expressions of the SVD_PG_EXPR derivations: their items (v6_pg_expr.h) */
	bool		constant_groups;	/* GROUP BY of expressions that read no column: no group when no row is read (the single set of no keys has a row only once a row was folded) */
	List	   *genref;			/* the Aggref of each SV6_GEN_PG and SV6_OSA aggregate, in the order of agg (v6_gen.h) */
	bool		tid_range;		/* the WHERE bounds ctid: the rows whose heap row's ctid is in [tid_lo, tid_hi), as block << 16 | offset (v6_vet.h) */
	uint64		tid_lo;
	uint64		tid_hi;
	uint32		lock_k;			/* LIMIT + OFFSET of a query with no ORDER BY, HAVING or DISTINCT: any K groups with their exact aggregates answer it, so the group table takes no group after the K-th (0: all groups) */
	struct SiftV6Exec *exec;	/* what the node that runs the scan prepared of the expressions above that need the executor (v6_pg_exec.h); set when the node begins, never packed */
} SiftV6Spec;

/* What the scan of partition k reads (k = 0 when the table is not partitioned) */
static inline Oid
sift_v6_spec_part_oid(const SiftV6Spec *s, int k)
{
	return s->nparts > 0 ? s->part_index[k] : s->relid;
}

static inline bool
sift_v6_spec_part_is_table(const SiftV6Spec *s, int k)
{
	return s->nparts > 0 ? s->part_is_table[k] : s->is_table;
}

/*
 * The vectors of a batch: argvec[j] is the argument of aggregate j, argvec[naggs + j] its second argument (sift.arg_max, sift.arg_min), and then the columns after the first of each
 * COUNT(DISTINCT (a, b, ..)), the ones of aggregate j from sift_v6_agg_xvec.
 */
static inline int
sift_v6_agg_xvec(const SiftV6Spec *s, int j)
{
	int			at = 2 * s->naggs;

	for (int i = 0; i < j; i++)
		at += s->agg[i].nxcols;
	return at;
}

/* and then the columns of the FILTER clauses, fcol[i] at sift_v6_spec_fvec + i */
static inline int
sift_v6_spec_fvec(const SiftV6Spec *s)
{
	return sift_v6_agg_xvec(s, s->naggs);
}

static inline int
sift_v6_spec_nvec(const SiftV6Spec *s)
{
	return sift_v6_spec_fvec(s) + s->nfcols;
}

static inline bool
sift_v6_spec_distinct(const SiftV6Spec *s)
{
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].kind == SV6_COUNT_DISTINCT)
			return true;
	return false;
}

/* An aggregate over sift.score(ctid): the rows of the fold have to come with their scores */
static inline bool
sift_v6_spec_reads_score(const SiftV6Spec *s)
{
	for (int j = 0; j < s->naggs; j++)
		if (sift_v6_kind_reads_column(s->agg[j].kind) && s->agg[j].arg.attno == SIFT_V6_SCORE_ATTNO)
			return true;
	return false;
}

/* What the planner side tells of the relation a jsonb qual or read is bound against */
typedef struct SiftV6Rel
{
	Index		relid;
	Oid			reloid;			/* the index, or the table */
	const IndexOptInfo *index;	/* NULL for a sift_table */
	const SiftColRel *colrel;	/* NULL for a sift_table */
	const SiftStoreSchema *table;	/* a sift_table: the schema of its store, whose attributes are those of the table */
	const SiftStoreOps *ops;	/* the format of the index, NULL when it holds nothing of it (a stale index) */
	const Bitmapset *notnull;	/* the attributes of the table a NOT NULL constraint holds */
} SiftV6Rel;

/*
 * agg/qual/jsonb.c: `doc ? 'k'`, jsonb_exists(doc, 'k') and jsonb_typeof(doc -> 'k') = 'type' (or IN a list of types) over a document of the index whose key is a leaf, as the element of
 * SiftV6Spec.jsonqual the scan keys are made from; and `doc ->> 'k'` and `(doc ->> 'k')::float8` as a column of the scan. NULL / false when the leaf does not hold what the query asks.
 */
extern List *sift_v6_json_qual(const SiftV6Rel *rel, const Expr *clause);
extern bool sift_v6_json_read(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out);
/*
 * `(doc -> .. ->> 'k')::float8`, and from PostgreSQL 18 on `(doc -> .. -> 'k')::type` (float8, and of a member of a map an integer), of a member of a map of float8 values of a document or of a path the
 * index declares float8, as a column of the scan (jleaf the leaf of the values), and the keys from the document; the text of a path the index declares is its leaf alone, with no keys (NIL)
 */
extern bool sift_v6_json_map_member(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out, List **path);
extern bool sift_v6_values_exact(const SiftV6Rel *rel);
/* The leaves of the relation: those of its index, or of the store of its table */
extern const struct SiftColSchema *sift_v6_rel_leaves(const SiftV6Rel *rel);
/* the virtual attribute of the member of a map e reads (sift_v6_member_vatt), and its type; InvalidAttrNumber: e reads none */
typedef int (*SiftV6MemberVatt) (void *arg, const Expr *e, Oid *typid);

/*
 * e with every jsonb_typeof(doc -> 'key') of a shredded scalar key as a Var of the virtual attribute SIFT_V6_JTYPE_VATT + leaf (varno rel->relid, a copy), and with values every
 * `doc ->> 'key'` and its float8 cast as one of the text or float8 kind: the scan reads a value a leaf cannot hold from the exception of its chunk, which a scan key (a FILTER) does not; the column such a Var is.
 * A key test (`?`, `?|`, `?&`) of a NOT NULL document, or one read for its truth alone (truth: e is a WHERE or FILTER clause), is the presence of its keys (SIFT_V6_JPRES_VATT); a containment
 * read for its truth alone is the conjunction of the tests of its leaves (agg/qual/contains.h)
 */
extern Expr *sift_v6_json_vars(const SiftV6Rel *rel, const Expr *e, bool values, bool truth, SiftV6MemberVatt member, void *arg);
/* sift_v6_json_vars of the tests of the presence of keys alone (`doc ? 'k'`, jsonb_exists and their NOT), which the scan of an unnest reads (SiftJsonbOps.unnest_keys) */
extern Expr *sift_v6_json_presence_vars(const SiftV6Rel *rel, const Expr *e, bool truth);
extern bool sift_v6_json_type_col(const SiftV6Rel *rel, const Var *v, SiftV6Col *out);
/* The statement reads the type and the value of one key apart from the scan keys: PostgreSQL's own plan decides which values it reads */
extern bool sift_v6_json_type_clash(const SiftV6Spec *s);
/* The attribute a column is an input of an expression or a qual by: its own, or the virtual one of what it reads of a key of a document */
static inline int
sift_v6_col_input_attno(const SiftV6Col *c)
{
	if (c->jmember > 0)
		return sift_v6_member_vatt(c->jmember, c->jsonb_cast);
	if (c->jleaf == 0)
		return c->attno;
	return sift_v6_jvatt_base(c->jtype, c->jint, c->typid) + c->jleaf - 1;
}

/* The attribute a batch of the scan holds the column in: a value of a key of a document is the leaf of the key (SIFT_V6_JLEAF_VATT), its type or presence the virtual one of it */
static inline int
sift_v6_col_batch_attno(const SiftV6Col *c)
{
	if (c->jleaf == 0)
		return c->attno;
	return c->jtype ? sift_v6_col_input_attno(c) : SIFT_V6_JLEAF_VATT + c->jleaf - 1;
}

/* The keys from the document to member jmember of a map (SiftV6Spec.jmembers) */
static inline List *
sift_v6_member_path(const SiftV6Spec *s, int jmember)
{
	return list_copy_tail(list_nth(s->jmembers, jmember - 1), 3);
}

/* The column of the scan the virtual attribute of a member is (sift_v6_member_vatt): the attribute of its document, the leaf of its values, the type and the cast */
static inline SiftV6Col
sift_v6_member_col(const SiftV6Spec *s, int attno)
{
	const int	at = (attno - SIFT_V6_JMEMBER_VATT) / 2;
	const List *m = list_nth(s->jmembers, at);

	return (SiftV6Col) {.attno = intVal(linitial(m)),.typid = (Oid) intVal(lthird(m)),.jleaf = intVal(lsecond(m)) + 1,.jmember = at + 1,.jsonb_cast = (attno - SIFT_V6_JMEMBER_VATT) % 2 == 1};
}

/* The column of the scan a virtual attribute of a key of document attribute att is: the type or the presence, the text or a cast of it, of the key whose leaf is in the attribute */
static inline SiftV6Col
sift_v6_vatt_col(int attno, int att, Oid typid, int scale)
{
	return (SiftV6Col) {.attno = att,.typid = typid,.scale = scale,.jleaf = sift_v6_vatt_leaf(attno) + 1,.jtype = sift_v6_vatt_is_type(attno),.jint = sift_v6_vatt_is_int_cast(attno)};
}

/* The column of the scan of the spec a virtual attribute is: a member of a map, or what it reads of a key of a document */
static inline SiftV6Col
sift_v6_spec_vatt_col(const SiftV6Spec *s, int attno, Oid typid, int scale)
{
	return sift_v6_vatt_is_member(attno) ? sift_v6_member_col(s, attno) : sift_v6_vatt_col(attno, attno, typid, scale);
}
/* e is doc -> 'k' or doc['k'] (as_text false), or doc ->> 'k' or its #>> '{}' (as_text true), of a column doc and a constant text key */
extern bool sift_v6_json_member(const Expr *e, bool as_text, const Var **doc, const char **key);
/* the jsonb column e is a chain of ->, ->> and subscripts with constant object keys over (NULL: it is not one), and the keys in order (NIL: e is the column) */
extern const Var *sift_v6_doc_chain(const Expr *e, List **path);
/* e is a shredded jsonb attribute of the relation: the column of the index (1-based) or the attribute of the table that holds it */
extern bool sift_v6_json_doc_att(const SiftV6Rel *rel, const Expr *e, int *att);
/* keys and nkeys with the scan keys of spec->jsonqual appended, in a new array: for keys that no IndexRuntimeKeyInfo points into */
extern void sift_v6_json_scankeys(const SiftV6Spec *spec, Relation index, ScanKey *keys, int *nkeys);

typedef enum SiftV6JsonExc
{
	SV6JX_NULL,					/* a JSON null, or a value whose cast is NULL */
	SV6JX_VALUE,
	SV6JX_RAISES,				/* PostgreSQL's cast of the value raises: sift_v6_json_exception_raise raises its error */
	SV6JX_INEXACT				/* the integer cast of a number a float8 leaf holds rounded (a tie, x.5, or an integer past 2^52): the decimal of the document decides it */
} SiftV6JsonExc;

typedef struct SiftV6JsonValue
{
	const char *text;			/* SV6C_TEXT */
	uint32		tlen;
	Datum		value;			/* the float8, bool or integer of the cast */
} SiftV6JsonValue;

/* the integer an integer vector holds of the bool or integer value of a cast */
static inline int64
sift_v6_json_int(Oid typid, Datum value)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(value);
		case INT4OID:
			return DatumGetInt32(value);
		case INT8OID:
			return DatumGetInt64(value);
		default:
			return DatumGetBool(value);
	}
}

/* The value of an exception as a column of class cls reads it (the `->>` text, its float8 or bool cast, or the cast of the jsonb to float8 or an integer), without raising: the scan raises only for the rows it keeps */
extern SiftV6JsonExc sift_v6_json_exception_read(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx, SiftV6JsonValue *out);
extern void sift_v6_json_exception_raise(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx);
/*
 * The integer cast of the jsonb of a number a float8 leaf holds, as numeric rounds it: SV6JX_RAISES when the type cannot hold it (sift_v6_json_float_int_raise raises PostgreSQL's error), SV6JX_INEXACT
 * when the decimals the leaf rounds to f do not all cast to one integer, which only the document of the row tells
 */
extern SiftV6JsonExc sift_v6_json_float_int(Oid typid, double f, Datum *out);
extern void sift_v6_json_float_int_raise(Oid typid, double f);
/* The member the keys lead to from the jsonb of the payload (a jsonb datum of len bytes), as the payload of one, made in cx; false: none (a key is missing, or a value on the way is no object) */
extern bool sift_v6_json_payload_member(const char *payload, uint32 len, const List *keys, MemoryContext cx, const char **out, uint32 *outlen);
/* the number of elements of the jsonb of the payload, a value of a list jsonb_array_length reads (SiftJsonbOps.array_length): false when it is no array, whose error sift_v6_json_array_length_raise raises */
extern bool sift_v6_json_array_length(const char *payload, uint32 len, MemoryContext cx, int64 *n);
extern void sift_v6_json_array_length_raise(const char *payload, uint32 len, MemoryContext cx);
/* `doc ? 'k'` of a key whose leaf the scan reads the presence of: the virtual attribute of the presence (InvalidAttrNumber: none) */
extern int	sift_v6_json_presence(const SiftV6Rel *rel, const Expr *clause);

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_plan_methods;

/* Prepares the expressions of spec that need the executor (agg/pg_exec.h) under the node ps, which begins: spec.exec */
extern void sift_v6_exec_begin(SiftV6Spec *spec, struct PlanState *ps);

extern void sift_agg_v6_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern Plan *sift_agg_v6_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);
/*
 * The aggregate of the statement over the columns of index ix (open as irel) of rel, or of the store of a sift_table, without its WHERE quals, for a scan that brings its own rows (agg/hybrid/plan.c): the keys and aggregates in
 * *spec, the entries of the rows the aggregate leaves in *scan_tlist (the keys, then the aggregates), the grouping expressions in *group_exprs; false when the index does not hold what the statement reads.
 * extra is the GroupPathExtraData of the upper path. sift_v6_plan_shape says whether the statement is of the shape the aggregate scan serves (one table, no windows or DISTINCT ON).
 */
extern bool sift_v6_compile_aggregate(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, IndexOptInfo *ix, Relation irel, const SiftStoreSchema *table, void *extra, SiftV6Spec *spec,
									  List **scan_tlist, List **group_exprs);
extern bool sift_v6_plan_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
/* is vector v of a batch (the argvec of sift_v6_group_update) one the generic or ordered-set aggregates of the spec read (v6_gen.c) */
extern bool sift_v6_vec_of_gen(const SiftV6Spec *spec, int v);

/* is vector v of a batch one that folds a NaN: the argument of an aggregate that takes it, or a numeric column of the FILTER clauses (agg/filter/eval.c puts a NaN above every number) */
static inline bool
sift_v6_vec_takes_nan(const SiftV6Spec *spec, int v)
{
	const int	f = v - sift_v6_spec_fvec(spec);

	if (f >= 0 && f < spec->nfcols)
		return spec->fcol[f].typid == NUMERICOID && spec->fcol[f].derive == 0;
	return (v < spec->naggs && sift_v6_kind_reads_column(spec->agg[v].kind) && sift_v6_agg_takes_nan(&spec->agg[v])) || sift_v6_vec_of_gen(spec, v);
}
extern bool sift_v6_agg_kind(const Aggref *a, SiftV6Kind *kind);
extern bool sift_v6_kind_takes(SiftV6Kind kind, Oid typid);
/* The aggregate of the facts of a join as a scan of the index (the tuple of its scan tlist): false when a key, an aggregate or a qual of the facts is not one the scan decides (scope_v6.c adds the members of a scope key) */
extern bool sift_v6_compile_fact(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, IndexOptInfo *ix, GroupPathExtraData *extra, SiftV6Spec *spec, List **scan_tlist);
/* stddev, variance, bool_or and bool_and by name and argument type (group_ext.c) */
extern bool sift_v6_ext_kind(const char *name, Oid argtype, SiftV6Kind *kind);
/* the methods of the path of the aggregate scan (v6_plan_rewrite.c makes the paths of its Top-K) */
extern const CustomPathMethods *sift_v6_group_path_methods(void);
extern List *sift_v6_spec_pack(const SiftV6Spec *s);
/* The spec of a plan; with the parameters of an execution (NULL: while planning), the external parameters its PostgreSQL expressions read are their values */
extern SiftV6Spec *sift_v6_spec_unpack(const List *l, ParamListInfo params);
/* The attribute of the parent that a column of the plan names, in the relation of partition source the node reads */
static inline AttrNumber
sift_v6_spec_source_attno(const SiftV6Spec *spec, int source, AttrNumber attno)
{
	return spec->nparts > 0 && attno > 0 && attno <= spec->part_ncols ? spec->part_attno[source * spec->part_ncols + attno - 1] : attno;
}

/* The plan as partition source of the node reads it: its batch quals name the attributes of the partition, which numbers its columns as it was made */
extern const SiftV6Spec *sift_v6_spec_of_source(const SiftV6Spec *spec, int source);


/* The groups: keys and aggregates folded from batches, then every group once */
typedef struct SiftV6Group SiftV6Group;

/* desc is the tuple the groups leave as: its attributes are the keys then the aggregates */
extern SiftV6Group *sift_v6_group_new(const SiftV6Spec *spec, MemoryContext cx, TupleDesc desc);
/* keyvec[i] is key i and argvec[j] the argument of aggregate j (NULL for count(*)); the rows are those of bs */
extern void sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
/* The derived columns of a batch that go to the groups g (NULL: elsewhere): derive(arg, k, rows) computes column k for rows. A column only the aggregates with a FILTER read is computed for the rows their clauses keep (group_derive.c) */
typedef void (*SiftV6DeriveFn) (void *arg, int k, const SiftBatchSel *rows);
extern void sift_v6_derive_batch(SiftV6Group *g, int nderive, const SiftBatchSel *bs, SiftVec *const *argvec, SiftV6DeriveFn derive, void *arg);
/* Rows that no key or filter tells apart, for a statement of count(*) alone (sift_v6_spec_count_only) */
extern bool sift_v6_spec_count_only(const SiftV6Spec *spec);
/* The grouping set every other lies inside, when the states of every aggregate add exactly: the scan updates it alone and sums the others from it (agg/group/sets.c); -1 for none */
extern int	sift_v6_spec_fold_set(const SiftV6Spec *spec);
extern void sift_v6_group_add_rows(SiftV6Group *g, uint64 n);
extern void sift_v6_group_finish(SiftV6Group *g);
/* The next group as the Datums of the keys then the aggregates; false after the last */
extern bool sift_v6_group_next(SiftV6Group *g, Datum *values, bool *isnull);
extern uint64 sift_v6_group_bytes(const SiftV6Group *g);
extern void sift_v6_group_spill_stats(const SiftV6Group *g, uint64 *puts, uint64 *bytes);
extern void sift_v6_group_free(SiftV6Group *g);
/* A row of a COUNT(DISTINCT) of a participant of a parallel run: the bytes of its key (the group and the value, or the value of a set of no keys) */
typedef void (*SiftV6DistPut) (void *arg, uint32 set_idx, uint32 slot, const void *bytes, uint32 len);
extern void sift_v6_group_scan_sink(SiftV6Group *g, SiftV6DistPut put, void *arg);
extern void sift_v6_group_recycle(SiftV6Group *g);
extern Datum sift_v6_i128_numeric(SiftI128 v);
/* v * 10^-scale as the numeric PostgreSQL makes of the digits: the scale is its display scale */
extern Datum sift_v6_scaled_numeric(SiftI128 v, int scale);
extern const char *sift_v6_group_mode(const SiftV6Group *g);

/* The scan: every batch of rows of the index of the spec goes to the group (or to the function of the caller) */
typedef struct SiftV6ScanStats
{
	uint64		batches;
	uint64		grans;			/* granules (units) the scan read, and those the zones and the skipping indexes ruled out unread */
	uint64		grans_pruned_zone;
	uint64		grans_pruned_skip;
	uint64		point_rows;		/* the rows of the granules read that the point directory of the index named (col/f2/point_dir.h) */
	uint64		src_filters;	/* the equalities of a periodic calendar step whose ranges the source left the granules outside of unread (agg/periodic.c) */
	uint64		rows;
	uint64		heap_vet_rows;
	uint64		heap_rows_dead;
	uint64		grans_preagg;	/* granules answered from their pre-aggregates (col/f2/agg/preagg.h) */
	uint64		grans_preagg_delta;	/* ... of them, those corrected for dead rows */
	uint64		grans_preagg_super;	/* the groups of them answered as one from the summary of the group */
	uint64		grans_preagg_net;	/* ... those answered net of the rows the statement leaves out that their summaries keep apart (exceptions) */
	uint64		grans_except_read;	/* the granules whose rows left out were kept apart but that were read all the same */
	uint64		heap_reread_rows;	/* rows whose value the leaves cannot hold, read again: the document an unnest takes apart from the heap, the value of a JSON key from its payload or heap row */
	uint64		chunks;			/* passes of a scan that takes the hits of a text query as bridges */
	uint64		bridge_tids;
	uint64		global_order_reads;	/* row Top-Ns answered off an order of the key the store keeps (the order arrays of Format 1), not off its units */
} SiftV6ScanStats;

/* The next chunk of the hits of the spec's text query as a finalized bridge that holds no lossy page, false at the end of the hits */
typedef bool (*SiftV6ChunkFn) (void *arg, const SiftTidBridge **bridge);

/* v6_text.c: the text query of a statement of the aggregate (the planner reads it from the quals, the executor feeds it to the scan as chunks) */
/* A text index of the rel that holds every row the query reads: the whole table, or the rows of a predicate the query implies */
extern bool sift_v6_text_index(const IndexOptInfo *ix);
/* The arms of a search are at most this many: the product of the branches of its ANDed clauses */
#define SIFT_V6_TEXT_MAX_ARMS	16
/* The clause is a search of a text index of rel, or an OR of searches of one: that index, and in branches the query (a text Const or $n) of each */
extern IndexOptInfo *sift_v6_text_match(RelOptInfo *rel, RestrictInfo *ri, List **branches);
/* The clause is a match of a text index of the relation, which no scan key or batch qual decides */
extern bool sift_v6_text_clause(RelOptInfo *rel, RestrictInfo *ri);
/* touch: the fraction of the rows of the store that the scan of the chunks reads (the granules the hits are in) */
extern double sift_v6_text_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *text, List *quals, List *arms, double *touch);
/* A btree or bitmap path on the other conditions of rel reads fewer rows than the scan walks postings (postings: the rows the text query matches), unless sift.agg_force is on: PostgreSQL's plan stands */
extern bool sift_v6_search_key_first(PlannerInfo *root, RelOptInfo *rel, double postings);

typedef struct SiftV6Text SiftV6Text;
/* The hits of the arms of the spec's search; a hit whose text the index leaves undecided is decided on the text of the row of heap the snapshot sees */
extern SiftV6Text *sift_v6_text_open(const SiftV6Spec *spec, Relation heap, Snapshot snapshot, MemoryContext parent);
extern bool sift_v6_text_chunk(void *text, const SiftTidBridge **bridge);
extern void sift_v6_text_close(SiftV6Text *t);

/*
 * The fold of the rows of a scan into the groups, which the zone scan of the format (core/store/ops.h) drives with the rows of a scan of the index and a caller with the rows of its own (agg/hybrid/exec_fold.c): sift_v6_fold_batch
 * takes a batch of rows (the ColView of each leaf, indexed by leaf; sift_v6_fold_cols are the ones to decode), the live ones decided by the stored values and the maybe ones they could not decide;
 * the recheck says whether the heap keeps a maybe row (visible, and passing the quals the scan holds). A fold lives in the memory context it began in.
 */
typedef struct SiftV6Scan SiftV6Fold;
typedef bool (*SiftV6Recheck) (void *arg, uint64 key);

/* The columns a fold reads: their column of the index (1-based), their leaf in the store and the class of their values */
typedef struct SiftV6FoldCol
{
	int			attno;
	int			leaf;
	SiftV6Class cls;
	int			scale;			/* a numeric: the digits after the point of the scaled integer the leaf holds */
} SiftV6FoldCol;

/* The rows of a batch as the vectors of the keys and of the arguments of the aggregates (keyvec[i] is key i, argvec[j] the argument of aggregate j); what a scan hands each batch to */
typedef void (*SiftV6BatchFn) (void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
typedef struct SiftV6Scan SiftV6Scan;

/* The rows the scan keeps go to rows (v6_rows_topn.h) instead of the groups */
typedef struct SiftV6Rows SiftV6Rows;

/*
 * The arms of a cohort of SiftV6Fuse (v6_fuse.h) that scan the same index with the same keys, in one pass: the rows are looked at in the heap once and every arm folds them into its groups. arms[0]
 * is the leader, whose keys, heap and snapshot the pass uses.
 */
struct SiftV6FuseCohort;

struct SiftClause;

struct SiftV6StarRun;

/*
 * An arm of a pass: its spec (with the columns of its gate when the arms have different keys), its groups, the gate: the rows of the pass it takes (NULL: all), and the dimensions of its star join
 * (NULL: none), read in one pass, that its rows go through before its groups
 */
typedef struct SiftV6ScanArm
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	const struct SiftClause *gate;
	struct SiftV6StarRun *star;
} SiftV6ScanArm;


#endif							/* SIFT_AGG_V6_H */
