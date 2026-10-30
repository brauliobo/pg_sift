/*
 * col/keycols/point.c - point_tid_columns by default: which keys list their rows' addresses and the gap a parallel build leaves
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "col/am.h"
#include "col/keycols/keycols_int.h"
#include "col/setsum.h"
#include "col/point.h"

/*
 * The point directory by the default formula. An equality on an integer column of nearly unique values that does not follow the order of the heap
 * reads every unit, whatever the zones and the key lists: the keys are spread over the whole range of each unit. The directory of heap addresses (option point_tid_columns: an equality or
 * an IN list returns the addresses with no unit read) answers it in a flat 0.2 M instructions, and a 16-key IN list in 0.49 M (a b-tree 0.50 M; the directory of
 * rows, point_columns, reads a unit for each key: 0.74 M). Measured on 200,000 to 1,000,000 random unique int4, int8 rows, one column: the equality 1.17 M and 5.17 M instructions without it (about
 * 0.17 M + 5 instructions a row), 0.2 M with it (a b-tree 0.14 M); the index 16.5 bytes a row larger with the table sized to a power of two (8.3 to 25.4 MB at 1,000,000 int8 rows, a b-tree 22.5 MB), 10.9 to 12.2 with the exact slot count (column format minor 33; 400,000 rows of 1.0, 0.75 and 0.5 distinct values a row: 10.9, 12.2 and 11.1), 3.6 to 4.7 with the packed table (minor 35, point_dir_bytes); CREATE INDEX
 * +21% instructions (1,756 to 2,126 a row, 1,719 to 2,092 for int4). B = log2(S) / log2(1.05) against C = G / 3 + W / 5.
 */
#define POINT_AUTO_FIXED		170000.0	/* instructions of an equality that reads the units, whatever the rows */
#define POINT_AUTO_ROW			5.0			/* ... and for each row of the table */
#define POINT_AUTO_LOOKUP		200000.0	/* instructions of an equality through the directory */
#define POINT_AUTO_MIN_SAVED	100000.0
#define POINT_AUTO_BUILD_PCT	21.0
#define POINT_AUTO_OVERHEAD		0.4			/* the bytes a row of an index beyond its values: units, zones, headers */
#define POINT_AUTO_UNIQUE		0.5			/* the distinct values of the column over the rows of the table: nearly unique */

/*
 * The bytes a row of the packed table of a point directory: the remainder of the key (the bits the range of the keys
 * has beyond one bucket of about SIFT_CPOINT_PK_BUCKET of them when it is as wide as the rows, else the 16 of the hash), the code of the row or the address (the bits of the rows,
 * and one for the offset numbers) and the 4 bytes of the directory of each bucket. range_bits < 0: the range is not known, the hash is assumed. Measured on 1,000,000 rows
 * of an int8: 3.6 bytes a row of 1 to 1,000,000 (a dense range), 4.7 of random 63 bit keys.
 */
static double
point_dir_bytes(double rows, double range_bits)
{
	double		payload = ceil(log2(Max(rows, 2.0))) + 1,
				rem = SIFT_CPOINT_PK_RBITS;

	if (range_bits >= 0)
		rem = Min(rem, Max(0.0, range_bits - log2(Max(rows, 2.0)) + log2(SIFT_CPOINT_PK_BUCKET)));
	return (rem + payload) / 8 + 4.0 / SIFT_CPOINT_PK_BUCKET;
}

/* the bits of the range of the values of an integer column, from the bounds of its histogram (the width of its type when it has none) */
static double
int_range_bits(Relation heap, AttrNumber hatt, Form_pg_attribute att)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		bits = att->attlen * 8.0;

	if (tp == NULL)
		return bits;
	if (att->attbyval && get_attstatsslot(&sslot, tp, STATISTIC_KIND_HISTOGRAM, InvalidOid, ATTSTATSSLOT_VALUES) && sslot.nvalues > 1)
	{
		int64		lo,
					hi;

		if (att->attlen == 2)
		{
			lo = DatumGetInt16(sslot.values[0]);
			hi = DatumGetInt16(sslot.values[sslot.nvalues - 1]);
		}
		else if (att->attlen == 4)
		{
			lo = DatumGetInt32(sslot.values[0]);
			hi = DatumGetInt32(sslot.values[sslot.nvalues - 1]);
		}
		else
		{
			lo = DatumGetInt64(sslot.values[0]);
			hi = DatumGetInt64(sslot.values[sslot.nvalues - 1]);
		}
		bits = Min(bits, log2((double) hi - (double) lo + 2));
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return bits;
}

/* about how many bytes a row of the index takes without the directory: an integer that follows the heap takes a byte or two, one that does not takes the bits of its range, the other columns what keycols_est_col_bytes says, a jsonb column with paths a byte for each (the index stores their values, not the documents: 5 leaves of 4 columns took 2.3 of the 16 bytes a row) */
static double
point_base_bytes(Relation heap, Relation index, const SiftColSchema *schema)
{
	double		bytes = POINT_AUTO_OVERHEAD;

	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		double		d,
					corr;
		int32		width;
		int			paths = 0;

		for (int c = 0; att->atttypid == JSONBOID && c < schema->ncols; c++)
			paths += schema->cols[c].attno == i + 1 && sift_col_def_has_kinds(&schema->cols[c]);
		if (paths > 0)
			bytes += paths;
		else if (hatt == 0 || !keycols_col_stats(heap, hatt, &d, &corr, &width))
			bytes += att->attlen > 0 ? att->attlen : 16;
		else if (att->attbyval && att->attlen > 0 && att->atttypid != FLOAT4OID && att->atttypid != FLOAT8OID)
			bytes += corr >= 0.9 ? 1.5 : Min(int_range_bits(heap, hatt, att) / 8, att->attlen);
		else
			bytes += Max(1.0, keycols_est_col_bytes(heap, att, d, width));
	}
	return bytes;
}

/* the entries of a comma separated list, 'auto: ' prefix and all (NULL or empty: none) */
static int
list_len(const char *list)
{
	int			n = 0;

	if (list == NULL || list[0] == '\0')
		return 0;
	for (const char *p = list; *p != '\0'; p++)
		n += *p == ',';
	return n + 1;
}

/*
 * The blocks a parallel build reserves in front of the segments of its participants (col/build_par.c): the segments they leave overlap, and the merge
 * that follows the build writes their rows once more, into this gap, where the file is cut behind it. About the bytes a row of the index takes by the
 * statistics (the point directories point_dir_bytes a row each, the order arrays 5), a tenth over; 0 without key_cut (the segments of the participants do not overlap,
 * there is nothing to merge) and when the table has no row estimate. A gap that is too small
 * puts the rest of the merge above the segments it replaces (the space step brings it down), one too large is cut with them.
 */
BlockNumber
sift_col_build_gap(Relation heap, Relation index)
{
	BlockNumber pages;
	double		tuples,
				allvis,
				row;

	if (!sift_col_key_cut(sift_col_rel(index)))
		return 0;
	table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvis);
	if (tuples < 1)
		return 0;
	row = point_base_bytes(heap, index, sift_col_rel_private(index, CurrentMemoryContext)->schema);
	row += point_dir_bytes(tuples, -1) * list_len(sift_col_options_point_tid_columns(index)) + 5.0 * list_len(sift_col_options_order_array_columns(index));
	return (BlockNumber) Min(tuples * row * 1.1 / BLCKSZ, (double) MaxBlockNumber / 2);
}

/* whether the comma separated list names the leaf ('a, b'; 'auto: a, b' as written by a build) */
static bool
in_list(const char *list, const char *name)
{
	bool		autolist;
	const char *p = sift_cpagg_auto_list(list, &autolist);
	size_t		len = strlen(name);

	for (; *p == ' '; p++)
		;
	while (*p != '\0')
	{
		const char *end = strchr(p, ',');
		size_t		n = end ? (size_t) (end - p) : strlen(p);

		while (n > 0 && p[n - 1] == ' ')
			n--;
		if (n == len && strncmp(p, name, len) == 0)
			return true;
		if (end == NULL)
			break;
		for (p = end + 1; *p == ' '; p++)
			;
	}
	return false;
}

void
sift_col_auto_point(Relation heap, Relation index)
{
	const char *opt;
	const char *ordopt;
	const char *plain;
	const SiftColRel *rel;
	StringInfoData list;
	bool		autolist;
	double		rows,
				base;

	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		return;
	rel = sift_col_rel_private(index, CurrentMemoryContext);	/* the options an earlier default wrote reach the relation here */
	opt = sift_col_options_point_tid_columns(index);
	plain = sift_col_options_point_columns(index);
	ordopt = sift_col_options_order_array_columns(index);
	(void) sift_cpagg_auto_list(opt, &autolist);
	if (plain[0] != '\0' || (opt[0] != '\0' && !autolist))
		return;
	rows = heap->rd_rel->reltuples;
	initStringInfo(&list);
	base = point_base_bytes(heap, index, rel->schema);
	for (int c = 0; c < rel->schema->ncols && rows > 0; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = keycols_heap_attno(index, d);
		double		distinct,
					corr,
					S,
					B,
					C,
					G,
					dir_bytes;
		int32		width;
		Oid			type = hatt == 0 ? InvalidOid : TupleDescAttr(RelationGetDescr(index), d->attno - 1)->atttypid;

		if (hatt == 0 || d->parent >= 0 || d->store != SIFT_COLS_INT || d->lane != 0 || type == DATEOID || type == TIMESTAMPOID || type == TIMESTAMPTZOID || in_list(ordopt, keycols_leaf_name(index, d)) || !sift_cset_leaf_ok(rel->schema, d) || strchr(keycols_leaf_name(index, d), ',') != NULL)
			continue;
		if (!keycols_col_stats(heap, hatt, &distinct, &corr, &width) || distinct < POINT_AUTO_UNIQUE * rows || corr >= 0.9)
			continue;
		S = (POINT_AUTO_FIXED + POINT_AUTO_ROW * rows) / POINT_AUTO_LOOKUP;
		dir_bytes = point_dir_bytes(rows, int_range_bits(heap, hatt, TupleDescAttr(RelationGetDescr(heap), hatt - 1)));
		G = 100.0 * dir_bytes / base;
		B = log2(Max(S, 1.0)) / log2(1.05);
		C = G / 3 + POINT_AUTO_BUILD_PCT / 5;
		if (S < 1.05 || (S - 1) * POINT_AUTO_LOOKUP < POINT_AUTO_MIN_SAVED || B < C)
			continue;
		keycols_append_name(&list, keycols_leaf_name(index, d));
		base += dir_bytes;
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": point_tid_columns includes \"%s\", chosen by the default formula: an equality on its nearly unique values, which do not follow the order of the table, would read every unit (about %.1f M instructions against %.1f M with the directory; predicted index size %+.0f%%, %.1f bytes a row, benefit %.0f against cost %.0f)",
						RelationGetRelationName(index), keycols_leaf_name(index, d), (POINT_AUTO_FIXED + POINT_AUTO_ROW * rows) / 1e6, POINT_AUTO_LOOKUP / 1e6, G, dir_bytes, B, C),
				 errhint("ALTER INDEX %s SET (point_tid_columns = 'off') and REINDEX keep the index without it.", RelationGetRelationName(index))));
	}
	if (list.len == 0)
	{
		if (opt[0] != '\0')
			sift_col_set_option(index, "point_tid_columns", "auto");
		return;
	}
	{
		StringInfoData v;

		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", list.data);
		sift_col_set_option(index, "point_tid_columns", v.data);
		pfree(v.data);
	}
}
