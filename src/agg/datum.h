/*
 * agg/datum.h - SVEC_DATUM, the vector of a PostgreSQL type the native vectors of agg/vec.h do not hold: Datum[n] with the type, length and pass-by-value flag of its values on the SiftVec, and the
 * conversions between it and the native vectors for the rows of a batch.
 *
 * The native vectors stay what the hot path runs on: int2, int4, int8, date, timestamp, timestamptz, bool, float4, float8, text and varchar. A value of any other type (numeric of a column the scan keeps
 * as scaled integers aside, interval, money, bytea, arrays, ...), or a result of a PostgreSQL expression of such a type, is a SVEC_DATUM; so is nothing else. The Datums of a vector are valid for the
 * batch that made them: the memory they point into is the one of the batch (the per-tuple memory of v6_pg_expr.h), reset once for the next one.
 */
#ifndef SIFT_AGG_V6_DATUM_H
#define SIFT_AGG_V6_DATUM_H

#include "postgres.h"

#include "lib/stringinfo.h"

#include "agg/ht/generic.h"
#include "agg/engine.h"
#include "agg/vec.h"

/* The room of a SVEC_DATUM vector of a batch: its Datums and its validity */
typedef struct SiftDatumOut
{
	SiftVec		vec;
	Datum	   *vals;
	uint64		valid[SIFT_VEC_WORDS];
} SiftDatumOut;

extern void sift_datum_out_init(SiftDatumOut *o, MemoryContext cx, Oid typid);

/* The type is one of the native vectors: its values are not Datums in a batch (the base type of a domain counts) */
extern bool sift_v6_type_native(Oid typid);
/* A key column of this type is grouped by the interner of agg/ht/generic.h: every type but those of the integer, boolean and text classes the table groups by their bytes */
extern bool sift_v6_key_generic(Oid typid);

/*
 * The Datum of row r of a native vector, as a value of typid (scale: the digits after the point of a numeric the vector holds as scaled integers); a value passed by reference is made in the
 * current memory context. The vector is of the class of typid: a numeric of NUM64 or NUM128 or of the scaled integers of an INT64 column, an integer of any width, a float, a string view, a uuid or an interval.
 */
extern Datum sift_v6_native_datum(const SiftVec *v, uint16 r, Oid typid, int scale);

/* The numeric NaN */
extern Datum sift_v6_numeric_nan(void);
/* The same for a row that may be NULL or a NaN (a numeric whose NaN the vector marks and calls invalid): *isnull says NULL, and a NaN is the datum PostgreSQL has for it */
extern Datum sift_v6_vec_datum(const SiftVec *v, uint16 r, Oid typid, int scale, bool *isnull);

/* The result d of a PostgreSQL expression of the native type typid, into row r of the data of a native vector: an integer, a double or a string view of the bytes of the datum */
extern void sift_v6_result_put(Oid typid, void *data, uint16 r, Datum d);
/* The type of the native vector the results of typid are (SVEC_I64, SVEC_F64 or SVEC_STRVIEW) */
extern SiftVecType sift_v6_result_vec_type(Oid typid);

/*
 * The vector v of a column of typid as the codes of the interner k, for the active rows of bs: codes[r] of a valid row (the NULL ones have no code and are not valid in *valid). scratch holds the datums
 * made on the way (numeric, string); it is reset by the caller once the batch is folded. The vector is native (of typid) or a SVEC_DATUM.
 */
extern void sift_v6_key_codes(SiftGenKey *k, const SiftVec *v, Oid typid, int scale, const SiftBatchSel *bs, MemoryContext scratch, int64 *codes, uint64 *valid);

/*
 * The generic keys of the groups of a statement (SiftV6Group): the keys whose type the table does not group by its bytes (sift_v6_key_generic), each with an interner. The batch of such a key goes to the
 * table as its vector of codes, an int8 key like any other (sift_v6_genkeys_apply), and the group leaves with the value of its code (sift_v6_genkeys_resolve). The spec the groups run on is a copy of the
 * statement's in which those keys are int8, so that the packing, the hashing, the compact table and the spill of the group see integers.
 */
typedef struct SiftV6GenKeys SiftV6GenKeys;

/* NULL when no key of the spec is generic */
extern SiftV6GenKeys *sift_v6_genkeys_new(MemoryContext cx, const SiftV6Spec *spec);
extern const SiftV6Spec *sift_v6_genkeys_spec(const SiftV6GenKeys *g);
/* keyvec with the keys that are generic replaced by their codes for the active rows of bs; valid until the next call */
extern SiftVec *const *sift_v6_genkeys_apply(SiftV6GenKeys *g, const SiftBatchSel *bs, SiftVec *const *keyvec);
/* values[k] of a key that is generic and not NULL: the value of the code the table holds there (only the keys whose bit of want is set) */
extern void sift_v6_genkeys_resolve(const SiftV6GenKeys *g, Datum *values, const bool *isnull, uint32 want);
extern uint64 sift_v6_genkeys_bytes(const SiftV6GenKeys *g);
/*
 * Key k as a value that leaves the interner, for the record of a group that goes to a spill: is key k generic; the hash of the value of a code (equal values share it, whatever the codes of the interner
 * they have); the value of a code serialized onto out; and the code of the value serialized at *p (a value this interner meets for the first time gets a new one), *p after it. The interner emptied
 * (sift_v6_genkeys_reset): no table holds a code of it.
 */
extern bool sift_v6_genkeys_is(const SiftV6GenKeys *g, int k);
extern uint32 sift_v6_genkeys_hash(const SiftV6GenKeys *g, int k, int64 code);
extern void sift_v6_genkeys_put(const SiftV6GenKeys *g, int k, int64 code, StringInfo out);
extern int64 sift_v6_genkeys_get(SiftV6GenKeys *g, int k, const char **p);
extern void sift_v6_genkeys_reset(SiftV6GenKeys *g);
extern void sift_v6_genkeys_free(SiftV6GenKeys *g);

#endif							/* SIFT_AGG_V6_DATUM_H */
