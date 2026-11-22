/*
 * col_smoke.c - the portable column engine (src/portable-col.list) running on the SQLite host: build a batch of rows, encode it as a
 * unit (plain and chunk codec policies), parse the unit back and compare every value. No SQLite library needed.
 *   make col-smoke
 */
#include <stdio.h>
#include <stdlib.h>

#include "core/host.h"

#include "col/v1/enc.h"
#include "col/codec.h"
#include "col/v1/seg_int.h"

#define NROWS 5000

/*
 * What the column engine asks of PostgreSQL and this test does not need: the caches of a segment, which is never opened here, and the hash of the sketches.
 */

/* ---------- the liveness run of a column segment (col/v1/seg/live.c): an index file of PostgreSQL; the embedded hosts keep no run to audit ---------- */

void
sift_cseg_live_verify(SiftColSegment *seg)
{
}

/* ---------- parsed column dictionaries (col/dictcache.c): not kept between queries ---------- */

SiftCDict *
sift_cseg_dict_cached(SiftColSegment *seg, int col, SiftCDictLoad load)
{
	return load(seg, col, seg->mcxt);
}

SiftCDict *
sift_cseg_dict_peek(SiftColSegment *seg, int col)
{
	return NULL;
}

/* nothing is counted: every dictionary that is tested is parsed whole, as it is once the backend has asked often */
uint32
sift_cseg_dict_asked(SiftColSegment *seg, int col)
{
	return UINT32_MAX;
}

/* there is no cache that could fail to hold a dictionary */
bool
sift_cseg_dict_nokeep(SiftColSegment *seg, int col)
{
	return false;
}

/* nothing is kept, so the cache has no limit to pass; a dictionary takes what its strings do */
Size
sift_cdict_cache_budget(void)
{
	return SIZE_MAX / 4;
}

Size
sift_cdict_parsed_bytes(Size nbytes, uint32 n)
{
	return nbytes;
}

/* nothing is counted: a dictionary whose hash was tried is parsed at once, as with sift_cseg_dict_asked */
uint64
sift_cseg_dict_spent(SiftColSegment *seg, int col, uint64 add)
{
	return UINT64_MAX;
}

void
sift_cseg_dict_heavy(SiftColSegment *seg, int col, uint64 spent)
{
}

/* ---------- the dictionary of a 5.x column (col/v1/seg/dict_ref.c): it names a value store, a relation of PostgreSQL that no embedded host has ---------- */

SiftCDict *
seg_dict_ref_load(SiftColSegment *seg, int col, uint64 at, uint64 len, MemoryContext mcxt)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the dictionary of a 5.x column is read by the PostgreSQL extension only")));
	return NULL;
}

/* ---------- segment heads, zone arrays and bisected positions (col/dictcache.c): not kept between queries, they stay in the segment's context ---------- */

const SegHead *
sift_cseg_head_find(Relation index, const SiftSegEntry *entry, const SiftSegHeaderData *h, MemoryContext mcxt)
{
	return NULL;
}

void
sift_cdict_retire(Relation index, const SiftDirSnapshot *snap)
{
}

void
sift_cseg_head_keep(Relation index, const SiftSegEntry *entry, MemoryContext cx, SegHead *hd, MemoryContext mcxt)
{
}

ZoneArrays *
sift_cseg_zones_find(SiftColSegment *seg, int col)
{
	return NULL;
}

void
sift_cseg_zones_keep(SiftColSegment *seg, int col, MemoryContext cx, ZoneArrays *z)
{
}

SegPts *
sift_cseg_pts_find(SiftColSegment *seg)
{
	return NULL;
}

void
sift_cseg_pts_keep(SiftColSegment *seg, MemoryContext cx, SegPts *p)
{
}

SegGOrdHead *
sift_cseg_gordh_find(SiftColSegment *seg, int col)
{
	return NULL;
}

void
sift_cseg_gordh_keep(SiftColSegment *seg, int col, MemoryContext cx, SegGOrdHead *h)
{
}

void
sift_cseg_gordh_grew(SiftColSegment *seg, int col, Size bytes)
{
}

void *
sift_cseg_sets_find(SiftColSegment *seg, int col)
{
	return NULL;
}

void
sift_cseg_sets_keep(SiftColSegment *seg, int col, MemoryContext cx, void *sets)
{
}

/* no cache of the positions a dictionary search bisected: every search reads the dictionary */
SiftCBisect *
sift_cseg_bisect(SiftColSegment *seg, int col, uint32 n)
{
	return NULL;
}

SiftCBisect *
sift_cseg_bisect_find(SiftColSegment *seg, int col)
{
	return NULL;
}

/* ---------- hashing (col/hll.c): the sketches of hll_columns are written by the PostgreSQL extension, whose hash they carry ---------- */

uint64
hash_bytes_extended(const unsigned char *k, int keylen, uint64 seed)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the sketch of a column is built by the PostgreSQL extension only"),
					errhint("Index the column with hll_columns in PostgreSQL.")));
	return 0;
}

uint64
hash_bytes_uint32_extended(uint32 k, uint64 seed)
{
	return hash_bytes_extended(NULL, 0, seed);
}

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
	sift_cunit_encode(b, NULL, NULL, pol, false, false, NULL, &out, &start, NULL);
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

/* rowid -> key mapping of a host (docs/COL-PORTS.md section 4): block = rowid >> shift, offset = (rowid & mask) + 1. Dense rowids, one unit per superblock */
static uint64
mapping_bytes(MemoryContext cx, const SiftColSchema *schema, int shift, int nrows, int *nunits)
{
	SiftColBatch *b = sift_cbatch_create(cx, schema);
	SiftCcPolicy pol[2];
	uint64		total = 0;
	int			i = 0;

	sift_cc_policy_default(&pol[0]);
	pol[1] = pol[0];
	*nunits = 0;
	while (i < nrows)
	{
		StringInfoData out;
		Size		start;
		uint32		sb = (uint32) (i >> shift) >> SIFT_SB_SHIFT;

		sift_cbatch_reset(b, sb);
		for (; i < nrows && ((uint32) (i >> shift) >> SIFT_SB_SHIFT) == sb; i++)
		{
			SiftColValue val[2] = {0};
			SiftColRow	row = {.ncols = 2, .val = val};
			char		str[16];

			val[0].v.i = 1700000000 + i * 3 + (i * 7) % 11;
			snprintf(str, sizeof(str), "status-%d", (i / 5) % 6);
			val[1].v.s = str;
			val[1].len = (uint32) strlen(str);
			ItemPointerSet(&row.tid, (uint32) (i >> shift), (i & ((1 << shift) - 1)) + 1);
			sift_cbatch_add(b, &row);
		}
		initStringInfo(&out);
		sift_cunit_encode(b, NULL, NULL, pol, false, false, NULL, &out, &start, NULL);
		total += out.len - start;
		(*nunits)++;
	}
	return total;
}

int
main(void)
{
	MemoryContext cx = AllocSetContextCreate(NULL, "col-smoke", ALLOCSET_DEFAULT_SIZES);
	SiftColDef	defs[2] = {
		{.id = 0, .parent = -1, .kind = SIFT_COLK_SCALAR, .store = SIFT_COLS_INT, .attno = 1, .type = SIFT_COLT_INT8, .path = ""},
		{.id = 1, .parent = -1, .kind = SIFT_COLK_SCALAR, .store = SIFT_COLS_STR, .attno = 2, .type = SIFT_COLT_TEXT, .path = ""},
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
	{
		int			n5, n8;
		uint64		b5 = mapping_bytes(cx, schema, 5, 1 << 18, &n5),
					b8 = mapping_bytes(cx, schema, 8, 1 << 18, &n8);

		printf("col-smoke: rowid >> 5: %d units, %.2f bytes/row; rowid >> 8: %d units, %.2f bytes/row\n",
			   n5, (double) b5 / (1 << 18), n8, (double) b8 / (1 << 18));
		check(b8 < b5, "rowid >> 8 packs smaller than rowid >> 5");
	}
	return 0;
}
