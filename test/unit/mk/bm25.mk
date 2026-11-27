# The text search index: the document of bm25/doc.c, the inverted buffer of bm25/inv.c and the bounds of bm25/tail.c, which their tests include (the symbols of the server it does not run link as stubs that trap: AUTOSTUB), and
# the buffers of the analyzer, the result of a parallel ranked search (bm25/parallel.c) and the doc a write buffer reader assembles (bm25/wread.c), the entries of the segment cache
# under a failed allocation (bm25/segcache.c) and the hits a ranked search holds past 1 GB (bm25/rank/search.c).
BM25_TESTS = bm25/doc bm25/doctext_window bm25/interrupts bm25/inv_terms bm25/keyed_huge bm25/livestream_huge bm25/par_collect bm25/phrase_huge \
             bm25/pos_records_huge bm25/posblk_huge bm25/rank_budget bm25/runs_batch_pos bm25/segcache_huge bm25/segcache_oom bm25/tail_bounds \
             bm25/wread_huge

TESTS    += $(BM25_TESTS) bm25/analyze_huge
AUTOSTUB += $(BM25_TESTS)
$(foreach t,$(BM25_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -Wno-unused-function))
LIB_bm25/analyze_huge = bm25/analyze/analyze.c bm25/analyze/config.c bm25/analyze/stem.c
LIB_bm25/doc          = $(LIB_bm25/analyze_huge)
LIB_bm25/par_collect  = core/par/topk.c
