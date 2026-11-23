/*
 * col/compress.c - the implementation of col/compress.h for every host: liblz4 and libzstd, each optional (a build defines SIFT_HAVE_LZ4 and
 * SIFT_HAVE_ZSTD from what the build machine provides; the PostgreSQL Makefile probes them, SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 leave one out).
 * Without either it answers "not available" and the codec writes no wrapped chunk.
 */

#include "col/codec.h"
#include "col/compress.h"

#ifdef SIFT_HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif
#ifdef SIFT_HAVE_ZSTD
#include <zdict.h>
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

#ifdef SIFT_HAVE_ZSTD
/* The contexts of the thread, made once: ZSTD_compress and ZSTD_decompress make and free one for each call, which a dictionary of 200,000 strings in blocks of 64 does 3,000 times (the bytes are the same) */
static _Thread_local ZSTD_CCtx *zstd_cctx;
static _Thread_local ZSTD_DCtx *zstd_dctx;
#endif

void
sift_colz_thread_fini(void)
{
#ifdef SIFT_HAVE_ZSTD
	ZSTD_freeCCtx(zstd_cctx);
	zstd_cctx = NULL;
	ZSTD_freeDCtx(zstd_dctx);
	zstd_dctx = NULL;
#endif
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
		size_t		r;

		if (zstd_cctx == NULL)
			zstd_cctx = ZSTD_createCCtx();
		r = zstd_cctx != NULL ? ZSTD_compressCCtx(zstd_cctx, dst, cap, src, n, level ? level : 3) : ZSTD_compress(dst, cap, src, n, level ? level : 3);

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
	{
		if (zstd_dctx == NULL)
			zstd_dctx = ZSTD_createDCtx();
		return (zstd_dctx != NULL ? ZSTD_decompressDCtx(zstd_dctx, dst, rawlen, src, clen) : ZSTD_decompress(dst, rawlen, src, clen)) == rawlen;
	}
#endif
	return false;
}

Size
sift_colz_zstd_train_dict(void *dst, Size cap, const void *samples_buf, const Size *sample_sizes, unsigned nsamples)
{
#ifdef SIFT_HAVE_ZSTD
	size_t		r;

	if (nsamples < SIFT_ZSTD_DICT_MIN_SAMPLES || cap < SIFT_ZSTD_DICT_MIN_BYTES)
		return 0;
	r = ZDICT_trainFromBuffer(dst, cap, samples_buf, sample_sizes, nsamples);
	return ZDICT_isError(r) ? 0 : r;
#else
	return 0;
#endif
}

void *
sift_colz_zstd_create_cdict(const void *dict, Size dict_len, int level)
{
#ifdef SIFT_HAVE_ZSTD
	return ZSTD_createCDict(dict, dict_len, level);
#else
	return NULL;
#endif
}

void
sift_colz_zstd_free_cdict(void *cdict)
{
#ifdef SIFT_HAVE_ZSTD
	ZSTD_freeCDict(cdict);
#endif
}

void *
sift_colz_zstd_create_ddict(const void *dict, Size dict_len)
{
#ifdef SIFT_HAVE_ZSTD
	return ZSTD_createDDict(dict, dict_len);
#else
	return NULL;
#endif
}

void
sift_colz_zstd_free_ddict(void *ddict)
{
#ifdef SIFT_HAVE_ZSTD
	ZSTD_freeDDict(ddict);
#endif
}

Size
sift_colz_compress_zdict(const void *cdict, const void *src, Size slen, void *dst, Size dcap)
{
#ifdef SIFT_HAVE_ZSTD
	size_t		r;

	if (zstd_cctx == NULL)
		zstd_cctx = ZSTD_createCCtx();
	if (zstd_cctx == NULL)
		return 0;
	r = ZSTD_compress_usingCDict(zstd_cctx, dst, dcap, src, slen, cdict);
	return ZSTD_isError(r) ? 0 : r;
#else
	return 0;
#endif
}

bool
sift_colz_decompress_zdict(const void *ddict, const void *src, Size slen, void *dst, Size dlen)
{
#ifdef SIFT_HAVE_ZSTD
	if (zstd_dctx == NULL)
		zstd_dctx = ZSTD_createDCtx();
	return zstd_dctx != NULL && ZSTD_decompress_usingDDict(zstd_dctx, dst, dlen, src, slen, ddict) == dlen;
#else
	return false;
#endif
}
