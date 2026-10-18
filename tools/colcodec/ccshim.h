/* The error capture of shim.c, shared by the standalone test and benchmark. */
#ifndef CC_CCSHIM_H
#define CC_CCSHIM_H

#include "sift_colcodec.h"

extern int	cc_sqlstate;
extern char cc_errmsg[256];

/* Runs fn(arg): true when it returned, false when it raised an error (cc_sqlstate and cc_errmsg tell which) */
extern bool cc_try(void (*fn) (void *), void *arg);

extern void cc_arena_reset(void);

/* The cpu module stand-in and the dispatch tables of the codec and of the column engine it runs on */
extern void cc_init(void);
/* The distinct SIMD levels this CPU runs, ascending (the first is scalar); returns their number */
extern int	cc_levels(SiftSimdLevel lv[3]);
/* Makes the tables of a level the active ones (sift_cck and sift_colk) */
extern void cc_use_level(SiftSimdLevel level);

#endif
