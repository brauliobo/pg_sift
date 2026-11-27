# The engine threads: the pool, the arena and the operators of src/agg that run on pool threads, on the host stand-in of agg/par/host.c.
POOL_LIB     = core/par/arena.c core/par/morsel.c core/par/pool.c core/par/radix.c core/par/topk.c
HOST         = $(B)/agg/par/host.o
GROUP_LIB    = agg/par/group.c agg/par/group_key.c agg/group/cmp.c agg/distinct/u64.c agg/kern/hash.c
DISTINCT_LIB = agg/par/distinct.c agg/par/distinct_p2.c agg/par/keytab.c agg/distinct/u64.c agg/kern/hash.c
SINK_LIB     = $(POOL_LIB) agg/par/wordsink.c $(GROUP_LIB) agg/pipe.c agg/star/exec.c agg/gather.c
WINDOW_LIB   = agg/win/datebin.c agg/tseries_fast.c agg/win/x_var.c

TESTS   += core/par/arena core/par/pool core/par/ring agg/inset agg/gather agg/jsonb_order agg/par/rows agg/par/radix agg/par/distinct \
           agg/par/derive agg/par/tseries agg/par/star agg/par/unnest agg/par/win
BENCHES += core/par/arena_bench agg/par/radix_bench
AUTOSTUB += agg/jsonb_order
XFLAGS_agg/jsonb_order = -Wno-unused-function

LIB_core/par/arena        = core/par/arena.c col/compress.c
LIB_core/par/arena_bench  = core/par/arena.c
LIB_core/par/pool         = $(POOL_LIB)
FX_core/par/pool          = $(HOST)
LIB_core/par/ring         = $(POOL_LIB)
FX_core/par/ring          = $(HOST)
LIB_agg/inset          = agg/inset/filter.c
LIB_agg/gather         = agg/gather.c
LIB_agg/par/rows       = $(POOL_LIB) agg/par/rows.c $(GROUP_LIB)
FX_agg/par/rows        = $(HOST)
LIB_agg/par/radix      = $(POOL_LIB) agg/par/group.c agg/par/group_key.c $(DISTINCT_LIB) agg/group/cmp.c
FX_agg/par/radix       = $(HOST)
LIB_agg/par/radix_bench = $(LIB_agg/par/radix)
FX_agg/par/radix_bench = $(HOST)
LIB_agg/par/distinct   = $(POOL_LIB) $(DISTINCT_LIB)
FX_agg/par/distinct    = $(HOST)
LIB_agg/par/derive     = agg/par/derive.c agg/expr/str.c
LIB_agg/par/tseries    = $(POOL_LIB) agg/par/tseries.c $(GROUP_LIB) $(WINDOW_LIB)
FX_agg/par/tseries     = $(HOST)
LIB_agg/par/star       = $(SINK_LIB) agg/par/star.c
FX_agg/par/star        = $(HOST) $(B)/agg/par/pipe_stubs.o
LIB_agg/par/unnest     = $(SINK_LIB) agg/par/unnest.c agg/unnest/unnest.c agg/unnest/pairs.c
FX_agg/par/unnest      = $(HOST) $(B)/agg/par/pipe_stubs.o
LIB_agg/par/win        = core/par/arena.c agg/par/winpart.c $(WINDOW_LIB)
FX_agg/par/win         = $(HOST)

TESTS    += agg/group/topk
AUTOSTUB += agg/group/topk
LIB_agg/group/topk = agg/group/topk.c

TESTS    += agg/distinct/u64 agg/distinct/set
AUTOSTUB += agg/distinct/u64 agg/distinct/set
XFLAGS_agg/distinct/u64      = -Wno-unused-function
XFLAGS_agg/distinct/set      = -Wno-unused-function
