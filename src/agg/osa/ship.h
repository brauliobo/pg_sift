/*
 * agg/osa/ship.h - the inputs of a WITHIN GROUP aggregate of one argument (mode, percentile_disc, percentile_cont) in a parallel run of SiftAggV6. The state of such an aggregate is every value of its
 * group, which no combine function merges: a participant hands the values of each group over with the record of the group, as (count, value) pairs (a value its mode() count holds once with its count,
 * a value of its sort once for each input), and the owner of the group takes them into its own count or sort, from which the group's answer is made as in one process.
 */
#ifndef SIFT_AGG_V6_OSA_SHIP_H
#define SIFT_AGG_V6_OSA_SHIP_H

#include "agg/gen/gen.h"

/* Whether the scan can hand the inputs of a over between participants */
extern bool sift_v6_osa_shippable(const Aggref *a);
/* The values of every group of ga taken out of its count and its sort, as the pairs of each group; its inputs start over empty */
extern void v6_osa_hold(V6Gen *gen, V6GenAgg *ga);
/* The pairs of group sid (held) as bytes of its record */
extern void v6_osa_put(V6GenAgg *ga, uint32 sid, StringInfo buf);
/* The pairs of a record into the group of state dst; the tail after them */
extern const char *v6_osa_take(V6Gen *gen, V6GenAgg *ga, V6Acc *dst, const char *tail);
/* The held pairs are gone: the records went out */
extern void v6_osa_release(V6GenAgg *ga);

#endif							/* SIFT_AGG_V6_OSA_SHIP_H */
