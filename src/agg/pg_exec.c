/*
 * agg/pg_exec.c - the expressions of a spec that the node prepares when it begins (agg/pg_exec.h).
 */
#include "postgres.h"

#include "agg/pg_exec.h"
#include "agg/qual/pg.h"

static bool
item_exec(Node *item)
{
	return sift_v6_pgexpr_exec(linitial(castNode(List, item)));
}

void
sift_v6_exec_begin(SiftV6Spec *spec, PlanState *ps)
{
	MemoryContext cx = ps->state->es_query_cxt;
	SiftV6Exec *x = MemoryContextAllocZero(cx, sizeof(SiftV6Exec));
	const int	nitems = list_length(spec->pgexpr);
	bool		any;

	x->ps = ps;
	x->qual = sift_v6_pgqual_prepare(cx, ps, spec->vqual);
	any = x->qual != NULL;
	x->item = MemoryContextAllocZero(cx, Max(nitems, 1) * sizeof(SiftV6PgExpr *));
	for (int i = 0; i < nitems; i++)
		if (item_exec(list_nth(spec->pgexpr, i)))
		{
			SiftV6PgItem it;
			SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

			sift_v6_pgexpr_item_at(spec, i, &it);
			for (int c = 0; c < it.nin; c++)
				pin[c] = (SiftV6PgIn) {.typid = it.col[c].typid,.scale = it.col[c].scale};
			x->item[i] = sift_v6_pgexpr_open(cx, ps, it.expr, it.nin, pin);
			x->item[i]->restype = it.restype;
			any = true;
		}
	if (any)
		spec->exec = x;
	else
		pfree(x);
}
