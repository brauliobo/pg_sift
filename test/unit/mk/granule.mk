# The tests that decode real granules: the granule writer (col/granule/fixture.c) and the decoders on the column codec, linked on the cpu stand-in of test/common.
# A test of this kind adds the sources it needs in LIB_PATH (src paths) and links GRANULE_LIB and GRANULE_FX (FX_PATH). col/granule/stubs.c holds the
# storage-layer symbols the writer and the reader reach and a test does not.
GRANULE_WRITE = col/granule/granule_enc.c col/granule/enc_frame.c col/granule/granule_put.c col/granule/enc_str.c col/granule/promo.c col/granule/promo_prefix.c col/granule/tid.c
GRANULE_READ  = col/granule/granule.c col/granule/granule_dec.c col/granule/granule_dec_a.c col/granule/granule_codes.c col/granule/point_decode.c \
                col/f2/f2_view.c col/granule/granule_filter.c col/granule/cfilt_str_simd.c col/granule/cfilt_cc.c col/granule/cfilt_kern.c \
                col/like_simd.c col/like_simd_x86.c col/like_simd_neon.c
GRANULE_LIB   = core/par/arena.c $(CODEC_SRC) $(GRANULE_WRITE) $(GRANULE_READ)
GRANULE_FX    = $(B)/col/granule/fixture.o $(B)/col/granule/stubs.o $(CPU)

TESTS += col/granule/decode
LIB_col/granule/decode = $(GRANULE_LIB)
# granule_filter.c is linked for the FSST chunk opener the point decode uses; its predicate engine is not run: col/granule/decode_stubs.c
FX_col/granule/decode  = $(GRANULE_FX) $(B)/col/granule/decode_stubs.o
