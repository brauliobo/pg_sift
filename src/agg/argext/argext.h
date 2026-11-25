/*
 * agg/argext/argext.h - sift.arg_max(val, cmp), sift.arg_min(val, cmp) and DISTINCT ON in SiftAggV6, with ClickHouse's argMax / argMin semantics: the value of val on the row where cmp is greatest
 * (least); a row whose cmp is NULL is ignored, a NULL val on the winning row is the result (NULL), and a group with no row whose cmp is not NULL has no result (NULL). When two rows tie on cmp the first
 * one offered wins, as with ORDER BY .. LIMIT 1 over a stable scan; which of them PostgreSQL's own aggregate returns after a parallel combine is as unspecified as its ORDER BY is.
 *
 * Every comparison is a memcmp of an order-preserving key (sift_ord_*: ASC or DESC, NULLS FIRST or LAST, int8, float8 with NaN above everything, bytes), so a cmp of any type and a DISTINCT ON with any
 * number of ORDER BY columns after the DISTINCT ON keys take the same path; one 64-bit word holds the key of the common case (a time or an integer) and its kernels read the vectors directly.
 *
 * SiftArgExt keeps the best row of each of a dense range of groups (the group ids of agg/ht/ht.h); two states merge by one more offer of each group's best, so a parallel combine is the same operation
 * as the scan. SiftDisOn is DISTINCT ON without the sort: a hash of the DISTINCT ON key to the best row (its ORDER BY key and a 64-bit tag, the row id the caller fetches the
 * row by); over its budget the groups and the rows that follow go to partitions on temporary files, which are solved one at a time, so the groups leave in no order (the caller sorts the G groups
 * when the query orders by the DISTINCT ON keys: G rows, not the N of the input).
 */
#ifndef SIFT_AGG_V6_ARGEXT_H
#define SIFT_AGG_V6_ARGEXT_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/batch.h"
#include "agg/bytes.h"

/* ---- order-preserving keys: memcmp of two keys of the same spec is the order of PostgreSQL ---- */

typedef struct SiftOrdSpec
{
	bool		desc;
	bool		nulls_first;	/* PostgreSQL's default: NULLS LAST for ASC, NULLS FIRST for DESC */
} SiftOrdSpec;

extern void sift_ord_null(SiftBytes *k, SiftOrdSpec s);
extern void sift_ord_i64(SiftBytes *k, SiftOrdSpec s, int64 v);
extern void sift_ord_f64(SiftBytes *k, SiftOrdSpec s, double v);
extern void sift_ord_bytes(SiftBytes *k, SiftOrdSpec s, const void *p, uint32 len);
/* The 64-bit key of an int8 and of a float8 for ASC: the unsigned order of the word is the order of the value */
extern uint64 sift_ord_u64_of_i64(int64 v);
extern uint64 sift_ord_u64_of_f64(double v);

/* ---- arg_max, arg_min ---- */

typedef enum SiftArgDir
{
	SIFT_ARG_MAX,
	SIFT_ARG_MIN
} SiftArgDir;

typedef enum SiftArgMode
{
	SIFT_ARG_I64,				/* cmp an int8, int4, date, timestamp or float8 (one word), val an int8 or int4 (one word) */
	SIFT_ARG_BYTES				/* cmp an order-preserving key, val bytes */
} SiftArgMode;

typedef struct SiftArgExt SiftArgExt;

extern SiftArgExt *sift_argext_create(MemoryContext cx, SiftArgDir dir, SiftArgMode mode);
extern void sift_argext_offer_i64(SiftArgExt *a, uint32 gid, uint64 cmp_key, int64 val, bool val_null);
extern void sift_argext_offer(SiftArgExt *a, uint32 gid, const void *cmp_key, uint32 cmp_len, const void *val, uint32 val_len, bool val_null);
/* The active rows of the batch into the groups gid[row] (NULL: group 0); cmp is an I64, I32 or F64 vector, val an I64 or I32 one (NULL: the row number of the batch as the value) */
extern void sift_argext_update(SiftArgExt *a, const SiftBatchSel *bs, const uint32 *gid, const SiftVec *cmp, const SiftVec *val);
/* The best rows of src into a: the combine of two partial states */
extern void sift_argext_merge(SiftArgExt *a, const SiftArgExt *src);
extern bool sift_argext_has(const SiftArgExt *a, uint32 gid);
/* false: the group has no row, or its winner's val is NULL */
extern bool sift_argext_val_i64(const SiftArgExt *a, uint32 gid, int64 *val);
extern bool sift_argext_val_bytes(const SiftArgExt *a, uint32 gid, const uint8 **val, uint32 *len);
extern uint32 sift_argext_ngroups(const SiftArgExt *a);
extern uint64 sift_argext_bytes(const SiftArgExt *a);
extern void sift_argext_free(SiftArgExt *a);

/*
 * The state of one group in the 32-byte accumulator of agg/group/group.c (n, v0, v1, pad): n counts the rows whose cmp is not NULL (0: no result), key is the order key of the best cmp, val its val, val_null
 * says the val is NULL. These are the offer and the merge of SiftArgExt on those four words, so the accumulator and the state of the heap path give the same answer.
 */
static inline bool
sift_argacc_better(SiftArgDir dir, uint64 cmp_key, uint64 best_key)
{
	return dir == SIFT_ARG_MAX ? cmp_key > best_key : cmp_key < best_key;
}

static inline void
sift_argacc_offer(int64 *n, uint64 *key, int64 *val, int64 *val_null, SiftArgDir dir, uint64 cmp_key, int64 v, bool v_null)
{
	if (*n == 0 || sift_argacc_better(dir, cmp_key, *key))
	{
		*key = cmp_key;
		*val = v;
		*val_null = v_null;
	}
	(*n)++;
}

static inline void
sift_argacc_merge(int64 *dn, uint64 *dkey, int64 *dval, int64 *dnull, SiftArgDir dir, int64 sn, uint64 skey, int64 sval, int64 snull)
{
	if (sn == 0)
		return;
	if (*dn == 0 || sift_argacc_better(dir, skey, *dkey))
	{
		*dkey = skey;
		*dval = sval;
		*dnull = snull;
	}
	*dn += sn;
}

/* The order key of row r of a cmp vector (I32, I64 or F64): the unsigned order of the word is the order of PostgreSQL */
static inline uint64
sift_argext_cmp_key(const SiftVec *cmp, uint32 r)
{
	switch (cmp->type)
	{
		case SVEC_I32:
			return sift_ord_u64_of_i64(((const int32 *) cmp->data)[r]);
		case SVEC_F64:
			return sift_ord_u64_of_f64(((const double *) cmp->data)[r]);
		default:
			return sift_ord_u64_of_i64(((const int64 *) cmp->data)[r]);
	}
}

/* ---- DISTINCT ON ---- */

typedef struct SiftDisOnStats
{
	uint64		rows;
	uint64		spilled_records;
	uint64		spilled_bytes;
	uint32		passes;
	uint32		max_level;
	uint64		peak_bytes;
} SiftDisOnStats;

typedef struct SiftDisOn SiftDisOn;
typedef void (*SiftDisOnEmit) (void *arg, uint64 tag, const uint8 *key, uint32 klen);

extern SiftDisOn *sift_dison_create(MemoryContext cx, uint64 budget_bytes, uint64 expect_bytes);
/* key: the encoded DISTINCT ON columns (equal bytes, equal group); ord: the encoded ORDER BY columns after them (the least wins); tag: what the caller fetches the row by */
extern void sift_dison_offer(SiftDisOn *d, const void *key, uint32 klen, const void *ord, uint32 olen, uint64 tag);
/* One int8 DISTINCT ON key and one ORDER BY column: the rows of the batch, tag = tag_base + row; a NULL key is a group of its own */
extern void sift_dison_update_i64(SiftDisOn *d, const SiftBatchSel *bs, const SiftVec *key, const SiftVec *ord, SiftOrdSpec ord_spec, uint64 tag_base);
/* Every group once, as its best row; the set is empty after it */
extern void sift_dison_finish(SiftDisOn *d, SiftDisOnEmit emit, void *emit_arg);
extern uint64 sift_dison_bytes(const SiftDisOn *d);
extern const SiftDisOnStats *sift_dison_stats(const SiftDisOn *d);
extern void sift_dison_free(SiftDisOn *d);

#endif							/* SIFT_AGG_V6_ARGEXT_H */
