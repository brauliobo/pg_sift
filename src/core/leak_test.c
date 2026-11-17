/*
 * core/leak_test.c - leak detection support compiled only with SIFT_TESTING.
 *
 * sift_leak_test_init, called from _PG_init, makes every backend that loads sift report the sift memory contexts
 * that outlive it: all sift contexts hang off a query, scan or function context, so one still allocated at
 * backend exit was leaked into a long-lived parent (the backend caches, named "sift cache ...", live that long on
 * purpose; the statement in flight, in MessageContext, is not one). The line "sift leak: memory context" in the server log is
 * matched by scripts/leakcheck.sh, which every suite runs through.
 *
 * sift._test_leak(kind, rel) (pg_sift_test) leaves a resource behind on purpose, to prove that scripts/leakcheck.sh
 * fails on each kind of leak (test/t/leakcheck_gate.sh).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <malloc.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "access/relation.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "core/errctx.h"
#include "core/sift.h"
#include "core/catname.h"

PG_FUNCTION_INFO_V1(sift_test_leak);
PG_FUNCTION_INFO_V1(sift_test_malloc_stats);
PG_FUNCTION_INFO_V1(sift_test_thp_disable);
PG_FUNCTION_INFO_V1(sift_test_errctx_report);

static void *volatile sink;		/* keeps the compiler from dropping the leaked allocations */

static void
report_contexts(MemoryContext parent)
{
	for (MemoryContext cx = parent->firstchild; cx != NULL; cx = cx->nextchild)
	{
		/* PostgreSQL resets it before it reads each command: what it holds at exit is the statement a terminated backend was running (its plan) */
		if (cx == MessageContext)
			continue;
		if (strncmp(cx->name, "sift", 4) == 0 && strncmp(cx->name, "sift cache", 10) != 0)
			elog(LOG, "sift leak: memory context \"%s\" (%zu bytes) still allocated in \"%s\" at backend exit",
				 cx->name, MemoryContextMemAllocated(cx, true), parent->name);
		report_contexts(cx);
	}
}

static void
leaked_contexts_at_exit(int code, Datum arg)
{
	if (TopMemoryContext != NULL)
		report_contexts(TopMemoryContext);
}

void
sift_leak_test_init(void)
{
	on_proc_exit(leaked_contexts_at_exit, 0);
}

/* sift._test_leak(kind text, rel regclass) returns void: buffer, relcache, tupledesc, snapshot, lock, context, malloc */
Datum
sift_test_leak(PG_FUNCTION_ARGS)
{
	const char *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			relid = sift_rel_must_exist(PG_GETARG_OID(1));

	if (strcmp(kind, "buffer") == 0)
	{
		Relation	rel = relation_open(relid, AccessShareLock);

		(void) ReadBuffer(rel, 0);	/* never released */
		relation_close(rel, NoLock);
	}
	else if (strcmp(kind, "relcache") == 0)
		(void) RelationIdGetRelation(relid);	/* never closed */
	else if (strcmp(kind, "tupledesc") == 0)
	{
		TupleDesc	td = CreateTemplateTupleDesc(1);

		td->tdrefcount = 1;		/* reference counted */
		PinTupleDesc(td);		/* never released */
	}
	else if (strcmp(kind, "snapshot") == 0)
		(void) RegisterSnapshot(GetLatestSnapshot());	/* never unregistered */
	else if (strcmp(kind, "lock") == 0)
	{
		Relation	rel = relation_open(relid, AccessShareLock);

		UnlockPage(rel, 0, ExclusiveLock);	/* never locked: "you don't own a lock" */
		relation_close(rel, AccessShareLock);
	}
	else if (strcmp(kind, "context") == 0)
		(void) AllocSetContextCreate(TopMemoryContext, "sift leaked", ALLOCSET_SMALL_SIZES);
	else if (strcmp(kind, "malloc") == 0)
	{
		for (int i = 0; i < 256; i++)	/* many, so that no stale pointer on the stack hides them all */
			sink = malloc(1000 + i);
		sink = NULL;
	}
	else
		elog(ERROR, "unknown leak kind \"%s\"", kind);
	PG_RETURN_VOID();
}

/* sift._test_errctx_report(rel regclass, n int) returns void: n reports under the error context of rel, to the server log only */
Datum
sift_test_errctx_report(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	for (int i = PG_GETARG_INT32(1); i > 0; i--)
	{
		SiftErrCtx	ctx;

		sift_errctx_push(&ctx, relid);
		ereport(LOG_SERVER_ONLY, (errhidestmt(true), errmsg_internal("sift test: a report under the error context of an index")));
		sift_errctx_pop(&ctx);
	}
	PG_RETURN_VOID();
}

/* sift._test_malloc_stats(trim bool) returns text (release: the C library gives back what it can first): what the C library holds for this backend, in bytes: arena (taken from the system), inuse (given out and not freed), free (held in the arena), mmap (blocks of their own) */
Datum
sift_test_malloc_stats(PG_FUNCTION_ARGS)
{
#ifdef __GLIBC__
	struct mallinfo2 mi;

	if (PG_GETARG_BOOL(0))
		malloc_trim(0);
	mi = mallinfo2();

	PG_RETURN_TEXT_P(cstring_to_text(psprintf("arena=%zu inuse=%zu free=%zu mmap=%zu", mi.arena, mi.uordblks, mi.fordblks, mi.hblkhd)));
#else
	PG_RETURN_NULL();
#endif
}

/* sift._test_thp_disable() returns void: this backend gets no transparent huge pages from now on, so that its anonymous memory grows by pages and not by steps of 2 MB (a memory slope of 12 bytes a row over 150,000 rows is 1.8 MB) */
Datum
sift_test_thp_disable(PG_FUNCTION_ARGS)
{
#ifdef PR_SET_THP_DISABLE
	if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0) != 0)
		elog(ERROR, "could not disable transparent huge pages: %m");
#endif
	PG_RETURN_VOID();
}

#endif							/* SIFT_TESTING */
