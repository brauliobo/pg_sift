# test_codec: the chunk codec of tools/colcodec on pool threads (the same sources, the cpu module of tools/kernels/shim.c)
CODEC_SRC = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec_policy.c col/codec_write.c col/codec_parse.c col/codec_blob.c \
            col/codec_cold.c col/codec_kern.c col/codec_avx2.c col/codec_avx512.c col/codec_neon.c col/compress.c col/fsst.c col/codec_a.c \
            bm25/kernels.c bm25/kernels_avx2.c bm25/kernels_avx512.c bm25/kernels_neon.c bm25/analyze.c bm25/analyze_config.c \
            bm25/analyze_stem.c col/v1/kern.c col/v1/kern_avx2.c col/v1/kern_avx512.c col/v1/kern_neon.c bm25/bitmap_scalar.c \
            bm25/bitmap_avx2.c bm25/bitmap_avx512.c bm25/bitmap_neon.c
TESTS += test_codec
LIB_test_codec = core/par/arena.c $(CODEC_SRC)
FX_test_codec  = $(B)/kshim.o
