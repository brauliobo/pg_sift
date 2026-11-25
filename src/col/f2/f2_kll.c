/*
 * col/f2/f2_kll.c - the sketches option, the build of SEC_KLL from the rows of a part, its read, and the compaction that adds sketches instead of rows. The sketch is col/kll.c.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "col/am.h"
#include "col/f2/f2_kll.h"
#include "col/f2/f2_store_schema.h"
#include "col/granule/part_int.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/rel.h"
#include "core/damage.h"

void
sift_f2_kll_validate(const char *value)
{
	uint32		n;
	const char *err;

	if (value == NULL)
		return;
	err = sift_kll_spec_parse(value, NULL, 0, &n);
	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid sketches option \"%s\": %s", value, err)));
}

bool
sift_f2_kll_type_ok(Oid type)
{
	switch (type)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

bool
sift_f2_kll_datum(Oid type, Datum d, double *out)
{
	switch (type)
	{
		case INT2OID:
			*out = (double) DatumGetInt16(d);
			return true;
		case INT4OID:
			*out = (double) DatumGetInt32(d);
			return true;
		case INT8OID:
			*out = (double) DatumGetInt64(d);
			return true;
		case FLOAT4OID:
			*out = (double) DatumGetFloat4(d);
			return isfinite(*out);
		case FLOAT8OID:
			*out = DatumGetFloat8(d);
			return isfinite(*out);
		case NUMERICOID:
			*out = DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, d));
			return isfinite(*out);
		case DATEOID:
			*out = (double) DatumGetDateADT(d);
			return !DATE_NOT_FINITE(DatumGetDateADT(d));
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			*out = (double) DatumGetTimestamp(d);
			return !TIMESTAMP_NOT_FINITE(DatumGetTimestamp(d));
		default:
			elog(ERROR, "sift: no quantile sketch of type %u", type);	/* check-errors: allow */
	}
	return false;
}

static int
find_column(TupleDesc desc, const SiftKllSpecCol *c)
{
	for (int i = 0; i < desc->natts; i++)
	{
		const char *name = NameStr(TupleDescAttr(desc, i)->attname);

		if (strlen(name) == c->len && strncmp(name, c->name, c->len) == 0)
			return i;
	}
	return -1;
}

static uint32
spec_columns(const char *sketches, SiftKllSpecCol *cols)
{
	uint32		n;
	const char *err = sift_kll_spec_parse(sketches, cols, SIFT_F2_KLL_MAX_COLS * 2, &n);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid sketches option \"%s\": %s", sketches, err)));
	if (n > SIFT_F2_KLL_MAX_COLS * 2)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sketches names %u columns, at most %d", n, SIFT_F2_KLL_MAX_COLS * 2),
						errhint("Name fewer columns in the sketches option.")));
	return n;
}

/* A numeric is a sketch's value when the index keeps it as one scaled int64 (numeric(p, s) of at most 18 digits): another one is the image of its digits, which is no number to add */
static bool
numeric_sketchable(Form_pg_attribute att)
{
	uint8		scale;

	return att->atttypid != NUMERICOID || (sift_col_numeric_scale(att, &scale) && sift_f2_attr_num64(att, &scale));
}

void
sift_f2_kll_columns(Relation index, const SiftColRel *r, const char *sketches, SiftF2KllCols *out)
{
	SiftKllSpecCol cols[SIFT_F2_KLL_MAX_COLS * 2];
	TupleDesc	desc = RelationGetDescr(index);
	uint32		n = spec_columns(sketches, cols);

	out->n = 0;
	for (uint32 i = 0; i < n; i++)
	{
		int			att;
		int			leaf;
		int			at;

		if (!cols[i].kll)
			continue;
		att = find_column(desc, &cols[i]);
		if (att < 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("sketches names column \"%.*s\", which index \"%s\" does not have", (int) cols[i].len, cols[i].name, RelationGetRelationName(index))));
		if (!sift_f2_kll_type_ok(TupleDescAttr(desc, att)->atttypid))
			ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%.*s\" has a type of no quantile sketch", (int) cols[i].len, cols[i].name),
							errhint("kll sketches int2, int4, int8, float4, float8, numeric, date, timestamp and timestamptz columns.")));
		if (!numeric_sketchable(TupleDescAttr(desc, att)))
			ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%.*s\" has a numeric type that has no quantile sketch", (int) cols[i].len, cols[i].name),
							errhint("kll sketches numeric(p, s) columns of at most 18 digits; sift.approx_percentile answers the others exactly.")));
		leaf = r->first[att + 1];
		for (at = out->n++; at > 0 && out->col[at - 1] > leaf; at--)
		{
			out->col[at] = out->col[at - 1];
			out->k[at] = out->k[at - 1];
		}
		out->col[at] = (uint16) leaf;
		out->k[at] = cols[i].k;
	}
}

char *
sift_f2_kll_hll_columns(const char *sketches)
{
	SiftKllSpecCol cols[SIFT_F2_KLL_MAX_COLS * 2];
	uint32		n = spec_columns(sketches, cols);
	StringInfoData names;

	initStringInfo(&names);
	for (uint32 i = 0; i < n; i++)
		if (!cols[i].kll)
			appendStringInfo(&names, "%s%.*s", names.len > 0 ? ", " : "", (int) cols[i].len, cols[i].name);
	return names.data;
}

SiftF2KllBuild *
sift_f2_kll_begin(MemoryContext cxt, const SiftF2KllCols *cols, uint64 part_gen)
{
	SiftF2KllBuild *b = MemoryContextAlloc(cxt, sizeof(SiftF2KllBuild));

	b->cxt = cxt;
	b->n = cols->n;
	b->col = cols->col;
	b->sk = MemoryContextAlloc(cxt, Max(cols->n, 1) * sizeof(SiftKll));
	for (int i = 0; i < cols->n; i++)
	{
		bool		ok = sift_kll_init_seeded(&b->sk[i], cols->k[i], part_gen * UINT64CONST(1000003) + cols->col[i], cxt);

		SIFT_INVARIANT(ok, "sift: sketch parameter k out of range");
	}
	return b;
}

void
sift_f2_kll_free(SiftF2KllBuild *b)
{
	for (int i = 0; i < b->n; i++)
		sift_kll_free(&b->sk[i]);
	pfree(b->sk);
	pfree(b);
}

void
sift_f2_kll_add_col(SiftF2KllBuild *b, int slot, const SiftColIn *c, uint32 nrows)
{
	double		div = 1.0;

	for (int i = 0; i < c->scale; i++)
		div *= 10.0;
	for (uint32 r = 0; r < nrows; r++)
	{
		double		v;

		if (c->isnull != NULL && c->isnull[r])
			continue;
		v = c->kind == SIFT_CELL_F8 ? c->f[r] : (double) c->i[r] / div;
		if (isfinite(v))
			sift_kll_add(&b->sk[slot], v);
	}
}

void
sift_f2_kll_add_datums(SiftF2KllBuild *b, int slot, Oid type, const Datum *vals, const bool *isnull, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		double		v;

		if (!isnull[i] && sift_f2_kll_datum(type, vals[i], &v))
			sift_kll_add(&b->sk[slot], v);
	}
}

void
sift_f2_kll_add_section(SiftF2KllBuild *b, SiftSecInput *secs, int *nsec)
{
	SiftKllSecIn *in = palloc(Max(b->n, 1) * sizeof(SiftKllSecIn));
	uint32		len;
	uint8	   *sec;

	for (int i = 0; i < b->n; i++)
		in[i] = (SiftKllSecIn) {b->col[i], SIFT_KLL_WHOLE_PART, &b->sk[i]};
	sec = sift_kll_sec_build(in, (uint32) b->n, b->cxt, &len);
	if (sec != NULL)
		sift_part_add_section(secs, nsec, SEC_KLL, 0, sec, len, false);
	pfree(in);
}

bool
sift_f2_kll_load(Relation rel, const SiftPart *part, SiftF2KllSec *out)
{
	uint64		len;

	if (sift_part_section(part, SEC_KLL) == NULL)
		return false;
	out->data = sift_part_read_section(rel, part, SEC_KLL, &len);
	out->len = (uint32) len;
	if (len > UINT32_MAX || !sift_kll_sec_check(out->data, out->len))
		SIFT_DAMAGE_DATA(rel, errmsg("index \"%s\": the quantile sketches of part %llu are damaged", RelationGetRelationName(rel), (unsigned long long) part->hdr.part_gen),
						 errhint("REINDEX the index."));
	return true;
}

bool
sift_f2_kll_merge_col(const SiftF2KllSec *sec, uint16 col, SiftKll *dst)
{
	const uint8 *blob;
	uint32		len;
	uint64		n;

	return sift_kll_sec_find(sec->data, sec->len, col, SIFT_KLL_WHOLE_PART, &blob, &len, &n) && sift_kll_merge_wire(dst, blob, len);
}

bool
sift_f2_kll_merge_part(SiftF2KllBuild *b, Relation rel, const SiftPart *part, const SiftPartDesc *desc)
{
	SiftF2KllSec sec;
	bool		ok = desc->ndead == 0 && sift_f2_kll_load(rel, part, &sec);

	for (int i = 0; ok && i < b->n; i++)
	{
		const uint8 *blob;
		uint32		len;
		uint64		n;

		ok = sift_kll_sec_find(sec.data, sec.len, b->col[i], SIFT_KLL_WHOLE_PART, &blob, &len, &n);
	}
	for (int i = 0; ok && i < b->n; i++)
	{
		bool		merged = sift_f2_kll_merge_col(&sec, b->col[i], &b->sk[i]);

		SIFT_INVARIANT(merged, "sift: a quantile sketch checked is missing");
	}
	if (desc->ndead == 0 && sift_part_section(part, SEC_KLL) != NULL)
		pfree(sec.data);
	return ok;
}
