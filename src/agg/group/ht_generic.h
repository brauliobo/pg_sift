/*
 * agg/group/ht_generic.h - the group keys of a type the salted table (agg/group/ht.h) cannot hash by its bytes: float8 (-0.0 and 0.0, and every NaN, are one group), numeric (1.0 and 1.00 are one
 * group), and every other type that has a hash function and an equality operator in its default operator classes (interval, money, bytea, enum, arrays, ranges, network types, ...).
 *
 * The descriptor comes from the type cache (lookup_type_cache: the hash function, the equality operator and the comparison function of the type) and is used with the collation of the key, so that two values
 * are one group exactly when the type's own equality says so. A key column of this kind is interned: each distinct value gets a dense code (1, 2, ...) the first time it is met, and the code
 * is the 8-byte integer key the table groups by. The slots of the table (SiftAggSlot64), its probe and its wide-key arena are those of an integer key and stay as they are; the value of a code is
 * the datum the interner kept (a copy, in its own memory context), and it is what a group of that key leaves as.
 */
#ifndef SIFT_AGG_GROUP_HT_GENERIC_H
#define SIFT_AGG_GROUP_HT_GENERIC_H

#include "postgres.h"

typedef struct SiftGenKey SiftGenKey;

/* The type has a hash function and an equality operator, so that its values can be grouped; false for json, xml, point and the like, which PostgreSQL does not group by hashing either */
extern bool sift_genkey_supported(Oid typid);
/* An empty interner of the values of typid compared under collid, in cx */
extern SiftGenKey *sift_genkey_new(MemoryContext cx, Oid typid, Oid collid);
/* The code of the value: that of an equal value met before, else a new one (the value is copied then). Codes are 1, 2, ...; a NULL has none: the caller keeps it out of the table's key */
extern uint32 sift_genkey_intern(SiftGenKey *k, Datum value);
/* The value of a code the interner gave */
extern Datum sift_genkey_datum(const SiftGenKey *k, uint32 code);
/* The comparison function of the type (-1, 0, 1), for an order over the distinct values */
extern int	sift_genkey_cmp(SiftGenKey *k, Datum a, Datum b);
/* How many distinct values and the bytes they take, with the interner's own tables */
extern uint32 sift_genkey_count(const SiftGenKey *k);
extern uint64 sift_genkey_bytes(const SiftGenKey *k);
extern Oid	sift_genkey_typid(const SiftGenKey *k);
/* The hash of the value of a code: the one the interner compares by, which equal values share whatever their bytes */
extern uint32 sift_genkey_code_hash(const SiftGenKey *k, uint32 code);
/* The interner emptied, its values and tables given back: no code stands for anything */
extern void sift_genkey_reset(SiftGenKey *k);
extern void sift_genkey_free(SiftGenKey *k);

#endif							/* SIFT_AGG_GROUP_HT_GENERIC_H */
