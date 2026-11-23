/*
 * agg/v6/plan_kind.c - which aggregate of a query the V6 engine runs, and over which types
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "nodes/pathnodes.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_argext.h"
#include "agg/v6/v6_argext_sql.h"

bool
sift_v6_agg_kind(const Aggref *a, SiftV6Kind *kind)
{
	const char *name = get_func_name(a->aggfnoid);
	const bool	catalog = get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE;
	const int	argdir = sift_argext_aggref_dir(a);

	if (argdir >= 0)
	{
		*kind = argdir == SIFT_ARG_MAX ? SV6_ARG_MAX : SV6_ARG_MIN;
		return a->aggfilter == NULL;
	}
	if (!catalog || name == NULL || a->aggfilter != NULL || a->aggorder != NIL || a->aggkind != AGGKIND_NORMAL || a->aggvariadic)
		return false;
	if (strcmp(name, "count") == 0)
		*kind = a->aggstar ? SV6_COUNT_STAR : a->aggdistinct != NIL ? SV6_COUNT_DISTINCT : SV6_COUNT;
	else if (a->aggdistinct != NIL)
		return false;
	else if (strcmp(name, "sum") == 0)
		*kind = SV6_SUM;
	else if (strcmp(name, "avg") == 0)
		*kind = SV6_AVG;
	else if (strcmp(name, "min") == 0)
		*kind = SV6_MIN;
	else if (strcmp(name, "max") == 0)
		*kind = SV6_MAX;
	else if (list_length(a->aggargtypes) != 1 || !sift_v6_ext_kind(name, linitial_oid(a->aggargtypes), kind))
		return false;
	return true;
}

bool
sift_v6_kind_takes(SiftV6Kind kind, Oid typid)
{
	const SiftV6Class cls = sift_v6_class(typid);

	switch (kind)
	{
		case SV6_COUNT:
			return cls != SV6C_NONE;
		case SV6_COUNT_DISTINCT:
			return cls != SV6C_NONE;
		case SV6_SUM:
			return typid != FLOAT4OID && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64);
		case SV6_AVG:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64;
		case SV6_MIN:
		case SV6_MAX:
			return cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64 || cls == SV6C_TEXT;
		case SV6_STDDEV_POP:
		case SV6_STDDEV_SAMP:
		case SV6_VAR_POP:
		case SV6_VAR_SAMP:
			return cls == SV6C_F64;
		case SV6_BOOL_OR:
		case SV6_BOOL_AND:
			return cls == SV6C_BOOL;
		case SV6_PCT_CONT:
			return typid == FLOAT8OID;
		case SV6_ARG_MAX:
		case SV6_ARG_MIN:
			return cls == SV6C_I32 || cls == SV6C_I64;
		default:
			return true;
	}
}
