/*
 * agg/cube.c - the statements a pre-aggregated cube can answer (v6_cube.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"

#include "agg/cube.h"

static bool
agg_cubeable(const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);

	if (a->fclause != 0 || a->nxcols != 0 || a->arg.derive > 0 || a->arg.src != SV6SRC_ATTR || a->arg.jleaf > 0)
		return false;
	switch (a->kind)
	{
		case SV6_COUNT_STAR:
			return true;
		case SV6_COUNT:
			return a->arg.attno > 0;
		case SV6_SUM:
		case SV6_AVG:
		case SV6_MIN:
		case SV6_MAX:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64;
		case SV6_COUNT_DISTINCT:
			return (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_TEXT) && a->arg.typid != NUMERICOID;
		default:
			return false;
	}
}

bool
sift_v6_spec_cubeable(const SiftV6Spec *spec)
{
	if (spec->nsets != 0 || spec->nbins != 0 || spec->nderive != 0 || spec->nfilters != 0 || spec->nfuncs != 0 || spec->ext != NULL || spec->text_queries != NIL || spec->jsonqual != NIL ||
		spec->vqual != NIL || spec->pgexpr != NIL || spec->genref != NIL || spec->constant_groups || spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1)
		return false;
	for (int i = 0; i < spec->nkeys; i++)
	{
		const SiftV6Col *k = &spec->key[i];
		const SiftV6Class cls = sift_v6_class(k->typid);

		if (k->src != SV6SRC_ATTR || k->jleaf > 0 || k->derive > 0 || (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_BOOL && cls != SV6C_TEXT) || k->typid == NUMERICOID)
			return false;
	}
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_cubeable(&spec->agg[j]))
			return false;
	return true;
}
