/*
 * agg/pg_exec.c - the expressions of a spec that the node prepares when it begins (agg/pg_exec.h).
 */
#include "postgres.h"

#include "agg/ext.h"
#include "agg/pg_exec.h"
#include "agg/pipe.h"
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
	MemoryContext old = MemoryContextSwitchTo(cx);

	x->ps = ps;
	x->qual = sift_v6_pgqual_prepare(cx, ps, spec->vqual);
	x->heap_qual = spec->ext != NULL ? ExecInitQual(spec->ext->heap_where, ps) : NULL;
	MemoryContextSwitchTo(old);
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
		}
	if (spec->ext != NULL)
		foreach_ptr(Expr, e, spec->ext->jexpr)
			if (sift_v6_pgexpr_exec((Node *) e))
			{
				if (x->joined == NULL)
					x->joined = MemoryContextAllocZero(cx, list_length(spec->ext->jexpr) * sizeof(SiftV6KeyExpr *));
				x->joined[foreach_current_index(e)] = sift_v6_key_expr_new(cx, ps, spec->ext, e);
			}
	spec->exec = x;
}

ExprContext *
sift_v6_exec_context(const SiftV6Spec *spec)
{
	return CreateExprContext(spec->exec->ps->state);
}
