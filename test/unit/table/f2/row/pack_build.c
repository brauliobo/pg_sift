/*
 * test/unit/table/f2/row/pack_build.c - the writer of SEC_ROW_PACK (table/f2/row/pack.c): a build answers a cancel between its blocks, and the section it writes is the header, the columns, the
 * directory and the blocks, each block at the offset its entry names and holding the raw bytes of its rows.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"

#include "table/f2/row/pack.c"

enum { ROWS = 1000, BLOCKS = (ROWS + SIFT_ROW_PACK_ROWS - 1) / SIFT_ROW_PACK_ROWS };

volatile sig_atomic_t InterruptPending = false;
static int	interrupts;

void
ProcessInterrupts(void)
{
	interrupts++;
}

void
appendStringInfoSpaces(StringInfo s, int n)
{
	while (n-- > 0)
		appendStringInfoChar(s, ' ');
}

bool
sift_colz_available(int alg)
{
	return false;
}

int
main(void)
{
	static uint64 tid[ROWS];
	static int64 v[ROWS];
	SiftColIn	col = {.kind = SIFT_CELL_INT,.i = v};
	SiftRows	rows = {.nrows = ROWS,.ncols = 1,.tid = tid,.cols = &col};
	Packer		pk = {.rows = &rows,.null_bm = palloc(1)};
	SiftRowPackCol pc;
	StringInfoData out,
				raw;
	SiftRowPackHdr hdr;
	const SiftRowPackBlockEntry *dir;
	bool		built;

	for (int r = 0; r < ROWS; r++)
	{
		tid[r] = (uint64) (r + 1) << 16 | 1;
		v[r] = r * 7;
	}
	initStringInfo(&out);
	InterruptPending = true;
	built = sift_row_pack_build(&rows, UINT64_C(1) << 30, &out);
	InterruptPending = false;
	SIFT_CHECKF(built, "the pack of %d integer rows is built", ROWS);
	SIFT_CHECKF(interrupts >= BLOCKS, "a pending cancel is answered at each of the %d blocks: %d times", BLOCKS, interrupts);
	memcpy(&hdr, out.data, sizeof(hdr));
	SIFT_CHECKF(hdr.magic == SIFT_ROW_PACK_MAGIC && hdr.nblocks == BLOCKS && hdr.nrows == ROWS, "the header names %d blocks of %d rows", BLOCKS, ROWS);
	SIFT_CHECKF(hdr.dir_off == TYPEALIGN(8, sizeof(hdr) + sizeof(SiftRowPackCol)) && hdr.data_off == TYPEALIGN(8, hdr.dir_off + BLOCKS * sizeof(SiftRowPackBlockEntry)),
				"the directory follows the columns and the blocks follow the directory: %u %llu", hdr.dir_off, (unsigned long long) hdr.data_off);
	(void) column_layout(&rows, 0, &pc);
	SIFT_CHECKF(memcmp(out.data + sizeof(hdr), &pc, sizeof(pc)) == 0, "the columns follow the header");
	pk.cols = &pc;
	initStringInfo(&raw);
	dir = (const SiftRowPackBlockEntry *) (out.data + hdr.dir_off);
	for (int b = 0; b < BLOCKS; b++)
	{
		const char *at = out.data + hdr.data_off + (Size) dir[b].off_qwords * 8;

		raw_block(&pk, &raw, b * SIFT_ROW_PACK_ROWS, Min(SIFT_ROW_PACK_ROWS, ROWS - b * SIFT_ROW_PACK_ROWS));
		SIFT_CHECKF(dir[b].comp_len == (uint32) raw.len && at + raw.len <= out.data + out.len && memcmp(at, raw.data, raw.len) == 0, "block %d holds the raw bytes of its rows", b);
	}
	return sift_check_summary();
}
