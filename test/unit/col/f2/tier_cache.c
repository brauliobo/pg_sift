/*
 * test/unit/col/f2/tier_cache.c - the reads of the directory the local cache of the tiered parts makes (col/f2/tier/cache.c): a miss fetches its chunks and writes their files, and the index
 * is made again from the whole directory only once a quarter of the budget was fetched since the last time, so that a read of many chunks below the budget lists the directory a bounded
 * number of times, not once for each miss. The directory is opened through the fd.c calls that the end of the transaction closes.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <dirent.h>
#include <signal.h>

#include "test/common/bench.h"

static int	dir_scans;

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

DIR *
AllocateDir(const char *dirname)
{
	dir_scans++;
	return opendir(dirname);
}

struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
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

int
main(void)
{
	test_misses_do_not_list_the_directory_each();
	return sift_check_summary();
}
