/*
 * agg/plan/node_hash.h - the hash of node trees that agrees with equal(): equal nodes share it, so a set keyed by it needs equal() only to confirm a hit.
 */
#ifndef SIFT_AGG_PLAN_NODE_HASH_H
#define SIFT_AGG_PLAN_NODE_HASH_H

#include "postgres.h"

#include "nodes/pg_list.h"

extern uint64 sift_node_hash(const Node *n);
extern uint64 sift_list_hash(const List *l);

#endif							/* SIFT_AGG_PLAN_NODE_HASH_H */
