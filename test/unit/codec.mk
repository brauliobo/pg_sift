# test_codec: the chunk codec of tools/colcodec on pool threads (the same sources, the cpu module of tools/kernels/shim.c)
CODEC_SRC = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec/policy.c col/codec/write.c col/codec/parse.c col/codec/blob.c \
            col/codec/cold.c col/codec/kern.c col/codec/avx2.c col/codec/avx512.c col/codec/neon.c col/compress.c col/fsst.c col/codec/a.c \
            bm25/kernels/kernels.c bm25/kernels/avx2.c bm25/kernels/avx512.c bm25/kernels/neon.c bm25/analyze/analyze.c bm25/analyze/config.c \
            bm25/analyze/stem.c col/v1/kern/kern.c col/v1/kern/avx2.c col/v1/kern/avx512.c col/v1/kern/neon.c bm25/bitmap/scalar.c \
            bm25/bitmap/avx2.c bm25/bitmap/avx512.c bm25/bitmap/neon.c
TESTS += test_codec
LIB_test_codec = core/par/arena.c $(CODEC_SRC)
FX_test_codec  = $(B)/kshim.o
