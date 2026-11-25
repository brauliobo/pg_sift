# test_filter and bench_decode_filter: the filters of a granule on pool threads, over the granules of the fixture of granule.mk.
FILTER_LIB = col/granule/granule_filter_a.c col/f2/f2_pred.c col/f2/f2_pred_ext.c col/v1/ctest.c core/f2/bitscatter.c
TESTS += test_filter
LIB_test_filter = $(GRANULE_LIB) $(FILTER_LIB)
FX_test_filter  = $(GRANULE_FX) $(B)/filter_stubs.o

BENCHES += bench_decode_filter
LIB_bench_decode_filter = $(LIB_test_filter)
FX_bench_decode_filter  = $(FX_test_filter)
