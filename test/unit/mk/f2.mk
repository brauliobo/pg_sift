# The tests of Format 2 and its helpers that include the sources they cover (SIFT_S49_STANDALONE) and link no library of src/: the symbols of the server they
# do not run link as stubs that trap (AUTOSTUB), so that a test that reaches one fails. col/f2/preagg_join links its source.
XFLAGS_BASE = -D_GNU_SOURCE -DSIFT_TESTING -Wno-unused-function

F2_TESTS = core/xid_cache core/writer_layout_chkpt col/f2/journal_flush core/f2/pace col/f2/delta_locate col/f2/scaling col/f2/tier_guard col/f2/tier_cache col/f2/tier_curl col/f2/shard_dir col/f2/shard_io col/f2/neighbours col/f2/rowbuf_grow col/f2/scan/stream_cap col/f2/del/pend_big table/f2/chain_cache_big table/f2/col_delta_compact table/f2/col_delta_merge table/f2/col_delta_batch table/f2/col_delta_sub_abort table/f2/rowlock_contend table/f2/lock/fast_intent table/f2/lock/touched table/f2/lock/esc table/f2/lock/ovf table/f2/vacuum_tids table/f2/col_delta_place table/f2/blob_spill table/f2/vacuum_xids table/f2/mvcc_any table/f2/gcache_unkept col/f2/track_cells table/f2/fetch_abort table/f2/fetch_subxip table/f2/lock/kept_end table/f2/row/pack_build col/f2/uniq/shm

TESTS    += col/f2/preagg_join $(F2_TESTS)
AUTOSTUB += $(F2_TESTS)
LIB_col/f2/preagg_join = col/f2/preagg/v2_read.c
$(foreach t,$(F2_TESTS),$(eval XFLAGS_$(t) = $(XFLAGS_BASE)))
XFLAGS_col/f2/tier_cache += -Wno-format-truncation
LIB_col/f2/rowbuf_grow = col/f2/batch.c
LIB_table/f2/lock/kept_end = table/f2/lock/ovf.c
