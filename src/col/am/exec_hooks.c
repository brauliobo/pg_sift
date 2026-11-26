/*
 * col/am/exec_hooks.c - the executor hooks of the sift_col access method: a statement that writes is noted when it starts and when it ends (col/am.h), and the
 * index-only scans of its plan are told which columns they read (col/am/ioss.c).
 */
#include "postgres.h"

#include "executor/executor.h"

#include "col/am.h"
#include "core/catalog.h"

static ExecutorStart_hook_type prev_executor_start;
static ExecutorFinish_hook_type prev_executor_finish;

static void
col_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && sift_db_has_extension())
	{
		sift_col_ioss_want(queryDesc);
		sift_col_stmt_start(queryDesc);
	}
}

/* The statement that loaded the library while it ran was not noted by col_executor_start: its AFTER triggers (the ones of track_changes) fire in ExecutorFinish */
static void
col_executor_finish(QueryDesc *queryDesc)
{
	if (sift_db_has_extension())
		sift_col_stmt_finish(queryDesc);
	if (prev_executor_finish)
		prev_executor_finish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

void
sift_col_executor_init(void)
{
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = col_executor_start;
	prev_executor_finish = ExecutorFinish_hook;
	ExecutorFinish_hook = col_executor_finish;
}
