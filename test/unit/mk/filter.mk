# The filters of a granule on pool threads, over the granules of the fixture of granule.mk.
FILTER_LIB = col/granule/granule_filter_a.c col/f2/f2_pred.c col/f2/f2_pred_ext.c col/v1/ctest.c core/f2/bitscatter.c

TESTS   += col/granule/filter
BENCHES += col/granule/filter_bench
LIB_col/granule/filter       = $(GRANULE_LIB) $(FILTER_LIB)
FX_col/granule/filter        = $(GRANULE_FX) $(B)/col/granule/filter_stubs.o
LIB_col/granule/filter_bench = $(LIB_col/granule/filter)
FX_col/granule/filter_bench  = $(FX_col/granule/filter)
