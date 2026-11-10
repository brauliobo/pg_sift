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

/*
 * The tests a row of the scope units goes through besides the scan's own: its key, and for IN and EXISTS of several keys one arm for each key whose rows the dedupe thins
 * (the keys of a member in two of them are left out of the units of all but the least: col/scan/scope.c), the OR of which is the conjunct. An arm costs a row AGG_SCOPE_ARM
 * tests: 50 keys read 330,000 entries in 80 ms where the join takes 8, 20 keys in 16 where one key takes 2 (300,000 events, 5,000 entities, 50 tenants).
 */
double
scope_tests(const SiftAggSpec *s)
{
	return s->scope == NULL ? 0.0 : s->scope->once ? 1.0 + (s->scope->nkeys - 1) * AGG_SCOPE_ARM : 1.0;
}
