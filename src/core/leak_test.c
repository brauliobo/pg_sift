/*
 * core/leak_test.c - leak detection support compiled only with SIFT_TESTING.
 *
 * sift_leak_test_init, called from _PG_init, makes every backend that loads sift report the sift memory contexts
 * that outlive it: all sift contexts hang off a query, scan or function context, so one still allocated at
 * backend exit was leaked into a long-lived parent (the backend caches, named "sift cache ...", live that long on
 * purpose). The line "sift leak: memory context" in the server log is
 * matched by scripts/leakcheck.sh, which every suite runs through.
 *
 * sift._test_leak(kind, rel) (pg_sift_test) leaves a resource behind on purpose, to prove that scripts/leakcheck.sh
 * fails on each kind of leak (test/t/leakcheck_gate.sh).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <stdlib.h>

#include "access/relation.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "core/sift.h"

PG_FUNCTION_INFO_V1(sift_test_leak);

static void *volatile sink;		/* keeps the compiler from dropping the leaked allocations */

static void
report_contexts(MemoryContext parent)
{
	for (MemoryContext cx = parent->firstchild; cx != NULL; cx = cx->nextchild)
	{
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
	Oid			relid = PG_GETARG_OID(1);

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

#endif							/* SIFT_TESTING */
