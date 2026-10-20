/*
 * col/types.c - where the types of PostgreSQL meet the leaf types of the column engine (docs/COL.md 1, docs/COL-LEAFTYPES-32.md): which
 * leaves an index attribute becomes, the type tag of an OID and back, the values a datum yields, and the constants of predicates on the
 * wide types. The format side (lane keys, canonical images) is sift_col_wide.[ch], portable.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/cash.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/varbit.h"
#include "utils/timestamp.h"
#include "utils/date.h"
#include "utils/uuid.h"
#include "utils/rangetypes.h"

#include "col/am.h"

/* ---------- type tags ---------- */

/* The storage class of a type the table AM holds as one leaf of a single lane (the index adds the wide types in sift_col_att_leaves) */
bool
sift_col_leaf_store(Oid typid, SiftColStore *store)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case BOOLOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
			*store = SIFT_COLS_INT;
			return true;
		case FLOAT4OID:
			*store = SIFT_COLS_F4;
			return true;
		case FLOAT8OID:
			*store = SIFT_COLS_F8;
			return true;
		case TEXTOID:
		case VARCHAROID:
			*store = SIFT_COLS_STR;
			return true;
	}
	return false;
}

/* The format's tag of a PostgreSQL type, and the type a tag stands for: the one place the engine's logical types meet type OIDs */
uint8
sift_col_type_of_oid(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
			return SIFT_COLT_BOOL;
		case INT2OID:
			return SIFT_COLT_INT2;
		case INT4OID:
			return SIFT_COLT_INT4;
		case INT8OID:
			return SIFT_COLT_INT8;
		case FLOAT4OID:
			return SIFT_COLT_FLOAT4;
		case FLOAT8OID:
			return SIFT_COLT_FLOAT8;
		case NUMERICOID:
			return SIFT_COLT_NUMERIC;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case NAMEOID:
			return SIFT_COLT_TEXT;
		case BYTEAOID:
			return SIFT_COLT_BYTEA;
		case DATEOID:
			return SIFT_COLT_DATE;
		case TIMESTAMPOID:
			return SIFT_COLT_TIMESTAMP;
		case TIMESTAMPTZOID:
			return SIFT_COLT_TIMESTAMPTZ;
		case UUIDOID:
			return SIFT_COLT_UUID;
		case JSONBOID:
			return SIFT_COLT_JSONB;
		case CASHOID:
			return SIFT_COLT_MONEY;
		case INTERVALOID:
			return SIFT_COLT_INTERVAL;
		case TIMEOID:
			return SIFT_COLT_TIME;
		case TIMETZOID:
			return SIFT_COLT_TIMETZ;
		case MACADDROID:
			return SIFT_COLT_MACADDR;
		case MACADDR8OID:
			return SIFT_COLT_MACADDR8;
		case INETOID:
		case CIDROID:
		case BITOID:
		case VARBITOID:
			return SIFT_COLT_VARLENA;
		default:
			if (type_is_enum(typid))
				return SIFT_COLT_ENUM;
			return OidIsValid(get_element_type(typid)) || type_is_range(typid) || type_is_multirange(typid) ? SIFT_COLT_VARLENA : SIFT_COLT_OTHER;
	}
}

static const Oid type_oids[SIFT_COLT_END] = {
	[SIFT_COLT_BOOL] = BOOLOID, [SIFT_COLT_INT2] = INT2OID, [SIFT_COLT_INT4] = INT4OID, [SIFT_COLT_INT8] = INT8OID,
	[SIFT_COLT_FLOAT4] = FLOAT4OID, [SIFT_COLT_FLOAT8] = FLOAT8OID, [SIFT_COLT_NUMERIC] = NUMERICOID, [SIFT_COLT_TEXT] = TEXTOID,
	[SIFT_COLT_BYTEA] = BYTEAOID, [SIFT_COLT_DATE] = DATEOID, [SIFT_COLT_TIMESTAMP] = TIMESTAMPOID, [SIFT_COLT_TIMESTAMPTZ] = TIMESTAMPTZOID,
	[SIFT_COLT_UUID] = UUIDOID, [SIFT_COLT_JSONB] = JSONBOID, [SIFT_COLT_MONEY] = CASHOID, [SIFT_COLT_INTERVAL] = INTERVALOID,
	[SIFT_COLT_TIME] = TIMEOID, [SIFT_COLT_TIMETZ] = TIMETZOID, [SIFT_COLT_MACADDR] = MACADDROID, [SIFT_COLT_MACADDR8] = MACADDR8OID
};

/* A leaf of a plain attribute yields exactly the type of the attribute in desc (varchar stays varchar); any other one the tag's type */
Oid
sift_col_def_typid(TupleDesc desc, const SiftColDef *def)
{
	if (def->attno > 0 && def->path[0] == '\0')
		return getBaseType(TupleDescAttr(desc, def->attno - 1)->atttypid);
	return type_oids[def->type];
}

/* ---------- the leaves of an attribute ---------- */

/*
 * numeric(p, s) has three forms: p <= 18 and 0 <= s <= 18 a scaled int64 (one INT leaf, the 3.1 form, and the one sift_table keeps); p <= 38 and
 * 0 <= s <= 38 otherwise a scaled 128-bit integer (two INT lanes, at most p digits so it fits); anything else (no typmod, more digits, a negative
 * scale, a scale above 38) the canonical image of the number in a STR head with the display scale as its INT lane.
 */
typedef enum NumericForm
{
	NUMERIC_NARROW,
	NUMERIC_WIDE,
	NUMERIC_IMAGE
} NumericForm;

static NumericForm
numeric_form(Form_pg_attribute att, uint8 *scale)
{
	int32		tm = att->atttypmod - VARHDRSZ;
	int			prec = (tm >> 16) & 0xFFFF,
				s = ((tm & 0x7FF) ^ 1024) - 1024;

	*scale = 0;
	if (att->atttypmod < VARHDRSZ || s < 0 || s > SIFT_COL_MAX_WIDE_SCALE || prec > SIFT_COL_MAX_WIDE_SCALE)
		return NUMERIC_IMAGE;
	*scale = s;
	return prec <= SIFT_COL_MAX_SCALE && s <= SIFT_COL_MAX_SCALE ? NUMERIC_NARROW : NUMERIC_WIDE;
}

/* The scale of numeric(p, s) when it is one INT leaf of the 3.1 form (p <= 18, 0 <= s <= 18) */
bool
sift_col_numeric_scale(Form_pg_attribute att, uint8 *scale)
{
	uint8		s;

	if (numeric_form(att, &s) != NUMERIC_NARROW)
		return false;
	*scale = s;
	return true;
}

/* The types of a sift_col index: the table AM's set plus char(n) and name, which are strings too (the table AM keeps its narrower set) */
static bool
col_leaf_store(Oid typid, SiftColStore *store)
{
	if (typid != BPCHAROID && typid != NAMEOID)
		return sift_col_leaf_store(typid, store);
	*store = SIFT_COLS_STR;
	return true;
}

/* A head and its lanes: a lane after the head is an INT leaf named "<attribute>#k" (a path no shredder produces) */
static int
make_lanes(Form_pg_attribute att, SiftColDef *defs, int lanes)
{
	for (int k = 0; k < lanes; k++)
	{
		SiftColDef *d = &defs[k];

		d->parent = -1;
		d->kind = SIFT_COLK_SCALAR;
		d->type = defs[0].type;
		d->scale = defs[0].scale;
		d->lanes = k == 0 && lanes > 1 ? lanes : 0;
		d->lane = k;
		d->path = k == 0 ? "" : psprintf("%s#%d", NameStr(att->attname), k);
		if (k > 0)
			d->store = SIFT_COLS_INT;
	}
	return lanes;
}

int
sift_col_att_leaves(Form_pg_attribute att, SiftColDef *defs)
{
	Oid			typid = att->atttypid;
	SiftColStore store = SIFT_COLS_INT;

	memset(defs, 0, SIFT_COL_MAX_LANES * sizeof(SiftColDef));
	defs[0].type = sift_col_type_of_oid(typid);
	defs[0].store = SIFT_COLS_INT;
	switch (typid)
	{
		case CASHOID:
			return make_lanes(att, defs, 1);
		case INTERVALOID:
			return make_lanes(att, defs, 3);
		case TIMEOID:
			return make_lanes(att, defs, 1);
		case NUMERICOID:
			switch (numeric_form(att, &defs[0].scale))
			{
				case NUMERIC_NARROW:
					return make_lanes(att, defs, 1);
				case NUMERIC_WIDE:
					return make_lanes(att, defs, 2);
				case NUMERIC_IMAGE:
					defs[0].store = SIFT_COLS_STR;
					return make_lanes(att, defs, 2);
			}
			break;
		default:
			if (col_leaf_store(typid, &store))
			{
				defs[0].store = store;
				defs[0].flags = typid == BPCHAROID && att->atttypmod < 0 ? SIFT_COLD_LOSSY : 0;
				return make_lanes(att, defs, 1);
			}
			if (defs[0].type == SIFT_COLT_ENUM)
				return make_lanes(att, defs, 1);
			if (defs[0].type == SIFT_COLT_UUID || defs[0].type == SIFT_COLT_BYTEA || sift_col_def_is_image(&defs[0]))
			{
				defs[0].store = SIFT_COLS_STR;
				return make_lanes(att, defs, 1);
			}
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift_col does not support the type %s of column \"%s\"", format_type_be(typid), NameStr(att->attname)),
			 errhint("Supported: smallint, integer, bigint, boolean, date, time, timetz, timestamp, timestamptz, interval, numeric, money, real, double precision, text, varchar, char(n), name, bytea, uuid, inet, cidr, macaddr, macaddr8, bit, varbit, enums, arrays, ranges and jsonb with paths.")));
	return 0;
}

/* ---------- numeric ---------- */

/* 10^scale as a numeric, for the lifetime of the backend */
static Datum
numeric_pow10(int scale)
{
	static Datum pow10[SIFT_COL_MAX_WIDE_SCALE + 1];

	if (pow10[scale] == (Datum) 0)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
		char		txt[SIFT_COL_MAX_WIDE_SCALE + 2] = "1";

		memset(txt + 1, '0', scale);
		txt[scale + 1] = '\0';
		pow10[scale] = DirectFunctionCall3(numeric_in, CStringGetDatum(txt), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
		MemoryContextSwitchTo(old);
	}
	return pow10[scale];
}

/* value times 10^scale, made an integer as round asks */
static Datum
numeric_scaled(int scale, Datum value, SiftColRound round)
{
	Datum		scaled = DirectFunctionCall2(numeric_mul, value, numeric_pow10(scale));

	if (round == SIFT_COL_ROUND_DOWN)
		return DirectFunctionCall1(numeric_floor, scaled);
	if (round == SIFT_COL_ROUND_UP)
		return DirectFunctionCall1(numeric_ceil, scaled);
	return DirectFunctionCall2(numeric_round, scaled, Int32GetDatum(0));
}

int64
sift_col_numeric_key(int scale, Datum value, SiftColRound round)
{
	return DatumGetInt64(DirectFunctionCall1(numeric_int8, numeric_scaled(scale, value, round)));
}

Datum
sift_col_key_numeric(int scale, int64 key)
{
	return NumericGetDatum(int64_div_fast_to_numeric(key, scale));
}

int
sift_col_numeric_wide_key(int scale, Datum value, SiftColRound round, int64 *hi, int64 *lo)
{
	char	   *txt = DatumGetCString(DirectFunctionCall1(numeric_out, numeric_scaled(scale, value, round)));
	sift_w128	v = 0;
	SiftW128Status st = sift_col_decimal_to_w128(txt, 0, &v);

	pfree(txt);
	if (st == SIFT_W128_ABOVE)
		v = (sift_w128) (((sift_wu128) 1 << 127) - 1);
	else if (st == SIFT_W128_BELOW)
		v = (sift_w128) ((sift_wu128) 1 << 127);
	else if (st != SIFT_W128_OK)
		return 2;				/* NaN or an infinity: not a number of the scaled domain */
	*hi = sift_col_wide_hi(v);
	*lo = sift_col_wide_lo(v);
	return st == SIFT_W128_ABOVE ? 1 : st == SIFT_W128_BELOW ? -1 : 0;
}

Datum
sift_col_wide_numeric(int scale, int64 hi, int64 lo)
{
	char		txt[SIFT_COL_W128_TEXT];

	sift_col_w128_to_decimal(sift_col_wide_join(hi, lo), scale, txt);
	return DirectFunctionCall3(numeric_in, CStringGetDatum(txt), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

uint8 *
sift_col_numeric_image(Datum value, uint32 *len, int32 *dscale)
{
	char	   *txt = DatumGetCString(DirectFunctionCall1(numeric_out, value));
	uint8	   *img = palloc(SIFT_NIMG_MAXLEN(strlen(txt)));

	*len = sift_col_numimg_from_text(txt, img, dscale);
	if (*len == 0)
		elog(ERROR, "sift_col: no image for the numeric \"%.40s\"", txt);
	pfree(txt);
	return img;
}

Datum
sift_col_image_numeric(const uint8 *img, uint32 len, int32 dscale)
{
	char	   *txt = palloc(SIFT_NIMG_TEXT_MAX);

	if (sift_col_numimg_to_text(img, len, dscale, txt, SIFT_NIMG_TEXT_MAX) == 0)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_col: a numeric image of %u bytes is damaged", len)));
	return DirectFunctionCall3(numeric_in, CStringGetDatum(txt), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

/* ---------- interval ---------- */

void
sift_col_interval_lanes(const Interval *iv, int64 *hi, int64 *lo, int64 *md)
{
	sift_w128	total = sift_col_interval_total(iv->time, iv->day, iv->month);

	*hi = sift_col_wide_hi(total);
	*lo = sift_col_wide_lo(total);
	*md = sift_col_interval_md(iv->day, iv->month);
}

Interval *
sift_col_lanes_interval(int64 hi, int64 lo, int64 md)
{
	Interval   *iv = palloc(sizeof(Interval));

	sift_col_interval_raw(sift_col_wide_join(hi, lo), md, &iv->time, &iv->day, &iv->month);
	return iv;
}

/* ---------- datum to values ---------- */

/* Every lane of one datum: v[0 .. max(lanes, 1)) are the values of the head and of the leaves after it */
void
sift_col_datum_values(const SiftColDef *def, Datum d, bool name, SiftColValue *v)
{
	int			lanes = Max(def->lanes, 1);

	for (int k = 0; k < lanes; k++)
		v[k] = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
	switch (def->type)
	{
		case SIFT_COLT_INT2:
			v->v.i = DatumGetInt16(d);
			break;
		case SIFT_COLT_INT4:
		case SIFT_COLT_DATE:
			v->v.i = DatumGetInt32(d);
			break;
		case SIFT_COLT_INT8:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
		case SIFT_COLT_MONEY:
			v->v.i = DatumGetInt64(d);
			break;
		case SIFT_COLT_BOOL:
			v->v.i = DatumGetBool(d);
			break;
		case SIFT_COLT_TIME:
			v->v.i = DatumGetTimeADT(d);
			break;
		case SIFT_COLT_ENUM:
			v->v.i = (int32) DatumGetObjectId(d);
			break;
		case SIFT_COLT_UUID:
			v->v.s = (const char *) DatumGetUUIDP(d);
			v->len = UUID_LEN;
			break;
		case SIFT_COLT_MACADDR:
			v->v.s = (const char *) DatumGetMacaddrP(d);
			v->len = sizeof(macaddr);
			break;
		case SIFT_COLT_MACADDR8:
			v->v.s = (const char *) DatumGetMacaddr8P(d);
			v->len = sizeof(macaddr8);
			break;
		case SIFT_COLT_TIMETZ:
			v->v.s = (const char *) DatumGetTimeTzADTP(d);
			v->len = 12;		/* typlen: the struct has 4 bytes of padding that no value owns */
			break;
		case SIFT_COLT_BYTEA:
		case SIFT_COLT_VARLENA:
			{
				struct varlena *image = pg_detoast_datum((struct varlena *) DatumGetPointer(d));

				v->v.s = (const char *) image;
				v->len = VARSIZE(image);
			}
			break;
		case SIFT_COLT_NUMERIC:
			if (def->store == SIFT_COLS_STR)
			{
				int32		dscale;

				v[0].v.s = (const char *) sift_col_numeric_image(d, &v[0].len, &dscale);
				v[1].v.i = dscale;
			}
			else if (numeric_is_nan(DatumGetNumeric(d)))
				for (int k = 0; k < lanes; k++)
					v[k].isnull = v[k].overflow = true;
			else if (lanes == 1)
				v->v.i = sift_col_numeric_key(def->scale, d, SIFT_COL_ROUND_NEAREST);
			else
			{
				char	   *txt = DatumGetCString(DirectFunctionCall1(numeric_out, d));
				sift_w128	w = 0;

				if (sift_col_decimal_to_w128(txt, def->scale, &w) != SIFT_W128_OK)
					elog(ERROR, "sift_col: the numeric %s does not fit a 128-bit integer scaled by 10^%d", txt, def->scale);
				pfree(txt);
				v[0].v.i = sift_col_wide_hi(w);
				v[1].v.i = sift_col_wide_lo(w);
			}
			break;
		case SIFT_COLT_INTERVAL:
			sift_col_interval_lanes(DatumGetIntervalP(d), &v[0].v.i, &v[1].v.i, &v[2].v.i);
			break;
		case SIFT_COLT_FLOAT4:
			v->v.f = DatumGetFloat4(d);
			break;
		case SIFT_COLT_FLOAT8:
			v->v.f = DatumGetFloat8(d);
			break;
		default:
			if (name)
			{
				v->v.s = NameStr(*DatumGetName(d));
				v->len = strlen(v->v.s);
			}
			else
			{
				text	   *t = DatumGetTextPP(d);

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
			}
	}
}
