/*
 * spool.c - BufFile spool files over the C library: an unnamed file in sq_tmpdir.
 */
#include "host.h"

#include <fcntl.h>

/* ---------- spool files ---------- */

struct BufFile
{
	FILE	   *f;
};

const char *sq_tmpdir;

/* An unnamed file in sq_tmpdir (the C library's tmpfile() is /tmp, which may be a RAM disk) */
static FILE *
spool_file(void)
{
	int			fd = sq_tmpdir ? open(sq_tmpdir, O_TMPFILE | O_RDWR, 0600) : -1;
	FILE	   *f = fd >= 0 ? fdopen(fd, "w+") : NULL;

	return f ? f : tmpfile();
}

BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *b = palloc(sizeof(BufFile));

	b->f = spool_file();
	if (b->f == NULL)
		elog(ERROR, "could not create a temporary file");
	return b;
}

void
BufFileWrite(BufFile *file, const void *ptr, size_t size)
{
	if (fwrite(ptr, 1, size, file->f) != size)
		elog(ERROR, "could not write to a temporary file");
}

void
BufFileReadExact(BufFile *file, void *ptr, size_t size)
{
	if (fread(ptr, 1, size, file->f) != size)
		elog(ERROR, "could not read from a temporary file");
}

size_t
BufFileRead(BufFile *file, void *ptr, size_t size)
{
	return fread(ptr, 1, size, file->f);
}

BufFile *
BufFileCreateFileSet(FileSet *fileset, const char *name)
{
	elog(ERROR, "sift: no shared file sets in this host");
	return NULL;
}

BufFile *
BufFileOpenFileSet(FileSet *fileset, const char *name, int mode, bool missing_ok)
{
	elog(ERROR, "sift: no shared file sets in this host");
	return NULL;
}

void
BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok)
{
	elog(ERROR, "sift: no shared file sets in this host");
}

int			maintenance_work_mem = 64 * 1024;

int
BufFileSeek(BufFile *file, int fileno, long offset, int whence)
{
	return fseek(file->f, offset, whence);
}

void
BufFileClose(BufFile *file)
{
	fclose(file->f);
	pfree(file);
}
