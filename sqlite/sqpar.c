#include "sqvtab.h"

int
sq_par_available(SqVtab *vt, SiftPlan *plan)
{
	return 0;
}

SqHit *
sq_par_rank(SqVtab *vt, SiftIndexState *state, SiftQuery **qs, int nqs, int64_t k, int flags, int64_t *nhits)
{
	return NULL;
}

int64_t
sq_par_count(SqVtab *vt, SiftIndexState *state, SiftQuery **qs, int nqs)
{
	return 0;
}
