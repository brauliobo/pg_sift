# The chunk codec of col/codec on pool threads (the cpu module is test/common/cpushim.c) and the decoders under it.
CODEC_SRC = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec/policy.c col/codec/write.c col/codec/parse.c col/codec/blob.c \
            col/codec/cold.c col/codec/kern.c col/codec/avx2.c col/codec/avx512.c col/codec/neon.c col/compress.c col/fsst.c col/codec/a.c \
            bm25/kernels/kernels.c bm25/kernels/avx2.c bm25/kernels/avx512.c bm25/kernels/neon.c bm25/analyze/analyze.c bm25/analyze/config.c \
            bm25/analyze/stem.c col/v1/kern/kern.c col/v1/kern/avx2.c col/v1/kern/avx512.c col/v1/kern/neon.c bm25/bitmap/scalar.c \
            bm25/bitmap/avx2.c bm25/bitmap/avx512.c bm25/bitmap/neon.c
CPU       = $(B)/cpushim.o

TESTS += col/codec/chunk col/codec/decoders
LIB_col/codec/chunk    = core/par/arena.c $(CODEC_SRC)
FX_col/codec/chunk     = $(CPU)
LIB_col/codec/decoders = core/par/arena.c col/fsst.c col/compress.c
