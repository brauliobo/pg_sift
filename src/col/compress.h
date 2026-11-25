/*
 * col/compress.h - the general-purpose compression libraries (LZ4, zstd) behind a host-neutral interface.
 *
 * The portable codec (col/codec/codec.c) layers these on streams of a column chunk (WRAP nodes). The implementation is the host's, resolved
 * at link time: src/col/compress.c for the PostgreSQL build (liblz4 and libzstd, each optional at build time), sqlite/host/colz.c answers "not
 * available". An algorithm that is not available makes the writer skip it and the reader raise ERRCODE_FEATURE_NOT_SUPPORTED for a chunk
 * that uses it. New algorithms are new ids in col/codec/codec.h (SIFT_CC_ALG_*) and a case here.
 */
#ifndef SIFT_COLZ_H
#define SIFT_COLZ_H

#include "core/host.h"

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

/*
 * Frees the zstd contexts the calling thread made (one for each thread that compressed or decompressed, kept for the life of the thread).
 * A thread of a pool calls it before it exits; the backend thread keeps its own.
 */
extern void sift_colz_thread_fini(void);

#define SIFT_ZSTD_DICT_MIN_BYTES	1024u
#define SIFT_ZSTD_DICT_MAX_BYTES	16384u
#define SIFT_ZSTD_DICT_SAMPLE_CAP	(128u * 1024u)
#define SIFT_ZSTD_DICT_MIN_SAMPLES	64u
#define SIFT_ZSTD_L2_LEVEL			3

/*
 * A dictionary of zstd trained on the nsamples samples (sample i is sample_sizes[i] bytes, one after the other in samples_buf), into dst (cap bytes). Returns its length, or 0 when
 * the build has no zstd, there are too few samples or the training fails. The handles below are opaque (NULL: none), made by the build that has zstd and freed by their owner.
 */
extern Size sift_colz_zstd_train_dict(void *dst, Size cap, const void *samples_buf, const Size *sample_sizes, unsigned nsamples);
extern void *sift_colz_zstd_create_cdict(const void *dict, Size dict_len, int level);
extern void sift_colz_zstd_free_cdict(void *cdict);
extern void *sift_colz_zstd_create_ddict(const void *dict, Size dict_len);
extern void sift_colz_zstd_free_ddict(void *ddict);

/* Compresses src[0..slen) with a dictionary of zstd into dst (dcap bytes, at least sift_colz_bound of zstd); returns the compressed size, or 0 when it did not fit or failed */
extern Size sift_colz_compress_zdict(const void *cdict, const void *src, Size slen, void *dst, Size dcap);

/* Decompresses exactly dlen bytes into dst; false on any error, including a stream that does not produce exactly dlen */
extern bool sift_colz_decompress_zdict(const void *ddict, const void *src, Size slen, void *dst, Size dlen);

#endif							/* SIFT_COLZ_H */
