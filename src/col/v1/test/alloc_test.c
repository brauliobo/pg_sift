/*
 * col/v1/test/alloc_test.c - exact memory accounting of a call: runs it n times in a context of its own and says how many bytes that holds.
 * A call that allocates for its caller and does not free (a leak in a long-lived context) shows as bytes that grow with n; a clean one holds the same bytes whatever n is.
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/genam.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/seg/int.h"
#include "col/v1/test/int.h"
#include "core/catname.h"

PG_FUNCTION_INFO_V1(sift_test_row_key_bytes);
PG_FUNCTION_INFO_V1(sift_test_option_syntax_bytes);
PG_FUNCTION_INFO_V1(sift_test_unit_room_failure);

/* the bytes left allocated in a context of its own by n calls of ask(arg) */
static int64
held_by(int n, void (*ask) (const void *arg), const void *arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift test alloc", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	MemoryContextCounters c = {0};

	for (; n > 0; n--)
		ask(arg);
	MemoryContextSwitchTo(old);
	MemoryContextMemConsumed(cx, &c);
	MemoryContextDelete(cx);
	return (int64) (c.totalspace - c.freespace);
}

static void
ask_track_on(const void *index)
{
	(void) sift_col_track_on((Relation) index);
}

/* sift._test_row_key_bytes(index regclass, n int) returns int8: the bytes that n questions whether the index tracks a key leave allocated in their context */
Datum
sift_test_row_key_bytes(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	int64		held = held_by(PG_GETARG_INT32(1), ask_track_on, index);

	index_close(index, AccessShareLock);
	PG_RETURN_INT64(held);
}

typedef struct SyntaxAsk
{
	const char *(*check) (const char *opt);
	const char *opt;
} SyntaxAsk;

static void
ask_syntax(const void *ask)
{
	const SyntaxAsk *a = ask;

	(void) a->check(a->opt);
}

/* sift._test_option_syntax_bytes(option text, opt text, n int) returns int8: the bytes that n syntax checks of the value of an option (preagg, rollup_leaves, calendar_columns) leave allocated in their context */
Datum
sift_test_option_syntax_bytes(PG_FUNCTION_ARGS)
{
	char	   *option = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SyntaxAsk	ask = {NULL, text_to_cstring(PG_GETARG_TEXT_PP(1))};

	if (strcmp(option, "preagg") == 0)
		ask.check = sift_cpagg_syntax;
	else if (strcmp(option, "rollup_leaves") == 0)
		ask.check = sift_cpagg_rollup_syntax;
	else if (strcmp(option, "calendar_columns") == 0)
		ask.check = sift_cpagg_calendar_syntax;
	else
		elog(ERROR, "unknown option \"%s\"", option);
	PG_RETURN_INT64(held_by(PG_GETARG_INT32(2), ask_syntax, &ask));
}

/*
 * sift._test_unit_room_failure(): a segment asks for a unit buffer no allocation can give (more than 1 GB), the error is caught and the segment's context ends, as a query that fails does;
 * the next segment then takes its buffer, which must be a whole one: it is written to its end
 */
Datum
sift_test_unit_room_failure(PG_FUNCTION_ARGS)
{
	MemoryContext outer = CurrentMemoryContext;
	MemoryContext failed = AllocSetContextCreate(outer, "sift test failed segment", ALLOCSET_SMALL_SIZES);
	SiftColSegment fseg = {.mcxt = failed};
	SegPriv		fpv = {0};
	MemoryContext next = AllocSetContextCreate(outer, "sift test next segment", ALLOCSET_SMALL_SIZES);
	SiftColSegment nseg = {.mcxt = next};
	SegPriv		npv = {0};
	const uint32 len = 100000;
	bool		whole;

	PG_TRY();
	{
		sift_cseg_unit_room(&fseg, &fpv, UINT32_C(1) << 31);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(outer);
		FlushErrorState();
	}
	PG_END_TRY();
	MemoryContextDelete(failed);
	sift_cseg_unit_room(&nseg, &npv, len);
	memset(npv.unitbuf, 0x5a, npv.unitcap);
	whole = npv.unitcap >= len + SIFT_COL_PAD;
	MemoryContextDelete(next);
	PG_RETURN_BOOL(whole);
}
#endif
