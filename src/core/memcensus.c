/*
 * core/memcensus.c - the memory census of the test builds (SIFT_TESTING), one greppable line a statement and one a backend.
 *
 * sift.test_memctx_census (kB, -1 = off) makes every backend that loads sift write, to the server log only, a line
 * "SIFTMEM stmt ..." for each top-level statement that grew its anonymous memory or its memory
 * long-lived contexts by at least that many kB (0: every statement), and "SIFTMEM exit ..." when it ends. scripts/pgtest.sh and
 * test/t/lib/common.bash switch it on for every cluster with SIFT_MEMCENSUS=KB; scripts/mem-report.py ranks the lines.
 * Off, the cost of a statement is the read of the setting.
 *
 * Keys of a line: pid, db, ms, anon_kb (resident minus shared pages), d_anon_kb (its growth over the statement), d_hwm_kb (growth of the peak
 * resident memory), top_kb and cache_kb (TopMemoryContext, CacheMemoryContext) with d_top_kb and d_cache_kb, kept_kb (TopMemoryContext
 * without the contexts of the message, the transaction and the portals: what the backend keeps across statements) with d_kept_kb, sift_kb
 * (the contexts whose name starts with "sift"), ctx (name:instances:kB, spaces as _, the largest first), q (the statement).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <ctype.h>
#include <sys/resource.h>
#include <unistd.h>

#include "commands/dbcommands.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "core/sift.h"

#define CENSUS_CTX		8
#define CENSUS_QUERY	96

typedef struct CtxSum
{
	const char *name;
	int			n;
	size_t		bytes;
} CtxSum;

typedef struct Mark
{
	long		anon_kb;
	long		hwm_kb;
	size_t		top;
	size_t		kept;
	size_t		cache;
	TimestampTz at;
} Mark;

static int	census_kb = -1;
static int	depth;
static Mark start;
static Mark last;
static bool have_last;
static long max_anon_kb;
static long max_hwm_kb;
static long statements;
static char dbname[NAMEDATALEN];

static ExecutorRun_hook_type prev_run;
static ProcessUtility_hook_type prev_utility;

#if PG_VERSION_NUM >= 180000
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count
#define RUN_ARGS	qd, dir, count
#else
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count, bool once
#define RUN_ARGS	qd, dir, count, once
#endif

static long
anon_kb(void)
{
	long		size,
				resident,
				shared;
	FILE	   *f = fopen("/proc/self/statm", "r");

	if (f == NULL)
		return 0;
	if (fscanf(f, "%ld %ld %ld", &size, &resident, &shared) != 3)
		resident = shared = 0;
	fclose(f);
	return (resident - shared) * (sysconf(_SC_PAGESIZE) / 1024);
}

static long
peak_kb(void)
{
	struct rusage ru;

	return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_maxrss : 0;
}

static size_t
allocated(MemoryContext cx)
{
	return cx != NULL ? MemoryContextMemAllocated(cx, true) : 0;
}

static size_t
portals_allocated(void)
{
	for (MemoryContext cx = TopMemoryContext->firstchild; cx != NULL; cx = cx->nextchild)
		if (strcmp(cx->name, "TopPortalContext") == 0)
			return MemoryContextMemAllocated(cx, true);
	return 0;
}

static void
take(Mark *m)
{
	m->anon_kb = anon_kb();
	m->hwm_kb = peak_kb();
	m->top = MemoryContextMemAllocated(TopMemoryContext, true);
	m->kept = m->top - allocated(MessageContext) - allocated(TopTransactionContext) - portals_allocated();
	m->cache = CacheMemoryContext != NULL ? MemoryContextMemAllocated(CacheMemoryContext, true) : 0;
	m->at = GetCurrentTimestamp();
}

static void
add_ctx(CtxSum *sums, int *nsums, const char *name, size_t bytes)
{
	int			i;

	for (i = 0; i < *nsums; i++)
		if (strcmp(sums[i].name, name) == 0)
			break;
	if (i == *nsums)
	{
		if (*nsums == CENSUS_CTX * 8)
			return;
		sums[i].name = name;
		sums[i].n = 0;
		sums[i].bytes = 0;
		(*nsums)++;
	}
	sums[i].n++;
	sums[i].bytes += bytes;
}

static size_t
sift_contexts(MemoryContext parent, CtxSum *sums, int *nsums)
{
	size_t		total = 0;

	for (MemoryContext cx = parent->firstchild; cx != NULL; cx = cx->nextchild)
	{
		if (strncmp(cx->name, "sift", 4) == 0)
		{
			size_t		bytes = MemoryContextMemAllocated(cx, true);

			add_ctx(sums, nsums, cx->name, bytes);
			total += bytes;
		}
		else
			total += sift_contexts(cx, sums, nsums);
	}
	return total;
}

static int
by_bytes_desc(const void *a, const void *b)
{
	const CtxSum *x = a;
	const CtxSum *y = b;

	return (x->bytes < y->bytes) - (x->bytes > y->bytes);
}

static void
append_ctx(StringInfo buf, CtxSum *sums, int nsums)
{
	qsort(sums, nsums, sizeof(CtxSum), by_bytes_desc);
	appendStringInfoString(buf, " ctx=");
	for (int i = 0; i < nsums && i < CENSUS_CTX; i++)
	{
		appendStringInfo(buf, "%s", i ? "," : "");
		for (const char *p = sums[i].name; *p != '\0'; p++)
			appendStringInfoChar(buf, isalnum((unsigned char) *p) ? *p : '_');
		appendStringInfo(buf, ":%d:%zu", sums[i].n, sums[i].bytes / 1024);
	}
	if (nsums == 0)
		appendStringInfoChar(buf, '-');
}

static void
append_query(StringInfo buf, const char *query)
{
	appendStringInfoString(buf, " q=\"");
	for (;;)
	{
		while (*query != '\0' && isspace((unsigned char) *query))
			query++;
		if (strncmp(query, "--", 2) != 0)
			break;
		while (*query != '\0' && *query != '\n')
			query++;
	}
	for (int i = 0; query[i] != '\0' && i < CENSUS_QUERY; i++)
		appendStringInfoChar(buf, isspace((unsigned char) query[i]) || query[i] == '"' ? ' ' : query[i]);
	appendStringInfoChar(buf, '"');
}

static void
census_line(const char *kind, const Mark *from, const Mark *now, const char *query)
{
	StringInfoData buf;
	CtxSum		sums[CENSUS_CTX * 8];
	int			nsums = 0;
	size_t		sift = sift_contexts(TopMemoryContext, sums, &nsums);

	initStringInfo(&buf);
	appendStringInfo(&buf, "SIFTMEM %s pid=%d db=%s ms=%ld anon_kb=%ld d_anon_kb=%ld d_hwm_kb=%ld top_kb=%zu d_top_kb=%zd kept_kb=%zu d_kept_kb=%zd"
					 " cache_kb=%zu d_cache_kb=%zd sift_kb=%zu",
					 kind, MyProcPid, dbname,
					 (long) ((now->at - from->at) / 1000), now->anon_kb, now->anon_kb - from->anon_kb, now->hwm_kb - from->hwm_kb,
					 now->top / 1024, ((ssize_t) now->top - (ssize_t) from->top) / 1024,
					 now->kept / 1024, ((ssize_t) now->kept - (ssize_t) from->kept) / 1024,
					 now->cache / 1024, ((ssize_t) now->cache - (ssize_t) from->cache) / 1024, sift / 1024);
	append_ctx(&buf, sums, nsums);
	if (query != NULL)
		append_query(&buf, query);
	ereport(LOG_SERVER_ONLY, (errhidestmt(true), errhidecontext(true), errmsg_internal("%s", buf.data)));
	pfree(buf.data);
}

static bool
over_threshold(const Mark *from, const Mark *now)
{
	return Max(now->anon_kb - from->anon_kb, ((ssize_t) now->kept - (ssize_t) from->kept) / 1024) >= census_kb;
}

static void
statement_begin(void)
{
	if (depth++ == 0 && census_kb >= 0)
	{
		if (dbname[0] == '\0')
			strlcpy(dbname, get_database_name(MyDatabaseId), sizeof(dbname));
		if (!have_last)
			take(&last);
		have_last = true;
		start = last;
		start.anon_kb = anon_kb();
		start.hwm_kb = peak_kb();
		start.at = GetCurrentTimestamp();
	}
}

static void
statement_end(const char *query)
{
	Mark		now;

	if (--depth != 0 || census_kb < 0)
		return;
	take(&now);
	last = now;
	statements++;
	max_anon_kb = Max(max_anon_kb, now.anon_kb);
	max_hwm_kb = Max(max_hwm_kb, now.hwm_kb);
	if (over_threshold(&start, &now))
		census_line("stmt", &start, &now, query);
}

static void
census_run(RUN_PARAMS)
{
	statement_begin();
	PG_TRY();
	{
		if (prev_run != NULL)
			prev_run(RUN_ARGS);
		else
			standard_ExecutorRun(RUN_ARGS);
	}
	PG_CATCH();
	{
		depth = 0;
		PG_RE_THROW();
	}
	PG_END_TRY();
	statement_end(qd->sourceText);
}

static void
census_utility(PlannedStmt *pstmt, const char *query, bool readonly, ProcessUtilityContext context,
			   ParamListInfo params, QueryEnvironment *env, DestReceiver *dest, QueryCompletion *qc)
{
	statement_begin();
	PG_TRY();
	{
		if (prev_utility != NULL)
			prev_utility(pstmt, query, readonly, context, params, env, dest, qc);
		else
			standard_ProcessUtility(pstmt, query, readonly, context, params, env, dest, qc);
	}
	PG_CATCH();
	{
		depth = 0;
		PG_RE_THROW();
	}
	PG_END_TRY();
	statement_end(query);
}

static void
census_at_exit(int code, Datum arg)
{
	Mark		now;

	if (census_kb < 0 || TopMemoryContext == NULL)
		return;
	take(&now);
	census_line("exit", &now, &now, NULL);
	ereport(LOG_SERVER_ONLY, (errhidestmt(true), errhidecontext(true),
							  errmsg_internal("SIFTMEM exit_peak pid=%d db=%s statements=%ld max_anon_kb=%ld max_hwm_kb=%ld",
											  MyProcPid, dbname, statements, max_anon_kb, max_hwm_kb)));
}

void
sift_memcensus_init(void)
{
	DefineCustomIntVariable("sift.test_memctx_census",
							"Test builds: log a SIFTMEM line for each top-level statement that grew memory by this many kB, and one at backend exit (-1: off).",
							NULL, &census_kb, -1, -1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	prev_run = ExecutorRun_hook;
	ExecutorRun_hook = census_run;
	prev_utility = ProcessUtility_hook;
	ProcessUtility_hook = census_utility;
	on_proc_exit(census_at_exit, 0);
}

#endif							/* SIFT_TESTING */
