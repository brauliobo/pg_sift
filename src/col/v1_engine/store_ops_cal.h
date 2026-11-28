/*
 * col/v1_engine/store_ops_cal.h - a key or a test of SiftAggV6 that is an expression of one date or timestamp leaf of a Format 1 unit (date_trunc, extract, ::date: agg/qual/grain.h), answered
 * from the groups of the leaf's histogram or from its calendar cells (col/v1_engine/store_ops_preagg.c): the key of a group is the expression of its value, the key of a cell the expression of
 * its first instant, which is the key of every row of the cell when the expression keeps its value over cells of that grain; a test is decided the same way.
 */
#ifndef SIFT_V1_STORE_OPS_CAL_H
#define SIFT_V1_STORE_OPS_CAL_H

#include "postgres.h"

#include "agg/engine.h"
#include "col/v1/src_exec.h"
#include "col/v1_engine/store_ops_preagg_pred.h"

typedef struct SiftV1CalKey SiftV1CalKey;
typedef struct SiftV1CalTest SiftV1CalTest;

/* The first key of spec as such a key, in cx, and its leaf of the layout in *leaf; NULL for any other key */
extern SiftV1CalKey *sift_v1_cal_key_open(MemoryContext cx, const SiftV6Spec *spec, const SiftSrcLayout *lay, int *leaf);

/* The groups of h give the key of their rows: the values of the leaf, or calendar cells of a grain the expression keeps its value over */
extern bool sift_v1_cal_key_fits(const SiftV1CalKey *k, const SiftSrcHist *h);

/* Row row of the next keys is the key of group r of h */
extern void sift_v1_cal_key_put(SiftV1CalKey *k, const SiftSrcHist *h, uint32 r, int row);

/* The keys of rows 0 .. n - 1 put since the last call, valid until the next one */
extern SiftVec *sift_v1_cal_key_vec(SiftV1CalKey *k, int n);

/* The boolean clause of the column vars (its one Var of the heap, varno 1) of type typid as such a test, in the current context; NULL for any other clause */
extern SiftV1CalTest *sift_v1_cal_test_of(const Node *clause, const List *vars, Oid typid);

/* Whether the rows of group r of h pass the test: a value of the leaf (a histogram) or the NULL group */
extern bool sift_v1_cal_test_passes(SiftV1CalTest *t, const SiftSrcHist *h, uint32 r);

/* What the test says of the rows of the calendar cell r of h: SV1V_SOME when it does not keep its value over cells of its grain */
extern SiftV1Verdict sift_v1_cal_test_cell(SiftV1CalTest *t, const SiftSrcHist *h, uint32 r);

#endif							/* SIFT_V1_STORE_OPS_CAL_H */
