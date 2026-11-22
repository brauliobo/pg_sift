/*
 * col/f2/f2_tier_cache.c - the bounded local cache of the ranges of objects (col/f2/f2_tier.h): a directory of chunk files, each the bytes of one fixed-size piece of an object and a trailer with
 * their CRC32C, named by the content hash of the object and the piece number, so a chunk is valid for every part and every relation that has those bytes. The index is in memory, least recently used
 * last; it is rebuilt from the directory (by modification time) when the cache opens. A chunk that is missing or fails its checksum is fetched again: the cache holds nothing that the object
 * does not. The directory never holds more than the budget (past one process; each backend keeps to the budget it sees, and an eviction of a chunk another backend uses is a refetch).
 */
#include "core/host.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miscadmin.h"

#include "col/f2/f2_tier_int.h"

#define CHUNK_MAGIC		0x4B434653U		/* "SFCK" */
#define NBUCKETS		4096
#define MAX_RUN_BYTES	(UINT32_C(16) << 20)

typedef struct Chunk
{
	uint64		hash_hi;
	uint64		hash_lo;
	uint32		idx;
	uint32		bytes;			/* payload bytes: the file is bytes + 8 */
	struct Chunk *prev;			/* towards the most recently used */
	struct Chunk *next;			/* towards the least recently used */
	struct Chunk *hnext;
} Chunk;

typedef struct ChunkTrailer
{
	uint32		crc32c;
	uint32		magic;
} ChunkTrailer;

struct SiftTierCache
{
	MemoryContext cx;			/* where the cache was opened: the index of chunks lives there, whatever statement reads */
	char		dir[480];
	uint64		budget;
	uint32		chunk_bytes;
	Chunk	   *mru;
	Chunk	   *lru;
	Chunk	   *buckets[NBUCKETS];
	SiftTierCacheStats st;
	uint64		store_errors;
};

static uint32
bucket_of(uint64 hi, uint64 lo, uint32 idx)
{
	uint64		h = (hi * 31 + lo + idx) * UINT64CONST(0x9E3779B97F4A7C15);

	return (uint32) (h >> 40) & (NBUCKETS - 1);
}

static Chunk *
find(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx)
{
	for (Chunk *e = c->buckets[bucket_of(hi, lo, idx)]; e != NULL; e = e->hnext)
		if (e->hash_hi == hi && e->hash_lo == lo && e->idx == idx)
			return e;
	return NULL;
}

static void
lru_unlink(SiftTierCache *c, Chunk *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		c->mru = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		c->lru = e->prev;
	e->prev = e->next = NULL;
}

static void
lru_push_front(SiftTierCache *c, Chunk *e)
{
	e->prev = NULL;
	e->next = c->mru;
	if (c->mru)
		c->mru->prev = e;
	c->mru = e;
	if (c->lru == NULL)
		c->lru = e;
}

static void
chunk_path(const SiftTierCache *c, const Chunk *e, char *out, size_t cap)
{
	snprintf(out, cap, "%s/%016llx%016llx-%08x.chk", c->dir, (unsigned long long) e->hash_hi, (unsigned long long) e->hash_lo, e->idx);
}

/* Takes e out of the index and the directory */
static void
drop(SiftTierCache *c, Chunk *e)
{
	char		path[600];
	Chunk	  **pp = &c->buckets[bucket_of(e->hash_hi, e->hash_lo, e->idx)];

	while (*pp != e)
		pp = &(*pp)->hnext;
	*pp = e->hnext;
	lru_unlink(c, e);
	chunk_path(c, e, path, sizeof(path));
	unlink(path);
	c->st.resident_bytes -= e->bytes + sizeof(ChunkTrailer);
	c->st.resident_chunks--;
	pfree(e);
}

static Chunk *
add(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx, uint32 bytes)
{
	Chunk	   *e = MemoryContextAllocZero(c->cx, sizeof(Chunk));
	uint32		b = bucket_of(hi, lo, idx);

	e->hash_hi = hi;
	e->hash_lo = lo;
	e->idx = idx;
	e->bytes = bytes;
	e->hnext = c->buckets[b];
	c->buckets[b] = e;
	lru_push_front(c, e);
	c->st.resident_bytes += bytes + sizeof(ChunkTrailer);
	c->st.resident_chunks++;
	return e;
}

static void
evict_to_budget(SiftTierCache *c)
{
	while (c->st.resident_bytes > c->budget && c->lru != NULL)
	{
		drop(c, c->lru);
		c->st.evictions++;
	}
}

typedef struct Found
{
	uint64		hi;
	uint64		lo;
	uint32		idx;
	uint32		bytes;
	int64		mtime_ns;
} Found;

static int
found_cmp(const void *a, const void *b)
{
	int64		x = ((const Found *) a)->mtime_ns,
				y = ((const Found *) b)->mtime_ns;

	return x < y ? -1 : x > y;
}

/* The chunk files of the directory, oldest first; the temporary files a crashed backend left are removed */
static void
adopt_directory(SiftTierCache *c)
{
	DIR		   *d = opendir(c->dir);
	struct dirent *de;
	Found	   *f = NULL;
	int			nf = 0,
				cap = 0;

	while (d != NULL && (de = readdir(d)) != NULL)
	{
		char		path[600];
		struct stat st;
		unsigned long long hi,
					lo;
		unsigned	idx;
		int			len = 0;

		snprintf(path, sizeof(path), "%s/%s", c->dir, de->d_name);
		if (strstr(de->d_name, ".tmp.") != NULL)
			unlink(path);
		else if (sscanf(de->d_name, "%16llx%16llx-%8x.chk%n", &hi, &lo, &idx, &len) == 3 && de->d_name[len] == '\0' && stat(path, &st) == 0 && st.st_size > (off_t) sizeof(ChunkTrailer) &&
				 st.st_size - sizeof(ChunkTrailer) <= c->chunk_bytes)
		{
			struct timespec mtime = sift_stat_mtime(&st);

			if (nf == cap)
			{
				cap = cap ? cap * 2 : 256;
				f = f ? repalloc(f, cap * sizeof(Found)) : palloc(cap * sizeof(Found));
			}
			f[nf++] = (Found) {hi, lo, idx, st.st_size - sizeof(ChunkTrailer), (int64) mtime.tv_sec * 1000000000 + mtime.tv_nsec};
		}
	}
	if (d != NULL)
		closedir(d);
	if (nf > 0)
		qsort(f, nf, sizeof(Found), found_cmp);
	for (int i = 0; i < nf; i++)
		add(c, f[i].hi, f[i].lo, f[i].idx, f[i].bytes);
	if (f != NULL)
		pfree(f);
	evict_to_budget(c);
}

SiftTierCache *
sift_tier_cache_open(const char *dir, uint64 budget_bytes, uint32 chunk_bytes, SiftTierErr *err)
{
	SiftTierCache *c;
	char		probe[sizeof(c->dir) + 2];

	if (chunk_bytes < 4096 || chunk_bytes % 4096 != 0 || chunk_bytes > MAX_RUN_BYTES || budget_bytes < chunk_bytes || strlen(dir) >= sizeof(c->dir) - 1)
	{
		sift_tier_fail(err, SIFT_TIER_E_ARG, "the cache needs a chunk of a multiple of 4096 bytes up to 16 MiB, a budget of at least one chunk and a directory name that fits");
		return NULL;
	}
	c = palloc0(sizeof(SiftTierCache));
	c->cx = CurrentMemoryContext;
	snprintf(c->dir, sizeof(c->dir), "%s", dir);
	c->budget = budget_bytes;
	c->chunk_bytes = chunk_bytes;
	snprintf(probe, sizeof(probe), "%s/", dir);
	for (char *p = probe + 1; *p; p++)
		if (*p == '/')
		{
			*p = '\0';
			if (mkdir(probe, 0700) != 0 && errno != EEXIST)
			{
				sift_tier_fail(err, SIFT_TIER_E_IO, "the cache directory %s cannot be made: %m", dir);
				pfree(c);
				return NULL;
			}
			*p = '/';
		}
	adopt_directory(c);
	return c;
}

void
sift_tier_cache_close(SiftTierCache *c)
{
	while (c->mru != NULL)
	{
		Chunk	   *e = c->mru;

		lru_unlink(c, e);
		pfree(e);
	}
	pfree(c);
}

const SiftTierCacheStats *
sift_tier_cache_stats(const SiftTierCache *c)
{
	return &c->st;
}

/* The slice [from, from + n) of the chunk, from its file, when the file is there and whole */
static bool
serve_hit(SiftTierCache *c, Chunk *e, uint32 from, uint32 n, char *dst, char *scratch)
{
	char		path[600];
	ChunkTrailer t;
	int			fd;
	bool		ok;

	chunk_path(c, e, path, sizeof(path));
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	ok = pread(fd, scratch, e->bytes, 0) == (ssize_t) e->bytes && pread(fd, &t, sizeof(t), e->bytes) == (ssize_t) sizeof(t) && t.magic == CHUNK_MAGIC &&
		t.crc32c == sift_tier_crc32c_extend(0, scratch, e->bytes);
	close(fd);
	if (!ok)
		return false;
	memcpy(dst, scratch + from, n);
	lru_unlink(c, e);
	lru_push_front(c, e);
	(void) utimensat(AT_FDCWD, path, NULL, 0);
	c->st.hits++;
	return true;
}

static void
store_chunk(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx, const char *payload, uint32 bytes)
{
	Chunk		probe = {hi, lo, idx, bytes, NULL, NULL, NULL};
	char		path[600],
				tmp[700];
	ChunkTrailer t = {sift_tier_crc32c_extend(0, payload, bytes), CHUNK_MAGIC};
	static uint32 serial;
	int			fd;
	bool		ok;

	chunk_path(c, &probe, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int) getpid(), serial++);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
	{
		c->store_errors++;
		return;
	}
	ok = write(fd, payload, bytes) == (ssize_t) bytes && write(fd, &t, sizeof(t)) == (ssize_t) sizeof(t);
	close(fd);
	if (!ok || rename(tmp, path) != 0)
	{
		unlink(tmp);
		c->store_errors++;
		return;
	}
	if (find(c, hi, lo, idx) == NULL)
		add(c, hi, lo, idx, bytes);
}

static uint32
chunk_bytes_of(const SiftTierCache *c, const SiftTierPartRef *ref, uint32 idx)
{
	return (uint32) Min((uint64) c->chunk_bytes, ref->byte_len - (uint64) idx * c->chunk_bytes);
}

/* Copies the part of [off, off + len) that chunk idx holds into dst (which starts at off); chunk holds the bytes of chunk idx */
static void
copy_slice(const SiftTierCache *c, uint64 off, uint64 len, uint32 idx, char *dst, const char *chunk)
{
	uint64		start = (uint64) idx * c->chunk_bytes,
				lo = Max(off, start),
				hi = Min(off + len, start + c->chunk_bytes);

	if (hi > lo)
		memcpy(dst + (lo - off), chunk + (lo - start), hi - lo);
}

void
sift_tier_cache_forget(SiftTierCache *c, const SiftTierPartRef *ref, uint64 off, uint64 len)
{
	for (uint64 i = off / c->chunk_bytes; len > 0 && i <= (off + len - 1) / c->chunk_bytes; i++)
	{
		Chunk	   *e = find(c, ref->obj_hash[0], ref->obj_hash[1], i);

		if (e != NULL)
			drop(c, e);
	}
}

SiftTierStatus
sift_tier_cache_read(SiftTierCache *c, SiftTierBackend *be, const char *key, const SiftTierPartRef *ref, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	uint64		hi = ref->obj_hash[0],
				lo = ref->obj_hash[1];
	uint32		first,
				last;
	char	   *scratch,
			   *out = dst;
	char	   *run = NULL;
	SiftTierStatus st = SIFT_TIER_OK;

	if (len == 0)
		return SIFT_TIER_OK;
	if (off + len > ref->byte_len || off + len < off)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "the range [%llu, +%llu) is outside the object of %llu bytes", (unsigned long long) off, (unsigned long long) len, (unsigned long long) ref->byte_len);
	first = off / c->chunk_bytes;
	last = (off + len - 1) / c->chunk_bytes;
	scratch = palloc(c->chunk_bytes);
	for (uint32 i = first; i <= last && st == SIFT_TIER_OK;)
	{
		Chunk	   *e;
		uint32		end;
		uint64		run_from,
					run_bytes;

		CHECK_FOR_INTERRUPTS();
		e = find(c, hi, lo, i);
		if (e != NULL)
		{
			uint64		start = (uint64) i * c->chunk_bytes,
						a = Max(off, start),
						z = Min(off + len, start + e->bytes);

			if (serve_hit(c, e, a - start, z - a, out + (a - off), scratch))
			{
				i++;
				continue;
			}
			drop(c, e);
		}
		end = i;
		while (end < last && find(c, hi, lo, end + 1) == NULL && (uint64) (end + 1 - i + 1) * c->chunk_bytes <= MAX_RUN_BYTES)
			end++;
		run_from = (uint64) i * c->chunk_bytes;
		run_bytes = (uint64) (end - i) * c->chunk_bytes + chunk_bytes_of(c, ref, end);
		run = run ? repalloc(run, run_bytes) : palloc(run_bytes);
		st = be->ops->get(be, key, run_from, run_bytes, run, err);
		if (st != SIFT_TIER_OK)
			break;
		c->st.fetches++;
		c->st.fetched_bytes += run_bytes;
		for (uint32 j = i; j <= end; j++)
		{
			const char *piece = run + (uint64) (j - i) * c->chunk_bytes;

			c->st.misses++;
			copy_slice(c, off, len, j, out, piece);
			store_chunk(c, hi, lo, j, piece, chunk_bytes_of(c, ref, j));
		}
		evict_to_budget(c);
		i = end + 1;
	}
	pfree(scratch);
	if (run)
		pfree(run);
	return st;
}
