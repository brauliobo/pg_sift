# test_filter and bench_decode_filter: the filters of a granule on pool threads, over the granules of the fixture of granule.mk (included by the Makefile with the other *.mk).
FILTER_LIB = col/granule/granule_filter_a.c col/f2/f2_pred.c col/f2/f2_pred_ext.c col/v1/ctest.c core/f2/bitscatter.c
GRANULE_TESTS += test_filter
LIB_test_filter = $(GRANULE_LIB) $(FILTER_LIB)
FX_test_filter = $(B)/filter_stubs.o

$(B)/filter_stubs.o: filter_stubs.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(TOOLS) -c -o $@ $<


BENCHES += bench_decode_filter
LIB_bench_decode_filter = $(LIB_test_filter)

.SECONDEXPANSION:
$(B)/bench_decode_filter: bench_decode_filter.c $$(call objs,bench_decode_filter) $(SHIM) $(GRANULE_FX) $$(FX_test_filter) $(HDR)
	$(CC) $(CFLAGS) -I$(TOOLS) -o $@ $< $(call objs,bench_decode_filter) $(SHIM) $(GRANULE_FX) $(FX_test_filter) $(LDFLAGS) $(LDLIBS) -lpgcommon
