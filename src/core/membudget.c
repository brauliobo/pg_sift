/*
 * core/membudget.c - the memory scopes of the main paths (core/membudget.h), compiled only with SIFT_TESTING.
 *
 * A scope is a stack entry (the paths nest: a rewrite runs a merge); a checkpoint measures the innermost. An error leaves the stack by the transaction callbacks:
 * the cleanup attribute of the scope variable does not run on a longjmp. The high-water marks of the scope names live in a table of the backend, written to the
 * server log at exit as SIFTMEM_SCOPE lines and returned by sift._test_mem_highwater().
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "storage/ipc.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "core/host.h"
#include "core/membudget.h"
#include "core/sift.h"

PG_FUNCTION_INFO_V1(sift_test_mem_highwater);

#define SIFT_MEM_DEPTH 16
#define SIFT_MEM_NAMES 256

typedef struct MemMark
{
	const char *name;
	Size		peak;
	Size		budget;
	uint64		begins;
	uint64		checks;
} MemMark;

typedef struct MemFrame
{
	MemMark    *mark;
	MemoryContext cx;
	Size		budget;
	const char *formula;
	SubTransactionId subxid;
} MemFrame;

static MemFrame frames[SIFT_MEM_DEPTH];
static int	depth;
static MemMark marks[SIFT_MEM_NAMES];
static int	nmarks;
static bool registered;
static bool sift_test_mem_check = true;
static double sift_test_mem_budget_scale = 1.0;

static MemMark *
mark_of(const char *name)
{
	for (int i = 0; i < nmarks; i++)
		if (marks[i].name == name || strcmp(marks[i].name, name) == 0)
			return &marks[i];
	SIFT_INVARIANT(nmarks < SIFT_MEM_NAMES, "sift: more memory scopes than the %d of core/membudget.c", SIFT_MEM_NAMES);
	marks[nmarks].name = name;
	return &marks[nmarks++];
}

static void
log_marks(int code, Datum arg)
{
	for (int i = 0; i < nmarks; i++)
		elog(LOG, "SIFTMEM_SCOPE name=%s peak=%zu budget=%zu checks=%llu", marks[i].name, marks[i].peak, marks[i].budget,
			 (unsigned long long) marks[i].checks);
}

static void
xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
		depth = 0;
}

static void
subxact_abort(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		while (depth > 0 && frames[depth - 1].subxid == mySubid)
			depth--;
}

SiftMemScope
sift_mem_scope_begin(const char *name, MemoryContext cx, Size budget, const char *formula)
{
	MemMark    *mark = mark_of(name);
	SiftMemScope scope = {depth};

	if (!registered)
	{
		on_proc_exit(log_marks, 0);
		RegisterXactCallback(xact_end, NULL);
		RegisterSubXactCallback(subxact_abort, NULL);
		registered = true;
	}
	SIFT_INVARIANT(depth < SIFT_MEM_DEPTH, "sift: memory scopes nest deeper than the %d of core/membudget.c", SIFT_MEM_DEPTH);
	mark->begins++;
	mark->budget = budget;
	frames[depth++] = (MemFrame) {mark, cx, budget, formula, GetCurrentSubTransactionId()};
	return scope;
}

void
sift_mem_scope_end(SiftMemScope *scope)
{
	if (scope->depth < depth)
		depth = scope->depth;
}

#define SIFT_MEM_TOP 3

typedef struct MemTop
{
	const char *name[SIFT_MEM_TOP];
	Size		bytes[SIFT_MEM_TOP];
} MemTop;

static void
top_contexts(MemoryContext cx, MemTop *top)
{
	Size		own = MemoryContextMemAllocated(cx, false);
	int			at = SIFT_MEM_TOP;

	while (at > 0 && own > top->bytes[at - 1])
		at--;
	if (at < SIFT_MEM_TOP)
	{
		memmove(&top->name[at + 1], &top->name[at], (SIFT_MEM_TOP - at - 1) * sizeof(top->name[0]));
		memmove(&top->bytes[at + 1], &top->bytes[at], (SIFT_MEM_TOP - at - 1) * sizeof(top->bytes[0]));
		top->name[at] = cx->name;
		top->bytes[at] = own;
	}
	for (MemoryContext c = cx->firstchild; c != NULL; c = c->nextchild)
		top_contexts(c, top);
}

static char *
top_text(MemoryContext cx)
{
	MemTop		top = {0};
	StringInfoData buf;

	top_contexts(cx, &top);
	initStringInfo(&buf);
	for (int i = 0; i < SIFT_MEM_TOP && top.name[i] != NULL; i++)
		appendStringInfo(&buf, "%s\"%s\" %zu", i ? ", " : "", top.name[i], top.bytes[i]);
	return buf.data;
}

void
sift_mem_checkpoint(void)
{
	MemFrame   *f;
	Size		used,
				limit;

	if (depth == 0 || !sift_test_mem_check)
		return;
	f = &frames[depth - 1];
	used = MemoryContextMemAllocated(f->cx, true);
	f->mark->checks++;
	if (used > f->mark->peak)
		f->mark->peak = used;
	limit = (Size) ((double) f->budget * sift_test_mem_budget_scale);
	if (used > limit)
		/* check-errors: allow XX000 is the code of a budget failure of the test build, which the CI reports by name */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("sift memory budget exceeded: %s: %zu > %zu bytes (%s)", f->mark->name, used, limit, f->formula),
						errdetail("The largest contexts of the scope, in bytes: %s.", top_text(f->cx))));
}

/* sift._test_mem_highwater(reset bool) returns setof (scope text, peak int8, budget int8, begins int8, checks int8): the scopes this backend ran and the most memory each held at a checkpoint */
Datum
sift_test_mem_highwater(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old;

	InitMaterializedSRF(fcinfo, 0);
	old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	for (int i = 0; i < nmarks; i++)
	{
		Datum		v[5] = {CStringGetTextDatum(marks[i].name), Int64GetDatum((int64) marks[i].peak), Int64GetDatum((int64) marks[i].budget),
		Int64GetDatum((int64) marks[i].begins), Int64GetDatum((int64) marks[i].checks)};
		bool		n[5] = {false, false, false, false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
	}
	MemoryContextSwitchTo(old);
	if (PG_GETARG_BOOL(0))
		for (int i = 0; i < nmarks; i++)
			marks[i].peak = marks[i].begins = marks[i].checks = 0;
	return (Datum) 0;
}

void
sift_membudget_init(void)
{
	DefineCustomRealVariable("sift.test_mem_budget_scale",
							 "Testing: the factor every memory budget of test/mem-budgets.tsv is multiplied by at a checkpoint (0.1 makes a path fail that holds more than a tenth of it).",
							 NULL, &sift_test_mem_budget_scale, 1.0, 0.0001, 1000.0, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_check",
							 "Testing: the checkpoints of the memory scopes measure and enforce the budgets (off: they return at once).",
							 NULL, &sift_test_mem_check, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

#endif							/* SIFT_TESTING */
