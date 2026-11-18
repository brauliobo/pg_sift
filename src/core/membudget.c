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
#include "core/membudget_table.h"
#include "core/sift.h"

PG_FUNCTION_INFO_V1(sift_test_mem_highwater);
PG_FUNCTION_INFO_V1(sift_test_mem_kinds_fn);
PG_FUNCTION_INFO_V1(sift_test_mem_trace_fn);

#define SIFT_MEM_DEPTH 16
#define SIFT_MEM_NAMES 256

typedef struct MemKinds MemKinds;
typedef struct MemTrace MemTrace;

typedef struct MemMark
{
	const char *name;
	Size		peak;
	Size		tree_peak;		/* the most the whole tree of the scope held at a checkpoint of it or of a scope inside it */
	MemKinds   *tree_kinds;		/* the kinds of context of the tree at that moment */
	MemTrace   *trace;			/* the tree of the scope along its checkpoints (sift.test_mem_trace) */
	Size		budget;
	const char *formula;
	Size		(*bytes) (void);
	uint64		begins;
	uint64		checks;
} MemMark;

typedef struct MemFrame
{
	MemMark    *mark;
	MemoryContext cx;
	Size		budget;
	SubTransactionId subxid;
} MemFrame;

static MemFrame frames[SIFT_MEM_DEPTH];
static int	depth;
static MemMark marks[SIFT_MEM_NAMES];
static int	nmarks;
static bool registered;
static bool sift_test_mem_check = true;
static bool sift_test_mem_kinds = false;
static bool sift_test_mem_trace = false;
static double sift_test_mem_budget_scale = 1.0;

static MemMark *
mark_of(const char *name)
{
	for (int i = 0; i < nmarks; i++)
		if (marks[i].name == name || strcmp(marks[i].name, name) == 0)
			return &marks[i];
	SIFT_INVARIANT(nmarks < SIFT_MEM_NAMES, "sift: more memory scopes than the %d of core/membudget.c", SIFT_MEM_NAMES);
	for (int i = 0; i < lengthof(budgets); i++)
		if (strcmp(budgets[i].name, name) == 0)
		{
			marks[nmarks] = (MemMark) {.name = name, .bytes = budgets[i].bytes, .formula = budgets[i].formula};
			return &marks[nmarks++];
		}
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("memory scope %s has no row in test/mem-budgets.tsv", name)));
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
sift_mem_scope_begin(const char *name, MemoryContext cx)
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
	mark->budget = mark->bytes();
	frames[depth++] = (MemFrame) {mark, cx, mark->budget, GetCurrentSubTransactionId()};
	return scope;
}

void
sift_mem_scope_end(SiftMemScope *scope)
{
	if (scope->depth < depth)
		depth = scope->depth;
}

#define SIFT_MEM_TOP 4
#define SIFT_MEM_KINDS 64

struct MemKinds
{
	int			n;
	char		name[SIFT_MEM_KINDS][NAMEDATALEN];
	Size		bytes[SIFT_MEM_KINDS];
	int			count[SIFT_MEM_KINDS];
};

/* the contexts of a tree by name: a path that makes a context per partition or per group shows as one kind with its instances */
static void
kinds_add(MemoryContext cx, MemKinds *k)
{
	int			i = 0;

	while (i < k->n && strcmp(k->name[i], cx->name) != 0)
		i++;
	if (i == k->n && k->n < SIFT_MEM_KINDS)
		strlcpy(k->name[k->n++], cx->name, NAMEDATALEN);
	if (i < k->n)
	{
		k->bytes[i] += MemoryContextMemAllocated(cx, false);
		k->count[i]++;
	}
	for (MemoryContext c = cx->firstchild; c != NULL; c = c->nextchild)
		kinds_add(c, k);
}

static char *
top_text(MemoryContext cx)
{
	MemKinds   *k = palloc0(sizeof(MemKinds));
	StringInfoData buf;

	kinds_add(cx, k);
	initStringInfo(&buf);
	for (int shown = 0; shown < SIFT_MEM_TOP; shown++)
	{
		int			big = -1;

		for (int i = 0; i < k->n; i++)
			if (k->count[i] > 0 && (big < 0 || k->bytes[i] > k->bytes[big]))
				big = i;
		if (big < 0)
			break;
		appendStringInfo(&buf, "%s\"%s\" %zu in %d", shown ? ", " : "", k->name[big], k->bytes[big], k->count[big]);
		k->count[big] = 0;
	}
	return buf.data;
}

#define TRACE_SAMPLES 256
#define TRACE_KINDS 12

typedef struct MemSample
{
	uint64		seq;
	Size		total;
	int			n;
	char		kind[TRACE_KINDS][NAMEDATALEN];
	Size		bytes[TRACE_KINDS];
} MemSample;

/* at most TRACE_SAMPLES samples of the checkpoints a scope saw, evenly spread: when they are full every second one is dropped and the stride doubles */
struct MemTrace
{
	uint64		seen;
	uint64		stride;
	int			n;
	MemSample	s[TRACE_SAMPLES];
};

static void
trace_note(MemFrame *f)
{
	MemTrace   *t = f->mark->trace;
	MemKinds   *k;
	MemSample  *s;

	if (t == NULL)
	{
		t = f->mark->trace = MemoryContextAllocZero(TopMemoryContext, sizeof(MemTrace));
		t->stride = 1;
	}
	if (t->seen++ % t->stride != 0)
		return;
	if (t->n == TRACE_SAMPLES)
	{
		for (int i = 0; i < TRACE_SAMPLES / 2; i++)
			t->s[i] = t->s[2 * i];
		t->n = TRACE_SAMPLES / 2;
		t->stride *= 2;
		if ((t->seen - 1) % t->stride != 0)
			return;
	}
	k = palloc0(sizeof(MemKinds));
	kinds_add(f->cx, k);
	s = &t->s[t->n++];
	memset(s, 0, sizeof(*s));
	s->seq = t->seen - 1;
	for (int i = 0; i < k->n; i++)
		s->total += k->bytes[i];
	for (; s->n < TRACE_KINDS; s->n++)
	{
		int			big = -1;

		for (int i = 0; i < k->n; i++)
			if (k->count[i] > 0 && (big < 0 || k->bytes[i] > k->bytes[big]))
				big = i;
		if (big < 0)
			break;
		strlcpy(s->kind[s->n], k->name[big], NAMEDATALEN);
		s->bytes[s->n] = k->bytes[big];
		k->count[big] = 0;
	}
	pfree(k);
}

/* sift.test_mem_kinds: the tree of every scope on the stack, a scope that nests another one included, is measured at each checkpoint, and the kinds of context of its largest are kept */
static void
tree_note(MemFrame *f)
{
	MemMark    *mark = f->mark;
	Size		used;

	if (f->cx == NULL)
		return;
	used = MemoryContextMemAllocated(f->cx, true);
	if (used <= mark->tree_peak)
		return;
	mark->tree_peak = used;
	if (mark->tree_kinds == NULL)
		mark->tree_kinds = MemoryContextAllocZero(TopMemoryContext, sizeof(MemKinds));
	memset(mark->tree_kinds, 0, sizeof(MemKinds));
	kinds_add(f->cx, mark->tree_kinds);
}

static void
frame_check(MemFrame *f)
{
	Size		used,
				limit;

	if (f->cx == NULL)
		return;
	used = MemoryContextMemAllocated(f->cx, true);
	f->mark->checks++;
	if (used > f->mark->peak)
		f->mark->peak = used;
	limit = (Size) ((double) f->budget * sift_test_mem_budget_scale);
	if (used > limit)
		/* check-errors: allow XX000 is the code of a budget failure of the test build, which the CI reports by name */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("sift memory budget exceeded: %s: %zu > %zu bytes (%s)", f->mark->name, used, limit, f->mark->formula),
						errdetail("The largest kinds of context of the scope, in bytes and instances: %s.", top_text(f->cx))));
}

static void
trees_note(void)
{
	for (int i = 0; i < depth; i++)
	{
		if (frames[i].cx == NULL)
			continue;
		tree_note(&frames[i]);
		if (sift_test_mem_trace)
			trace_note(&frames[i]);
	}
}

void
sift_mem_checkpoint(void)
{
	if (depth == 0 || !sift_test_mem_check)
		return;
	if (sift_test_mem_kinds || sift_test_mem_trace)
		trees_note();
	frame_check(&frames[depth - 1]);
}

/* the checkpoint of the scope `name` on the stack, whatever scope is in hand */
void
sift_mem_checkpoint_of(const char *name)
{
	if (!sift_test_mem_check)
		return;
	for (int i = depth - 1; i >= 0; i--)
		if (strcmp(frames[i].mark->name, name) == 0)
		{
			frame_check(&frames[i]);
			return;
		}
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
		{
			marks[i].peak = marks[i].tree_peak = marks[i].begins = marks[i].checks = 0;
			if (marks[i].trace != NULL)
			{
				memset(marks[i].trace, 0, sizeof(MemTrace));
				marks[i].trace->stride = 1;
			}
		}
	return (Datum) 0;
}

/* sift._test_mem_trace() returns setof (scope text, seq int8, kind text, bytes int8): the samples of the trace of each scope (sift.test_mem_trace), a row "(tree)" with the whole tree and one for each of its largest kinds */
Datum
sift_test_mem_trace_fn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old;

	InitMaterializedSRF(fcinfo, 0);
	old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	for (int i = 0; i < nmarks; i++)
	{
		const MemTrace *t = marks[i].trace;

		for (int j = 0; t != NULL && j < t->n; j++)
		{
			const MemSample *s = &t->s[j];
			bool		n[4] = {false, false, false, false};
			Datum		v[4] = {CStringGetTextDatum(marks[i].name), Int64GetDatum((int64) s->seq), CStringGetTextDatum("(tree)"), Int64GetDatum((int64) s->total)};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
			for (int k = 0; k < s->n; k++)
			{
				v[2] = CStringGetTextDatum(s->kind[k]);
				v[3] = Int64GetDatum((int64) s->bytes[k]);
				tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
			}
		}
	}
	MemoryContextSwitchTo(old);
	return (Datum) 0;
}

/*
 * sift._test_mem_kinds() returns setof (scope text, kind text, instances int8, bytes int8): the census of the scopes this backend ran with sift.test_mem_kinds on. Per
 * scope, a row of kind "(tree)" with the most its whole tree held at a checkpoint (the scopes nested in it counted), then the kinds of context of the tree at that moment
 * by name, the instances of a kind summed.
 */
Datum
sift_test_mem_kinds_fn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old;

	InitMaterializedSRF(fcinfo, 0);
	old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	for (int i = 0; i < nmarks; i++)
	{
		const MemKinds *k = marks[i].tree_kinds;
		bool		n[4] = {false, false, false, false};
		Datum		v[4] = {CStringGetTextDatum(marks[i].name), CStringGetTextDatum("(tree)"), Int64GetDatum(0), Int64GetDatum((int64) marks[i].tree_peak)};

		if (k == NULL || marks[i].tree_peak == 0)
			continue;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
		for (int j = 0; j < k->n; j++)
		{
			v[1] = CStringGetTextDatum(k->name[j]);
			v[2] = Int64GetDatum(k->count[j]);
			v[3] = Int64GetDatum((int64) k->bytes[j]);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, n);
		}
	}
	MemoryContextSwitchTo(old);
	return (Datum) 0;
}

void
sift_membudget_init(void)
{
	DefineCustomRealVariable("sift.test_mem_budget_scale",
							 "Testing: the factor every memory budget of test/mem-budgets.tsv is multiplied by at a checkpoint (0.1 makes a path fail that holds more than a tenth of it).",
							 NULL, &sift_test_mem_budget_scale, 1.0, 0.0001, 1000.0, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_kinds",
							 "Testing: the checkpoints measure the whole tree of every scope on the stack and keep its kinds of context at the largest (sift._test_mem_kinds()).",
							 NULL, &sift_test_mem_kinds, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_trace",
							 "Testing: the checkpoints keep up to 256 samples of the tree of every scope on the stack, by kind of context (sift._test_mem_trace()).",
							 NULL, &sift_test_mem_trace, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_check",
							 "Testing: the checkpoints of the memory scopes measure and enforce the budgets (off: they return at once).",
							 NULL, &sift_test_mem_check, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

#endif							/* SIFT_TESTING */
