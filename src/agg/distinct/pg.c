/*
 * agg/distinct/pg.c - the datums of a COUNT(DISTINCT) row as key bytes (v6_distinct_pg.h).
 */
#include "agg/distinct/pg.h"

#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "utils/uuid.h"
#include "utils/varlena.h"

bool
sift_distinct_type_ok(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case DATEOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case BYTEAOID:
		case UUIDOID:
		case NUMERICOID:
			return true;
		default:
			return false;
	}
}

bool
sift_distinct_collation_ok(Oid collid)
{
	return !OidIsValid(collid) || sift_collation_deterministic(collid);
}

static void
key_varlena(SiftDistinctKey *k, Datum d, bool trim_blanks)
{
	struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
	char	   *p = VARDATA_ANY(v);
	int			len = (int) VARSIZE_ANY_EXHDR(v);

	if (trim_blanks)
		len = bpchartruelen(p, len);
	sift_dkey_bytes(k, p, (uint32) len);
	if ((Pointer) v != DatumGetPointer(d))
		pfree(v);
}

static void
key_numeric(SiftDistinctKey *k, Datum d)
{
	char	   *txt = numeric_normalize(DatumGetNumeric(d));

	sift_dkey_bytes(k, txt, (uint32) strlen(txt));
	pfree(txt);
}

void
sift_distinct_row_key(SiftDistinctKey *k, int ncols, const Oid *typids, const Datum *values, const bool *nulls)
{
	sift_dkey_reset(k);
	for (int c = 0; c < ncols; c++)
	{
		Datum		d = values[c];

		if (nulls[c])
		{
			sift_dkey_null(k);
			continue;
		}
		switch (typids[c])
		{
			case BOOLOID:
				sift_dkey_i64(k, DatumGetBool(d));
				break;
			case INT2OID:
				sift_dkey_i64(k, DatumGetInt16(d));
				break;
			case INT4OID:
			case DATEOID:
				sift_dkey_i64(k, DatumGetInt32(d));
				break;
			case OIDOID:
				sift_dkey_i64(k, (int64) DatumGetObjectId(d));
				break;
			case INT8OID:
			case TIMEOID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				sift_dkey_i64(k, DatumGetInt64(d));
				break;
			case FLOAT4OID:
				sift_dkey_f64(k, (double) DatumGetFloat4(d));
				break;
			case FLOAT8OID:
				sift_dkey_f64(k, DatumGetFloat8(d));
				break;
			case TEXTOID:
			case VARCHAROID:
			case BYTEAOID:
				key_varlena(k, d, false);
				break;
			case BPCHAROID:
				key_varlena(k, d, true);
				break;
			case UUIDOID:
				sift_dkey_bytes(k, DatumGetUUIDP(d)->data, UUID_LEN);
				break;
			case NUMERICOID:
				key_numeric(k, d);
				break;
			default:
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("count distinct: no exact key for type %u", typids[c]),
								errhint("Plan the query without SiftAggV6 (SET sift.enable_agg = off).")));
		}
	}
}
