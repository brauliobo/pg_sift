/*
 * agg/cost/card_est.h - the cardinality of a scan from what the index already knows: no page of the index is read and nothing is sampled at plan time. The
 * statistics of a leaf (a column or a path of a jsonb) are folded when a part is written: the rows, the NULLs, the documents that hold a key, the range of the
 * values, the most common ones, the fences of 16 equal-depth buckets, the number of distinct values; and a deterministic sample of up to 64 granules keeps the zone
 * of every leaf, so that the predicates of a query are tried against the same granules together.
 *
 * sift_card_estimate runs a query's predicates over them in four steps: the partitions its window leaves (the rows of the partitions that overlap it, by the part
 * of each that does), the share of each predicate alone from the statistics of its leaf, the share of all of them from the granules of the sample (a granule a zone
 * rules out for any predicate has none, a granule a zone covers for all keeps its rows, the others take the predicates' shares most selective first, each under
 * the square root of the one before: a conjunction is never less selective than its best predicate, nor as selective as the product of independent ones), and the
 * bounds those shares have: not above the best predicate, not below their product unless the sample is every granule and proves there is none. A scope proves its
 * rows by the root of its keys. The distinct values a group by or a join key keeps under the rows left follow Yao's formula.
 *
 * What the planner gets is the estimate with how far it can be trusted (confidence: 1 when every predicate was answered by what was counted, less where a
 * default stands in), so that it replaces PostgreSQL's own only when it knows better. Nothing here reads shared memory: the structures are a view of the summary
 * the manifest keeps, a call takes a few microseconds, and it allocates only for an IN list of more than 256 values (sorted once, a copy).
 */
#ifndef SIFT_AGG_V6_CARD_EST_H
#define SIFT_AGG_V6_CARD_EST_H

#include "postgres.h"

#define SIFT_CARD_SAMPLE_GRANULES	64
#define SIFT_CARD_MCV_MAX			16
#define SIFT_CARD_HIST_BUCKETS		16
#define SIFT_CARD_MAX_PREDS			32
#define SIFT_CARD_SCOPE_PROBES		16

#define SIFT_CSTAT_HAS_RANGE		(UINT16_C(1) << 0)
#define SIFT_CSTAT_HAS_NDV			(UINT16_C(1) << 1)
#define SIFT_CSTAT_HAS_KEY_PRESENT	(UINT16_C(1) << 2)
#define SIFT_CSTAT_HAS_HIST			(UINT16_C(1) << 3)
#define SIFT_CSTAT_HAS_MCV			(UINT16_C(1) << 4)
#define SIFT_CSTAT_HAS_SKIP			(UINT16_C(1) << 7)

#define SIFT_MCV_IS_HASH			1

/* A common value: the value of an integer leaf, the hash of a string one */
typedef struct SiftCardMcv
{
	uint64		val_or_hash;
	float4		freq;			/* of the rows of the leaf */
	uint32		flags;
} SiftCardMcv;

StaticAssertDecl(sizeof(SiftCardMcv) == 16, "SiftCardMcv is 16 bytes");

typedef struct SiftColCardStats
{
	uint16		col;
	uint16		flags;
	uint8		n_hist;			/* the fences are n_hist + 1 */
	uint8		n_mcv;
	uint8		pad[2];
	uint64		total_rows;
	uint64		null_rows;
	uint64		key_present_rows;	/* the documents that hold the key of the leaf */
	double		ndv_est;
	double		avg_comp_bytes_row;
	double		avg_dec_bytes_row;
	double		zone_overlap_ratio;	/* 0: the zones follow one another, 1: every zone spans the range */
	double		zone_monotonicity;	/* -1 .. 1: the order of the zones along the parts */
	int64		min_i64;
	int64		max_i64;
	int64		hist_bounds[SIFT_CARD_HIST_BUCKETS + 1];
	SiftCardMcv mcvs[SIFT_CARD_MCV_MAX];
} SiftColCardStats;

#define SIFT_CZONE_KEY_NONE			1	/* no document of the granule holds the key */
#define SIFT_CZONE_KEY_ALL			2	/* every document does */

typedef struct SiftCardZone
{
	int64		min;
	int64		max;
	uint32		nnull;
	uint32		flags;
} SiftCardZone;

StaticAssertDecl(sizeof(SiftCardZone) == 24, "SiftCardZone is 24 bytes");

typedef struct SiftSampleGranule
{
	uint32		part_idx;
	uint16		nrows;
	uint16		pad;
	float4		weight;
	uint32		pad2;
} SiftSampleGranule;

typedef struct SiftCardPartition
{
	uint64		rows;			/* live rows */
	int64		lo;				/* bounds of the leading key or time column of the partition, both inclusive */
	int64		hi;
} SiftCardPartition;

typedef struct SiftCardScopeRoot
{
	int64		key;
	uint64		nrows;
} SiftCardScopeRoot;

/* The summary of one index as the manifest keeps it (col/f2/f2_card.h folds it; sample_zones is [ncols][n_sample]) */
typedef struct SiftCardStatsView
{
	uint64		total_live_rows;
	uint64		total_granules;
	uint64		total_bytes;	/* what the parts of the index take */
	uint32		ncols;
	uint32		nparts;
	uint32		nfiles;			/* the parts of every partition: a scan opens each of them before its zones leave a granule */
	uint32		n_sample;
	bool		has_l0;			/* rows not yet in a part exist, and no partition describes them */
	bool		rows_from_pg;	/* the rows of a scan are PostgreSQL's own: the view prices the granules a scan reads, and the groups it makes, not the rows of the relation */
	const SiftColCardStats *cols;
	const SiftCardPartition *parts;
	const SiftSampleGranule *sample;
	const SiftCardZone *sample_zones;
	const SiftCardScopeRoot *scope;	/* sorted by key */
	uint32		n_scope;
	uint64		scope_total_rows;	/* the sum of the nrows of the roots (0: not folded, the roots are summed) */
} SiftCardStatsView;

typedef enum SiftCardPredKind
{
	SCP_IS_NULL,
	SCP_IS_NOT_NULL,
	SCP_KEY_EXISTS,				/* jsonb_exists(doc, key), doc ? key */
	SCP_KEY_ABSENT,				/* NOT jsonb_exists(doc, key) */
	SCP_EQ,
	SCP_IN,
	SCP_RANGE,					/* lo <= leaf <= hi, an open side has no has_ flag; the caller turns < and > into inclusive bounds of an integer leaf */
	SCP_LIKE_PREFIX,
	SCP_SCOPE_IN				/* the member rows of the scope keys */
} SiftCardPredKind;

typedef struct SiftCardPred
{
	SiftCardPredKind kind;
	uint16		col;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
	const uint64 *vals;			/* EQ (one), IN, SCOPE_IN: the values, or their hashes when is_hash */
	uint32		nvals;
	bool		is_hash;
	bool		unknown_len;	/* IN over a parameter array: its length is not known */
	bool		dict_absent;	/* a dictionary of the leaf proves that the value is not in it */
	uint32		dict_lo;		/* LIKE: the codes of the prefix in a dictionary of dict_n values (dict_n 0: no dictionary) */
	uint32		dict_hi;
	uint32		dict_n;
} SiftCardPred;

typedef struct SiftCardQuery
{
	const SiftCardPred *preds;
	uint32		npreds;
	bool		has_window;		/* the partitions are those that overlap [win_lo, win_hi] */
	int64		win_lo;
	int64		win_hi;
	int32		lead_col;		/* the leaf the scan or sort leads with, -1 for none */
	const uint16 *payload_cols;
	uint32		npayload;
	bool		has_uncovered;	/* quals remain that no leaf describes */
} SiftCardQuery;

typedef struct SiftCardEst
{
	double		total_live_rows;
	double		est_rows;
	double		selectivity;
	double		g_eff;
	double		g_total;
	double		b_pred_comp;
	double		b_pay_dec;
	double		key_correlation;
	double		confidence;
	bool		exact_zero;
	bool		used_scope_l2;
	double		decided[SIFT_CARD_MAX_PREDS];	/* of each predicate, the share of the rows of the granules kept whose zone decides it for every row (sampled granules; 0 without a sample) */
	bool		has_uncovered_quals;
} SiftCardEst;

extern void sift_card_estimate(const SiftCardStatsView *s, const SiftCardQuery *q, SiftCardEst *out);

/* The mean of the values of an integer leaf that are not NULL: of the midpoints of its equal-depth buckets, or of its range when it has none; -1 without a range */
extern double sift_card_mean(const SiftColCardStats *cs);
/* The distinct values of a leaf of d (of n rows) that r_out rows keep */
extern double sift_card_ndv(double d, double n, double r_out);
/* The groups of several keys, each with the distinct values d[i] it keeps, over r_out rows; cells: the groups the pre-aggregate has (0: none) */
extern double sift_card_ndv_multi(const double *d, int nkeys, double r_out, double cells);
/* The rows of the scope keys: exact when a root holds each (at most SIFT_CARD_SCOPE_PROBES keys), else the mean root times the keys */
extern double sift_card_scope_rows(const SiftCardStatsView *s, const uint64 *keys, uint32 nkeys, bool *exact);

#endif							/* SIFT_AGG_V6_CARD_EST_H */
