# The limits of the operators of src/agg at one allocation: a hash join row, a parallel record of a set of no keys.
TESTS    += agg/join/grace agg/par/group_fold
AUTOSTUB += agg/join/grace agg/par/group_fold
XFLAGS_agg/join/grace     = -Wno-unused-function
XFLAGS_agg/par/group_fold = -Wno-unused-function
LIB_agg/par/group_fold  = agg/par/dist.c
