/*
 * sift_colz.h - the general-purpose compression libraries (LZ4, zstd) behind a host-neutral interface.
 *
 * The portable codec (sift_colcodec.c) layers these on streams of a column chunk (WRAP nodes). The implementation is the host's, resolved
 * at link time: src/sift_colz.c for the PostgreSQL build (liblz4 and libzstd, each optional at build time), sqlite/host/colz.c answers "not
 * available". An algorithm that is not available makes the writer skip it and the reader raise ERRCODE_FEATURE_NOT_SUPPORTED for a chunk
 * that uses it. New algorithms are new ids in sift_colcodec.h (SIFT_CC_ALG_*) and a case here.
 */
#ifndef SIFT_COLZ_H
#define SIFT_COLZ_H

#include "sift_host.h"

/* True when the build can both compress and decompress with the algorithm (SIFT_CC_ALG_LZ4, SIFT_CC_ALG_ZSTD) */
extern bool sift_colz_available(int alg);

/* The most compressed bytes n input bytes can need */
extern Size sift_colz_bound(int alg, Size n);

/*
 * Compresses src[0..n) into dst (cap bytes, at least sift_colz_bound). level 0 is the default of the algorithm (lz4 1, zstd 3); for lz4 a
 * level of 3 or more selects the high-compression coder (LZ4HC). Returns the compressed size, or 0 when it did not fit or failed.
 */
extern Size sift_colz_compress(int alg, int level, const void *src, Size n, void *dst, Size cap);

/* Decompresses exactly rawlen bytes into dst from clen bytes; false on any error, including a stream that does not produce exactly rawlen */
extern bool sift_colz_decompress(int alg, const void *src, Size clen, void *dst, Size rawlen);

#endif							/* SIFT_COLZ_H */
