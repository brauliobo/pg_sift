/*
 * core/errctx.h - the context line of the errors raised while the index (or sift_table) is read or maintained: `sift index "schema.name"`, and
 * `, segment N` where the caller knows it. A decoder below a scan cannot name the index (it gets bytes); the scope that called it does, so every
 * entry into the engine pushes one for the length of the call (scripts/errctx.tsv lists the files whose corruption messages rely on it,
 * scripts/check-errors.sh enforces it).
 */
#ifndef SIFT_ERRCTX_H
#define SIFT_ERRCTX_H

#include "postgres.h"

typedef struct SiftErrCtx
{
	ErrorContextCallback cb;
	Oid			relid;
	uint64		segment;
	bool		segmented;
	const void *owner;			/* who froze the line below (sift_errctx_freeze); NULL: nobody */
	const char *what;			/* the frozen line: "index" or "table", and the name */
	char	   *name;
} SiftErrCtx;

/* Four stores: nothing is looked up until an error is reported. Pop before the relation can go away, in the order of the pushes */
extern void sift_errctx_push(SiftErrCtx *ctx, Oid relid);
extern void sift_errctx_pop(SiftErrCtx *ctx);

/* The scope of a function that reports what is wrong with an index (sift.check, col_verify, ...): the damage it finds is its answer, and does not take the index out of service */
extern void sift_errctx_push_diagnostic(SiftErrCtx *ctx, Oid relid);

/* The relation of the innermost scope on the error stack (false: none); *diagnostic: a diagnostic scope is on the stack */
extern bool sift_errctx_scope(Oid *relid, bool *diagnostic);

/* The segment (generation) the errors from now on belong to */
static inline void
sift_errctx_segment(SiftErrCtx *ctx, uint64 gen)
{
	ctx->segment = gen;
	ctx->segmented = true;
}

static inline void
sift_errctx_whole(SiftErrCtx *ctx)
{
	ctx->segmented = false;
}

/*
 * Resolves the line of every scope on the stack that is not frozen yet and keeps it there until sift_errctx_thaw(owner), so that a report raised where no lock may be taken
 * (core/extlock.h) reads no catalog and no cache. A scope pushed after the freeze is not frozen
 */
extern void sift_errctx_freeze(const void *owner);
extern void sift_errctx_thaw(const void *owner);

/* "schema.name" of the relation of a frozen scope, NULL when no frozen scope is of it */
extern const char *sift_errctx_frozen_name(Oid relid);

/* "schema.name" of the relation, "OID n" when it is gone */
extern char *sift_errctx_name(Oid relid);

#endif							/* SIFT_ERRCTX_H */
