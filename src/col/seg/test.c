/*
 * col/seg/test.c - test hooks that name the byte span of a section of a segment
 * so that a test can damage the section and see the open or the
 * verify refuse it; compiled only with SIFT_TESTING
 */
#include "core/host.h"
#include "col/ktab.h"
#include "col/setsum.h"
#include "col/seg_int.h"

#ifdef SIFT_TESTING
bool
sift_cseg_test_super_span(SiftColSegment *seg, uint64 *off, uint64 *len)
{
	const PaggHeader *h = seg_pagg_header(seg);

	if (h->version < 4 || !PAGG_SUPERDIR(h))
		return false;
	*off = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SUPERDIR(h);
	*len = PAGG_SUPER_BYTES(h);
	return true;
}
#endif
#ifdef SIFT_TESTING
/* The span of the sorted runs in the stream and, for each byte of it, 2 when it is structure (header, records, directories, the head of a run), 1 when it is an entry of a run and 0 for the padding between blocks */
bool
sift_cseg_test_run_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PaggHeader *h = seg_pagg_header(seg);
	RunsHeader	rh;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS || h->version < 5 || !PAGG_RUNS(h))
		return false;
	*off = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_RUNS(h);
	*len = PAGG_RUNS_BYTES(h);
	seg_seg_read(seg, *off, sizeof(rh), &rh);
	*used = palloc0(*len);
	memset(*used, 2, sizeof(rh) + (uint64) rh.nleaves * sizeof(RunsLeaf));
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const RunsLeaf *rl = seg_run_leaf(seg, c);
		uint64		dir = rl ? rl->dir_off - PAGG_RUNS(h) : 0;

		if (rl == NULL)
			continue;
		memset(*used + dir, 2, (uint64) (seg->nsb + 1) * sizeof(uint64));
		memset(*used + dir + (uint64) (seg->nsb + 1) * sizeof(uint64), 1, rl->bytes);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCRun	r;
			uint64		at;

			if (sift_cseg_run(seg, c, u, &r, &at))
				memset(*used + (at - *off), 2, SIFT_CRUN_HDR);
		}
	}
	return true;
}

/* The same for the small-set summaries: 2 for the header, the records, the directories and the head of a blob, 1 for the rest of a blob */
bool
sift_cseg_test_set_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PaggHeader *h = seg_pagg_header(seg);
	SetsHeader	sh;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS || h->version < 3 || !PAGG_SETS(h))
		return false;
	*off = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SETS(h);
	*len = PAGG_SETS_BYTES(h);
	seg_seg_read(seg, *off, sizeof(sh), &sh);
	*used = palloc0(*len);
	memset(*used, 2, sizeof(sh) + (uint64) sh.nleaves * sizeof(SetsLeaf));
	for (int c = 0; c < seg->schema->ncols; c++)
	{
		const SetsLeaf *sl = seg_set_leaf(seg, c);
		uint64		dir = sl ? sl->dir_off - PAGG_SETS(h) : 0;

		if (sl == NULL)
			continue;
		memset(*used + dir, 2, (uint64) (seg->nsb + 1) * sizeof(uint64));
		memset(*used + dir + (uint64) (seg->nsb + 1) * sizeof(uint64), 1, sl->bytes);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint32		blen;
			const uint8 *blob = sift_cseg_set(seg, c, u, &blen);
			uint64		start = dir + (uint64) (seg->nsb + 1) * sizeof(uint64);
			uint64		o;

			if (blob == NULL)
				continue;
			seg_seg_read(seg, *off + dir + (uint64) u * sizeof(uint64), sizeof(o), &o);
			memset(*used + start + o, 2, SIFT_CSET_HDR);
		}
	}
	return true;
}

/* The same for the key tables: every byte is used */
bool
sift_cseg_test_ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	if (!seg_ktab_span(seg, off, len))
		return false;
	*used = palloc(*len);
	memset(*used, 1, *len);
	{
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "key table damage map", ALLOCSET_SMALL_SIZES);
		uint8 *buf = MemoryContextAlloc(cx, *len);

		seg_seg_read(seg, *off, *len, buf);
		for (uint64 at = 0; at < *len;)
		{
			SiftCHistDim d;
			uint32 n = sift_ktab_decode(seg->schema, buf + at, *len - at, cx, &d);

			if (n == 0)
				corrupt(seg, "The key table at %llu of the section is malformed.", ULL(at));
			memset(*used + at, 2, SIFT_KTAB_HEADER_BYTES);
			at += n;
		}
		MemoryContextDelete(cx);
	}
	return true;
}

/* The same for the point directories: 2 for the header, the records of the leaves and the footer, 1 for the tables of slots and the lists */
bool
sift_cseg_test_point_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used)
{
	const PointLeaf *pl = seg_points_read(seg);
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICT);
	uint32		hdr[4];
	uint64		hat;

	if (pl == NULL)
		return false;
	*off = s->off + PRIV(seg)->dict_end;
	*len = s->len - PRIV(seg)->dict_end;
	hat = seg->entry.format >= SIFT_COL_FORMAT_ROOT ? *len - 2 * sizeof(hdr) : 0;
	seg_seg_read(seg, *off + hat, sizeof(hdr), hdr);
	*used = palloc0(*len);
	memset(*used + hat, 2, sizeof(hdr));
	memset(*used + (hat > 0 ? hat - (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN : sizeof(hdr)), 2, (uint64) hdr[2] * SIFT_CPOINT_RECLEN);
	memset(*used + *len - 16, 2, 16);
	for (int c = 0; c < seg->schema->ncols; c++)
		if (pl[c].nslots > 0)
			memset(*used + (pl[c].at - *off), 1, pl[c].packed ? (uint64) pl[c].words * sizeof(uint32) : (uint64) pl[c].nslots * sizeof(uint64) + (uint64) pl[c].words * sizeof(uint32));
	return true;
}
#endif
