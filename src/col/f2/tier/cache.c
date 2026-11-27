/*
 * col/f2/tier/cache.c - the bounded local cache of the ranges of objects (col/f2/tier/tier.h): a directory of chunk files, each the bytes of one fixed-size piece of an object and a trailer with
 * their CRC32C, named by the content hash of the object and the piece number, so a chunk is valid for every part and every relation that has those bytes. The index is in memory, least recently used
 * last; it is rebuilt from the directory (by modification time) when the cache opens. A chunk that is missing or fails its checksum is fetched again: the cache holds nothing that the object
 * does not. The directory is shared by the backends: a chunk another backend wrote is found by its file and served, and the index is made again from the directory once this backend has
 * fetched a quarter of the budget since it last did, so that the eviction keeps the bytes of the directory, not the ones of this backend, to the budget within a quarter of it per backend (an
 * eviction of a chunk another backend uses is a refetch). One backend at a time makes it, under a lock of the directory it does not wait for: a backend that finds a peer at it, or that holds
 * more than the budget between two, evicts from its own index, so that a miss costs no read of the whole directory and many readers do not list it at once.
 */
#include "core/host.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/fd.h"

#include "col/f2/tier/int.h"

#define CHUNK_MAGIC		0x4B434653U		/* "SFCK" */
#define MIN_BUCKETS		64
#define MAX_FIRST_BUCKETS	(UINT32_C(1) << 20)
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
	Chunk	  **buckets;		/* a power of two of chains, doubled when a chain would average more than three quarters of a chunk */
	uint32		nbuckets;
	SiftTierCacheStats st;
	uint64		store_errors;
	uint64		unsynced_bytes; /* fetched since the index was last made from the directory */
};

static uint32
hash_of(uint64 hi, uint64 lo, uint32 idx)
{
	return (uint32) (((hi * 31 + lo + idx) * UINT64CONST(0x9E3779B97F4A7C15)) >> 24);
}

static uint32
bucket_of(const SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx)
{
	return hash_of(hi, lo, idx) & (c->nbuckets - 1);
}

static Chunk *
find(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx)
{
	for (Chunk *e = c->buckets[bucket_of(c, hi, lo, idx)]; e != NULL; e = e->hnext)
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
	Chunk	  **pp = &c->buckets[bucket_of(c, e->hash_hi, e->hash_lo, e->idx)];

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

/* The chains again, twice as many: a cache of many chunks keeps a chain of about one however much it holds */
static void
grow_buckets(SiftTierCache *c)
{
	uint32		n = c->nbuckets * 2;
	Chunk	  **buckets = MemoryContextAllocZero(c->cx, n * sizeof(Chunk *));

	for (uint32 i = 0; i < c->nbuckets; i++)
		for (Chunk *e = c->buckets[i], *next; e != NULL; e = next)
		{
			uint32		b = hash_of(e->hash_hi, e->hash_lo, e->idx) & (n - 1);

			next = e->hnext;
			e->hnext = buckets[b];
			buckets[b] = e;
		}
	pfree(c->buckets);
	c->buckets = buckets;
	c->nbuckets = n;
}

static Chunk *
add(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx, uint32 bytes)
{
	Chunk	   *e = MemoryContextAllocZero(c->cx, sizeof(Chunk));
	uint32		b;

	if (c->st.resident_chunks * 4 > (uint64) c->nbuckets * 3)
		grow_buckets(c);
	b = bucket_of(c, hi, lo, idx);
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

/* A chunk of the directory that this backend has not seen: a peer wrote it, and its file is there with the size that chunk of the object has */
static Chunk *
adopt_peer(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx, uint32 bytes)
{
	Chunk		probe = {hi, lo, idx, bytes, NULL, NULL, NULL};
	char		path[600];
	struct stat st;

	chunk_path(c, &probe, path, sizeof(path));
	return stat(path, &st) == 0 && st.st_size == (off_t) (bytes + sizeof(ChunkTrailer)) ? add(c, hi, lo, idx, bytes) : NULL;
}

static void
evict_down_to(SiftTierCache *c, uint64 bytes)
{
	while (c->st.resident_bytes > bytes && c->lru != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		drop(c, c->lru);
		c->st.evictions++;
	}
}

static void
evict_to_budget(SiftTierCache *c)
{
	evict_down_to(c, c->budget);
}

/* A temporary file of the cache that no live backend writes: the process whose id it names is gone (a name that does not carry one is nobody's) */
static bool
tmp_file_is_dead(const char *name)
{
	const char *at = strstr(name, ".tmp.");
	long		pid = at != NULL ? strtol(at + 5, NULL, 10) : 0;

	return pid <= 0 || (kill((pid_t) pid, 0) != 0 && errno == ESRCH);
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

/* The chunk files of the directory d, oldest first; the temporary files of backends that are gone are removed */
static void
adopt_directory(SiftTierCache *c, DIR *d)
{
	struct dirent *de;
	Found	   *f = NULL;
	int			nf = 0,
				cap = 0;

	while ((de = ReadDir(d, c->dir)) != NULL)
	{
		char		path[600];
		struct stat st;
		unsigned long long hi,
					lo;
		unsigned	idx;
		int			len = 0;

		CHECK_FOR_INTERRUPTS();
		snprintf(path, sizeof(path), "%s/%s", c->dir, de->d_name);
		if (strstr(de->d_name, ".tmp.") != NULL)
		{
			if (tmp_file_is_dead(de->d_name))
				unlink(path);
		}
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
	FreeDir(d);
	if (nf > 0)
		qsort(f, nf, sizeof(Found), found_cmp);
	for (int i = 0; i < nf; i++)
		add(c, f[i].hi, f[i].lo, f[i].idx, f[i].bytes);
	if (f != NULL)
		pfree(f);
	evict_to_budget(c);
}

static void
forget_index(SiftTierCache *c)
{
	while (c->mru != NULL)
	{
		Chunk	   *e = c->mru;

		lru_unlink(c, e);
		pfree(e);
	}
	memset(c->buckets, 0, c->nbuckets * sizeof(Chunk *));
	c->st.resident_bytes = 0;
	c->st.resident_chunks = 0;
}

/* The index as the directory is now, within the budget; while a peer makes its own (the lock goes with the descriptor of the directory), this one evicts from the index it has */
static void
sync_directory(SiftTierCache *c)
{
	DIR		   *d = AllocateDir(c->dir);

	if (d != NULL && flock(dirfd(d), LOCK_EX | LOCK_NB) != 0)
	{
		FreeDir(d);
		evict_to_budget(c);
		return;
	}
	forget_index(c);
	adopt_directory(c, d);
	c->unsynced_bytes = 0;
}

static void
fetched(SiftTierCache *c, uint64 bytes)
{
	c->unsynced_bytes += bytes;
	if (c->unsynced_bytes >= c->budget / 4)
		sync_directory(c);
	else
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
	c->nbuckets = pg_nextpower2_32(Max(MIN_BUCKETS, (uint32) Min(budget_bytes / chunk_bytes * 2, MAX_FIRST_BUCKETS)));
	c->buckets = MemoryContextAllocZero(c->cx, c->nbuckets * sizeof(Chunk *));
	snprintf(probe, sizeof(probe), "%s/", dir);
	for (char *p = probe + 1; *p; p++)
		if (*p == '/')
		{
			*p = '\0';
			if (mkdir(probe, 0700) != 0 && errno != EEXIST)
			{
				sift_tier_fail(err, SIFT_TIER_E_IO, "the cache directory %s cannot be made: %m", dir);
				pfree(c->buckets);
				pfree(c);
				return NULL;
			}
			*p = '/';
		}
	sync_directory(c);
	return c;
}

void
sift_tier_cache_close(SiftTierCache *c)
{
	forget_index(c);
	pfree(c->buckets);
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

/* The chunk written to tmp and renamed to path; false with errno set when the file system refused */
static bool
write_chunk_file(const char *tmp, const char *path, const char *payload, uint32 bytes, const ChunkTrailer *t)
{
	int			fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	bool		ok;
	int			saved;

	if (fd < 0)
		return false;
	ok = write(fd, payload, bytes) == (ssize_t) bytes && write(fd, t, sizeof(*t)) == (ssize_t) sizeof(*t);
	saved = errno;
	close(fd);
	if (ok && rename(tmp, path) != 0)
	{
		saved = errno;
		ok = false;
	}
	if (!ok)
		unlink(tmp);
	errno = saved;
	return ok;
}

static void
store_chunk(SiftTierCache *c, uint64 hi, uint64 lo, uint32 idx, const char *payload, uint32 bytes)
{
	Chunk		probe = {hi, lo, idx, bytes, NULL, NULL, NULL};
	char		path[600],
				tmp[700];
	ChunkTrailer t = {sift_tier_crc32c_extend(0, payload, bytes), CHUNK_MAGIC};
	static uint32 serial;
	bool		ok;

	chunk_path(c, &probe, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int) getpid(), serial++);
	ok = write_chunk_file(tmp, path, payload, bytes, &t);
	if (!ok && (errno == ENOSPC || errno == EDQUOT))
	{
		evict_down_to(c, c->st.resident_bytes / 2);
		ok = write_chunk_file(tmp, path, payload, bytes, &t);
	}
	if (!ok)
	{
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

static Chunk *
lookup(SiftTierCache *c, const SiftTierPartRef *ref, uint32 idx)
{
	Chunk	   *e = find(c, ref->obj_hash[0], ref->obj_hash[1], idx);

	return e != NULL ? e : adopt_peer(c, ref->obj_hash[0], ref->obj_hash[1], idx, chunk_bytes_of(c, ref, idx));
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
		e = lookup(c, ref, i);
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
		while (end < last && lookup(c, ref, end + 1) == NULL && (uint64) (end + 1 - i + 1) * c->chunk_bytes <= MAX_RUN_BYTES)
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
		fetched(c, run_bytes);
		i = end + 1;
	}
	pfree(scratch);
	if (run)
		pfree(run);
	return st;
}
