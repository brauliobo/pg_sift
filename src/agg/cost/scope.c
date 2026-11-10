/*
 * agg/cost/scope.c - what a scope read leaves of the table to read (agg/plan_scope.c)
 *
 * The scope units of a key hold exactly the rows of the members that have a pair with it (col/scope.h): a scope read of the keys reads those rows and no others, where the entity
 * path (the join the second pass took out) reads the units its key filter on the member column leaves, which hold the rows of other members too and, for facts ordered by time,
 * most units. The share is the one of the rows of the table: the pairs PostgreSQL expects the keys of the scope relation to have (its statistics of the key column, the most common
 * keys by their own frequency, not the join's estimate) over the distinct members of the scanned table, each member holding its share of the rows.
 */
#include "postgres.h"

#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/cost/int.h"

/* the share of the rows of the scanned table the scope conjunct of s leaves: 1 without one, or when the pass did not take the join out (the marker has no relation to ask) */
double
scope_share(AggCtx *c, const SiftAggSpec *s)
{
	const ScopeTake *t = c->scope_take;
	double		nd;

	if (s->scope == NULL || t == NULL)
		return 1.0;
	nd = column_distinct(c, t->fk, false);
	return Min(t->pairs / (nd > 0 ? nd : DEFAULT_NUM_DISTINCT), 1.0);
}
