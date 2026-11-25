/*
 * col/f2/tid/hash.c - the address hash directory of a part of a sift_table (f2_tid_hash.h)
 *
 * check-memory: allow-allocs every array here follows the rows of one part, which are bounded by the build buffer, not the rows of the table
 */
#include "postgres.h"

#include "col/f2/tid/hash.h"
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
sift_tid_hash_build(const SiftRows *rows, uint16 level, const int *gstart, int ng, bool rpack, StringInfo out)
{
	const uint32 log2_buckets = sift_tid_hash_log2(rows->nrows, SIFT_TID_HASH_V2_LOAD),
				nb = UINT32_C(1) << log2_buckets;
	SiftTidHashHdr hdr = {SIFT_TID_HASH_MAGIC, SIFT_TID_HASH_VER_V2, rpack ? SIFT_TID_HASH_FLAG_RPACK : 0, log2_buckets, (uint32) rows->nrows};
	SiftTidHashSlotV2 *slots;
	uint32	   *start,
			   *at;

	if (level == 0 || (rows->nsort == 0 && !rpack) || rows->ncol_ids == 0 || rows->nrows < SIFT_TID_HASH_MIN_ROWS || ng > UINT16_MAX + 1)
		return false;
	start = palloc0((nb + 1) * sizeof(uint32));
	at = palloc(nb * sizeof(uint32));
	for (uint64 r = 0; r < rows->nrows; r++)
		start[sift_tid_hash_bucket(rows->tid[r], log2_buckets) + 1]++;
	for (uint32 b = 0; b < nb; b++)
		start[b + 1] += start[b];
	memcpy(at, start, nb * sizeof(uint32));
	appendBinaryStringInfo(out, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(out, (char *) start, (nb + 1) * sizeof(uint32));
	enlargeStringInfo(out, (Size) rows->nrows * sizeof(SiftTidHashSlotV2));
	slots = (SiftTidHashSlotV2 *) (out->data + out->len);
	for (int g = 0; g < ng; g++)
		for (int r = gstart[g]; r < gstart[g + 1]; r++)
			slots[at[sift_tid_hash_bucket(rows->tid[r], log2_buckets)]++] = (SiftTidHashSlotV2) {(uint32) r, sift_tid_hash_fp(rows->tid[r]), (uint16) g};
	out->len += (uint64) rows->nrows * sizeof(SiftTidHashSlotV2);
	out->data[out->len] = '\0';
	pfree(start);
	pfree(at);
	return true;
}

static SiftTidHashHdr
read_header(Relation rel, const SiftPart *part, const SiftSecDirEntry *se, uint64 nrows)
{
	SiftTidHashHdr hdr;
	uint64		slot_bytes;

	sift_part_read_section_bytes(rel, part, se, 0, sizeof(hdr), &hdr);
	slot_bytes = hdr.version == SIFT_TID_HASH_VER_V2 ? sizeof(SiftTidHashSlotV2) : sizeof(uint32);
	if (hdr.magic != SIFT_TID_HASH_MAGIC || (hdr.version != SIFT_TID_HASH_VER_V1 && hdr.version != SIFT_TID_HASH_VER_V2) || hdr.nrows != nrows || hdr.log2_buckets == 0 || hdr.log2_buckets > 30 ||
		se->len != sizeof(hdr) + ((UINT64_C(1) << hdr.log2_buckets) + 1) * sizeof(uint32) + nrows * slot_bytes)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory is not the size of its part"));
	return hdr;
}

static int
hits_v1(const uint32 *ref, uint32 n, SiftTidHashHit *hits)
{
	int			k = 0;

	for (uint32 i = 0; i < n; i++)
		if (k == 0 || hits[k - 1].granule_id != ref[i] >> SIFT_TID_HASH_ROW_BITS)
			hits[k++] = (SiftTidHashHit) {SIFT_TID_HASH_NO_ROW, ref[i] >> SIFT_TID_HASH_ROW_BITS};
	return k;
}

static int
hits_v2(const SiftTidHashSlotV2 *slots, uint32 n, uint16 fp, SiftTidHashHit *hits)
{
	int			k = 0;

	for (uint32 i = 0; i < n; i++)
		if (slots[i].fp16 == fp)
			hits[k++] = (SiftTidHashHit) {slots[i].row_idx, slots[i].granule_id};
	return k;
}

#define CHECK_CHUNK		4096

/* The whole directory: its buckets start at 0, never descend and end at the rows of the part, and every entry names a granule and a row of the part */
void
sift_tid_hash_check(Relation rel, const SiftPart *part, uint64 nrows)
{
	const SiftSecDirEntry *se = sift_part_section(part, SEC_TID_HASH);
	const SiftTidHashHdr hdr = read_header(rel, part, se, nrows);
	const bool	v2 = hdr.version == SIFT_TID_HASH_VER_V2;
	const uint64 nb = UINT64_C(1) << hdr.log2_buckets,
				entries_at = sizeof(hdr) + (nb + 1) * sizeof(uint32);
	uint32		starts[CHECK_CHUNK],
				prev = 0;
	SiftTidHashSlotV2 slots[CHECK_CHUNK];

	for (uint64 at = 0; at < nb + 1; at += CHECK_CHUNK)
	{
		const uint64 n = Min(CHECK_CHUNK, nb + 1 - at);

		sift_part_read_section_bytes(rel, part, se, sizeof(hdr) + at * sizeof(uint32), n * sizeof(uint32), starts);
		for (uint64 i = 0; i < n; i++)
		{
			if (starts[i] < prev || starts[i] > nrows || (at + i == 0 && starts[i] != 0))
				SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory has buckets that do not follow one another"));
			prev = starts[i];
		}
	}
	if (prev != nrows)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory holds %u entries for the %llu rows of its part", prev, (unsigned long long) nrows));
	for (uint64 at = 0; at < nrows; at += CHECK_CHUNK)
	{
		const uint64 n = Min(CHECK_CHUNK, nrows - at);

		CHECK_FOR_INTERRUPTS();
		sift_part_read_section_bytes(rel, part, se, entries_at + at * (v2 ? sizeof(SiftTidHashSlotV2) : sizeof(uint32)), n * (v2 ? sizeof(SiftTidHashSlotV2) : sizeof(uint32)), slots);
		for (uint64 i = 0; i < n; i++)
		{
			const uint32 granule = v2 ? slots[i].granule_id : ((const uint32 *) slots)[i] >> SIFT_TID_HASH_ROW_BITS;

			if (granule >= part->hdr.ngranules || (v2 && slots[i].row_idx >= nrows))
				SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory names a row that its part does not have"));
		}
	}
}

int
sift_tid_hash_find(Relation rel, const SiftPart *part, uint64 nrows, uint64 tid, SiftTidHashHit **hits)
{
	const SiftSecDirEntry *se = sift_part_section(part, SEC_TID_HASH);
	const SiftTidHashHdr hdr = read_header(rel, part, se, nrows);
	const bool	v2 = hdr.version == SIFT_TID_HASH_VER_V2;
	const uint64 slot_bytes = v2 ? sizeof(SiftTidHashSlotV2) : sizeof(uint32),
				entries_at = sizeof(hdr) + ((UINT64_C(1) << hdr.log2_buckets) + 1) * sizeof(uint32);
	uint32		span[2];
	void	   *raw;
	int			n;

	sift_part_read_section_bytes(rel, part, se, sizeof(hdr) + (uint64) sift_tid_hash_bucket(tid, hdr.log2_buckets) * sizeof(uint32), sizeof(span), span);
	if (span[0] > span[1] || span[1] > nrows || span[1] - span[0] > MaxAllocSize / sizeof(SiftTidHashHit))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory has a bucket past its entries"));
	raw = palloc(Max(span[1] - span[0], 1) * slot_bytes);
	*hits = palloc(Max(span[1] - span[0], 1) * sizeof(SiftTidHashHit));
	sift_part_read_section_bytes(rel, part, se, entries_at + (uint64) span[0] * slot_bytes, (uint64) (span[1] - span[0]) * slot_bytes, raw);
	n = v2 ? hits_v2(raw, span[1] - span[0], sift_tid_hash_fp(tid), *hits) : hits_v1(raw, span[1] - span[0], *hits);
	pfree(raw);
	for (int i = 0; i < n; i++)
		if ((*hits)[i].granule_id >= part->hdr.ngranules || (v2 && (*hits)[i].row_idx >= nrows))
			SIFT_DAMAGE_DATA(rel, errmsg("sift: an address hash directory names a row that its part does not have"));
#ifdef SIFT_TESTING
	lookups++;
	granules += n;
#endif
	return n;
}
