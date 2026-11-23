/*
 * col/f2/f2_tid_hash.c - the address hash directory of a part in key order (f2_tid_hash.h)
 */
#include "postgres.h"

#include "col/f2/f2_tid_hash.h"
#include "core/damage.h"

#ifdef SIFT_TESTING
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"

static uint64 lookups,
			granules;

PG_FUNCTION_INFO_V1(sift_test_f2_tid_hash_stats);

/* (reset): the lookups of an address through the hash directory since the last reset, and the granules they named, as "lookups granules" */
Datum
sift_test_f2_tid_hash_stats(PG_FUNCTION_ARGS)
{
	char	   *s = psprintf("%llu %llu", (unsigned long long) lookups, (unsigned long long) granules);

	if (PG_GETARG_BOOL(0))
		lookups = granules = 0;
	PG_RETURN_TEXT_P(cstring_to_text(s));
}
#endif

bool
sift_tid_hash_build(const SiftRows *rows, uint16 level, const int *gstart, int ng, StringInfo out)
{
	const uint32 log2_buckets = sift_tid_hash_log2(rows->nrows),
				nb = UINT32_C(1) << log2_buckets;
	SiftTidHashHdr hdr = {SIFT_TID_HASH_MAGIC, SIFT_TID_HASH_VERSION, 0, log2_buckets, (uint32) rows->nrows};
	uint32	   *start,
			   *at,
			   *ref;

	if (level == 0 || rows->nsort == 0 || rows->ncol_ids == 0 || rows->nrows < SIFT_TID_HASH_MIN_ROWS)
		return false;
	start = palloc0((nb + 1) * sizeof(uint32));
	at = palloc(nb * sizeof(uint32));
	ref = palloc((Size) rows->nrows * sizeof(uint32));
	for (uint64 r = 0; r < rows->nrows; r++)
		start[sift_tid_hash_bucket(rows->tid[r], log2_buckets) + 1]++;
	for (uint32 b = 0; b < nb; b++)
		start[b + 1] += start[b];
	memcpy(at, start, nb * sizeof(uint32));
	for (int g = 0; g < ng; g++)
		for (int r = gstart[g]; r < gstart[g + 1]; r++)
			ref[at[sift_tid_hash_bucket(rows->tid[r], log2_buckets)]++] = ((uint32) g << SIFT_TID_HASH_ROW_BITS) | (uint32) (r - gstart[g]);
	appendBinaryStringInfo(out, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(out, (char *) start, (nb + 1) * sizeof(uint32));
	appendBinaryStringInfo(out, (char *) ref, (uint64) rows->nrows * sizeof(uint32));
	pfree(start);
	pfree(at);
	pfree(ref);
	return true;
}

int
sift_tid_hash_granules(Relation rel, const SiftPart *part, uint64 nrows, uint64 tid, uint32 **gids)
{
	const SiftSecDirEntry *se = sift_part_section(part, SEC_TID_HASH);
	const uint32 log2_buckets = sift_tid_hash_log2(nrows),
				nb = UINT32_C(1) << log2_buckets;
	const uint64 entries_at = sizeof(SiftTidHashHdr) + (uint64) (nb + 1) * sizeof(uint32);
	uint32		span[2],
			   *ref;
	int			n = 0;

	if (se->len != entries_at + nrows * sizeof(uint32))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory is not the size of its part"));
	sift_part_read_section_bytes(rel, part, se, sizeof(SiftTidHashHdr) + (uint64) sift_tid_hash_bucket(tid, log2_buckets) * sizeof(uint32), sizeof(span), span);
	if (span[0] > span[1] || span[1] > nrows)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory has a bucket past its entries"));
	ref = palloc(Max(span[1] - span[0], 1) * sizeof(uint32));
	*gids = palloc(Max(span[1] - span[0], 1) * sizeof(uint32));
	sift_part_read_section_bytes(rel, part, se, entries_at + (uint64) span[0] * sizeof(uint32), (uint64) (span[1] - span[0]) * sizeof(uint32), ref);
	for (uint32 i = 0; i < span[1] - span[0]; i++)
		if (n == 0 || (*gids)[n - 1] != ref[i] >> SIFT_TID_HASH_ROW_BITS)
			(*gids)[n++] = ref[i] >> SIFT_TID_HASH_ROW_BITS;
	pfree(ref);
#ifdef SIFT_TESTING
	lookups++;
	granules += n;
#endif
	return n;
}
