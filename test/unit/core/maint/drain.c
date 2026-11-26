/*
 * test/unit/core/maint/drain.c - the hold of core/maint/drain.c on a database whose workers must leave: a statement that runs inside another one (an event trigger, a DO block) takes and lets
 * go its own hold (HELD_MAX deep at most), an error lets go every hold, and the jobs of a database that is gone are forgotten before its hold is let go (the launcher starts no worker for them in between).
 */
#include "postgres.h"

#include "core/maint/drain.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;
Oid			MyDatabaseId = 5;
ProcessUtility_hook_type ProcessUtility_hook;
static Latch latch;
struct Latch *MyLatch = &latch;

static SiftMaintShmem table;
static int	holds,
			releases,
			purges,
			nested,
			depth;
static bool purged_while_held,
			fail_inner;
static void (*exit_cb) (int, Datum);

SiftMaintShmem *
sift_maint_table(void)
{
	return &table;
}

bool
sift_maint_db_hold(SiftMaintShmem *m, Oid db)
{
	holds++;
	return true;
}

void
sift_maint_db_release(SiftMaintShmem *m, Oid db)
{
	SIFT_CHECK(releases < holds);
	releases++;
}

int
sift_maint_db_workers(SiftMaintShmem *m, Oid db)
{
	return 0;
}

void
sift_maint_db_purge(SiftMaintShmem *m, Oid db)
{
	purges++;
	purged_while_held = holds > releases;
}

void
sift_shm_manifest_invalidate_db(Oid db)
{
}

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	exit_cb = function;
}

void
pgstat_clear_backend_activity_snapshot(void)
{
}

int
pgstat_fetch_stat_numbackends(void)
{
	return 0;
}

TimestampTz
GetCurrentTimestamp(void)
{
	return 0;
}

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
throw_error(void)
{
	pg_re_throw();
}

static String ext_name = {.type = T_String,.sval = "pg_sift"};
static DropStmt drop_ext = {.type = T_DropStmt,.removeType = OBJECT_EXTENSION};
static PlannedStmt pstmt = {.type = T_PlannedStmt,.utilityStmt = (Node *) &drop_ext};

static void
run_drop(void)
{
	drain_utility(&pstmt, "DROP EXTENSION pg_sift", false, PROCESS_UTILITY_TOPLEVEL, NULL, NULL, NULL, NULL);
}

/* The statement runs another one inside it, as an event trigger does, until `nested` deep; with fail_inner the innermost one fails */
static void
statement(PlannedStmt *p, const char *q, bool ro, ProcessUtilityContext c, ParamListInfo params, QueryEnvironment *env, DestReceiver *dest, QueryCompletion *qc)
{
	if (depth++ < nested)
		run_drop();
	else if (fail_inner)
		throw_error();
	depth--;
}

static void
reset(int n, bool fail)
{
	holds = releases = purges = depth = 0;
	purged_while_held = false;
	nested = n;
	fail_inner = fail;
}

static void
test_single(void)
{
	reset(0, false);
	run_drop();
	SIFT_CHECKF(holds == 1 && releases == 1 && purges == 1, "one hold, let go: %d holds, %d releases", holds, releases);
	SIFT_CHECKF(purged_while_held, "the jobs of the database are forgotten while it is still held");
}

static void
test_nested(void)
{
	reset(3, false);
	run_drop();
	SIFT_CHECKF(holds == 4 && releases == 4, "every nested statement lets go its own hold: %d holds, %d releases", holds, releases);
	SIFT_CHECKF(purges == 4, "each statement forgets the jobs: %d purges", purges);
}

static void
test_nested_error(void)
{
	volatile bool failed = false;

	reset(2, true);
	PG_TRY();
	{
		run_drop();
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	SIFT_CHECK(failed);
	SIFT_CHECKF(holds == 3 && releases == 3 && purges == 0, "an error lets go every hold and forgets no job: %d holds, %d releases, %d purges", holds, releases, purges);
	SIFT_CHECK(exit_cb != NULL);
	exit_cb(1, (Datum) 0);
	SIFT_CHECKF(releases == 3, "the exit lets go nothing twice: %d releases", releases);
}

/* Past HELD_MAX nested statements the next one is refused, and the holds of the others are let go */
static void
test_too_deep(void)
{
	volatile bool failed = false;

	reset(HELD_MAX, false);
	pgshim_error_hook = throw_error;
	PG_TRY();
	{
		run_drop();
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	pgshim_error_hook = NULL;
	SIFT_CHECKF(failed && holds == HELD_MAX && releases == HELD_MAX, "too deep: %d holds, %d releases", holds, releases);
}

int
main(void)
{
	List	   *objects = calloc(1, offsetof(List, initial_elements) + sizeof(ListCell));

	*objects = (List) {.type = T_List,.length = 1,.max_length = 1,.elements = objects->initial_elements};
	objects->elements[0].ptr_value = &ext_name;
	drop_ext.objects = objects;
	prev_utility = statement;
	pgshim_keep();
	test_single();
	test_nested();
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf of a PG_TRY that the ERROR goes to */
	test_nested_error();
	test_too_deep();
#endif
	free(objects);
	return sift_check_summary();
}
