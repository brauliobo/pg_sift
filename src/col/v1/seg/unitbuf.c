/*
 * col/v1/seg/unitbuf.c - the buffer a segment reads a unit record into, kept by the backend between statements (core/cachebudget.h).
 */
#include "col/v1/seg_int.h"
#include "core/cachebudget.h"

/*
 * The buffer of a unit record is the largest of a lookup after the cursors (a unit of 100,000 rows is 400 kB), and a block that size grows the heap and goes back to the kernel
 * with its top when the statement ends (75 page faults for a lookup of 0.35 million instructions): the backend keeps the buffers of the segments it has closed, and the next
 * statement opens its segments with them. Taken by the first unit a segment reads, given back when the segment's context ends.
 */
#define UNIT_KEEP		4

#define UNIT_KEEP_BYTES	((Size) 4 << 20)

static MemoryContext unit_spare_cx;

static struct
{
	uint8	   *buf;
	Size		cap;
} unit_spare[UNIT_KEEP];

static int	nunit_spare;
static uint64 unit_spare_tick;

static uint64
unit_spare_oldest(void)
{
	return nunit_spare > 0 ? unit_spare_tick : UINT64_MAX;
}

static bool
unit_spare_evict_one(void)
{
	if (nunit_spare == 0)
		return false;
	pfree(unit_spare[--nunit_spare].buf);
	return true;
}

static void
unit_spare_evict_all(void)
{
	while (unit_spare_evict_one())
		;
}

static const SiftCacheKind unit_spare_kind = {"col_unit_spare", unit_spare_oldest, unit_spare_evict_one, unit_spare_evict_all};

static void
unit_give_back(SegPriv *pv)
{
	if (pv->unitbuf == NULL)
		return;
	if (nunit_spare < UNIT_KEEP && pv->unitcap <= UNIT_KEEP_BYTES)
	{
		unit_spare[nunit_spare++] = (typeof(unit_spare[0])) {pv->unitbuf, pv->unitcap};
		unit_spare_tick = sift_cache_tick();
		sift_cache_commit();
	}
	else
		pfree(pv->unitbuf);
	pv->unitbuf = NULL;
	pv->unitcap = 0;
}

static void
unit_release(void *arg)
{
	unit_give_back(arg);
}

void
sift_cseg_unit_release(SiftColSegment *seg)
{
	unit_give_back(PRIV(seg));
}

Size
sift_cseg_unit_bytes(const SiftColSegment *seg)
{
	return PRIV(seg)->unitcap;
}

/* The buffer of a unit record holds len bytes and the SIFT_COL_PAD bytes the kernels read past the packed payload of its last chunk */
void
sift_cseg_unit_room(SiftColSegment *seg, SegPriv *pv, uint32 len)
{
	if (len + SIFT_COL_PAD <= pv->unitcap)
		return;
	if (!pv->unit_kept)
	{
		MemoryContextCallback *cb;

		if (unit_spare_cx == NULL)
		{
			unit_spare_cx = sift_cache_create_context("sift cache unit buffers");
			sift_cache_register(&unit_spare_kind);
		}
		cb = MemoryContextAlloc(seg->mcxt, sizeof(MemoryContextCallback));
		*cb = (MemoryContextCallback) {unit_release, pv, NULL};
		MemoryContextRegisterResetCallback(seg->mcxt, cb);
		pv->unit_kept = true;
	}
	if (pv->unitbuf == NULL && nunit_spare > 0)
	{
		pv->unitbuf = unit_spare[--nunit_spare].buf;
		pv->unitcap = unit_spare[nunit_spare].cap;
	}
	if (len + SIFT_COL_PAD > pv->unitcap)
	{
		Size		cap = Max(len + SIFT_COL_PAD, 2 * pv->unitcap);

		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, cap) : MemoryContextAlloc(unit_spare_cx, cap);
		pv->unitcap = cap;
	}
}
