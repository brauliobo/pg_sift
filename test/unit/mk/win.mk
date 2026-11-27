# The bounds of the window executors at any work_mem: the ring of agg/window.c, the pending rows of agg/win/datebin.c and the carry of agg/win/carry.c stay within one palloc and spill past it.
TESTS    += agg/window agg/win/datebin agg/win/carry
AUTOSTUB += agg/window agg/win/datebin agg/win/carry
XFLAGS_agg/window      = -Wno-unused-function
XFLAGS_agg/win/datebin = -Wno-unused-function
XFLAGS_agg/win/carry   = -Wno-unused-function
