/*
 * test/unit/table/f2/vacuum_xids.c - the dead rows VACUUM of a sift_table looks for (table/f2/vacuum.c, collect_part): a granule whose rows a few writers inserted interleaved asks the
 * process array of each writer once, not once a row (every TransactionIdIsInProgress takes ProcArrayLock and walks every backend), and the rows of the writer that aborted are the dead ones.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "access/transam.h"
#include "utils/rel.h"

extern FullTransactionId sift_standby_safe_horizon(Relation index, bool urgent_freeze);

#include "table/f2/vacuum.c"
#include "core/xid_cache.c"

#define WRITERS 4
#define ABORTED_XID 1003

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

static SiftRowVer vers[SIFT_GRANULE_ROWS];
static int	in_progress_calls;
static uint32 dead_found;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

bool
TransactionIdIsInProgress(TransactionId xid)
{
	in_progress_calls++;
	return false;
}

bool
TransactionIdDidCommit(TransactionId xid)
{
	return xid != ABORTED_XID;
}

FullTransactionId
ReadNextFullTransactionId(void)
{
	return FullTransactionIdFromEpochAndXid(0, 2000);
}

#if PG_VERSION_NUM >= 180000
void
vacuum_delay_point(bool is_analyze)
{
}
#else
void
vacuum_delay_point(void)
{
}
#endif

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

void
sift_f2_vacuum_progress_scanned(int done, int nunits)
{
}

static SiftF2PartPin *the_pin;

SiftF2PartPin *
sift_f2_pin_open_uncached(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	return the_pin;
}

void
sift_f2_pin_release(SiftF2PartPin *pin)
{
}

SiftRowVer *
sift_part_read_vers(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g)
{
	return vers;
}

void
sift_f2_pin_granule_tids(Relation rel, SiftF2PartPin *pin, uint32 g, uint64 *out)
{
	for (uint32 r = 0; r < SIFT_GRANULE_ROWS; r++)
		out[r] = r;
}

static void
on_dead(void *vac_ctx, const uint64 *tids, uint32 ntids)
{
	dead_found += ntids;
}

int
main(void)
{
	static SiftPart part;
	SiftGranuleDesc gdir = {.nrows = SIFT_GRANULE_ROWS};
	SiftF2PartPin pin = {.part = &part,.gdir = &gdir};
	const SiftF2HostOps host = {.on_vacuum_dead_tids = on_dead};
	SiftF2Store store = {.host = &host};
	VacCtx		vc = {.parts_total = 1};
	SiftPartitionEntry e = {0};
	SiftPartDesc d = {0};

	part.hdr.ngranules = 1;
	the_pin = &pin;
	for (uint32 r = 0; r < SIFT_GRANULE_ROWS; r++)
		vers[r] = (SiftRowVer) {.xmin = 1000 + r % WRITERS};
	collect_part(&vc, &store, &e, &d, 900);
	SIFT_CHECKF(dead_found == SIFT_GRANULE_ROWS / WRITERS, "%u rows dead, the aborted writer inserted %u", dead_found, SIFT_GRANULE_ROWS / WRITERS);
	SIFT_CHECKF(in_progress_calls <= WRITERS, "a granule of %d interleaved writers asked the process array %d times", WRITERS, in_progress_calls);
	return sift_check_summary();
}
