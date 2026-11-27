/*
 * agg/scope/show.c - the EXPLAIN lines every node of the scope aggregate prints, whatever format reads it (docs/SCOPES.md): the relation and the scope it reads (Scope Index), the keys (Scope
 * Keys: their number when the statement names them by constants, else the expression the node evaluates when it starts, and in a run the number it had; all: every key of the summary),
 * whether a fact counts once or once for each pair of its member (Scope Semantics), the scope whose key the statement groups by (Scope Group Key), and in a run why the node ran the join
 * instead (Scope Fallback).
 */
#include "postgres.h"

#include "commands/explain.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "agg/scope/scope.h"

static const char *
scope_name(Relation rel, int ord)
{
	const SiftScopeSpecs *specs = sift_scope_specs(rel);

	return specs != NULL && ord < specs->n ? specs->s[ord]->key_name : "?";
}

/* The keys a constant names: one, or the distinct elements of an array that are not NULL */
static int
const_keys(const Const *c)
{
	ArrayType  *a;
	Datum	   *vals;
	bool	   *nulls;
	int			n,
				u = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const Oid	elem = get_element_type(c->consttype);

	if (c->constisnull)
		return 0;
	if (!OidIsValid(elem))
		return 1;
	a = DatumGetArrayTypeP(c->constvalue);
	get_typlenbyvalalign(elem, &typlen, &typbyval, &typalign);
	deconstruct_array(a, elem, typlen, typbyval, typalign, &vals, &nulls, &n);
	for (int i = 0; i < n; i++)
	{
		bool		seen = nulls[i];

		for (int j = 0; j < i && !seen; j++)
			seen = !nulls[j] && datumIsEqual(vals[i], vals[j], typbyval, typlen);
		u += !seen;
	}
	return u;
}

static void
show_keys(const ScopeShow *s, PlanState *ps, List *ancestors, ExplainState *es)
{
	char	   *expr;

	if (s->key == NULL)
	{
		ExplainPropertyText("Scope Keys", "all", es);
		return;
	}
	if (IsA(s->key, Const))
	{
		ExplainPropertyInteger("Scope Keys", NULL, const_keys((const Const *) s->key), es);
		return;
	}
	expr = deparse_expression((Node *) s->key, set_deparse_context_plan(es->deparse_cxt, ps->plan, ancestors), es->verbose, false);
	ExplainPropertyText("Scope Keys", s->nkeys < 0 ? expr : psprintf("%s (" INT64_FORMAT " keys)", expr, s->nkeys), es);
}

void
sift_v6_scope_show(const ScopeShow *s, PlanState *ps, List *ancestors, ExplainState *es)
{
	ExplainPropertyText("Scope Index", psprintf("%s (%s)", RelationGetRelationName(s->rel), scope_name(s->rel, s->spec->ord)), es);
	show_keys(s, ps, ancestors, es);
	ExplainPropertyText("Scope Semantics", s->spec->once ? "once" : "per pair", es);
	if (s->spec->by_key || s->spec->grouped_const)
		ExplainPropertyText("Scope Group Key", scope_name(s->rel, s->spec->ord), es);
	if (s->fallback != NULL)
		ExplainPropertyText("Scope Fallback", s->fallback, es);
}
