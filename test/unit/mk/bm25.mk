# The text search index: the inverted buffer of bm25/inv.c, which its test includes (the symbols of the server it does not run link as stubs that trap: AUTOSTUB).
BM25_TESTS = bm25/inv_terms

TESTS    += $(BM25_TESTS)
AUTOSTUB += $(BM25_TESTS)
$(foreach t,$(BM25_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -Wno-unused-function))
