/*
 * test/unit/table/f2/col_delta_place.c - where the fold of a column delta chain finds the rows its records name (table/f2/col_delta/fold.c, place_rows): outputs that hold one address twice
 * are reported, with nothing written past the placement, whether the extra rows outnumber the addresses wanted or not.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"

#include "table/f2/col_delta/fold.c"

volatile sig_atomic_t InterruptPending = false;

enum { ROWS = 4 };

static SiftGranuleDesc dir[1] = {{.nrows = ROWS}};
static SiftTidIndexEntry tix[1] = {{.tid_min = 1,.tid_max = ROWS}};
static uint64 part_tids[2][ROWS] = {{1, 2, 3, 4}, {1, 2, 3, 4}};

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

SiftPart *
sift_part_open(Relation rel, BlockNumber header_blk, uint64 part_gen, uint32 part_id)
{
	SiftPart   *part = palloc0(sizeof(SiftPart));

	part->hdr.ngranules = 1;
	part->hdr.part_gen = part_gen;
	return part;
}

SiftGranuleDesc *
sift_part_granule_dir(Relation rel, const SiftPart *part)
{
	return dir;
}

void *
sift_part_read_section(Relation rel, const SiftPart *part, uint16 tag, uint64 *len)
{
	*len = sizeof(tix);
	return tix;
}

uint64 *
sift_part_read_tids(Relation rel, const SiftPart *part, const SiftGranuleDesc *d, uint32 g)
{
	return part_tids[part->hdr.part_gen];
}

/* Two outputs, each with every row of the addresses wanted (more rows than addresses), or with the first nwant of them */
static void
place_case(int nwant)
{
	SiftPartDesc out[2] = {{.part_gen = 0}, {.part_gen = 1}};
	uint64		want[ROWS] = {1, 2, 3, 4};
	Placement	pl;

	SIFT_CHECKF(!place_rows(NULL, 7, out, 2, want, nwant, &pl), "two rows of one address in %d wanted are reported", nwant);
	SIFT_CHECKF(pl.n <= nwant, "the placement holds at most the %d addresses wanted, not %d", nwant, pl.n);
	pfree(pl.v);
}

int
main(void)
{
	SiftPartDesc out[1] = {{.part_gen = 0}};
	uint64		want[ROWS] = {1, 2, 3, 4};
	Placement	pl;

	SIFT_CHECKF(place_rows(NULL, 7, out, 1, want, ROWS, &pl) && pl.n == ROWS, "one output places every row once");
	pfree(pl.v);
	for (int nwant = 1; nwant <= ROWS; nwant++)
		place_case(nwant);
	return sift_check_summary();
}
