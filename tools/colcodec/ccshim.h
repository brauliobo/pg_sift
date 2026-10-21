/* The dispatch of the chunk codec, shared by the standalone test and benchmark (the backend symbols are tools/pgshim.c). */
#ifndef CC_CCSHIM_H
#define CC_CCSHIM_H

#include "col/codec.h"

#include "../pgshim.h"

/* The cpu module stand-in and the dispatch tables of the codec and of the column engine it runs on */
extern void cc_init(void);
/* The distinct SIMD levels this CPU runs, ascending (the first is scalar); returns their number */
extern int	cc_levels(SiftSimdLevel lv[3]);
/* Makes the tables of a level the active ones (sift_cck and sift_colk) */
extern void cc_use_level(SiftSimdLevel level);

#endif
