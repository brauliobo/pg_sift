/*
 * test/unit/core/writer_layout_chkpt.c - the LAYOUT write of the metapage and the directory (core/writer.c, layout_write) holds back the redo pointer of a checkpoint
 * (DELAY_CHKPT_START) only while it writes: a WAL write, a flush or a file extend that fails leaves the backend without the flag, or every later checkpoint waits for it.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/pgshim.h"

#include "core/writer.c"

typedef enum
{
	FAIL_NONE,
	FAIL_WAL,
	FAIL_FLUSH,
	FAIL_EXTEND,
} FailAt;

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;
PGPROC	   *MyProc;
int			wal_level = WAL_LEVEL_REPLICA;
static FailAt fail_at;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
fail_if(FailAt at)
{
	if (fail_at == at)
		ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("could not extend file: No space left on device")));
}

void
sift_page_init(Page page, SiftPageType type, uint64 gen)
{
	memset(page, 0, BLCKSZ);
}

void
PageSetChecksumInplace(Page page, BlockNumber blkno)
{
}

void
log_newpages(RelFileLocator *rlocator, ForkNumber forknum, int num_pages, BlockNumber *blknos, Page *pages, bool page_std)
{
	fail_if(FAIL_WAL);
}

void
XLogFlush(XLogRecPtr record)
{
	fail_if(FAIL_FLUSH);
}

void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, const void *buffer, bool skipFsync)
{
	fail_if(FAIL_EXTEND);
}

static bool
write_fails(SiftWriteTarget *wt)
{
	volatile bool failed = false;

	PG_TRY();
	{
		layout_write(wt);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	return failed;
}

int
main(void)
{
	static const char *const what[] = {"nothing", "the WAL write", "the WAL flush", "the file extend"};
	PGPROC		proc = {0};
	FormData_pg_class form = {.relpersistence = RELPERSISTENCE_PERMANENT};
	SMgrRelationData srel = {0};
	RelationData index = {.rd_rel = &form,.rd_smgr = &srel};
	SiftWriteTarget wt = {.index = &index,.mode = SIFT_WRITE_LAYOUT};

	MyProc = &proc;
	pgshim_error_hook = pg_re_throw;
	for (fail_at = FAIL_NONE; fail_at <= FAIL_EXTEND; fail_at++)
	{
		proc.delayChkptFlags = 0;
		SIFT_CHECKF(write_fails(&wt) == (fail_at != FAIL_NONE), "%s: the layout write did not fail as the storage did", what[fail_at]);
		SIFT_CHECKF((proc.delayChkptFlags & DELAY_CHKPT_START) == 0, "%s failed: the backend still delays every checkpoint", what[fail_at]);
	}
	return sift_check_summary();
}
