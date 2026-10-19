/*
 * sift_colz.c - the implementation of sift_colz.h for every host: liblz4 and libzstd, each optional (a build defines SIFT_HAVE_LZ4 and
 * SIFT_HAVE_ZSTD from what the build machine provides; the PostgreSQL Makefile probes them, SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 leave one out).
 * Without either it answers "not available" and the codec writes no wrapped chunk.
 */
#include "sift_host.h"

#include "sift_colcodec.h"
#include "sift_colz.h"

#ifdef SIFT_HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif
#ifdef SIFT_HAVE_ZSTD
#include <zstd.h>
#endif

bool
sift_colz_available(int alg)
{
#ifdef SIFT_HAVE_LZ4
	if (alg == SIFT_CC_ALG_LZ4)
		return true;
#endif
#ifdef SIFT_HAVE_ZSTD
	if (alg == SIFT_CC_ALG_ZSTD)
		return true;
#endif
	return false;
}

Size
sift_colz_bound(int alg, Size n)
{
#ifdef SIFT_HAVE_LZ4
	if (alg == SIFT_CC_ALG_LZ4)
		return (Size) LZ4_compressBound((int) n);
#endif
#ifdef SIFT_HAVE_ZSTD
	if (alg == SIFT_CC_ALG_ZSTD)
		return ZSTD_compressBound(n);
#endif
	return 0;
}

Size
sift_colz_compress(int alg, int level, const void *src, Size n, void *dst, Size cap)
{
#ifdef SIFT_HAVE_LZ4
	if (alg == SIFT_CC_ALG_LZ4)
	{
		int			r = level >= 3 ? LZ4_compress_HC(src, dst, (int) n, (int) cap, level) : LZ4_compress_default(src, dst, (int) n, (int) cap);

		return r > 0 ? (Size) r : 0;
	}
#endif
#ifdef SIFT_HAVE_ZSTD
	if (alg == SIFT_CC_ALG_ZSTD)
	{
		size_t		r = ZSTD_compress(dst, cap, src, n, level ? level : 3);

		return ZSTD_isError(r) ? 0 : r;
	}
#endif
	return 0;
}

bool
sift_colz_decompress(int alg, const void *src, Size clen, void *dst, Size rawlen)
{
#ifdef SIFT_HAVE_LZ4
	if (alg == SIFT_CC_ALG_LZ4)
		return LZ4_decompress_safe(src, dst, (int) clen, (int) rawlen) == (int) rawlen;
#endif
#ifdef SIFT_HAVE_ZSTD
	if (alg == SIFT_CC_ALG_ZSTD)
		return ZSTD_decompress(dst, rawlen, src, clen) == rawlen;
#endif
	return false;
}
