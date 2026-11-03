/*
 * bm25/analyze_ts_test.c - test-only text search template sift._test_syn, compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension: a dictionary that maps words to others (MAP = 'kater=katze,hunde=hund') and answers nothing for the rest, so a
 * chain in front of german_stem is exercised without a synonym file; FILTER = true marks its answers as input of the next dictionary.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "commands/defrem.h"
#include "fmgr.h"
#include "tsearch/ts_public.h"

PG_FUNCTION_INFO_V1(sift_test_syn_init);
PG_FUNCTION_INFO_V1(sift_test_syn_lexize);

typedef struct SynDict
{
	char	   *map;			/* "from=to,from=to" */
	bool		filter;
} SynDict;

Datum
sift_test_syn_init(PG_FUNCTION_ARGS)
{
	SynDict    *d = palloc0(sizeof(SynDict));

	foreach_ptr(DefElem, e, (List *) PG_GETARG_POINTER(0))
	{
		if (strcmp(e->defname, "map") == 0)
			d->map = defGetString(e);
		else if (strcmp(e->defname, "filter") == 0)
			d->filter = defGetBoolean(e);
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unrecognized synonym parameter: \"%s\"", e->defname)));
	}
	PG_RETURN_POINTER(d);
}

Datum
sift_test_syn_lexize(PG_FUNCTION_ARGS)
{
	SynDict    *d = (SynDict *) PG_GETARG_POINTER(0);
	const char *in = (const char *) PG_GETARG_POINTER(1);
	int			len = PG_GETARG_INT32(2);

	for (const char *s = d->map; s && *s;)
	{
		const char *eq = strchr(s, '='),
				   *end = strchr(s, ',');

		end = end ? end : s + strlen(s);
		if (eq && eq - s == len && pg_strncasecmp(s, in, len) == 0)
		{
			TSLexeme   *res = palloc0(2 * sizeof(TSLexeme));

			res[0].lexeme = pnstrdup(eq + 1, end - eq - 1);
			res[0].flags = d->filter ? TSL_FILTER : 0;
			PG_RETURN_POINTER(res);
		}
		s = *end ? end + 1 : end;
	}
	PG_RETURN_POINTER(NULL);
}

#endif							/* SIFT_TESTING */
