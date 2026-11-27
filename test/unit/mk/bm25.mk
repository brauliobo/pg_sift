# The text search index: the inverted buffer of bm25/inv.c and the bounds of bm25/tail.c, which their tests include (the symbols of the server it does not run link as stubs that trap: AUTOSTUB), and
# the buffers of the analyzer.
BM25_TESTS = bm25/inv_terms bm25/tail_bounds

TESTS    += $(BM25_TESTS) bm25/analyze_huge
AUTOSTUB += $(BM25_TESTS)
$(foreach t,$(BM25_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -Wno-unused-function))
LIB_bm25/analyze_huge = bm25/analyze/analyze.c bm25/analyze/config.c bm25/analyze/stem.c
