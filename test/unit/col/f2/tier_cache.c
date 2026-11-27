/*
 * test/unit/col/f2/tier_cache.c - the reads of the directory the local cache of the tiered parts makes (col/f2/tier/cache.c): a miss fetches its chunks and writes their files, and the index
 * is made again from the whole directory only once a quarter of the budget was fetched since the last time, so that a read of many chunks below the budget lists the directory a bounded
 * number of times, not once for each miss, also once the cache is full; one backend at a time lists it, and the listing and the eviction take a cancel. The directory is opened through the fd.c
 * calls that the end of the transaction closes.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <dirent.h>
#include <signal.h>
#include <sys/file.h>

#include "test/common/bench.h"

static int	dir_scans,
			dir_reads,
			interrupts;

static DIR *
counted_opendir(const char *path)
{
	dir_scans++;
	return opendir(path);
}

#define opendir counted_opendir

#include "col/f2/tier/cache.c"

#undef opendir

volatile sig_atomic_t InterruptPending = 0;

void
ProcessInterrupts(void)
{
	interrupts++;
}

DIR *
AllocateDir(const char *dirname)
{
	dir_scans++;
	return opendir(dirname);
}

struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	dir_reads++;
	return dir != NULL ? readdir(dir) : NULL;
}

int
FreeDir(DIR *dir)
{
	return dir != NULL ? closedir(dir) : 0;
}

uint32
sift_tier_crc32c_extend(uint32 crc, const void *data, size_t len)
{
	for (size_t i = 0; i < len; i++)
		crc = crc * 31 + ((const uint8 *) data)[i];
	return crc;
}

enum { CHUNK = 4096, NCHUNKS = 64, BUDGET_CHUNKS = 256 };

static int	gets;

static SiftTierStatus
fake_get(SiftTierBackend *be, const char *key, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	gets++;
	for (uint64 i = 0; i < len; i++)
		((uint8 *) dst)[i] = (uint8) ((off + i) * 7);
	return SIFT_TIER_OK;
}

static const SiftTierBackendOps fake_ops = {.get = fake_get};

static void
test_misses_do_not_list_the_directory_each(void)
{
	char		dir[] = "/tmp/sift-unit-tier-cache-XXXXXX";
	SiftTierBackend be = {.ops = &fake_ops};
	SiftTierPartRef ref = {.byte_len = (uint64) NCHUNKS * CHUNK,.obj_hash = {0x1234, 0x5678}};
	SiftTierErr err = {0};
	SiftTierCache *c;
	char		buf[CHUNK];
	char		cmd[64];

	SIFT_CHECKF(mkdtemp(dir) != NULL, "a scratch directory");
	c = sift_tier_cache_open(dir, (uint64) BUDGET_CHUNKS * CHUNK, CHUNK, &err);
	SIFT_CHECKF(c != NULL, "the cache opens");
	dir_scans = 0;
	for (int i = 0; i < NCHUNKS; i++)
		SIFT_CHECKF(sift_tier_cache_read(c, &be, "k", &ref, (uint64) i * CHUNK, CHUNK, buf, &err) == SIFT_TIER_OK, "a miss reads");
	SIFT_CHECKF(gets == NCHUNKS, "each miss fetched once (%d)", gets);
	SIFT_CHECKF(dir_scans <= NCHUNKS * 4 / BUDGET_CHUNKS + 1, "%d misses below the budget listed the directory %d times", NCHUNKS, dir_scans);
	for (int i = 0; i < NCHUNKS; i++)
		SIFT_CHECKF(sift_tier_cache_read(c, &be, "k", &ref, (uint64) i * CHUNK, CHUNK, buf, &err) == SIFT_TIER_OK && buf[1] == (char) ((uint64) i * CHUNK * 7 + 7),
					"a second read is served from the cache");
	SIFT_CHECKF(gets == NCHUNKS, "the second read fetched nothing (%d)", gets);
	sift_tier_cache_close(c);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	SIFT_CHECKF(system(cmd) == 0, "the scratch directory goes");
}

static SiftTierCache *
scratch_cache(char *dir, uint32 budget_chunks)
{
	SiftTierErr err = {0};
	SiftTierCache *c;

	SIFT_CHECKF(mkdtemp(dir) != NULL, "a scratch directory");
	c = sift_tier_cache_open(dir, (uint64) budget_chunks * CHUNK, CHUNK, &err);
	SIFT_CHECKF(c != NULL, "the cache opens");
	return c;
}

static void
scratch_drop(SiftTierCache *c, const char *dir)
{
	char		cmd[64];

	sift_tier_cache_close(c);
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	SIFT_CHECKF(system(cmd) == 0, "the scratch directory goes");
}

static void
read_chunks(SiftTierCache *c, int from, int n)
{
	SiftTierBackend be = {.ops = &fake_ops};
	SiftTierPartRef ref = {.byte_len = (uint64) 1024 * CHUNK,.obj_hash = {0x4321, 0x8765}};
	SiftTierErr err = {0};
	char		buf[CHUNK];

	for (int i = from; i < from + n; i++)
		SIFT_CHECKF(sift_tier_cache_read(c, &be, "k", &ref, (uint64) i * CHUNK, CHUNK, buf, &err) == SIFT_TIER_OK, "a miss reads");
}

/* A full cache evicts from its own index between the listings: the misses past the budget do not list the directory each */
static void
test_full_cache_misses_do_not_list_the_directory_each(void)
{
	char		dir[] = "/tmp/sift-unit-tier-cache-XXXXXX";
	SiftTierCache *c = scratch_cache(dir, 16);

	dir_scans = 0;
	read_chunks(c, 0, 128);
	SIFT_CHECKF(dir_scans <= 128 * 4 / 16 + 1, "128 misses of a cache of 16 chunks listed the directory %d times", dir_scans);
	SIFT_CHECKF(sift_tier_cache_stats(c)->resident_chunks <= 16, "the index holds %llu chunks, the budget 16", (unsigned long long) sift_tier_cache_stats(c)->resident_chunks);
	scratch_drop(c, dir);
}

/* While a peer lists the directory (it holds the lock of the directory), a backend does not list it too and keeps its own chunks to the budget */
static void
test_one_backend_lists_the_directory_at_a_time(void)
{
	char		dir[] = "/tmp/sift-unit-tier-cache-XXXXXX";
	SiftTierCache *c = scratch_cache(dir, 16);
	int			peer = open(dir, O_RDONLY | O_DIRECTORY);

	SIFT_CHECKF(peer >= 0 && flock(peer, LOCK_EX | LOCK_NB) == 0, "the peer holds the lock of the directory");
	dir_reads = 0;
	read_chunks(c, 0, 64);
	SIFT_CHECKF(dir_reads == 0, "the directory a peer lists was read %d times", dir_reads);
	SIFT_CHECKF(sift_tier_cache_stats(c)->resident_chunks <= 16, "the index holds %llu chunks, the budget 16", (unsigned long long) sift_tier_cache_stats(c)->resident_chunks);
	close(peer);
	read_chunks(c, 64, 8);
	SIFT_CHECKF(dir_reads > 0, "the directory is listed once the peer is done");
	scratch_drop(c, dir);
}

/* The listing of a directory of many chunks and the eviction that follows take a cancel */
static void
test_listing_and_eviction_take_a_cancel(void)
{
	char		dir[] = "/tmp/sift-unit-tier-cache-XXXXXX";
	SiftTierCache *c = scratch_cache(dir, 64);
	SiftTierErr err = {0};

	read_chunks(c, 0, 64);
	sift_tier_cache_close(c);
	interrupts = 0;
	InterruptPending = 1;
	c = sift_tier_cache_open(dir, (uint64) 16 * CHUNK, CHUNK, &err);
	InterruptPending = 0;
	SIFT_CHECKF(c != NULL, "the cache opens again");
	SIFT_CHECKF(interrupts >= 64 + 48, "a listing of 64 chunks and an eviction of 48 checked for a cancel %d times", interrupts);
	scratch_drop(c, dir);
}

int
main(void)
{
	test_misses_do_not_list_the_directory_each();
	test_full_cache_misses_do_not_list_the_directory_each();
	test_one_backend_lists_the_directory_at_a_time();
	test_listing_and_eviction_take_a_cancel();
	return sift_check_summary();
}
