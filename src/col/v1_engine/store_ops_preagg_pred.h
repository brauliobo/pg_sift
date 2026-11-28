/*
 * col/v1_engine/store_ops_preagg_pred.h - the tests of a statement in the terms the pre-aggregates of a Format 1 unit decide (col/v1_engine/store_ops_preagg.c): a clause of one leaf, and what
 * the zone of the unit, a group of its histogram or a calendar cell says of it.
 */
#ifndef SIFT_V1_STORE_OPS_PREAGG_PRED_H
#define SIFT_V1_STORE_OPS_PREAGG_PRED_H

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodes.h"
#include "utils/relcache.h"

#include "col/v1/reader.h"
#include "col/v1/src_exec.h"

typedef enum SiftV1Verdict
{
	SV1V_NONE,					/* no row passes the test */
	SV1V_SOME,					/* the summary does not tell */
	SV1V_ALL					/* every row passes */
} SiftV1Verdict;

typedef enum SiftV1PredKind
{
	SV1P_NULL,
	SV1P_NOTNULL,
	SV1P_CMP,					/* leaf op val */
	SV1P_ANY,					/* leaf = ANY (set) */
	SV1P_AND,					/* the n tests of kid, all of the leaf */
	SV1P_OR,
	SV1P_CAL					/* a test of the leaf through calendar calls (extract(month FROM d) = 3), made on a group or a cell (col/v1_engine/store_ops_cal.h) */
} SiftV1PredKind;

typedef enum SiftV1CmpOp
{
	SV1O_LT = 1,
	SV1O_LE,
	SV1O_EQ,
	SV1O_GE,
	SV1O_GT,
	SV1O_NE
} SiftV1CmpOp;

typedef struct SiftV1Pred
{
	SiftV1PredKind kind;
	int			leaf;
	SiftV1CmpOp op;
	int64		val;
	int			n;
	int64	   *set;
	text	  **str;			/* a test of a string leaf (a comparison with str[0], = ANY of the n of them): made on the strings of the dictionary of each group, never on the zone */
	Oid			collid;			/* ... the collation of a comparison */
	struct SiftV1Pred *kid;		/* SV1P_AND, SV1P_OR: n of them */
	struct SiftV1CalTest *cal;	/* SV1P_CAL */
} SiftV1Pred;

/* A leaf a cube holds as it is stored: not wide, not lossy, not a numeric (whose integers carry a scale); and one of integers */
static inline bool
sift_v1_leaf_plain(const SiftSrcLeaf *l)
{
	return l != NULL && !l->wide && !l->lossy && l->typid != NUMERICOID;
}

static inline bool
sift_v1_leaf_int(const SiftSrcLeaf *l)
{
	return sift_v1_leaf_plain(l) && (l->vtype == SCV_I32 || l->vtype == SCV_I64);
}

/* The test a clause of the statement makes on one leaf of host (an index or a sift_table); false for any other clause. An OR of ANDs of the tests of one leaf is one test */
extern bool sift_v1_pred_of(Relation host, const SiftSrcLayout *lay, const Node *clause, SiftV1Pred *p);

/* What a zone of the leaf says of the test, its rows included */
extern SiftV1Verdict sift_v1_pred_verdict(const SiftV1Pred *p, const SiftSrcUnitAgg *z);

/* Whether the rows of group r of h pass the test of its dimension */
extern bool sift_v1_pred_passes(const SiftV1Pred *p, const SiftSrcHist *h, uint32 r);

/* What the test of a date or timestamp leaf of type typid says of the rows of the calendar cell r of h (SiftSrcHist.grain), not its NULL group */
extern SiftV1Verdict sift_v1_pred_cell(const SiftV1Pred *p, Oid typid, const SiftSrcHist *h, uint32 r);

/* The tests of quals (clauses over the columns of the table of host) the zones of a unit decide: NULL when one is not such a test; whether the zones of a unit unit_rows answers for say
 * that every row passes them all */
typedef struct SiftV1ZoneTests SiftV1ZoneTests;
extern SiftV1ZoneTests *sift_v1_zone_tests(Relation host, const SiftSrcLayout *lay, const List *quals);
extern bool sift_v1_zone_tests_all(const SiftV1ZoneTests *t, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit);

#endif							/* SIFT_V1_STORE_OPS_PREAGG_PRED_H */
