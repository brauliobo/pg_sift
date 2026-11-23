/*
 * col/f2/f2_aux_budget.h - what a part may spend on the sections it carries for the reader's benefit alone: the marks of its key (SEC_KEY_MARKS), the statistics of its columns
 * (SEC_COL_STATS), the skip index (SEC_SKIP_INDEX) and the sketches of quantiles (SEC_KLL). Their sum is at most a hundredth of the bytes of the part's data, and a part that would fit a slot
 * of a packed page (SIFT_PACKED_SLOT_MAX_BYTES) with none of them gets none, so that they never keep it from one. Sections are served in the order of what each saves a query: the zones are
 * never in question (they are not here), then the marks, the statistics, the skip index and the sketches. No setting: the numbers are the part's own.
 * Portable.
 */
#ifndef SIFT_F2_AUX_BUDGET_H
#define SIFT_F2_AUX_BUDGET_H

#include "core/host.h"

#define SIFT_AUX_CAP_PERMILLE		10		/* of the data of the part, all four sections together */
#define SIFT_AUX_COMP_CAP_PERMILLE	15		/* ... and of the data that compresses (what is not a uuid or a raw blob) */
#define SIFT_AUX_MARKS_PERMYRIAD	15		/* of the data of the part, what the marks may take, and as much the statistics */
#define SIFT_AUX_STATS_PERMYRIAD	15
#define SIFT_AUX_SKIP_COMP_PERMILLE	5		/* of the data that compresses, what the skip index may take */
#define SIFT_AUX_MARKS_LOOKUP_NG	1		/* above this many granules the marks are what a lookup of the key reads, not an aid: they are kept at what they take (charged, not capped) */
#define SIFT_AUX_STATS_MIN_ROWS		4096
#define SIFT_AUX_SKIP_MIN_ROWS		16384
#define SIFT_AUX_KLL_MIN_ROWS		32768
#define SIFT_AUX_KLL_FULL_ROWS		131072
#define SIFT_AUX_KLL_SMALL_K		100
#define SIFT_AUX_BLOOM_MIN_NDV		65		/* fewer distinct values are exact in the dictionary, the zones or a set */
#define SIFT_AUX_TRIGRAM_MIN_AVG	12

typedef struct SiftAuxBudget
{
	uint64		cap;			/* bytes all the sections may take together */
	uint64		rem;			/* what is left after those charged */
	uint64		d_total;
	uint64		d_comp;
	uint32		nrows;
	uint32		ng;
	uint32		level;
} SiftAuxBudget;

/*
 * d_total: bytes of the granules and the dictionaries of the part; d_comp: of those, the ones that compress (not the uuids, the raw blobs and the chunks stored as they were); fixed_inline: bytes
 * the part takes in its header page without any of the four sections. A part whose header page and data fit SIFT_PACKED_SLOT_MAX_BYTES with them gets a cap of 0.
 */
extern void sift_aux_begin(SiftAuxBudget *b, uint64 d_total, uint64 d_comp, uint64 fixed_inline, uint32 nrows, uint32 ng, uint32 level);

/* Whether the part gets the marks of its key (nbytes, charged when it does): it has more than one granule to tell apart and either many (SIFT_AUX_MARKS_LOOKUP_NG) or they fit */
extern bool sift_aux_marks(SiftAuxBudget *b, uint64 nbytes);
/* Whether the part gets statistics of its columns (nbytes, charged when it does) */
extern bool sift_aux_col_stats(SiftAuxBudget *b, uint64 nbytes);
/* The bytes the skip index of the part may take: 0 for no skip index; the caller charges what it wrote with sift_aux_charge */
extern uint64 sift_aux_skip_budget(const SiftAuxBudget *b);
/* The k of the sketches of the part: 0 none, the smaller of spec_k and SIFT_AUX_KLL_SMALL_K below SIFT_AUX_KLL_FULL_ROWS, else spec_k */
extern uint32 sift_aux_kll_k(const SiftAuxBudget *b, uint32 spec_k);
/* How many of ncols sketches of bytes_per_col each fit what is left; charges them */
extern uint32 sift_aux_kll_cols(SiftAuxBudget *b, uint32 ncols, uint64 bytes_per_col);
/* Charges bytes that a section took; false when they were more than what was left (rem is then 0) */
extern bool sift_aux_charge(SiftAuxBudget *b, uint64 bytes);

/* A column's bloom filter is of use when its values are not few and the part has granules to rule out */
extern bool sift_aux_bloom_ok(const SiftAuxBudget *b, uint64 ndv);
/* ... and its trigrams when it is text of some length that no dictionary or promotion holds */
extern bool sift_aux_trigram_ok(const SiftAuxBudget *b, bool is_text, uint64 avg_len, bool promoted_or_dict);

#endif
