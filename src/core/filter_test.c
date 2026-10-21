/*
 * core/filter_test.c - test-only SQL function sift._test_filter, compiled only
 * with SIFT_TESTING and registered by the pg_sift_test extension: a TIDBitmap
 * built from tids and lossy blocks, converted to masks (core/filter.c) under a
 * byte budget and probed.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/htup_details.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "core/filter.h"

PG_FUNCTION_INFO_V1(sift_test_filter);

#define NOUT	6

/* Is the probe in the superblock's block mask of its page: must agree with sift_filter_has */
static bool
block_has(const SiftFilter *f, SiftKey key)
{
	const SiftFilterSB *fs = sift_filter_sb(f, sift_block_sb(sift_key_block(key)));
	uint64		mask[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;

	if (fs == NULL)
		return false;
	sift_filter_block(f, fs, page / SIFT_BLK_PAGES, mask);
	return mask[page % SIFT_BLK_PAGES][off / 64] >> (off & 63) & 1;
}

/*
 * (tids tid[], lossy_blocks int8[], maxbytes int8, probes tid[]) returns
 * (superblocks, exact_pages, lossy_pages, tuples, has bool[], block_mismatches)
 */
Datum
sift_test_filter(PG_FUNCTION_ARGS)
{
	ArrayType  *tids = PG_GETARG_ARRAYTYPE_P(0),
			   *lossy = PG_GETARG_ARRAYTYPE_P(1),
			   *probes = PG_GETARG_ARRAYTYPE_P(3);
	int64		maxbytes = PG_GETARG_INT64(2);
	TIDBitmap  *tbm = tbm_create(64 * 1024 * 1024, NULL);
	ItemPointer tid = (ItemPointer) ARR_DATA_PTR(tids);
	int64	   *blk = (int64 *) ARR_DATA_PTR(lossy);
	int			ntid = ArrayGetNItems(ARR_NDIM(tids), ARR_DIMS(tids)),
				nlossy = ArrayGetNItems(ARR_NDIM(lossy), ARR_DIMS(lossy)),
				nprobe = ArrayGetNItems(ARR_NDIM(probes), ARR_DIMS(probes));
	ItemPointer probe = (ItemPointer) ARR_DATA_PTR(probes);
	SiftFilter *f;
	Datum	   *has = palloc(Max(nprobe, 1) * sizeof(Datum)),
				values[NOUT];
	bool		nulls[NOUT] = {0};
	int64		mismatches = 0;
	TupleDesc	tupdesc;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (ntid > 0)
		tbm_add_tuples(tbm, tid, ntid, false);
	for (int i = 0; i < nlossy; i++)
		tbm_add_page(tbm, (BlockNumber) blk[i]);
	f = sift_filter_build(tbm, maxbytes, CurrentMemoryContext);
	for (int i = 0; i < nprobe; i++)
	{
		SiftKey		key = sift_key_from_tid(&probe[i]);
		bool		h = sift_filter_has(f, key);

		mismatches += h != block_has(f, key);
		has[i] = BoolGetDatum(h);
	}
	values[0] = Int64GetDatum(f->nsb);
	values[1] = Int64GetDatum(f->exact_pages);
	values[2] = Int64GetDatum(f->lossy_pages);
	values[3] = Int64GetDatum(f->tuples);
	values[4] = PointerGetDatum(construct_array(has, nprobe, BOOLOID, 1, true, TYPALIGN_CHAR));
	values[5] = Int64GetDatum(mismatches);
	tbm_free(tbm);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
