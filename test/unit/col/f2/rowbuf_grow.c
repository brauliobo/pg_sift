/*
 * test/unit/col/f2/rowbuf_grow.c - a row buffer (col/f2/rowbuf/rowbuf.c) refuses the rows and the elements one allocation of its arrays does not hold, with a limit that says how to stay
 * under it: no array asks for more than 1 GB (the 40 bytes of the key of a row reach it at 26.8 million rows), and the count of the elements of a group does not wrap past 2^32.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "col/f2/rowbuf/rowbuf.c"

#define ROWS_PAST_1GB	((int) (MaxAllocSize / SIFT_BOUND_BYTES + 1))
#define LIMIT_MESSAGE	"sift: a row buffer holds at most"

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

static void
reserve_rows(void *arg)
{
	reserve(arg, ROWS_PAST_1GB);
}

static void
elements_past_uint32(void *arg)
{
	SiftRowBuf *rb = arg;
	int			g = rb->groups[0];

	rb->nelem[g] = rb->ecap[g] = PG_UINT32_MAX - 8;
	reserve_elems(rb, 0, 64);
}

int
main(void)
{
	SiftCellKind kinds[2] = {SIFT_CELL_INT, SIFT_CELL_INT};
	int16		parent[2] = {-1, 0};
	SiftPartSpec rows = {.ncols = 1,.kinds = kinds};
	SiftPartSpec groups = {.ncols = 2,.kinds = kinds,.parent = parent};
	SiftRowBuf *rb = sift_rowbuf_new(&rows);

	SIFT_CHECKF(!pgshim_try(reserve_rows, rb) && strncmp(pgshim_errmsg, LIMIT_MESSAGE, strlen(LIMIT_MESSAGE)) == 0, "%d rows, past 1 GB of keys, are refused by the limit of the buffer, not by an allocation: %s", ROWS_PAST_1GB, pgshim_errmsg);
	pgshim_reset();
	rb = sift_rowbuf_new(&groups);
	SIFT_CHECKF(!pgshim_try(elements_past_uint32, rb) && strncmp(pgshim_errmsg, LIMIT_MESSAGE, strlen(LIMIT_MESSAGE)) == 0, "elements past 2^32 in a group are refused, not wrapped");
	return sift_check_summary();
}
