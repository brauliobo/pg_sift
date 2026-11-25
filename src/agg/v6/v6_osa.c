/*
 * agg/v6/v6_osa.c - SV6_OSA: which WITHIN GROUP aggregates the scan runs and which of their inputs count (v6_osa.h)
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"

#include "agg/v6/v6_osa.h"

bool
sift_v6_osa_eligible(const Aggref *a)
{
	const bool	hyp = a->aggkind == AGGKIND_HYPOTHETICAL;

	if (a->aggkind != AGGKIND_ORDERED_SET && !hyp)
		return false;
	if (hyp ? list_length(a->aggorder) != list_length(a->args) : (list_length(a->aggorder) != 1 || list_length(a->args) != 1))
		return false;
	foreach_node(SortGroupClause, sc, a->aggorder)
		if (!OidIsValid(sc->sortop))
			return false;
	return true;
}

bool
v6_osa_takes(const V6GenAgg *ga, const bool *nulls)
{
	return ga->hyp || !nulls[0];
}
