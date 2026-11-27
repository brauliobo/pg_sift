# The Top-N tournament of agg/topn/tourn.c: the rows that tie theta WITH TIES stay within the bytes the tournament is given and within one allocation.
TESTS    += agg/topn/tourn
AUTOSTUB += agg/topn/tourn
XFLAGS_agg/topn/tourn = -Wno-unused-function
