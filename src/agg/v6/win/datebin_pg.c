/*
 * agg/v6/win/datebin_pg.c - the PostgreSQL side of v6_datebin_win.h.
 */
#include "agg/v6/win/datebin_pg.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/numeric.h"

void
sift_datebin_raise(SiftDateBinStatus s)
{
	int			code;

	switch (s)
	{
		case SDB_OK:
			return;
		case SDB_STRIDE_MONTHS:
			code = ERRCODE_FEATURE_NOT_SUPPORTED;
			break;
		default:
			code = ERRCODE_DATETIME_VALUE_OUT_OF_RANGE;
			break;
	}
	ereport(ERROR, (errcode(code), errmsg("%s", sift_datebin_message(s))));
}

Datum
sift_numavg_datum(const SiftNumAvg *a)
{
	char		txt[1100];

	SIFT_INVARIANT(a->ok && sift_numavg_text(a, txt, sizeof(txt)) > 0, "numeric average: no value to convert");
	return DirectFunctionCall3(numeric_in, CStringGetDatum(txt), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}
