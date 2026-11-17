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

#include "executor/nodeAgg.h"

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

int
sift_aggpart_max(void)
{
	return (int) Min((Size) sift_limit(partitions, SIFT_AGG_MAX_PARTS), Max((Size) get_hash_memory_limit(), (Size) sift_limit(partition_memory, SIFT_AGG_PART_FLOOR)) / SIFT_AGG_PART_STATE);
}

Relation
heap_part(PartSource *ps, int i)
{
	if (ps->heap[i] == NULL)
		ps->heap[i] = table_open(ps->heapoid[i], AccessShareLock);
	return ps->heap[i];
}

void
open_part(PartSource *ps, int i)
{
	MemoryContext cx;

	if (ps->src[i] != NULL)
		return;
	heap_part(ps, i);
	if (OidIsValid(ps->indexoid[i]))
		ps->index[i] = index_open(ps->indexoid[i], AccessShareLock);
	cx = AllocSetContextCreate(ps->mcxt, "sift agg partition", ALLOCSET_DEFAULT_SIZES);
	ps->pcx[i] = cx;
	ps->src[i] = sift_colsrc_open_part(ps->heap[i], ps->index[i], ps->snapshot, cx, ps->view[i]);
	ps->gen[i]++;
	ps->nopen++;
	for (int k = 0; k < ps->nop; k++)
		(void) part_apply(&ps->op[k], ps->src[i]);
}

static void
close_part(PartSource *ps, int i)
{
	SiftSrcCounters pc;

	sift_colsrc_counters(ps->src[i], &pc);
	counters_add(&ps->done, &pc);
	sift_colsrc_close(ps->src[i]);
	if (ps->index[i])
		index_close(ps->index[i], NoLock);
	MemoryContextDelete(ps->pcx[i]);
	ps->src[i] = NULL;
	ps->index[i] = NULL;
	ps->pcx[i] = NULL;
	ps->nopen--;
}

/*
 * A partition the scan is done with gives its source back: the scan holds the first (it is the layout of all of them), the one it is at and those handed out,
 * so that the memory of the sources follows the partitions the scan is in, not all of them. The source is opened again on the copy of the directory it was counted on.
 */
void
release_part(PartSource *ps, int i)
{
	if (ps->src[i] != NULL && ps->nopen > ps->keep && i != 0 && i != ps->last && i != ps->cur && !ps->hold[i])
		close_part(ps, i);
}

/* what an open source says of the view of its directory, kept by the wrapper to open the source again as it was counted */
static void
keep_view(PartSource *ps, int i)
{
	if (ps->view[i] != NULL)
		return;
	ps->vlen[i] = sift_colsrc_view_size(ps->src[i]);
	ps->view[i] = MemoryContextAlloc(ps->mcxt, ps->vlen[i]);
	sift_colsrc_view_export(ps->src[i], (void *) ps->view[i]);
}

/*
 * The directories the partitions' sources read, for the workers of a parallel scan to open on (the layout is, per partition, the bytes it exported and their
 * number as a Size, 0 for none): the partitions the leader counted are open when it exports them, and the pruned ones have no superblocks in the items.
 */
Size
part_view_size(void *impl)
{
	PartSource *ps = impl;
	Size		size = 0;

	count_sbs(ps);
	for (int i = 0; i < ps->n; i++)
		size += sizeof(Size) + ps->vlen[i];
	return size;
}

void
part_view_export(void *impl, void *buf)
{
	PartSource *ps = impl;
	char	   *p = buf;

	for (int i = 0; i < ps->n; i++)
	{
		memcpy(p, &ps->vlen[i], sizeof(Size));
		if (ps->vlen[i] > 0)
			memcpy(p + sizeof(Size), ps->view[i], ps->vlen[i]);
		p += sizeof(Size) + ps->vlen[i];
	}
}

static void
slice_views(PartSource *ps, const char *view)
{
	for (int i = 0; view != NULL && i < ps->n; i++)
	{
		Size		len;

		memcpy(&len, view, sizeof(Size));
		ps->view[i] = len > 0 ? view + sizeof(Size) : NULL;
		ps->vlen[i] = len;
		view += sizeof(Size) + len;
	}
}

static SiftSrcSource *
part_new(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt, const void *view)
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
	ps->pcx = MemoryContextAllocZero(mcxt, n * sizeof(MemoryContext));
	ps->gen = MemoryContextAllocZero(mcxt, n * sizeof(uint32));
	ps->hold = MemoryContextAllocZero(mcxt, n * sizeof(bool));
	ps->view = MemoryContextAllocZero(mcxt, n * sizeof(void *));
	ps->vlen = MemoryContextAllocZero(mcxt, n * sizeof(Size));
	slice_views(ps, view);
	ps->indexoid = MemoryContextAlloc(mcxt, n * sizeof(Oid));
	memcpy(ps->indexoid, indexes, n * sizeof(Oid));
	ps->off = MemoryContextAllocZero(mcxt, (n + 1) * sizeof(uint32));
	ps->last = -1;
	open_part(ps, 0);
	ps->keep = (int) Max(1, get_hash_memory_limit() / 2 / MemoryContextMemAllocated(ps->pcx[0], true));
	ps->nleaves = sift_colsrc_source_layout(ps->src[0])->nleaves;
	ps->cur = 0;
	out->prov = &part_provider;
	out->impl = ps;
	return out;
}

SiftSrcSource *
sift_aggpart_open(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt, const void *view)
{
	return part_new(parent, n, heaps, indexes, snapshot, mcxt, view);
}

void
part_close(void *impl)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
	{
		if (ps->src[i])
			close_part(ps, i);
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

/* the superblocks of a partition and the view its source opens on: an open source (the first, opened for the layout) says its own, else the index is read for them and no source is opened */
static uint32
count_part(PartSource *ps, int i)
{
	SiftSrcPartCount c;
	Relation	index = NULL;

	if (ps->src[i] != NULL)
	{
		keep_view(ps, i);
		return sift_colsrc_nsb(ps->src[i]);
	}
	if (OidIsValid(ps->indexoid[i]))
		index = index_open(ps->indexoid[i], AccessShareLock);
	sift_colsrc_count_part(heap_part(ps, i), index, ps->snapshot, ps->mcxt, &c);
	if (index != NULL)
		index_close(index, NoLock);
	ps->view[i] = c.view;
	ps->vlen[i] = c.vlen;
	return c.nsb;
}

/* the superblocks the partitions have now, counted once: the units are numbered by them for as long as the scan runs, and a partition opens on the directory it was counted on */
void
count_sbs(PartSource *ps)
{
	uint64		total = 0;

	if (ps->have_off)
		return;
	ps->opened = true;
	for (int i = 0; i < ps->n; i++)
	{
		ps->off[i] = (uint32) total;
		if (ps->skip == NULL || !ps->skip[i])
			total += count_part(ps, i);
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
	for (int i = 0; i < ps->n; i++)
		if ((ps->skip == NULL || !ps->skip[i]) && !sift_colsrc_view_sb_heap(ps->src[0], ps->view[i]))
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
