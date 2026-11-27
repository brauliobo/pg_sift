# The skip index of a part (col/skip/build.c), included by its test; the symbols of the server it does not run link as stubs that trap (AUTOSTUB).
TESTS    += col/skip/build
AUTOSTUB += col/skip/build
XFLAGS_col/skip/build = -D_GNU_SOURCE -Wno-unused-function
LIB_col/skip/build    = core/sbbf.c col/f2/aux_budget.c
FX_col/skip/build     = $(B)/cpushim.o
