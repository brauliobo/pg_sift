/*
 * col/f2/v6/proj/key.h - the scan keys of a statement as the predicates of col/f2/v6/proj/pred.h: =, <, <=, >=, > and an IN list (a key with SK_SEARCHARRAY) on integers, dates, timestamps,
 * booleans, numerics (as the scaled integer of their leaf), text and varchar (= and IN for any deterministic collation, ordering only for a bytewise one) and uuid.
 */
#ifndef SIFT_COL_F2_V6_PROJ_KEY_H
#define SIFT_COL_F2_V6_PROJ_KEY_H

#include "access/skey.h"
#include "access/tupdesc.h"

#include "col/f2/v6/proj/pred.h"
#include "col/f2/store/store.h"

/* false: no such predicate is exact on cells; the statement reads the base. The constants are allocated in cx. */
extern bool sift_v6_proj_pred_of_key(const SiftF2Store *store, TupleDesc td, const ScanKeyData *sk, SiftProjPred *out, MemoryContext cx);
extern bool sift_v6_proj_preds_of_keys(const SiftF2Store *store, TupleDesc td, const ScanKeyData *keys, int nkeys, SiftProjPred **out, MemoryContext cx);

/* The quals of a FILTER (WHERE ..) of an aggregate, as the scan's quals are (clauses over INDEX_VAR attributes of index), as predicates of a spec; false when one is not exact on cells or there are more than max: the aggregate is then read from the base, never with the filter left out */
extern bool sift_v6_proj_preds_of_quals(const SiftF2Store *store, Relation index, const List *quals, SiftProjSpecPred *out, int max, int *n, MemoryContext cx);

/* Whether strings of the collation compare as their bytes do (an ORDER BY or a range on a text column can then be answered from a projection that keeps its bytes in order) */
extern bool sift_v6_proj_collation_bytewise(Oid collid);

#endif							/* SIFT_COL_F2_V6_PROJ_KEY_H */
