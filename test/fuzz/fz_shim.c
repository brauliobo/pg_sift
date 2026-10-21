/*
 * fz_shim.c - what the fuzz targets need beyond tools/pgshim.c (test/fuzz/README.md): fz_try() sorts the errors of a decoder of untrusted
 * bytes by SQLSTATE (the answer it may give to corrupt bytes or a finding), the CPU dispatch, the stream the targets read and the storage
 * symbols they never reach.
 */
#include "postgres.h"

#include <stdarg.h>
#include <stdio.h>

#include "storage/buffile.h"

#include "sift.h"
#include "col/codec.h"
#include "col/col.h"
#include "col/shred.h"
#include "sift_store.h"
#include "fz_shim.h"

extern void kt_cpu_init(void);	/* tools/kernels/shim.c */

void
fz_bad(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	fputs("FINDING: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	abort();
}

/*
 * An error that is not of a corruption class (an elog(ERROR) is XX000, "internal error") is safe but wrong: the caller of a reader tells
 * corrupt data from a bug by the SQLSTATE. Reported once per message as FINDING-LOW; FZ_STRICT=1 makes it fatal.
 */
static void
fz_wrong_class(void)
{
	static uint64 seen[64];
	static int	nseen;
	uint64		h = 1469598103934665603ULL;

	for (const char *p = pgshim_errmsg; *p && !(*p >= '0' && *p <= '9'); p++)
		h = (h ^ (uint8) *p) * 1099511628211ULL;
	if (getenv("FZ_STRICT"))
		fz_bad("error of class %d (XX000 is 0) where corrupt input should raise a corruption error: %s", pgshim_sqlstate, pgshim_errmsg);
	for (int i = 0; i < nseen; i++)
		if (seen[i] == h)
			return;
	if (nseen < 64)
		seen[nseen++] = h;
	fprintf(stderr, "FINDING-LOW: SQLSTATE class %d (0 = XX000) for corrupt input: %s (%s)\n", pgshim_sqlstate, pgshim_errmsg, pgshim_errloc);
}

bool
fz_try(void (*fn) (void *), void *arg)
{
	if (pgshim_try(fn, arg))
		return true;
	if (getenv("FZ_NOERR"))
		fz_bad("error %d where the input should be valid: %s (%s)", pgshim_sqlstate, pgshim_errmsg, pgshim_errloc);
	switch (pgshim_sqlstate)
	{
		case ERRCODE_DATA_CORRUPTED:
		case ERRCODE_INDEX_CORRUPTED:
		case ERRCODE_FEATURE_NOT_SUPPORTED:
		case ERRCODE_PROGRAM_LIMIT_EXCEEDED:
		case ERRCODE_INVALID_PARAMETER_VALUE:
		case ERRCODE_SYNTAX_ERROR:
		case ERRCODE_OUT_OF_MEMORY:
			break;
		default:
			fz_wrong_class();
	}
	return false;
}

/* ---------- CPU and dispatch ---------- */

void
fz_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
	sift_shred_init();
}

int
fz_levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (!n || sift_cc_kernels_for((SiftSimdLevel) l) != sift_cc_kernels_for(lv[n - 1]) ||
			sift_col_kernels_for((SiftSimdLevel) l) != sift_col_kernels_for(lv[n - 1]) ||
			sift_shred_kernels_for((SiftSimdLevel) l) != sift_shred_kernels_for(lv[n - 1]))
			lv[n++] = (SiftSimdLevel) l;
	return n;
}

void
fz_use_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
	sift_shred_kern = sift_shred_kernels_for(level);
}

bool
fz_skip(const char *name)
{
	const char *l = getenv("FZ_SKIP"),
			   *p;
	size_t		n = strlen(name);

	for (p = l; p && *p;)
	{
		const char *e = strchr(p, ',');
		size_t		len = e ? (size_t) (e - p) : strlen(p);

		if (len == n && !memcmp(p, name, n))
			return true;
		p = e ? e + 1 : p + len;
	}
	return false;
}

uint8 *
fz_copy(const uint8 *data, size_t len, size_t pad, size_t align)
{
	uint8	   *p = aligned_alloc(align, (len + pad + align - 1) & ~(align - 1));

	if (!p)
		abort();
	memcpy(p, data, len);
	memset(p + len, 0, pad);
	return p;
}

/* ---------- symbols the linked sources mention and the targets never reach ---------- */

#define UNREACHED(name) fz_bad("%s reached: the fuzz target links no storage layer", name)

BufFile *
BufFileCreateTemp(bool interXact)
{
	UNREACHED("BufFileCreateTemp");
}

void
BufFileClose(BufFile *f)
{
	UNREACHED("BufFileClose");
}

void
BufFileWrite(BufFile *f, const void *p, size_t n)
{
	UNREACHED("BufFileWrite");
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	UNREACHED("sift_sw_tell");
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	UNREACHED("sift_sw_write");
}

void
sift_sw_write_file(SiftStreamWriter *sw, BufFile *file, uint64 len)
{
	UNREACHED("sift_sw_write_file");
}

/* The stream a target reads: one buffer, bounds-checked as the real reader checks a segment (sift_sr_get is the only place that knows the size) */
static const uint8 *fz_stream_buf;
static uint64 fz_stream_len;

void
fz_stream_set(const uint8 *buf, uint64 len)
{
	fz_stream_buf = buf;
	fz_stream_len = len;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	if (off > fz_stream_len || len > fz_stream_len - off)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("sift: read of %zu bytes at %llu is outside the stream of %llu",
																	len, (unsigned long long) off, (unsigned long long) fz_stream_len)));
	return fz_stream_buf + off;
}

void
sift_sr_release(SiftStreamReader *sr)
{
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
}
