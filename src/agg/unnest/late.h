/*
 * agg/unnest/late.h - when PostgreSQL evaluates the value of a member of jsonb_each or of a cast member of jsonb_array_elements for a pair: the cast that raised for the member read from the heap
 * (sift_v6_recfn_values) is the error of the statement only for a pair whose group key, or whose aggregate with its FILTER passing, reads the value. The items are the keys and the aggregates of the
 * statement that read the element, over the columns of the table (as the scan tuple, the heap row) and of the element (INNER_VAR, the member as the function gives it): [filter, args], a filter of
 * true for a key; an item whose expressions read anything else, and the one item of a statement whose joins or tests may read the value, is [true, NIL], which reads it for every pair.
 */
#ifndef SIFT_AGG_V6_UNNEST_LATE_H
#define SIFT_AGG_V6_UNNEST_LATE_H

#include "postgres.h"

#include "nodes/execnodes.h"

#include "agg/plan/int.h"
#include "agg/unnest/jsonb.h"
#include "agg/unnest/test.h"

/* The items of the statement p plans (NIL: no column of the element is computed late) */
extern List *sift_v6_late_probe_plan(const V6Plan *p);
/* The quals of the element of p as the statement has them, over the member (SiftV6Ext.late_where) */
extern List *sift_v6_late_where_plan(const V6Plan *p);

typedef struct SiftV6LateProbe SiftV6LateProbe;

/* The items and the quals of the element (SiftV6Ext.late_where) over the members of rec, the evaluation of the function of the rows read from the heap */
extern SiftV6LateProbe *sift_v6_late_probe_open(const List *items, const List *where, EState *estate, const SiftV6RecFn *rec);
/* The pair of the row of econtext's scan tuple and member k of the last chunk of rec, whose cast raised err: raises when an expression of the statement reads the member, as that expression raises */
extern void sift_v6_late_probe_check(SiftV6LateProbe *lp, ExprContext *econtext, uint32 k, ErrorData *err);

/*
 * The elements of a chunk of nel members of rec, read from the heap, that the quals ee keep (NULL: all), and in *late the error of the late columns of each kept member (NULL: rec computes none).
 * When the quals read a late column, a member whose cast raised is tested by the quals as the statement has them: the error is the statement's when they reach the cast, as they do in its plan.
 */
extern uint64 *sift_v6_late_keep(SiftV6LateProbe *lp, SiftV6ElemEval *ee, SiftV6RecFn *rec, ExprContext *econtext, MemoryContext cx, SiftVec *ev, uint32 nel, ErrorData ***late);

#endif							/* SIFT_AGG_V6_UNNEST_LATE_H */
