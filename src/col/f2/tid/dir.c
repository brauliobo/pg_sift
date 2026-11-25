/*
 * col/f2/tid/dir.c - the directory of the addresses of a part pin (col/f2/tid/dir.h).
 *
 * check-memory: allow-allocs one entry for each row of the part, which the budget of core/cachebudget.h admits before the directory is made
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/tid/dir.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

#define ST_SORT sort_entries
#define ST_ELEMENT_TYPE uint64
#define ST_COMPARE(a, b) (*(a) < *(b) ? -1 : (*(a) > *(b) ? 1 : 0))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define GRANULE_BITS	16
#define GRANULE_MASK	((UINT64CONST(1) << GRANULE_BITS) - 1)

typedef struct SiftTidDir
{
	uint64		n;
	uint64	   *entry;			/* ascending: the address above GRANULE_BITS, the granule below */
} SiftTidDir;

static uint64
part_rows(const SiftF2PartPin *pin)
{
	uint64		rows = 0;

	for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		rows += pin->gdir[g].nrows;
	return rows;
}

static SiftTidDir *
dir_build(Relation rel, SiftF2PartPin *pin)
{
	const uint32 ng = pin->part->hdr.ngranules;
	const uint64 rows = part_rows(pin);
	MemoryContext scratch;
	SiftTidDir *d;
	uint64	   *tids;

	if (ng > GRANULE_MASK + 1 || pin->uncached || !sift_cache_room(rows * sizeof(uint64)))
		return NULL;
	d = MemoryContextAlloc(pin->cx, sizeof(SiftTidDir));
	d->entry = MemoryContextAlloc(pin->cx, Max(rows, 1) * sizeof(uint64));
	d->n = 0;
	scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 tid directory");
	tids = palloc(SIFT_GRANULE_ROWS * sizeof(uint64));
	for (uint32 g = 0; g < ng; g++)
	{
		MemoryContext old = MemoryContextSwitchTo(scratch);

		sift_f2_pin_granule_tids(rel, pin, g, tids);
		MemoryContextSwitchTo(old);
		for (uint16 r = 0; r < pin->gdir[g].nrows; r++)
			d->entry[d->n++] = (tids[r] << GRANULE_BITS) | g;
		MemoryContextReset(scratch);
	}
	MemoryContextDelete(scratch);
	pfree(tids);
	sort_entries(d->entry, d->n);
	sift_cache_commit();
	return d;
}

bool
sift_f2_tid_dir_granules(Relation rel, SiftF2PartPin *pin, uint64 tid, bool make, uint32 **gids, int *n)
{
	const SiftTidDir *d = pin->tdir;
	uint64		lo = 0,
				hi;

	if (d == NULL && make && !pin->tdir_denied)
	{
		pin->tdir = dir_build(rel, pin);
		pin->tdir_denied = pin->tdir == NULL;
		d = pin->tdir;
	}
	if (d == NULL)
		return false;
	hi = d->n;
	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if ((d->entry[mid] >> GRANULE_BITS) < tid)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (hi = lo; hi < d->n && (d->entry[hi] >> GRANULE_BITS) == tid; hi++)
		;
	*n = (int) (hi - lo);
	*gids = palloc(sizeof(uint32) * Max(*n, 1));
	for (int k = 0; k < *n; k++)
		(*gids)[k] = (uint32) (d->entry[lo + k] & GRANULE_MASK);
	return true;
}
