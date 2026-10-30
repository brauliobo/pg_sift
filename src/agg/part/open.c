/*
 * agg/part/open.c - a partitioned table as one column source: open and close
 * The source of every partition opened behind one provider, the offsets of the superblocks
 * of the partitions, and the close.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"

#include "core/limits.h"
#include "agg/part/int.h"

/*
 * What PostgreSQL checks for a query on a partitioned table: row security of the table, not the partitions'. Its privileges were checked by the
 * executor for the columns the query reads, as the role or as the owner of the view the table is read through: a check here for the invoking role on
 * the whole table refused a role with column privileges or a view the plain plan answers.
 */
static void
check_access(Relation parent)
{
	Oid			relid = RelationGetRelid(parent);

	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col indexes cannot be used on table \"%s\", which has row-level security", RelationGetRelationName(parent)),
				 errhint("Run the statement as the table owner or as a role with BYPASSRLS; the normal plan applies the policies.")));
}

void
open_part(PartSource *ps, int i)
{
	if (ps->src[i] != NULL)
		return;
	if (ps->heap[i] == NULL)
		ps->heap[i] = table_open(ps->heapoid[i], AccessShareLock);
	if (OidIsValid(ps->indexoid[i]))
		ps->index[i] = index_open(ps->indexoid[i], AccessShareLock);
	ps->src[i] = sift_colsrc_open_part(ps->heap[i], ps->index[i], ps->snapshot, ps->mcxt);
}

/* the sources of the partitions the scan reads: opened at the first use (the pruned ones are not opened at all) */
void
open_rest(PartSource *ps)
{
	if (ps->opened)
		return;
	ps->opened = true;
	for (int i = 0; i < ps->n; i++)
		if (ps->skip == NULL || !ps->skip[i])
			open_part(ps, i);
}

SiftSrcSource *
sift_aggpart_open(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt)
{
	PartSource *ps = MemoryContextAllocZero(mcxt, sizeof(PartSource));
	SiftSrcSource *out = MemoryContextAlloc(mcxt, sizeof(SiftSrcSource));

	SIFT_INVARIANT(n > 0 && n <= sift_limit(partitions, SIFT_AGG_MAX_PARTS), "sift: %d partitions in a scan, from 1 to %d", n,
				   (int) sift_limit(partitions, SIFT_AGG_MAX_PARTS));
	check_access(parent);
	ps->n = n;
	ps->parent = parent;
	ps->snapshot = RegisterSnapshot(snapshot);	/* once for all the sources: the partitions' own registrations are held by this one (SIFT_COLO_SNAP_HELD) */
	ps->mcxt = mcxt;
	ps->heapoid = MemoryContextAlloc(mcxt, n * sizeof(Oid));
	memcpy(ps->heapoid, heaps, n * sizeof(Oid));
	ps->heap = MemoryContextAllocZero(mcxt, n * sizeof(Relation));
	ps->index = MemoryContextAllocZero(mcxt, n * sizeof(Relation));
	ps->src = MemoryContextAllocZero(mcxt, n * sizeof(SiftSrcSource *));
	ps->indexoid = MemoryContextAlloc(mcxt, n * sizeof(Oid));
	memcpy(ps->indexoid, indexes, n * sizeof(Oid));
	ps->off = MemoryContextAllocZero(mcxt, (n + 1) * sizeof(uint32));
	open_part(ps, 0);
	ps->nleaves = sift_colsrc_source_layout(ps->src[0])->nleaves;
	ps->id = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(PartId));
	ps->dict = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(SiftSrcDict));
	ps->cur = 0;
	out->prov = &part_provider;
	out->impl = ps;
	return out;
}

void
part_close(void *impl)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
	{
		if (ps->src[i])
		{
			sift_colsrc_close(ps->src[i]);
			if (ps->index[i])
				index_close(ps->index[i], NoLock);
		}
		if (ps->heap[i])
			table_close(ps->heap[i], NoLock);
	}
	UnregisterSnapshot(ps->snapshot);
}

const SiftSrcLayout *
part_source_layout(const void *impl)
{
	const PartSource *ps = impl;

	return sift_colsrc_source_layout(ps->src[0]);
}

/* the superblocks the partitions have now, counted once: the units are numbered by them for as long as the source is open */
void
count_sbs(PartSource *ps)
{
	uint64		total = 0;

	if (ps->have_off)
		return;
	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
	{
		ps->off[i] = (uint32) total;
		if (ps->skip == NULL || !ps->skip[i])
			total += sift_colsrc_nsb(ps->src[i]);
	}
	if (total > PG_UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the partitions of a scan hold more superblocks than the scan can number"),
						errhint("Filter on the partition key so that the scan reads fewer partitions, or set sift.enable_agg to off for this query.")));
	ps->off[ps->n] = (uint32) total;
	ps->have_off = true;
}

bool
part_sb_heap(void *impl)
{
	PartSource *ps = impl;

	count_sbs(ps);
	open_rest(ps);				/* a worker takes its offsets from the leader and has opened nothing yet */
	for (int i = 0; i < ps->n; i++)
		if ((ps->skip == NULL || !ps->skip[i]) && !sift_colsrc_sb_heap(ps->src[i]))
			return false;
	return true;
}

uint32
part_nsb(void *impl)
{
	PartSource *ps = impl;

	count_sbs(ps);
	return ps->off[ps->n];
}

void
sift_aggpart_offsets(SiftSrcSource *src, uint32 *off)
{
	PartSource *ps = src->impl;

	count_sbs(ps);
	memcpy(off, ps->off, (ps->n + 1) * sizeof(uint32));
}

void
sift_aggpart_set_offsets(SiftSrcSource *src, const uint32 *off)
{
	PartSource *ps = src->impl;

	memcpy(ps->off, off, (ps->n + 1) * sizeof(uint32));
	ps->have_off = true;
	/* the partitions without superblocks (pruned, or empty) are never read: not opened */
	ps->skip = palloc0(ps->n * sizeof(bool));
	for (int i = 0; i < ps->n; i++)
		ps->skip[i] = off[i + 1] == off[i];
}
