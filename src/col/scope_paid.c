/*
 * col/scope_paid.c - what the reads of a scope index paid for the pairs that changed since its floor (col/scope.h)
 *
 * A read that finds changed members pays for them on top of the filled read, until the scope segments are written again, and the maintenance sees no statement. The sum of what the reads paid is
 * weighed against the price of the rewrite (refresh_priced): the rewrite is due when it comes to half of it. The sum lives in the metapage of the index (SiftMetaPageData.scope_paid, one number for
 * each scope), beside the floor it is the sum for: the switch that moves the floor makes it zero in the same record (scope_floor.c), so it needs no key and no table, an index has its own, however many
 * there are, and a restart or a crash keeps what the log has (the table of the shared memory it replaces was empty after either, and had 64 slots that the write buffers' reads shared).
 *
 * A read adds to a small list of its backend, and the end of its statement (sift_col_vacuum_tick) writes the list in one record of the metapage alone, which does not touch the directory, so no scan that holds a copy of
 * it is told of a change. A backend that ends loses nothing, and a burst of fast reads in one session counts in full (a write once for each naptime lost all but the first of them with the session). An index nobody
 * changes the pairs of is never written; one that is costs a record of some hundred bytes for each read that pays for a changed pair, up to half the price of the rewrite and not past it (the rule is met then:
 * a rewrite that cannot run does not make every read write). A standby cannot write: its reads count nowhere (docs/COL-SCOPE.md).
 */
#include "postgres.h"

#include "access/xlog.h"
#include "utils/memutils.h"

#include "col/scope.h"
#include "core/restrict.h"
#include "core/store.h"

typedef struct Pending
{
	Oid			index;
	int			ord;
	uint64		epoch;			/* the floor the reads paid under */
	uint64		instr;
} Pending;

static Pending *pending;
static int	npending,
			cap;

static Pending *
pending_of(Oid index, int ord)
{
	for (int i = 0; i < npending; i++)
		if (pending[i].index == index && pending[i].ord == ord)
			return &pending[i];
	if (npending == cap)
	{
		cap = Max(cap * 2, 8);
		pending = pending ? repalloc(pending, cap * sizeof(Pending)) : MemoryContextAlloc(TopMemoryContext, cap * sizeof(Pending));
	}
	pending[npending] = (Pending) {.index = index,.ord = ord};
	return &pending[npending++];
}

void
sift_scope_paid_note(Relation index, int ord, uint64 epoch, uint64 instr)
{
	Pending    *p = pending_of(RelationGetRelid(index), ord);

	if (p->epoch != epoch)
		*p = (Pending) {.index = p->index,.ord = ord,.epoch = epoch};
	p->instr += instr;
}

uint64
sift_scope_paid(Relation index, const SiftMetaPageData *meta, int ord)
{
	SiftConnStamp floor[SIFT_SCOPE_JOURNALS];
	uint64		epoch,
				paid = meta->scope_paid[ord];

	sift_scope_floor_get(meta, ord, floor);
	epoch = sift_scope_epoch(floor, ord);
	for (int i = 0; i < npending; i++)
		if (pending[i].index == RelationGetRelid(index) && pending[i].ord == ord && pending[i].epoch == epoch)
			paid += pending[i].instr;
	return paid;
}

typedef struct Flush
{
	Oid			index;
	double		half;
} Flush;

/* The pending sums of the index, added to the sums of the scopes whose floor they were paid under; the others are not owed to the floor now. Once the sums come to half the price the rule is met: more would be written for nothing */
static bool
add_pending(SiftMetaPageData *meta, void *arg)
{
	const Flush *f = arg;
	double		total = 0;
	bool		changed = false;

	for (int s = 0; s < SIFT_META_SCOPES; s++)
		total += (double) meta->scope_paid[s];
	for (int i = 0; i < npending && total < f->half; i++)
	{
		SiftConnStamp floor[SIFT_SCOPE_JOURNALS];

		if (pending[i].index != f->index || pending[i].instr == 0)
			continue;
		sift_scope_floor_get(meta, pending[i].ord, floor);
		if (sift_scope_epoch(floor, pending[i].ord) != pending[i].epoch)
			continue;
		meta->scope_paid[pending[i].ord] += pending[i].instr;
		total += (double) pending[i].instr;
		changed = true;
	}
	return changed;
}

bool
sift_scope_paid_pending(Oid index)
{
	for (int i = 0; i < npending; i++)
		if (pending[i].index == index && pending[i].instr > 0)
			return true;
	return false;
}

void
sift_scope_paid_flush(Relation index)
{
	Flush		f = {.index = RelationGetRelid(index)};
	SiftDirSnapshot *dir;
	int			n = 0;

	/* a metapage record, not a catalog row, but the contexts are the same: no write in a standby, a parallel operation or a VACUUM (sift_catalog_write_ok) */
	if (!sift_scope_paid_pending(f.index) || RecoveryInProgress() || !sift_catalog_write_ok())
		return;
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	f.half = sift_scope_refresh_half(index, dir);
	pfree(dir);
	sift_meta_update(index, add_pending, &f);
	for (int i = 0; i < npending; i++)
		if (pending[i].index != f.index)
			pending[n++] = pending[i];
	npending = n;
}
