/*
 * col/maint/roots.c - the string roots of the documents of the segments written before they said they were flagged (SIFT_SEG_ROOTS).
 *
 * A segment of an older library stores a document that is a string as a missing key in every column below it, so doc ? 'k' reads all the NULL slots of such a
 * segment again from the heap. VACUUM looks at the heap once: where no version of any row holds a document of the index that is a string (a JSON string at the
 * root, which no application writes by accident), the segments it has now cannot hold one, and the entry gets the flag. The rows written afterwards are the write buffer's, which
 * is flagged. A heap that has such a document keeps the segments as they are (a read of them stays exact, and is priced as a read of the heap), and each VACUUM looks again.
 */
#include "postgres.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "utils/jsonb.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"
#include "core/ops.h"

typedef struct RootsGens
{
	uint64	   *gen;
	int			n;
} RootsGens;

static void
set_roots(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const RootsGens *g = arg;

	for (int i = 0; i < dir->nsegs; i++)
		for (int k = 0; k < g->n; k++)
			if (dir->segs[i].gen == g->gen[k] && dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
				dir->segs[i].flags |= SIFT_SEG_ROOTS;
}

/* is there a version of a row, dead or alive, whose document in a jsonb column of the index is a string? */
static bool
string_root_in_heap(Relation index, Relation heap)
{
	TupleDesc	desc = RelationGetDescr(index);
	IndexInfo  *info = BuildIndexInfo(index);
	EState	   *estate = CreateExecutorState();
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	TableScanDesc scan = sift_table_beginscan(heap, SnapshotAny, 0, NULL);
	bool		found = false;
	uint64		n = 0;

	GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
	while (!found && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];

		if ((++n & 1023) == 0)
			sift_vacuum_delay_point();
		FormIndexDatum(info, slot, estate, values, isnull);
		for (int i = 0; !found && i < info->ii_NumIndexAttrs; i++)
		{
			JsonbValue	v;

			found = TupleDescAttr(desc, i)->atttypid == JSONBOID && !isnull[i] && JsonbExtractScalar(&DatumGetJsonbP(values[i])->root, &v) && v.type == jbvString;
		}
		ResetPerTupleExprContext(estate);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	return found;
}

/*
 * VACUUM, with the maintenance lock held: flag the segments without it when the heap has no document that is a string. The expressions of the index run on every
 * version of every row, which they were not meant for: a failure leaves the segments as they are.
 */
void
maint_roots_verify(Relation index, Relation heap)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	RootsGens	g = {.gen = palloc(SIFT_MAX_SEGMENTS * sizeof(uint64))};
	uint64		ndocs = 0;
	TimestampTz start = GetCurrentTimestamp();
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	bool		found = true;

	if (RecoveryInProgress())
		return;
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && !(snap->segs[i].flags & SIFT_SEG_ROOTS))
		{
			g.gen[g.n++] = snap->segs[i].gen;
			ndocs += snap->segs[i].ndocs;
		}
	if (g.n == 0)
		return;
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		found = string_root_in_heap(index, heap);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
		ereport(LOG, (errmsg("sift_col index \"%s\": the string roots of its documents could not be looked for: %s", RelationGetRelationName(index), e->message)));
		FreeErrorData(e);
	}
	PG_END_TRY();
	if (found)
		return;
	sift_dir_update(index, &(SiftDirChange) {.fn = set_roots,.arg = &g,.extra = InvalidBuffer});
	sift_log_maint(index, "flagged the string roots of", start, ndocs, 0);
}
