/*
 * agg/v6/par/pipe_stubs.c - the server calls of agg/v6/pipe.c in a unit test of the join pipe: the base type of a type is the type (the tests use no domain), and the calls the pipe the tests build never makes
 * (expression keys, residual quals, the unmatched rows of a lookup join) end the test; the pipe has no residual quals.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/v6/datum.h"
#include "agg/v6/expr/str.h"
#include "agg/v6/join/lookup.h"
#include "agg/v6/expr/pg.h"
#include "agg/v6/topn/rows.h"

#define SERVER_CALL() (fprintf(stderr, "unit: %s is a server call\n", __func__), abort())

Oid
exprType(const Node *expr)
{
	SERVER_CALL();
}

Oid
getBaseType(Oid typid)
{
	return typid;
}

void
get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval)
{
	SERVER_CALL();
}

bool
sift_v6_type_native(Oid typid)
{
	return sift_v6_class(typid) != SV6C_NONE && typid != NUMERICOID;
}

void
sift_vx_out_init(SiftVxOut *o, MemoryContext cx)
{
	SERVER_CALL();
}

void
sift_v6_lookup_mark(const SiftStarDim *d, const uint32 *dim_row, uint32 n)
{
	SERVER_CALL();
}

uint32
sift_v6_lookup_unmatched(const SiftStarDim *d, uint32 *next, uint32 *dim_row, uint32 cap)
{
	SERVER_CALL();
}

SiftV6Resid *
sift_v6_resid_new(MemoryContext cx, const SiftV6Ext *x)
{
	return NULL;
}

void
sift_v6_resid_free(SiftV6Resid *r)
{
	Assert(r == NULL);
}

bool
sift_v6_resid_has(const SiftV6Resid *r, int dim)
{
	return false;
}

void
sift_v6_resid_filter(SiftV6Resid *r, int dim, const SiftVec *const *cols, SiftBatchSel *bs)
{
	SERVER_CALL();
}

SiftV6PgExpr *
sift_v6_pgexpr_open(MemoryContext cx, PlanState *ps, Expr *e, int nin, const SiftV6PgIn *in)
{
	SERVER_CALL();
}

void
sift_v6_pgexpr_derive(SiftV6PgExpr *x, const SiftVec *const *in, const SiftBatchSel *bs, SiftVxOut *out)
{
	SERVER_CALL();
}

bool
sift_v6_pgexpr_vars(const Expr *e, Index relid, Index elem_relid, List **vars)
{
	SERVER_CALL();
}

Expr *
sift_v6_pgexpr_renumber(const Expr *e, Index relid, Index elem_relid, const List *vars)
{
	SERVER_CALL();
}

void
sift_v6_rows_update(SiftV6Rows *r, const SiftBatchSel *bs, SiftVec *const *keyvec, const uint64 *tid)
{
	SERVER_CALL();
}
