/*
 * test_gpu_part - which parts the device reads (col/gpu.c): a compacted part of the table is read on it, a level 0 part and a part in object storage are read on the CPU, which streams the object
 * through the cache of the tier and does not stop a scan on the wait for a remote read in the planning of the device.
 */
#include "postgres.h"

#include "core/f2/manifest.h"
#include "col/gpu.h"
#include "unit.h"

SiftVisClass
sift_vis_part_class(const SiftPartDesc *d, bool has_pend_del, FullTransactionId snap_xmin)
{
	return SIFT_VIS_PART_CLEAN;
}

SiftVisClass
sift_vis_granule_class(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId dv_xid_max, bool pend_hit, FullTransactionId snap_xmin)
{
	return SIFT_VIS_GRANULE_SETTLED;
}

uint32
sift_chunk_validity_len(const SiftChunkRef *ref, uint32 nrows)
{
	abort();
}

bool
sift_gpu_blob_unwrap(const uint8 *blob, uint32 len, StringInfo out, const char **why)
{
	abort();
}

static bool
eligible(const SiftPartDesc *d, const char **why)
{
	SiftVisClass cls;

	*why = NULL;
	return sift_gpu_part_eligible(d, InvalidFullTransactionId, &cls, why);
}

static void
test_compacted_part(void)
{
	const SiftPartDesc d = {.level = 2};
	const char *why;

	CHECK(eligible(&d, &why));
	CHECK(why == NULL);
}

static void
test_level_zero_part(void)
{
	const SiftPartDesc d = {.level = 0};
	const char *why;

	CHECK(!eligible(&d, &why));
	CHECK(why != NULL && strstr(why, "level 0") != NULL);
}

static void
test_part_in_object_storage(void)
{
	const SiftPartDesc d = {.level = 2,.ext_flags = SIFT_PART_EXT_REMOTE};
	const char *why;

	CHECK(!eligible(&d, &why));
	CHECK(why != NULL && strstr(why, "object storage") != NULL);
}

int
main(void)
{
	test_compacted_part();
	test_level_zero_part();
	test_part_in_object_storage();
	puts("test_gpu_part: ok");
	return 0;
}
