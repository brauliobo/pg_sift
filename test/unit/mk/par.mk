# The engine threads: the pool, the arena and the operators of src/agg/v6 that run on pool threads, on the host stand-in of agg/v6/par/host.c.
POOL_LIB     = core/par/arena.c core/par/morsel.c core/par/pool.c core/par/radix.c core/par/topk.c
HOST         = $(B)/agg/v6/par/host.o
GROUP_LIB    = agg/v6/par/group.c agg/v6/par/group_key.c agg/v6/v6_group_cmp.c agg/v6/v6_distinct_u64.c agg/kern6_hash.c
DISTINCT_LIB = agg/v6/par/distinct.c agg/v6/par/distinct_p2.c agg/v6/par/keytab.c agg/v6/v6_distinct_u64.c agg/kern6_hash.c
SINK_LIB     = $(POOL_LIB) agg/v6/par/wordsink.c $(GROUP_LIB) agg/v6/v6_pipe.c agg/v6/v6_star_exec.c agg/v6/v6_gather.c
WINDOW_LIB   = agg/v6/v6_datebin_win.c agg/v6/v6_tseries_fast.c agg/v6/v6_winx_var.c

TESTS   += core/par/arena core/par/pool core/par/ring agg/v6/inset agg/v6/gather agg/v6/par/rows agg/v6/par/radix agg/v6/par/distinct \
           agg/v6/par/derive agg/v6/par/tseries agg/v6/par/star agg/v6/par/unnest agg/v6/par/win
BENCHES += core/par/arena_bench agg/v6/par/radix_bench

LIB_core/par/arena        = core/par/arena.c col/compress.c
LIB_core/par/arena_bench  = core/par/arena.c
LIB_core/par/pool         = $(POOL_LIB)
FX_core/par/pool          = $(HOST)
LIB_core/par/ring         = $(POOL_LIB)
FX_core/par/ring          = $(HOST)
LIB_agg/v6/inset          = agg/v6/v6_inset_filter.c
LIB_agg/v6/gather         = agg/v6/v6_gather.c
LIB_agg/v6/par/rows       = $(POOL_LIB) agg/v6/par/rows.c $(GROUP_LIB)
FX_agg/v6/par/rows        = $(HOST)
LIB_agg/v6/par/radix      = $(POOL_LIB) agg/v6/par/group.c agg/v6/par/group_key.c $(DISTINCT_LIB) agg/v6/v6_group_cmp.c
FX_agg/v6/par/radix       = $(HOST)
LIB_agg/v6/par/radix_bench = $(LIB_agg/v6/par/radix)
FX_agg/v6/par/radix_bench = $(HOST)
LIB_agg/v6/par/distinct   = $(POOL_LIB) $(DISTINCT_LIB)
FX_agg/v6/par/distinct    = $(HOST)
LIB_agg/v6/par/derive     = agg/v6/par/derive.c agg/v6/v6_expr_str.c
LIB_agg/v6/par/tseries    = $(POOL_LIB) agg/v6/par/tseries.c $(GROUP_LIB) $(WINDOW_LIB)
FX_agg/v6/par/tseries     = $(HOST)
LIB_agg/v6/par/star       = $(SINK_LIB) agg/v6/par/star.c
FX_agg/v6/par/star        = $(HOST) $(B)/agg/v6/par/pipe_stubs.o
LIB_agg/v6/par/unnest     = $(SINK_LIB) agg/v6/par/unnest.c agg/v6/v6_unnest.c agg/v6/v6_unnest_pairs.c
FX_agg/v6/par/unnest      = $(HOST) $(B)/agg/v6/par/pipe_stubs.o
LIB_agg/v6/par/win        = core/par/arena.c agg/v6/par/winpart.c $(WINDOW_LIB)
FX_agg/v6/par/win         = $(HOST)
