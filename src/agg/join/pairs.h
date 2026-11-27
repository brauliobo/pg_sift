/*
 * agg/join/pairs.h - the expressions of a statement over the pairs of a join (agg/pipe.h): its Vars as the columns of the pairs (SiftV6Ext.rcol, the columns of the fact table, of the
 * element of its unnest and the payload of its dimensions), each one the virtual attribute 1.. of its place there, which PostgreSQL's evaluator reads from the slot of the column.
 */
#ifndef SIFT_AGG_V6_JOIN_PAIRS_H
#define SIFT_AGG_V6_JOIN_PAIRS_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/plan/int.h"

/* e over the columns of the pairs of p->xseed, which gets those it lacks; NULL when a Var of e is none of them, or e holds what the evaluator of the pipe cannot run (a sub-plan, a placeholder) */
extern Node *sift_v6_pair_expr(V6Plan *p, Node *e);

#endif							/* SIFT_AGG_V6_JOIN_PAIRS_H */
