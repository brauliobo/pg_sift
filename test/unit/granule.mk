# The tests that decode real granules: the granule writer (granule_fixture.c) and the decoders on the column codec, linked on the cpu stand-in of tools/kernels.
# A test of this kind is test_X.c, listed in GRANULE_TESTS (after this file is included), and links GRANULE_LIB and GRANULE_FX; a test adds the sources it needs
# in LIB_test_X (src paths) as the other tests do. granule_stubs.c holds the storage-layer symbols the writer and the reader reach and a test does not.
GRANULE_CODEC = col/codec/codec.c col/codec/filter.c col/codec/get.c col/codec_policy.c col/codec_write.c col/codec_parse.c col/codec_blob.c col/codec_cold.c \
                col/codec_kern.c col/codec_avx2.c col/codec_avx512.c col/codec_neon.c col/compress.c col/fsst.c \
                col/v1/kern.c col/v1/kern_avx2.c col/v1/kern_avx512.c col/v1/kern_neon.c \
                bm25/kernels.c bm25/kernels_avx2.c bm25/kernels_avx512.c bm25/kernels_neon.c \
                bm25/bitmap_scalar.c bm25/bitmap_avx2.c bm25/bitmap_avx512.c bm25/bitmap_neon.c bm25/analyze.c bm25/analyze_stem.c
GRANULE_WRITE = col/granule/granule_enc.c col/granule/enc_str.c col/granule/promo.c col/granule/promo_prefix.c col/granule/tid.c
GRANULE_READ  = col/granule/granule.c col/granule/granule_dec.c col/granule/granule_codes.c col/granule/point_decode.c col/f2/f2_view.c \
                col/granule/granule_filter.c col/granule/cfilt_str_simd.c col/granule/cfilt_cc.c col/granule/cfilt_kern.c \
                col/like_simd.c col/like_simd_x86.c col/like_simd_neon.c
GRANULE_LIB   = core/par/arena.c $(GRANULE_CODEC) $(GRANULE_WRITE) $(GRANULE_READ)
GRANULE_FX    = $(B)/granule_fixture.o $(B)/granule_stubs.o $(B)/kshim.o
GRANULE_TESTS += test_granule
LIB_test_granule = $(GRANULE_LIB)
# granule_filter.c is linked for the FSST chunk opener the point decode uses; its predicate engine is not run: granule_filter_stubs.c
FX_test_granule = $(B)/granule_filter_stubs.o

$(B)/granule_fixture.o $(B)/granule_stubs.o $(B)/granule_filter_stubs.o: $(B)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS) -c -o $@ $<

$(B)/kshim.o: $(TOOLS)/kernels/shim.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS)/kernels -c -o $@ $<

.SECONDEXPANSION:
$(addprefix $(B)/,$(GRANULE_TESTS)): $(B)/%: %.c $$(call objs,$$*) $(SHIM) $(GRANULE_FX) $$(FX_$$*) $(HDR)
	$(CC) $(CFLAGS) -I$(TOOLS) -o $@ $< $(call objs,$*) $(SHIM) $(GRANULE_FX) $(FX_$*) $(LDFLAGS) $(LDLIBS) -lpgcommon
