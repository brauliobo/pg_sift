/*
 * agg/spec/join.h - the plan of a SiftAgg: the joins and the combos
 * The lookup joins, the first-row choice, the slots and the combos that give a tuple the
 * columns of a joined table; the second of the three parts of agg/spec.h.
 */
#ifndef SIFT_AGG_SPEC_JOIN_H
#define SIFT_AGG_SPEC_JOIN_H

#include "agg/spec/expr.h"

/*
 * A table joined by equality to leaves of the scan or to expressions of them (INNER, LEFT, SEMI or ANTI): a tuple's join key is read
 * from the leaves, the table's rows with that key that pass the rest of the ON clause are its matches (any number: the join is exact
 * for duplicates, a tuple is repeated per match, and a LEFT join keeps a tuple without a match once, with NULL columns). The table is
 * read when the scan starts, with the scan's snapshot, or is the output of a child plan (a sub-select, VALUES list or function: its rows are
 * the ones PostgreSQL's plan of it returns, run once in the query's snapshot). The columns the expressions read are colatt[]; they reach the expressions as
 * Params nleaves + i of the combo that owns the join.
 */
typedef enum SiftJoinKind
{
	SJ_INNER,
	SJ_LEFT,
	SJ_SEMI,					/* EXISTS / IN: a tuple is kept once if some row matches, and its columns are the table's no more */
	SJ_ANTI,					/* NOT EXISTS: a tuple is kept if no row matches */
	SJ_RIGHT,					/* the table is the preserved side of a LEFT JOIN of the scanned one: a tuple without a match is dropped, a row nothing matched is an output row of NULL leaves */
	SJ_FULL						/* the same, and a tuple without a match is kept once with NULL columns */
} SiftJoinKind;

/* the kinds that keep the rows of the joined table nothing matched */
static inline bool
sift_aggjoin_preserves(SiftJoinKind kind)
{
	return kind == SJ_RIGHT || kind == SJ_FULL;
}

#define SIFT_AGG_MAX_FORD	8		/* ORDER BY columns of a first-row child */
#define SIFT_AGG_MAX_FOUT	16		/* output columns of a first-row child that can be mapped to the table's */

/*
 * A correlated child that is `SELECT columns FROM table WHERE column = parameter [AND ...] LIMIT 1` over a table whose unique index is
 * on those columns returns the one row with the key, whatever its plan: it can be read like a lookup table instead (SiftAggJoin.first).
 */
typedef struct SiftAggFirst
{
	bool		ok;				/* the child has that form (planner) */
	bool		use;			/* the planner expects the lookup table to cost less than running the child per input */
	Oid			relid;
	int			rows;			/* the planner's rows of the table: the child runs this many eighths of before the table is read (hybrid) */
	int			nkey;
	int			keyatt[SIFT_AGG_MAX_JKEYS];	/* column of the table */
	Oid			keytype[SIFT_AGG_MAX_JKEYS];
	int			keyarg[SIFT_AGG_MAX_JKEYS];	/* position of the parameter, in the join's params and args */
	int			ncol;			/* output columns of the child */
	int			col[SIFT_AGG_MAX_FOUT];		/* the column of the table each is, 0 when it is not one */
	int			nord;			/* the ORDER BY columns of the child: its row is the key's best by them (a unique index holds the key and these) */
	int			ordatt[SIFT_AGG_MAX_FORD];	/* column of the table */
	Oid			ordtype[SIFT_AGG_MAX_FORD];
	bool		orddesc[SIFT_AGG_MAX_FORD];
	bool		ordnullsfirst[SIFT_AGG_MAX_FORD];
	bool		hybrid;			/* the key may repeat (no unique index) or the child tests its parameters alone: the table is loaded with the rows of a key that agree on the columns read, and the child runs for the keys that do not */
	Node	   *pqual;			/* what the child tests of its parameters alone (a condition without a column of the table): the child has no row where it does not hold */
	List	   *qual;			/* what else the child tests of its row (comparisons of columns with constants, null tests), over the table's Vars: the lookup table holds the rows that pass */
	List	   *params;			/* executor only: the child's parameters once the join is a lookup table (EXPLAIN names them) */
} SiftAggFirst;

typedef struct SiftAggJoin
{
	Oid			relid;			/* the table; InvalidOid when the rows are those of a plan (child) */
	int			child;			/* index in the node's child plans (CustomScan.custom_plans): a sub-select, VALUES list or function; -1: the table */
	SiftJoinKind kind;
	int			subplan;		/* native plan ID: a correlated child, or the plan of a child in another query level; 0 for a once-loaded lookup */
	List	   *params;		/* native PARAM_EXEC IDs supplied for each cached input */
	List	   *args;			/* their expressions over the combo inputs */
	Node	   *sig;			/* native correlated plan: its query with the parameters numbered by position, so that two arms with the same child are known as such */
	int			nkey;
	int			keyatt[SIFT_AGG_MAX_JKEYS];		/* column of the joined table */
	Oid			keytype[SIFT_AGG_MAX_JKEYS];
	int			keypos[SIFT_AGG_MAX_JKEYS];		/* position of the leaf in the combo; -1: the key is keyexpr */
	Node	   *tkey[SIFT_AGG_MAX_JKEYS];		/* the key is this expression of the joined table's columns (a cast, keyatt 0) evaluated for each row loaded; NULL: the column keyatt */
	Node	   *keyexpr[SIFT_AGG_MAX_JKEYS];	/* an expression of the combo's leaves (Params), evaluated per distinct tuple of their values */
	bool		keynull[SIFT_AGG_MAX_JKEYS];	/* NULL equals NULL (an element of a jsonb array): a NULL key finds the rows whose key is NULL */
	bool		scoped;			/* resolved for the elements of an array by a SAP_ELEM_EXISTS, not by a stage of the tuples; always a correlated child (params) */
	int			fslot;			/* slot of the combo that is the rest of the ON clause (leaves and columns of the row), -1 none: a row matches if it holds */
	int			nidx;			/* conjuncts of the rest of the ON clause `column = value` or `column = ANY (array)` (a constant, parameter or stable expression): a btree index can load the rows */
	int			idxatt[SIFT_AGG_MAX_JIDX];
	Oid			idxop[SIFT_AGG_MAX_JIDX];		/* the equality operator, column on the left */
	bool		idxany[SIFT_AGG_MAX_JIDX];		/* the value is an array: the column equals one of its elements */
	Oid			idxcoll[SIFT_AGG_MAX_JIDX];
	Node	   *idxval[SIFT_AGG_MAX_JIDX];
	int			ncol;
	int			colatt[SIFT_AGG_MAX_JCOLS];
	Oid			coltype[SIFT_AGG_MAX_JCOLS];
	int32		coltypmod[SIFT_AGG_MAX_JCOLS];
	Oid			colcoll[SIFT_AGG_MAX_JCOLS];
	SiftAggFirst first;			/* a correlated child that is a lookup of one row by unique key */
	char	   *desc;
} SiftAggJoin;

typedef struct SiftAggSlot
{
	bool		is_key;			/* the id of the value in the output table of key `key` (of aggregate `agg`, if >= 0), or a boolean */
	int			key;
	int			agg;
	Node	   *expr;			/* over Params 0..nleaves-1 (the leaves), then the join columns */
	bool		direct;			/* a combo without a join: the value itself (a number into the combo's values, 0 NULL), for the aggregates SAA_X_* */
} SiftAggSlot;

/*
 * A combo evaluates expressions of up to SIFT_AGG_MAX_CLEAVES inputs (source leaves or columns of earlier lookup outputs)
 * and, with a join, of the columns of the matching row with
 * PostgreSQL's own evaluator, once per distinct tuple of the leaves' values (dictionary entries are compared by value). A combo
 * with a join gives the slots of every output row of a tuple: its matches, or one row of NULLs.
 */
typedef struct SiftAggCombo
{
	int			nleaves;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	int			refcombo[SIFT_AGG_MAX_CLEAVES]; /* leaf -1: an earlier lookup output */
	int			refcol[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	int			join;			/* index in SiftAggSpec.join, -1 none */
	int			level;			/* of the leaves: 0 row, 1 element */
	int			nslots;
	SiftAggSlot *slot;
} SiftAggCombo;

static inline bool
is_ordered_agg(const SiftAggAgg *a)
{
	return a->kind == SAA_PCT_CONT || a->kind == SAA_PCT_DISC;
}

/* mode() WITHIN GROUP (ORDER BY x) is a SAA_PCT_DISC with the fraction -1 (the fractions of a percentile are in [0, 1]): the value of most rows, the first of them in the order of the sort on a tie */
bool sift_aggspec_gtopn_kind(const SiftAggAgg *a);

static inline bool
osa_is_mode(const SiftAggAgg *a)
{
	return a->kind == SAA_PCT_DISC && a->frac < 0;
}

/* percentile_disc over a 64-bit integer, a timestamp or a decimal (the integer over 10^scale): its values are kept as the integers, a double holds only 53 bits */
static inline bool
osa_int_key(const SiftAggAgg *a)
{
	return a->kind == SAA_PCT_DISC && (a->argtype == INT8OID || a->argtype == TIMESTAMPOID || a->argtype == TIMESTAMPTZOID || a->argtype == NUMERICOID);
}

#endif							/* SIFT_AGG_SPEC_JOIN_H */
