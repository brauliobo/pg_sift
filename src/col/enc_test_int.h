/*
 * col/enc_test_int.h - shared between the files of the L1 tests (enc_test.c, gen_test.c, schema_test.c, batch_test.c, chunk_test.c, unit_test.c).
 */
#ifndef SIFT_ENC_TEST_INT_H
#define SIFT_ENC_TEST_INT_H

#include "common/pg_prng.h"

#include "col/enc.h"

typedef struct TCtx
{
	pg_prng_state rng;
	int64		checks;
	int64		failures;
	uint32		encodings;		/* bit per chunk encoding written: CONST, PACK, FOR 8, 16, 32, 64; bit 9: the chunk codec, bit 10: string lists, bit 11: inline text */
	bool		wide;			/* the next text leaf gets tens of thousands of distinct values (codes of 16 and 17 bits) */
	bool		full;			/* this round is a full superblock: 256 dense pages */
} TCtx;

static inline uint32
dict_len(const SiftCDict *d, uint32 code)
{
	return d->off[code + 1] - d->off[code];
}

/* Counts a check; the first few failures are reported as warnings so that the regression output shows them */
#define CHECK(t, cond, ...) \
	do { \
		(t)->checks++; \
		if (!(cond)) \
		{ \
			if (++(t)->failures <= 5) \
				elog(WARNING, "col_enc check failed at line %d: %s", __LINE__, psprintf(__VA_ARGS__)); \
		} \
	} while (0)

#define RAND(t, n)		((uint32) pg_prng_uint64_range(&(t)->rng, 0, (n) - 1))
#define CHANCE(t, p)	((p) > 0 && pg_prng_double(&(t)->rng) < (p))

typedef struct LeafProf
{
	int			mode;			/* INT: 0 constant, 1 narrow, 2 wide, 3 clustered; F: 0 specials, 1 integers, 2 any; STR: pool */
	int			bits;
	int64		base;
	double		pnull;
	double		povf;
	uint32		npool;
	char	  **pool;
	uint32	   *plen;
	double		step;
	bool		seqpick;		/* STR: the pool entry is picked by the row number, so every value appears */
} LeafProf;

typedef struct Case
{
	SiftColSchema *schema;
	LeafProf   *prof;
	uint32		sb;
} Case;

typedef struct Ref
{
	SiftColValue *v;
	uint32		n;
} Ref;

/* Runs fn and returns the SQLSTATE it raised (0: none) */
#define ERRCODE_OF(stmt) \
	({ \
		int			code_ = 0; \
		MemoryContext old_ = CurrentMemoryContext; \
		PG_TRY(); { stmt; } \
		PG_CATCH(); \
		{ \
			ErrorData *e_; \
			MemoryContextSwitchTo(old_); \
			e_ = CopyErrorData(); \
			code_ = e_->sqlerrcode; \
			FlushErrorState(); \
		} \
		PG_END_TRY(); \
		code_; \
	})

static inline bool
ref_null(const SiftColValue *v)
{
	return v->isnull || v->overflow;
}

/* gen_test.c */
extern uint8 store_type(SiftColStore s);
extern Case *gen_case(TCtx *t);
extern SiftKey *gen_keys(TCtx *t, uint32 sb, uint32 nrows, uint32 *nout);
extern SiftColRow *gen_rows(TCtx *t, const Case *c, const SiftKey *keys, uint32 n);
extern SiftColBatch *batch_of(const Case *c, const SiftColRow *rows, uint32 n);
extern Ref ref_leaf(const Case *c, const SiftColRow *rows, uint32 n, int col);
extern bool naive_row(SiftColStore store, SiftColOp op, const SiftColValue *cs, int nc, const SiftColValue *v);
extern void gen_pred(TCtx *t, const SiftColDef *d, const Ref *ref, SiftColPred *p, SiftColValue *consts, int maxconsts);

/* schema_test.c */
extern void check_schema(TCtx *t, const Case *c);
extern void check_dict(TCtx *t);

/* batch_test.c */
extern void check_batch_rows(TCtx *t, const char *what, const Case *c, const SiftColBatch *b, const SiftColRow *rows, uint32 n);
extern void check_merge(TCtx *t, const Case *c, const SiftKey *keys, uint32 n);
extern void check_vers(TCtx *t, const Case *c, const SiftColRow *rows, uint32 n);
extern void check_errors(TCtx *t, const Case *c, const SiftColRow *rows, uint32 n);

/* chunk_test.c */
extern void check_zone(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCZone *z, bool tags);
extern void check_zone_inline(TCtx *t, const char *what, const Ref *ref, const SiftCZone *z, bool tags);
extern void check_tests(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch, const SiftCZone *z, bool direct);
extern void check_strs(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch);
extern void check_lengths(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch);
extern void check_decode(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch);
extern void check_tags(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch, bool tags);
extern void check_tags_corrupt(TCtx *t, const char *what, const uint8 *chunk, uint32 len);

/* unit_test.c */
extern SiftCcPolicy *random_policy(TCtx *t, int ncols);
extern void check_unit(TCtx *t, const Case *c, const SiftColBatch *b, const SiftColRow *rows, uint32 n, const SiftCcPolicy *pol);

#endif							/* SIFT_ENC_TEST_INT_H */
