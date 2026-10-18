/*
 * col_smoke.c - the portable column engine (src/portable-col.list) running on the SQLite host: build a batch of rows, encode it as a
 * unit (plain and chunk codec policies), parse the unit back and compare every value. No SQLite library needed.
 *   make col-smoke
 */
#include <stdio.h>
#include <stdlib.h>

#include "sift_host.h"

#include "sift_col_enc.h"
#include "sift_colcodec.h"

#define NROWS 5000

static void
check(bool ok, const char *what)
{
	if (!ok)
	{
		fprintf(stderr, "col-smoke: FAILED: %s\n", what);
		exit(1);
	}
}

static void
roundtrip(const SiftColBatch *b, const SiftCcPolicy *pol, const char *name)
{
	StringInfoData out;
	Size		start;
	SiftCUnitView v;
	SiftCChunk	c;
	int64	   *dst = calloc(NROWS + SIFT_COL_PAD, sizeof(int64));

	initStringInfo(&out);
	sift_cunit_encode(b, NULL, pol, &out, &start, NULL);
	check(sift_cunit_parse((const uint8 *) out.data + start, out.len - start, b->schema, &v), name);
	check(v.nrows == NROWS, "nrows");
	for (int col = 0; col < 2; col++)
	{
		check(sift_cunit_chunk(&v, col, &c) && c.n == NROWS, "chunk");
		sift_cchunk_decode(&c, dst);
		for (uint32 i = 0; i < NROWS; i++)
		{
			SiftColValue val;

			sift_cbatch_value(b, col, i, &val);
			if (val.isnull)
				check((c.nulls[i >> 6] >> (i & 63) & 1) != 0, "null bit");
			else if (col == 0)
				check(dst[i] == val.v.i, "int value");
			else
			{
				uint32		len;
				const char *s = sift_cdict_str(b->dict[1], (uint32) dst[i], &len);

				check(len == val.len && memcmp(s, val.v.s, len) == 0, "string value");
			}
		}
	}
	printf("col-smoke: %s ok, unit %d bytes for %d rows\n", name, (int) (out.len - start), NROWS);
	free(dst);
}

int
main(void)
{
	MemoryContext cx = AllocSetContextCreate(NULL, "col-smoke", ALLOCSET_DEFAULT_SIZES);
	SiftColDef	defs[2] = {
		{.id = 0, .parent = -1, .kind = SIFT_COLK_SCALAR, .store = SIFT_COLS_INT, .attno = 1, .typid = 20, .path = ""},
		{.id = 1, .parent = -1, .kind = SIFT_COLK_SCALAR, .store = SIFT_COLS_STR, .attno = 2, .typid = 25, .path = ""},
	};
	SiftColSchema *schema;
	SiftColBatch *b;
	SiftCcPolicy pol[2];
	static char buf[NROWS][16];

	CurrentMemoryContext = cx;
	schema = sift_cschema_create(cx, defs, 2);
	b = sift_cbatch_create(cx, schema);
	sift_cbatch_reset(b, 0);
	for (int i = 0; i < NROWS; i++)
	{
		SiftColValue val[2] = {0};
		SiftColRow	row = {.ncols = 2, .val = val};

		val[0].v.i = (i * 7) % 1000;
		snprintf(buf[i], sizeof(buf[i]), "s%d", i % 37);
		val[1].v.s = buf[i];
		val[1].len = (uint32) strlen(buf[i]);
		val[0].isnull = i % 11 == 0;
		val[1].isnull = i % 13 == 0;
		ItemPointerSet(&row.tid, i / 250, i % 250 + 1);
		sift_cbatch_add(b, &row);
	}
	roundtrip(b, NULL, "plain chunks");
	sift_cc_policy_default(&pol[0]);
	pol[1] = pol[0];
	roundtrip(b, pol, "chunk codec");
	return 0;
}
