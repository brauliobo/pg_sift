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
 * fails on each kind of leak (test/t/core/leakcheck_gate.sh).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <stdlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "access/relation.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "core/errctx.h"
#include "core/sift.h"
#include "core/catname.h"
#include "col/dsmreg.h"

/* MEM[MEM-CORE-16]: test-only intentional leak fixture (SIFT_TESTING) */
PG_FUNCTION_INFO_V1(sift_test_leak);
PG_FUNCTION_INFO_V1(sift_test_malloc_stats);
PG_FUNCTION_INFO_V1(sift_test_thp_disable);
PG_FUNCTION_INFO_V1(sift_test_rss_peak_start);
PG_FUNCTION_INFO_V1(sift_test_rss_peak_stop);
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

/* sift._test_leak(kind text, rel regclass) returns void: buffer, relcache, tupledesc, snapshot, lock, temp, dsm, shared, context, malloc, palloc */
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

		TupleDescInitEntry(td, 1, "leaked", INT4OID, -1, 0);
		sift_tupdesc_finalize(td);
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
	else if (strcmp(kind, "temp") == 0)
		(void) BufFileCreateTemp(false);	/* never closed */
	else if (strcmp(kind, "dsm") == 0)
		(void) dsm_create(4096, 0);		/* never detached */
	else if (strcmp(kind, "shared") == 0)
		sift_dsm_test_leak_user();		/* a shared table that a backend counted and never gave back */
	else if (strcmp(kind, "context") == 0)
		(void) AllocSetContextCreate(TopMemoryContext, "sift leaked", ALLOCSET_SMALL_SIZES);
	else if (strcmp(kind, "palloc") == 0)
	{
		sink = MemoryContextAlloc(TopMemoryContext, 100);	/* a chunk of a long-lived context nothing points to: valgrind's definitely lost (scripts/valgrind.sh --leaks-selftest) */
		sink = NULL;
	}
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

#ifdef __linux__

#define RSS_SAMPLE_NS 100000

static int	rss_fd = -1;
static timer_t rss_timer;
static bool rss_running;
static volatile int64 rss_peak_kb;

/* RssAnon of /proc/self/status in kB: open, pread and a scan, all safe in a signal handler */
static int64
rss_anon_kb(void)
{
	char		buf[4096];
	ssize_t		n = pread(rss_fd, buf, sizeof(buf) - 1, 0);
	char	   *p;
	int64		kb = 0;

	if (n <= 0)
		return 0;
	buf[n] = '\0';
	p = strstr(buf, "RssAnon:");
	if (p == NULL)
		return 0;
	for (p += 8; *p == ' ' || *p == '\t'; p++)
		;
	for (; *p >= '0' && *p <= '9'; p++)
		kb = kb * 10 + (*p - '0');
	return kb;
}

static void
rss_sample(void)
{
	int64		kb = rss_anon_kb();

	if (kb > rss_peak_kb)
		rss_peak_kb = kb;
}

static void
rss_handler(int signo)
{
	int			saved = errno;

	rss_sample();
	errno = saved;
}

/*
 * sift._test_rss_peak_start() returns bigint: the anonymous memory of this backend in kB, and from now on a timer of the backend
 * samples it every 100 microseconds of wall time, in the signal handler, so a sample is taken whenever the backend runs
 * and no other process (a polling reader, the other jobs of a runner) decides which part of a statement is seen.
 */
Datum
sift_test_rss_peak_start(PG_FUNCTION_ARGS)
{
	struct sigevent ev = {0};
	struct sigaction sa = {0};
	struct itimerspec its = {{0, RSS_SAMPLE_NS}, {0, RSS_SAMPLE_NS}};

	if (rss_running)
		elog(ERROR, "the memory peak is already measured");
	if (rss_fd < 0 && (rss_fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC)) < 0)
		elog(ERROR, "could not open /proc/self/status: %m");
	rss_peak_kb = rss_anon_kb();
	sa.sa_handler = rss_handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGRTMIN, &sa, NULL) != 0)
		elog(ERROR, "could not install the sampling handler: %m");
	ev.sigev_notify = SIGEV_SIGNAL;
	ev.sigev_signo = SIGRTMIN;
	if (timer_create(CLOCK_MONOTONIC, &ev, &rss_timer) != 0 || timer_settime(rss_timer, 0, &its, NULL) != 0)
		elog(ERROR, "could not start the sampling timer: %m");
	rss_running = true;
	PG_RETURN_INT64(rss_peak_kb);
}

/* sift._test_rss_peak_stop() returns bigint: the highest anonymous memory in kB since _test_rss_peak_start(), 0 when none runs */
Datum
sift_test_rss_peak_stop(PG_FUNCTION_ARGS)
{
	if (!rss_running)
		PG_RETURN_INT64(0);
	timer_delete(rss_timer);
	rss_running = false;
	rss_sample();
	PG_RETURN_INT64(rss_peak_kb);
}

#else

Datum
sift_test_rss_peak_start(PG_FUNCTION_ARGS)
{
	elog(ERROR, "the memory peak is measured on Linux only");
	PG_RETURN_NULL();
}

Datum
sift_test_rss_peak_stop(PG_FUNCTION_ARGS)
{
	return sift_test_rss_peak_start(fcinfo);
}

#endif

#endif							/* SIFT_TESTING */
