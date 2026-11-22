/*
 * col/f2/f2_tier_io.c - the locations of the object store, the status every call of it returns and the file:// backend: a directory of immutable objects written to a temporary name, synced, renamed
 * and the directory synced, read by pread. It is what a cold NVMe or NFS tier uses and what every offline test runs on.
 */
#include "core/host.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miscadmin.h"

#include "col/f2/f2_tier_int.h"

SiftTierStatus
sift_tier_fail(SiftTierErr *err, SiftTierStatus code, const char *fmt, ...)
{
	va_list		ap;

	if (err != NULL)
	{
		err->code = code;
		va_start(ap, fmt);
		vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
		va_end(ap);
	}
	return code;
}

static bool
alphabet_ok(const char *s)
{
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-' || *s == '/'))
			return false;
	return true;
}

static bool
path_ok(const char *s)
{
	return alphabet_ok(s) && strstr(s, "..") == NULL && strstr(s, "//") == NULL;
}

bool
sift_tier_key_ok(const char *key)
{
	return key[0] != '\0' && key[0] != '/' && strlen(key) < 160 && path_ok(key);
}

SiftTierStatus
sift_tier_uri_parse(const char *uri, SiftTierUri *u, SiftTierErr *err)
{
	const char *rest;
	size_t		n;

	memset(u, 0, sizeof(*u));
	if (uri == NULL || strlen(uri) >= sizeof(u->path))
		return sift_tier_fail(err, SIFT_TIER_E_URI, "the location is missing or too long");
	if (strncmp(uri, "file://", 7) == 0)
	{
		u->is_file = true;
		rest = uri + 7;
		if (rest[0] != '/')
			return sift_tier_fail(err, SIFT_TIER_E_URI, "a file:// location is file:///absolute/directory");
	}
	else if (strncmp(uri, "s3://", 5) == 0 || strncmp(uri, "r2://", 5) == 0)
	{
		const char *slash;

		u->is_r2 = uri[0] == 'r';
		rest = uri + 5;
		slash = strchr(rest, '/');
		n = slash ? (size_t) (slash - rest) : strlen(rest);
		if (n < 3 || n >= sizeof(u->bucket))
			return sift_tier_fail(err, SIFT_TIER_E_URI, "the location needs a bucket name of 3 to 63 characters");
		memcpy(u->bucket, rest, n);
		for (size_t i = 0; i < n; i++)
			if (!((u->bucket[i] >= 'a' && u->bucket[i] <= 'z') || (u->bucket[i] >= '0' && u->bucket[i] <= '9') || u->bucket[i] == '-' || u->bucket[i] == '.'))
				return sift_tier_fail(err, SIFT_TIER_E_URI, "the bucket name has a character other than a-z, 0-9, '-' and '.'");
		rest = slash ? slash : "";
	}
	else
		return sift_tier_fail(err, SIFT_TIER_E_URI, "the location must start with file://, s3:// or r2://");
	if (!path_ok(rest))
		return sift_tier_fail(err, SIFT_TIER_E_URI, "the location has a user, password, query, escape or a character other than letters, digits and . _ - /, or has '..' or '//'");
	snprintf(u->path, sizeof(u->path), "%s", rest);
	n = strlen(u->path);
	while (n > 1 && u->path[n - 1] == '/')
		u->path[--n] = '\0';
	if (!u->is_file && u->path[0] == '/')
		memmove(u->path, u->path + 1, strlen(u->path));
	return SIFT_TIER_OK;
}

SiftTierStatus
sift_tier_uri_check(const char *uri, SiftTierErr *err)
{
	SiftTierUri u;

	return sift_tier_uri_parse(uri, &u, err);
}

/* ---- file:// ---- */

typedef struct FileBe
{
	SiftTierBackend base;
	char		dir[480];
} FileBe;

static int
file_path(const FileBe *b, const char *key, char *out, size_t cap)
{
	return sift_tier_key_ok(key) ? snprintf(out, cap, "%s/%s", b->dir, key) : -1;
}

/* mkdir -p of the directory part of path (after the first len0 bytes, which exist) */
static int
make_parents(char *path, size_t len0)
{
	for (char *p = path + len0 + 1; *p; p++)
	{
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) != 0 && errno != EEXIST)
		{
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	return 0;
}

static void
sync_dir_of(const char *path)
{
	char		d[1024];
	char	   *slash;
	int			fd;

	snprintf(d, sizeof(d), "%s", path);
	slash = strrchr(d, '/');
	if (slash == NULL)
		return;
	*slash = '\0';
	fd = open(d, O_RDONLY | O_DIRECTORY);
	if (fd >= 0)
	{
		(void) fsync(fd);
		close(fd);
	}
}

static void
file_etag(const struct stat *st, char *etag)
{
	struct timespec mtime = sift_stat_mtime(st);

	snprintf(etag, SIFT_TIER_ETAG_LEN, "f%llx-%llx-%llx", (unsigned long long) st->st_size, (unsigned long long) mtime.tv_sec, (unsigned long long) mtime.tv_nsec);
}

static bool
write_all(int fd, const char *p, size_t n)
{
	while (n > 0)
	{
		ssize_t		w = write(fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		n -= w;
	}
	return true;
}

/* The object's bytes copied from src to fd; a host error of the source closes and removes the file and goes on */
static SiftTierStatus
copy_to_fd(int fd, const char *tmp, const SiftTierSrc *src, SiftTierErr *err)
{
	char	   *buf = palloc(SIFT_TIER_CHUNK);
	SiftTierStatus st = SIFT_TIER_OK;

	PG_TRY();
	{
		for (uint64 off = 0; off < src->len && st == SIFT_TIER_OK;)
		{
			uint64		n = Min(SIFT_TIER_CHUNK, src->len - off);

			CHECK_FOR_INTERRUPTS();
			src->read(src->arg, off, n, buf);
			if (!write_all(fd, buf, n))
				st = sift_tier_fail(err, SIFT_TIER_E_IO, "writing %s failed: %m", tmp);
			off += n;
		}
	}
	PG_CATCH();
	{
		close(fd);
		unlink(tmp);
		pfree(buf);
		PG_RE_THROW();
	}
	PG_END_TRY();
	pfree(buf);
	return st;
}

static SiftTierStatus
file_put(SiftTierBackend *be, const char *key, const SiftTierSrc *src, const char *sha256_hex, SiftTierObjInfo *info, SiftTierErr *err)
{
	const FileBe *b = (const FileBe *) be;
	static uint32 serial;
	char		path[1024],
				tmp[1100];
	struct stat st;
	int			fd;
	SiftTierStatus rc;

	(void) sha256_hex;
	if (file_path(b, key, path, sizeof(path)) < 0)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	if (make_parents(path, strlen(b->dir)) != 0)
		return sift_tier_fail(err, SIFT_TIER_E_IO, "the directory of object %s cannot be made: %m", key);
	snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int) getpid(), serial++);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return sift_tier_fail(err, SIFT_TIER_E_IO, "%s cannot be created: %m", tmp);
	rc = copy_to_fd(fd, tmp, src, err);
	if (rc == SIFT_TIER_OK && (fsync(fd) != 0 || fstat(fd, &st) != 0))
		rc = sift_tier_fail(err, SIFT_TIER_E_IO, "%s cannot be synced: %m", tmp);
	close(fd);
	if (rc == SIFT_TIER_OK && rename(tmp, path) != 0)
		rc = sift_tier_fail(err, SIFT_TIER_E_IO, "%s cannot be renamed into place: %m", tmp);
	if (rc != SIFT_TIER_OK)
	{
		unlink(tmp);
		return rc;
	}
	sync_dir_of(path);
	info->len = st.st_size;
	file_etag(&st, info->etag);
	return SIFT_TIER_OK;
}

static SiftTierStatus
file_head(SiftTierBackend *be, const char *key, SiftTierObjInfo *info, SiftTierErr *err)
{
	char		path[1024];
	struct stat st;

	if (file_path((const FileBe *) be, key, path, sizeof(path)) < 0)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	if (stat(path, &st) != 0)
		return sift_tier_fail(err, errno == ENOENT ? SIFT_TIER_E_NOT_FOUND : SIFT_TIER_E_IO, "object %s: %m", key);
	info->len = st.st_size;
	file_etag(&st, info->etag);
	return SIFT_TIER_OK;
}

static SiftTierStatus
file_get(SiftTierBackend *be, const char *key, uint64 off, uint64 len, void *dst, SiftTierErr *err)
{
	char		path[1024];
	char	   *out = dst;
	int			fd;

	if (file_path((const FileBe *) be, key, path, sizeof(path)) < 0)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return sift_tier_fail(err, errno == ENOENT ? SIFT_TIER_E_NOT_FOUND : SIFT_TIER_E_IO, "object %s: %m", key);
	while (len > 0)
	{
		ssize_t		r = pread(fd, out, Min(len, SIFT_TIER_CHUNK), off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
		{
			close(fd);
			return r < 0 ? sift_tier_fail(err, SIFT_TIER_E_IO, "reading object %s: %m", key) : sift_tier_fail(err, SIFT_TIER_E_CORRUPT, "object %s ends before byte %llu", key, (unsigned long long) off);
		}
		out += r;
		off += r;
		len -= r;
	}
	close(fd);
	return SIFT_TIER_OK;
}

static SiftTierStatus
file_del(SiftTierBackend *be, const char *key, SiftTierErr *err)
{
	char		path[1024];

	if (file_path((const FileBe *) be, key, path, sizeof(path)) < 0)
		return sift_tier_fail(err, SIFT_TIER_E_ARG, "%s is not an object name", key);
	if (unlink(path) != 0 && errno != ENOENT)
		return sift_tier_fail(err, SIFT_TIER_E_IO, "deleting object %s: %m", key);
	sync_dir_of(path);
	return SIFT_TIER_OK;
}

static SiftTierStatus
file_list(SiftTierBackend *be, const char *prefix, void (*fn) (void *arg, const char *key), void *arg, SiftTierErr *err)
{
	const FileBe *b = (const FileBe *) be;
	char		dir[1024],
				key[1024];
	const char *slash = strrchr(prefix, '/');
	size_t		dlen = slash ? (size_t) (slash - prefix) : 0;
	DIR		   *d;
	struct dirent *e;

	snprintf(dir, sizeof(dir), "%s/%.*s", b->dir, (int) dlen, prefix);
	d = opendir(dir);
	if (d == NULL)
		return errno == ENOENT ? SIFT_TIER_OK : sift_tier_fail(err, SIFT_TIER_E_IO, "listing %s: %m", prefix);
	while ((e = readdir(d)) != NULL)
	{
		snprintf(key, sizeof(key), "%.*s%s%s", (int) dlen, prefix, dlen ? "/" : "", e->d_name);
		if (e->d_name[0] != '.' && strncmp(key, prefix, strlen(prefix)) == 0 && sift_tier_key_ok(key))
			fn(arg, key);
	}
	closedir(d);
	return SIFT_TIER_OK;
}

static void
file_close(SiftTierBackend *be)
{
	pfree(be);
}

static const SiftTierBackendOps file_ops = {file_put, file_head, file_get, file_del, file_list, file_close};

SiftTierBackend *
sift_tier_file_open(const char *dir, SiftTierErr *err)
{
	FileBe	   *b = palloc0(sizeof(FileBe));
	char		probe[sizeof(b->dir) + 8];

	snprintf(b->dir, sizeof(b->dir), "%s", dir);
	snprintf(probe, sizeof(probe), "%s/", dir);
	if (make_parents(probe, 0) != 0 || (mkdir(b->dir, 0700) != 0 && errno != EEXIST))
	{
		sift_tier_fail(err, SIFT_TIER_E_IO, "the directory %s cannot be made: %m", dir);
		pfree(b);
		return NULL;
	}
	b->base.ops = &file_ops;
	b->base.is_local = true;
	return &b->base;
}

SiftTierBackend *
sift_tier_open(const SiftTierOpen *o, SiftTierErr *err)
{
	SiftTierUri u;

	if (sift_tier_uri_parse(o->uri, &u, err) != SIFT_TIER_OK)
		return NULL;
	if (u.is_file)
		return sift_tier_file_open(u.path, err);
	return sift_tier_s3_open(o, u.is_r2, u.bucket, u.path, err);
}

void
sift_tier_close(SiftTierBackend *be)
{
	be->ops->close(be);
}
