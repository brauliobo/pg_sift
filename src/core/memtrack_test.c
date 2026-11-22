/*
 * core/memtrack_test.c - the test telemetry of the memory trackers (core/memtrack.h), compiled only with SIFT_TESTING.
 *
 * Each tracker name has a mark: the most its tree held at a measurement, the budget, the trackers begun and the measurements. The marks are written to the server log
 * at backend exit (SIFTMEM_SCOPE lines) and returned by sift._test_mem_highwater(); with sift.test_mem_kinds the kinds of context of the largest tree are kept
 * (sift._test_mem_kinds()), with sift.test_mem_trace up to 256 samples of the tree along the measurements (sift._test_mem_trace()).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "funcapi.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "core/host.h"
#include "core/memtrack.h"
#include "core/sift.h"

PG_FUNCTION_INFO_V1(sift_test_mem_highwater);
PG_FUNCTION_INFO_V1(sift_test_mem_kinds_fn);
PG_FUNCTION_INFO_V1(sift_test_mem_trace_fn);

#define MARK_NAMES 256
#define ASSERT_PCT 115
#define MB			((Size) 1024 * 1024)

/*
 * What a tracker holds that no setting sizes (the pages of a decoded segment, a search's open segments, the vectors of a kernel): added to its budget by the assertion of the test build,
 * measured for each scope at the smallest settings the tests use. A scope that is not here is held to its budget. These rows are the part of the memory work that is not done: each goes
 * when its structure is bounded by a setting.
 */
static const struct
{
	const char *name;
	Size		fixed;
}			scope_fixed[] = {
	{"agg_group", 16 * MB}, {"agg_group_nospill", 16 * MB}, {"bm25_build", 16 * MB}, {"bm25_merge", 8 * MB}, {"bm25_promote", 8 * MB}, {"bm25_search", 32 * MB}, {"bm25_verify", 8 * MB},
	{"col_build", 16 * MB}, {"col_build_open", 4 * MB}, {"col_build_spool", 4 * MB}, {"col_key_cut_layout", 64 * MB}, {"col_merge_inputs", 8 * MB}, {"col_merge_window", 24 * MB},
	{"col_scope_refill", 128 * MB}, {"col_tomb_log", 8 * MB}, {"col_verify", 64 * MB}, {"col_writer_order", 16 * MB}, {"col_writer_spool", 16 * MB},
	{"index_vacuum", 16 * MB},
};

/* the scopes whose structure grows by doubling its arrays (the group table of an aggregate): the assertion allows the step that crosses the budget */
static bool
scope_doubles(const char *name)
{
	return strcmp(name, "agg_group") == 0 || strcmp(name, "agg_group_nospill") == 0;
}

static Size
scope_fixed_bytes(const char *name)
{
	for (int i = 0; i < lengthof(scope_fixed); i++)
		if (strcmp(scope_fixed[i].name, name) == 0)
			return scope_fixed[i].fixed;
	return 0;
}

typedef struct MemKinds MemKinds;
typedef struct MemTrace MemTrace;

typedef struct MemMark
{
	const char *name;
	Size		peak;
	Size		tree_peak;
	MemKinds   *tree_kinds;
	MemTrace   *trace;
	Size		budget;
	uint64		begins;
	uint64		checks;
	const char *parent;
} MemMark;

static MemMark marks[MARK_NAMES];
static int	nmarks;
static bool registered;
static bool sift_test_mem_check = true;
static bool sift_test_mem_kinds = false;
static bool sift_test_mem_trace = false;
static double sift_test_mem_budget_scale = 1.0;

static void
log_marks(int code, Datum arg)
{
	for (int i = 0; i < nmarks; i++)
		elog(LOG, "SIFTMEM_SCOPE name=%s peak=%zu budget=%zu checks=%llu", marks[i].name, marks[i].peak, marks[i].budget,
			 (unsigned long long) marks[i].checks);
}

static MemMark *
mark_of(const char *name)
{
	for (int i = 0; i < nmarks; i++)
		if (marks[i].name == name || strcmp(marks[i].name, name) == 0)
			return &marks[i];
	SIFT_INVARIANT(nmarks < MARK_NAMES, "sift: more memory trackers than the %d of core/memtrack_test.c", MARK_NAMES);
	if (!registered)
	{
		on_proc_exit(log_marks, 0);
		registered = true;
	}
	marks[nmarks] = (MemMark) {.name = name};
	return &marks[nmarks++];
}

void
sift_memtrack_begin(SiftMemTracker *t)
{
	MemMark    *m = mark_of(t->name);

	m->begins++;
	m->budget = t->budget;
	m->parent = t->parent ? t->parent->name : NULL;
}

#define MEM_TOP 4
#define MEM_KINDS 64

struct MemKinds
{
	int			n;
	char		name[MEM_KINDS][NAMEDATALEN];
	Size		bytes[MEM_KINDS];
	int			count[MEM_KINDS];
};

/* the contexts of a tree by name: a path that makes a context per partition or per group shows as one kind with its instances */
static void
kinds_add(MemoryContext cx, MemKinds *k)
{
	int			i = 0;

	while (i < k->n && strcmp(k->name[i], cx->name) != 0)
		i++;
	if (i == k->n && k->n < MEM_KINDS)
		strlcpy(k->name[k->n++], cx->name, NAMEDATALEN);
	if (i < k->n)
	{
		k->bytes[i] += MemoryContextMemAllocated(cx, false);
		k->count[i]++;
	}
	for (MemoryContext c = cx->firstchild; c != NULL; c = c->nextchild)
		kinds_add(c, k);
}

static int
kinds_largest(const MemKinds *k)
{
	int			big = -1;

	for (int i = 0; i < k->n; i++)
		if (k->count[i] > 0 && (big < 0 || k->bytes[i] > k->bytes[big]))
			big = i;
	return big;
}

static char *
top_text(MemoryContext cx)
{
	MemKinds   *k = palloc0(sizeof(MemKinds));
	StringInfoData buf;

	kinds_add(cx, k);
	initStringInfo(&buf);
	for (int shown = 0; shown < MEM_TOP; shown++)
	{
		int			big = kinds_largest(k);

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

/* at most TRACE_SAMPLES samples of the measurements of a tracker, evenly spread: when they are full every second one is dropped and the stride doubles */
struct MemTrace
{
	uint64		seen;
	uint64		stride;
	int			n;
	MemSample	s[TRACE_SAMPLES];
};

static void
trace_note(MemMark *m, MemoryContext cx)
{
	MemTrace   *t = m->trace;
	MemKinds   *k;
	MemSample  *s;

	if (t == NULL)
	{
		t = m->trace = MemoryContextAllocZero(TopMemoryContext, sizeof(MemTrace));
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
	kinds_add(cx, k);
	s = &t->s[t->n++];
	memset(s, 0, sizeof(*s));
	s->seq = t->seen - 1;
	for (int i = 0; i < k->n; i++)
		s->total += k->bytes[i];
	for (; s->n < TRACE_KINDS; s->n++)
	{
		int			big = kinds_largest(k);

		if (big < 0)
			break;
		strlcpy(s->kind[s->n], k->name[big], NAMEDATALEN);
		s->bytes[s->n] = k->bytes[big];
		k->count[big] = 0;
	}
	pfree(k);
}

static void
tree_note(MemMark *m, MemoryContext cx, Size used)
{
	if (used <= m->tree_peak)
		return;
	m->tree_peak = used;
	if (m->tree_kinds == NULL)
		m->tree_kinds = MemoryContextAllocZero(TopMemoryContext, sizeof(MemKinds));
	memset(m->tree_kinds, 0, sizeof(MemKinds));
	kinds_add(cx, m->tree_kinds);
}

void
sift_memtrack_note(SiftMemTracker *t, Size used)
{
	MemMark    *m = mark_of(t->name);

	m->checks++;
	m->peak = Max(m->peak, used);
	m->budget = t->budget;
	if (sift_test_mem_kinds)
		tree_note(m, t->root_cx, used);
	if (sift_test_mem_trace)
		trace_note(m, t->root_cx);
}

/* the kinds and the trace of the tree of a tracker that holds the one measured, without a measurement of its own */
static void
census_note(SiftMemTracker *t)
{
	MemMark    *m = mark_of(t->name);

	if (sift_test_mem_kinds)
		tree_note(m, t->root_cx, MemoryContextMemAllocated(t->root_cx, true));
	if (sift_test_mem_trace)
		trace_note(m, t->root_cx);
}

void
sift_mem_assert_within(SiftMemTracker *t)
{
	Size		used;
	Size		limit;

	if (!sift_test_mem_check)
		return;
	used = sift_mem_used(t);
	limit = (Size) (((double) t->budget / 100.0 * (scope_doubles(t->name) ? 200 : ASSERT_PCT) + (double) scope_fixed_bytes(t->name)) *
					sift_test_mem_budget_scale);
	if (used > limit)
		/* check-errors: allow XX000 is the code of a budget failure of the test build, which the CI reports by name */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("sift memory budget exceeded: %s: %zu > %zu bytes", t->name, used, limit),
						errdetail("The largest kinds of context of the tracker, in bytes and instances: %s.", top_text(t->root_cx))));
	if (t->parent != NULL)
		census_note(t->parent);
}

/* sift._test_mem_highwater(reset bool) returns setof (scope text, peak int8, budget int8, begins int8, checks int8, parent text): the trackers this backend ran and the most memory each held at a measurement; parent names the tracker whose tree holds this one's */
Datum
sift_test_mem_highwater(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old;

	InitMaterializedSRF(fcinfo, 0);
	old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	for (int i = 0; i < nmarks; i++)
	{
		Datum		v[6] = {CStringGetTextDatum(marks[i].name), Int64GetDatum((int64) marks[i].peak), Int64GetDatum((int64) marks[i].budget),
		Int64GetDatum((int64) marks[i].begins), Int64GetDatum((int64) marks[i].checks), 0};
		bool		n[6] = {false, false, false, false, false, marks[i].parent == NULL};

		if (marks[i].parent != NULL)
			v[5] = CStringGetTextDatum(marks[i].parent);

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

/* sift._test_mem_trace() returns setof (scope text, seq int8, kind text, bytes int8): the samples of the trace of each tracker (sift.test_mem_trace), a row "(tree)" with the whole tree and one for each of its largest kinds */
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
 * sift._test_mem_kinds() returns setof (scope text, kind text, instances int8, bytes int8): the census of the trackers this backend ran with sift.test_mem_kinds on. Per
 * tracker, a row of kind "(tree)" with the most its whole tree held at a measurement, then the kinds of context of the tree at that moment by name, the instances of a kind summed.
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
sift_memtrack_define_test_gucs(void)
{
	DefineCustomRealVariable("sift.test_mem_budget_scale",
							 "Testing: the factor the budget of every memory tracker is multiplied by when a path asserts it is within it (0.1 makes a path fail that holds more than a tenth of it).",
							 NULL, &sift_test_mem_budget_scale, 1.0, 0.0001, 1000.0, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_kinds",
							 "Testing: the measurements of the memory trackers keep the kinds of context of the largest tree of each (sift._test_mem_kinds()).",
							 NULL, &sift_test_mem_kinds, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_trace",
							 "Testing: the measurements of the memory trackers keep up to 256 samples of the tree of each, by kind of context (sift._test_mem_trace()).",
							 NULL, &sift_test_mem_trace, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_mem_check",
							 "Testing: the assertions of the memory trackers measure and enforce the budgets (off: they return at once).",
							 NULL, &sift_test_mem_check, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

#endif							/* SIFT_TESTING */
