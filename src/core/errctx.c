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
#include "core/store/store.h"

/* A background worker reports its failure after the transaction is gone, with its context still on the stack: no catalog is read then */
char *
sift_errctx_name(Oid relid)
{
	char	   *name = IsTransactionState() ? get_rel_name(relid) : NULL;
	char	   *schema = name ? get_namespace_name(get_rel_namespace(relid)) : NULL;
	char	   *full;

	if (schema == NULL)
	{
		if (name != NULL)
			pfree(name);
		return psprintf("OID %u", relid);
	}
	full = psprintf("%s.%s", schema, name);
	pfree(name);
	pfree(schema);
	return full;
}

const char *
sift_index_ident(Relation index)
{
	return quote_qualified_identifier(get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index));
}

/* "index" or "table" for the line of a scope, and its name: the lookups of a report, which sift_errctx_freeze() makes ahead of a section that cannot take a lock */
static char *
errctx_describe(Oid relid, const char **what)
{
	char		kind = IsTransactionState() ? get_rel_relkind(relid) : '\0';

	*what = kind == RELKIND_INDEX || kind == '\0' ? "index" : "table";
	return sift_errctx_name(relid);
}

static void
errctx_callback(void *arg)
{
	SiftErrCtx *ctx = arg;
	const char *what = ctx->what;
	char	   *name = ctx->name != NULL ? ctx->name : errctx_describe(ctx->relid, &what);

	if (ctx->segmented)
		errcontext("sift %s \"%s\", segment %llu", what, name, (unsigned long long) ctx->segment);
	else
		errcontext("sift %s \"%s\"", what, name);
	/* the callback runs in ErrorContext, which only an error that is caught empties: a report that is not one keeps what is left in it */
	if (name != ctx->name)
		pfree(name);
}

/* The diagnostic scope reports like the other, and is told from it by its callback: a push costs the same four stores */
static void
errctx_diagnostic_callback(void *arg)
{
	errctx_callback(arg);
}

void
sift_errctx_push(SiftErrCtx *ctx, Oid relid)
{
	ctx->cb = (ErrorContextCallback) {.callback = errctx_callback,.arg = ctx,.previous = error_context_stack};
	ctx->relid = relid;
	ctx->segmented = false;
	ctx->owner = NULL;
	ctx->name = NULL;
	error_context_stack = &ctx->cb;
}

void
sift_errctx_push_diagnostic(SiftErrCtx *ctx, Oid relid)
{
	sift_errctx_push(ctx, relid);
	ctx->cb.callback = errctx_diagnostic_callback;
}

bool
sift_errctx_scope(Oid *relid, bool *diagnostic)
{
	bool		found = false;

	*diagnostic = false;
	for (ErrorContextCallback *cb = error_context_stack; cb != NULL; cb = cb->previous)
	{
		if (cb->callback != errctx_callback && cb->callback != errctx_diagnostic_callback)
			continue;
		if (!found)
			*relid = ((SiftErrCtx *) cb->arg)->relid;
		found = true;
		*diagnostic |= cb->callback == errctx_diagnostic_callback;
	}
	return found;
}

void
sift_errctx_freeze(const void *owner)
{
	for (ErrorContextCallback *cb = error_context_stack; cb != NULL; cb = cb->previous)
	{
		SiftErrCtx *ctx = cb->arg;

		if ((cb->callback != errctx_callback && cb->callback != errctx_diagnostic_callback) || ctx->name != NULL)
			continue;
		ctx->name = errctx_describe(ctx->relid, &ctx->what);
		ctx->owner = owner;
	}
}

const char *
sift_errctx_frozen_name(Oid relid)
{
	for (ErrorContextCallback *cb = error_context_stack; cb != NULL; cb = cb->previous)
	{
		SiftErrCtx *ctx = cb->arg;

		if ((cb->callback == errctx_callback || cb->callback == errctx_diagnostic_callback) && ctx->name != NULL && ctx->relid == relid)
			return ctx->name;
	}
	return NULL;
}

void
sift_errctx_thaw(const void *owner)
{
	for (ErrorContextCallback *cb = error_context_stack; cb != NULL; cb = cb->previous)
	{
		SiftErrCtx *ctx = cb->arg;

		if ((cb->callback != errctx_callback && cb->callback != errctx_diagnostic_callback) || ctx->owner != owner)
			continue;
		pfree(ctx->name);
		ctx->name = NULL;
		ctx->owner = NULL;
	}
}

void
sift_errctx_pop(SiftErrCtx *ctx)
{
	Assert(error_context_stack == &ctx->cb);
	error_context_stack = ctx->cb.previous;
}
