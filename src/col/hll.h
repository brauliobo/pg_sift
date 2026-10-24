/*
 * col/hll.h - the HyperLogLog sketch of sift.approx_count_distinct: 16,384 registers of a
 * 64-bit hash, relative standard error 1.04 / sqrt(16384) = 0.81%. In memory it is a list of (register, rank) entries that becomes the register array past
 * SIFT_HLL_SPARSE entries; stored in a unit (column format minor 20, the option hll_columns) it is the smaller of the sorted list and the packed registers.
 * The estimate is a function of the registers alone, so every plan, merge order and stored sketch gives the same number for the same set of values.
 */
#ifndef SIFT_COL_HLL_H
#define SIFT_COL_HLL_H

#include "core/host.h"

#include "col/enc.h"
#include "col/preagg.h"

#define SIFT_HLL_P			14
#define SIFT_HLL_M			(1 << SIFT_HLL_P)
#define SIFT_HLL_SPARSE		4096			/* entries (3 bytes each) at which the list is as large as the packed registers (6 bits each) */
#define SIFT_HLL_HDR		8
#define SIFT_HLL_TOP_RANK	(65 - SIFT_HLL_P)	/* the rank of a hash whose 50 low bits are zero */
#define SIFT_HLL_LIST		1				/* stored form: n entries of 3 bytes, ascending by register */
#define SIFT_HLL_DENSE		2				/* stored form: SIFT_HLL_M registers of 6 bits */

typedef struct SiftHll
{
	MemoryContext cxt;
	uint32		n;				/* entries in e (unsorted, a register may repeat) while sparse */
	uint32		cap;
	uint32	   *e;				/* register << 6 | rank */
	uint8	   *r;				/* the register array once dense, else NULL */
} SiftHll;

extern void sift_hll_init(SiftHll *h, MemoryContext cxt);
extern void sift_hll_free(SiftHll *h);
extern void sift_hll_add(SiftHll *h, uint64 hash);
/* Merges a stored sketch (sift_hll_check passed) or another one into h */
extern void sift_hll_merge_blob(SiftHll *h, const uint8 *blob);
extern void sift_hll_merge(SiftHll *h, const SiftHll *o);
/* The estimate, rounded: 0 for an empty sketch */
extern int64 sift_hll_estimate(SiftHll *h);
/* Appends the canonical stored form (sparse when it has at most SIFT_HLL_SPARSE registers set, else packed; empty: nothing and false) */
extern bool sift_hll_encode(SiftHll *h, StringInfo out);
/* Whether a stored blob is well formed: form, count, ascending registers, ranks within 1..SIFT_HLL_TOP_RANK, zero padding, the canonical choice of form */
extern bool sift_hll_check(const uint8 *blob, uint32 len);
/*
 * The stored sketch of the values of a leaf in a unit, appended to out; false (nothing appended) when the unit has no value or one overflowed (the sketch
 * would miss it), or the leaf has no keys. dict: the segment's dictionary of a text or uuid leaf, whose keys are its codes. The writer and the verifier both build it here.
 */
extern bool sift_hll_build(const SiftColDef *def, const SiftCPaggCol *c, const SiftCDict *dict, StringInfo out);

/* The hash of a key of an integer leaf as PostgreSQL's extended hash of its type: int2, int4 and date hash as int4, int8 and the timestamps as int8 */
extern uint64 sift_hll_hash_i32(int32 v);
extern uint64 sift_hll_hash_i64(int64 v);
/* The hash of the bytes of a text, varchar or uuid value (hashtextextended of a deterministic collation, uuid_hash_extended) */
extern uint64 sift_hll_hash_bytes(const void *p, int len);

/* The option hll_columns, resolved like order_columns: the leaves that keep a sketch in every unit */
extern const char *sift_hll_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_hll_syntax(const char *opt);
/* An int2, int4, int8, date, timestamp, timestamptz, text, varchar or uuid column of the top level whose values the unit can hash */
extern bool sift_hll_leaf_ok(const SiftColSchema *s, const SiftColDef *d);

#endif							/* SIFT_COL_HLL_H */
