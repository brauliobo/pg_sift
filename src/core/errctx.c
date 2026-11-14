/*
 * core/errctx.c - the error context of an index: see errctx.h
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "core/errctx.h"
#include "core/store.h"

/* A background worker reports its failure after the transaction is gone, with its context still on the stack: no catalog is read then */
char *
sift_errctx_name(Oid relid)
{
	char	   *name = IsTransactionState() ? get_rel_name(relid) : NULL;
	char	   *schema = name ? get_namespace_name(get_rel_namespace(relid)) : NULL;

	if (schema == NULL)
		return psprintf("OID %u", relid);
	return psprintf("%s.%s", schema, name);
}

const char *
sift_index_ident(Relation index)
{
	return quote_qualified_identifier(get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index));
}

static void
errctx_callback(void *arg)
{
	SiftErrCtx *ctx = arg;
	char	   *name = sift_errctx_name(ctx->relid);
	char		kind = IsTransactionState() ? get_rel_relkind(ctx->relid) : '\0';
	const char *what = kind == RELKIND_INDEX || kind == '\0' ? "index" : "table";

	if (ctx->segmented)
		errcontext("sift %s \"%s\", segment %llu", what, name, (unsigned long long) ctx->segment);
	else
		errcontext("sift %s \"%s\"", what, name);
}

void
sift_errctx_push(SiftErrCtx *ctx, Oid relid)
{
	ctx->cb = (ErrorContextCallback) {.callback = errctx_callback,.arg = ctx,.previous = error_context_stack};
	ctx->relid = relid;
	ctx->segmented = false;
	error_context_stack = &ctx->cb;
}

void
sift_errctx_pop(SiftErrCtx *ctx)
{
	Assert(error_context_stack == &ctx->cb);
	error_context_stack = ctx->cb.previous;
}
