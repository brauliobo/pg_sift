/*
 * col/build/notice.c - what CREATE INDEX tells the user: the NOTICEs of the choices the build made
 * key overflow, point addresses, order arrays, foreign keys, volatile expressions
 * one function for each notice
 */
#include "postgres.h"
#include <math.h>
#include "access/table.h"
#include "catalog/pg_constraint.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#ifdef SIFT_TESTING
#endif
#include "col/shred.h"
#include "core/limits.h"
#include "col/build/build_int.h"

/*
 * The keys of a wildcard path (m.*) past the dictionary of the shredder (SIFT_SHRED_DEFAULT_KEYS per index) leave the key and its values as
 * overflow, and a query that reads the leaf keeps the normal plan: the units that hold one are counted from the zones of the finished segments.
 */
void
colb_notice_key_overflow(Relation index)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	const SiftColLeafStats *ls = NULL;

	for (int c = 0; c < d->ncols; c++)
	{
		const SiftColDef *def = &d->cols[c];
		size_t		n = def->path ? strlen(def->path) : 0;

		if (def->parent < 0 || n <= 4 || strcmp(def->path + n - 4, "#key") != 0)
			continue;
		if (ls == NULL)
			ls = sift_col_leaf_stats(index);
		if (ls[c].novf > 0)
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": the keys of %.*s passed the %d keys the index keeps in %.0f of %.0f units (%.1f%%)",
							RelationGetRelationName(index), (int) (n - 4), def->path, (int) sift_limit(shred_keys, SIFT_SHRED_DEFAULT_KEYS),
							ls[c].novf, ls[c].nunits, 100.0 * ls[c].novf / Max(ls[c].nunits, 1)),
					 errdetail("The keys and values past the limit are read from the heap, and a query that reads the leaf keeps the normal plan."),
					 errhint("Declare the keys that queries read by name in the paths option instead of a wildcard, or keep the map out of the index.")));
	}
}

/* key_order fills the options the user did not write: said once, with the values it gave them */
void
colb_notice_key_order(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);
	StringInfoData what;

	if (o->ko_lead == 0)
		return;
	initStringInfo(&what);
	if (o->ko_key && o->key_cut == 0)
		appendStringInfo(&what, "key_cut = '%s', ", (const char *) o + o->ko_key);
	if (!o->order_columns || *((const char *) o + o->order_columns) == '\0')
		appendStringInfo(&what, "order_columns = '%s', ", (const char *) o + o->ko_last);
	if (o->order_array_columns == 0)
		appendStringInfo(&what, "order_array_columns = '%s', ", (const char *) o + o->ko_last);
	if (o->ko_group && (!o->order_array_group || *((const char *) o + o->order_array_group) == '\0'))
		appendStringInfo(&what, "order_array_group = '%s', ", (const char *) o + o->ko_group);
	if (o->ko_key && (!o->point_tid_columns || *((const char *) o + o->point_tid_columns) == '\0'))
		appendStringInfo(&what, "point_tid_columns = '%s', ", (const char *) o + o->ko_key);
	if (what.len == 0)
		return;
	what.data[what.len - 2] = '\0';
	if (o->ko_auto)
	{
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": key_order = 'auto' chooses '%s' and sets %s", RelationGetRelationName(index), sift_col_options_key_order(index) + 5 + strspn(sift_col_options_key_order(index) + 5, " "), what.data),
				 errdetail("The rows of each unit are kept in the order of the time column and the order arrays are built on it, grouped by the entity with 'group, key': ORDER BY .. LIMIT and OFFSET over the time column and the latest rows of each group read entries, not units. The plain order arrays of other columns are not built."),
				 errhint("To build the index with the order arrays the default chooses without it, run ALTER INDEX %s SET (key_order = 'off') and REINDEX.", quote_identifier(RelationGetRelationName(index)))));
		return;
	}
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": key_order = '%s' sets %s", RelationGetRelationName(index), sift_col_options_key_order(index), what.data),
			 errdetail("The options you wrote stay. The rows of each unit are kept in the order of the key and the order arrays are built on it, which pays when the table is stored in that order: ORDER BY .. LIMIT, OFFSET, ranges and lists of values read entries and units, not rows at random. "
					   "On a table in arrival order an ordered LIMIT costs what it does without the option; with 'group, key' the latest rows of each group cost b-tree-class instructions and key_cut is left to the build."),
			 errhint("To build the index without it, run ALTER INDEX %s RESET (key_order) and REINDEX.", quote_identifier(RelationGetRelationName(index)))));
}

/* The first release that reads a column format minor, as scripts/upgrade_check.sql lists it: the segments of 4.0 are minor 40 and up, the others of 3.7 are 33 to 39 */
static const char *
minor_lib(int minor)
{
	return minor >= SIFT_COL_FORMAT_ENDORD ? "4.0" : "3.7";
}

/* The option takes a longer build for the heap addresses it lists, and its segments are column format minor 33 or more (a build of 3.7 writes 37), which a library before 3.7 refuses: said once, where it is used */
void
colb_notice_point_tid(Relation index)
{
	const SiftColLeafStats *ls;
	const SiftColIndexMeta *d;
	double		units = 0;
	bool		autolist;

	(void) sift_cpagg_auto_list(sift_col_options_point_tid_columns(index), &autolist);
	if (sift_col_options_point_tid_columns(index)[0] == '\0' || strcmp(sift_col_options_point_tid_columns(index), "off") == 0 || autolist)	/* the choice of the build says so itself */
		return;
	if (sift_col_options_key_cut(index)[0] != '\0')
	{
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": point_tid_columns lists the rows of the segment on a key-cut index", RelationGetRelationName(index)),
				 errdetail("The units of a key-cut segment are not block ranges, so a heap address does not name its unit: a lookup reads the unit of each row, as with point_columns.")));
		return;
	}
	d = sift_col_index_defs(index);
	ls = sift_col_leaf_stats(index);
	for (int c = 0; c < d->ncols; c++)
		units = Max(units, ls[c].point_tid_units);
	if (units <= 0)
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": point_tid_columns lists the heap addresses of the rows", RelationGetRelationName(index)),
			 errdetail("A lookup of the access method returns the addresses without reading a unit, except for a unit with a dead row; the segments are column format minor %d or more, which a library before %s cannot read, and the build sorts the keys of the column.", SIFT_COL_FORMAT_EXACTSLOT, minor_lib(SIFT_COL_FORMAT_EXACTSLOT)),
			 errhint("Drop the option, or set sift.col_format_minor to 14, to list the rows of the segment instead.")));
}

/*
 * Why each column of the build's choice has the order it has, from the largest segment: the units a scan with a bound in the middle of the values reads (sift_cgord_pay) are few (the zones prune the
 * bound: the ends) or many (the whole order). One phrase a column in the NOTICE, which does not vary with the data of a build; the figures (units, cold blocks, MB at
 * SIFT_CGORD_PAY_BLOCK a block, against the MB of the whole order) are logged at DEBUG1. Nothing when the index has no segment yet.
 */
static void
order_choice_lines(Relation index, const SiftColLeafStats *ls, StringInfo out)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	int			pick = -1;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (pick < 0 || snap->segs[i].ndocs > snap->segs[pick].ndocs))
			pick = i;
	if (pick >= 0)
	{
		SiftColSegment *seg = sift_cseg_open(index, &snap->segs[pick], CurrentMemoryContext);

		for (int c = 0; c < r->schema->ncols; c++)
		{
			SiftCGOrdPay pay;
			bool		ends = ls[c].gord_ends_rows > 0;
			const char *name = sift_col_leaf_name(index, &r->schema->cols[c]);

			if ((ls[c].gord_rows > 0 || ends) && ls[c].gord_grp_rows < ls[c].gord_rows + ls[c].gord_ends_rows && sift_cseg_gorder_pay(seg, c, &pay))
			{
				appendStringInfo(out, "%s%s: %s", out->len ? "; " : "", name, ends ? "the ends (a bound in the middle of its values reads few units: its zones prune it)" :
								 "the whole order (a bound in the middle of its values reads many units)");
				elog(DEBUG1, "sift_col index \"%s\": %s: a bound in the middle of its values reads about %.1f units (%.0f cold blocks, %.2f MB at %d kB a block) against %.2f MB for the whole order of a segment of %u rows",
					 RelationGetRelationName(index), name, pay.units, pay.blocks, pay.saved / (1024.0 * 1024.0), SIFT_CGORD_PAY_BLOCK / 1024, pay.bytes / (1024.0 * 1024.0), (uint32) snap->segs[pick].ndocs);
			}
		}
		sift_cseg_close(seg);
	}
	pfree(snap);
}

/* The column format minor the complete order arrays of an index have (a compact one is newer than the others), and the first release that reads it */
static int
order_minor(const SiftColLeafStats *ls, int ncols)
{
	for (int c = 0; c < ncols; c++)
		if (ls[c].gord_perm > 0)
			return SIFT_COL_FORMAT_TEXTPERM;
	for (int c = 0; c < ncols; c++)
		if (ls[c].gord_compact_chunks > 0)
			return SIFT_COL_FORMAT_ORDLAYOUT;
	return SIFT_COL_FORMAT_ADDRPACK;
}

/*
 * The option keeps a sorted array of every row of a column, or only the ends of the order: its bytes are said once, where they are paid, with the columns the build
 * chose by name. One NOTICE for the index, whatever the choices (the ends, the whole order, a second order by a group, the columns weighed by the default formula).
 */
void
colb_notice_order_array(Relation index)
{
	const SiftColLeafStats *ls;
	const SiftColIndexMeta *d;
	const char *opt = sift_col_options_order_array_columns(index);
	StringInfoData names,
				detail,
				hint,
				why;
	bool		autolist,
				gauto;
	const char *glist = sift_cpagg_auto_list(sift_col_options_order_array_group(index), &gauto);
	double		rows = 0,
				bytes = 0;
	int			n = 0,
				ne = 0,
				minor;
	char	   *lines;

	(void) sift_cpagg_auto_list(opt, &autolist);
	lines = autolist ? sift_col_gorder_lines(index) : NULL;
	if (opt == NULL || (opt[0] == '\0' && !autolist))
		return;
	while (*glist == ' ')
		glist++;
	if (strpbrk(glist, ",;") != NULL)
		glist = pnstrdup(glist, strpbrk(glist, ",;") - glist);	/* 'auto: g, a; c': the arrays after the group are the ones that have a second order, a without the plain order */
	d = sift_col_index_defs(index);
	ls = sift_col_leaf_stats(index);
	initStringInfo(&names);
	for (int c = 0; c < d->ncols; c++)
	{
		const char *name = sift_col_leaf_name(index, &d->cols[c]);

		if (ls[c].gord_rows > 0)
		{
			rows += ls[c].gord_rows;
			bytes += ls[c].gord_bytes;
			n++;
		}
		if (ls[c].gord_rows > 0 || ls[c].gord_ends_rows > 0)
			appendStringInfo(&names, "%s%s%s", names.len ? ", " : "", name, ls[c].gord_rows > 0 ? "" : " (ends only)");
		ne += ls[c].gord_ends_rows > 0;
	}
	minor = order_minor(ls, d->ncols);
	if (ne > 0)
		minor = Max(minor, SIFT_COL_FORMAT_ENDORD);
	if (n + ne == 0 && lines == NULL)
		return;
	if (!autolist)
	{
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": order_array_columns adds the sorted keys and heap addresses to the sorted runs of %d column%s", RelationGetRelationName(index), n, n == 1 ? "" : "s"),
				 errdetail("%.1f bytes a row for each column, %.1f MB together; the segments are column format minor %d, which a library before %s cannot read. ORDER BY column LIMIT n reads n entries of the array and n rows of the heap, whatever the number of units, and a range of the column (up to 16,384 rows) hands the heap the addresses between its bounds without reading a unit.", bytes / rows, bytes / (1024.0 * 1024.0), minor, minor_lib(minor)),
				 errhint("Drop the option, or set sift.col_format_minor to 14, to keep the sorted runs only.")));
		return;
	}
	initStringInfo(&why);
	if (n + ne > 0)
		order_choice_lines(index, ls, &why);
	initStringInfo(&detail);
	if (n > 0)
		appendStringInfo(&detail, "%.1f bytes a row for each whole array, %.1f MB together. ", bytes / rows, bytes / (1024.0 * 1024.0));
	if (ne > 0)
		appendStringInfo(&detail, "An ends-only array keeps the first and last %u entries of each segment and its first NULL rows, under 0.1 bytes a row of those columns. ", sift_cgord_ends());
	if (n + ne > 0)
		appendStringInfo(&detail, "The segments are column format minor %d, which a library before %s cannot read. ", minor, minor_lib(minor));
	appendStringInfo(&detail, "The build chose the columns from the statistics of the table: 100,000 rows or more, 1,000 or more distinct values and a correlation with the heap order under 0.99 (the zones cannot skip units), at most %d of them, and for a text column or a column of few values that the default formula pays (a table with no statistics is analyzed first).", SIFT_CGORD_ENDS_LEAVES);
	if (n + ne > 0)
		appendStringInfo(&detail, " A column keeps the whole order when a scan with a bound in the middle of its values would read more cold blocks than the order's bytes are worth (%d kB of index a block), else the ends.%s%s%s", SIFT_CGORD_PAY_BLOCK / 1024, why.len ? " The choice of each column: " : "", why.data, why.len ? "." : "");
	if (gauto && glist[0] != '\0')
		appendStringInfo(&detail, " order_array_group = 'auto' adds a second order of each date, timestamp and integer array sorted by %s first (the integer column of most values that has 100 or more and at least 3 rows apiece).", glist);
	if (n + ne > 0)
		appendStringInfo(&detail, " ORDER BY column [DESC] LIMIT n, and the same with a bound at the end of the order (column >= x ORDER BY column DESC LIMIT n), read n entries of an end, whatever the number of units, for n and an OFFSET up to the entries of the end; a range of a whole-order column (up to 16,384 rows) hands the heap the addresses between its bounds without reading a unit; a range in the middle of an ends-only column reads the units.%s", gauto && glist[0] != '\0' ? " WHERE group = x ORDER BY column LIMIT n reads n entries of x." : "");
	if (lines != NULL)
		appendStringInfo(&detail, " %s", lines);
	initStringInfo(&hint);
	appendStringInfo(&hint, "To drop them, run ALTER INDEX %s SET (order_array_columns = 'off') and REINDEX; order_array_columns = 'column' keeps the whole order of a column; set sift.col_format_minor to 14 to write none.", quote_identifier(RelationGetRelationName(index)));
	if (gauto && glist[0] != '\0')
		appendStringInfoString(&hint, " To drop the second order, set order_array_group = 'off'.");
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": order_array_columns = 'auto' keeps %s%s", RelationGetRelationName(index), n + ne > 0 ? "sorted arrays of " : "no sorted array", n + ne > 0 ? names.data : ""),
			 errdetail_internal("%s", detail.data),
			 errhint("%s", hint.data)));
}

/* The layout of the order array of each column, the share of its chunks that are compact and its bytes a row: said when the option asks for one or the build kept a compact chunk */
void
colb_notice_order_layout(Relation index)
{
	const SiftColLeafStats *ls;
	const SiftColIndexMeta *d;
	StringInfoData out,
				ties;
	bool		any = false;
	const char *opt = sift_col_options_order_array_columns(index);

	/* no order array, no layout to say (and the leaf statistics are not read for an index that has none) */
	if (opt == NULL || opt[0] == '\0' || strcmp(opt, "off") == 0)
		return;
	d = sift_col_index_defs(index);
	ls = sift_col_leaf_stats(index);
	initStringInfo(&out);
	initStringInfo(&ties);
	for (int c = 0; c < d->ncols; c++)
	{
		const char *name = sift_col_leaf_name(index, &d->cols[c]);

		if (ls[c].gord_rows <= 0)
			continue;
		any = any || ls[c].gord_compact_chunks > 0;
		if (ls[c].gord_tie_chunks > 0)
			appendStringInfo(&ties, "%s%s: %.0f chunks have long runs of equal keys; order_layout = auto would keep exact keys there", ties.len ? "; " : "", name, ls[c].gord_tie_chunks);
		appendStringInfo(&out, "%s%s: %s, %.1f bytes a row", out.len ? "; " : "", name,
						 ls[c].gord_compact_chunks <= 0 ? "exact keys" : psprintf("rounded keys in %.1f%% of %.0f chunks, exact in the rest", 100.0 * ls[c].gord_compact_chunks / ls[c].gord_chunks, ls[c].gord_chunks),
						 ls[c].gord_bytes / ls[c].gord_rows);
	}
	if (ties.len > 0)
		appendStringInfoChar(&ties, '.');
	if (out.len == 0 || !(any || sift_col_options_order_layout(index)[0] != '\0'))
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": order_layout: %s", RelationGetRelationName(index), out.data),
			 errdetail("A compact chunk of an order array keeps the heap address of each row and its key rounded to a bucket of a few bits; the heap settles ties and gives the exact key of the rows a scan needs. The build keeps a chunk compact where that is at least a fifth smaller than keeping the exact keys and it has no run of %d equal keys or more. The segments with a compact chunk are column format minor %d, which a library before %s cannot read.%s%s", SIFT_CGORD_TIE_RUN, SIFT_COL_FORMAT_ORDLAYOUT, minor_lib(SIFT_COL_FORMAT_ORDLAYOUT), ties.len ? " " : "", ties.data),
			 errhint("To write exact keys only, run ALTER INDEX .. SET (order_layout = 'whole') and REINDEX, or a merge; 'compact: column' and 'whole: column' choose it for a column.")));
}

/*
 * The columns of the foreign keys of the table are the keys a join from it reads (a fact table's keys to its dimensions). When one is not a column of the index, an
 * aggregate scan that joins the table to the referenced one keeps the normal plan (join_condition) and a lookup join cannot filter the table by the keys of the
 * rows its filters keep. The column adds a few bytes a row, more than a default may add, so the build says it once.
 */
void
colb_notice_fk(Relation heap, Relation index)
{
	Relation	conrel = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;
	StringInfoData names;
	Oid			to = InvalidOid;
	Bitmapset  *seen = NULL;

	initStringInfo(&names);
	ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(heap)));
	scan = systable_beginscan(conrel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		bool		isnull;
		Datum		d;
		ArrayType  *arr;
		int16	   *att;

		if (con->contype != CONSTRAINT_FOREIGN)
			continue;
		d = heap_getattr(tup, Anum_pg_constraint_conkey, RelationGetDescr(conrel), &isnull);
		if (isnull)
			continue;
		arr = DatumGetArrayTypeP(d);
		att = (int16 *) ARR_DATA_PTR(arr);
		for (int k = 0; k < ARR_DIMS(arr)[0]; k++)
		{
			bool		listed = false;

			for (int i = 0; i < index->rd_index->indnatts; i++)
				listed |= index->rd_index->indkey.values[i] == att[k];
			if (listed || bms_is_member(att[k], seen))
				continue;
			seen = bms_add_member(seen, att[k]);
			to = OidIsValid(to) ? to : con->confrelid;
			if (names.len > 0)
				appendStringInfoString(&names, ", ");
			appendStringInfoString(&names, quote_identifier(get_attname(RelationGetRelid(heap), att[k], false)));
		}
	}
	systable_endscan(scan);
	table_close(conrel, AccessShareLock);
	if (names.len == 0)
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": the foreign key columns %s of \"%s\" are not columns of the index", RelationGetRelationName(index), names.data,
					RelationGetRelationName(heap)),
			 errdetail("A join to \"%s\" on them keeps the normal plan (join_condition) and the scan cannot filter the table by the keys of another table's rows; a column adds a few bytes a row.",
					   get_rel_name(to)),
			 errhint("Add the columns to the index if queries join on them.")));
}

/*
 * A column the index covers cannot be changed by a HOT update: every UPDATE of it writes a new row version into every index of the table, and the index keeps the
 * old one until the next merge. Measured on the 13-column index of the web API (updated_at and a counter covered): 0% HOT updates against 28% without the index,
 * 23.7k against 38.7k updates a second, 2,175 against 1,493 bytes of WAL an update. The columns that look rewritten often (a timestamp of the last change, a
 * counter, a version) are named once; leaving one out is the choice that keeps updates HOT, covering it the choice that keeps its aggregates.
 */
bool
sift_col_volatile_name(const char *name)
{
	static const char *const suffix[] = {"updated_at", "updated_on", "modified_at", "modified_on", "touched_at", "synced_at", "checked_at", "_count", "_counter", "_version"};
	static const char *const exact[] = {"version", "likes", "views", "hits", "counter", "count"};
	size_t		n = strlen(name);

	for (int i = 0; i < lengthof(exact); i++)
		if (pg_strcasecmp(name, exact[i]) == 0)
			return true;
	for (int i = 0; i < lengthof(suffix); i++)
	{
		size_t		m = strlen(suffix[i]);

		if (n >= m && pg_strcasecmp(name + n - m, suffix[i]) == 0)
			return true;
	}
	return false;
}

void
colb_notice_volatile(Relation heap, Relation index)
{
	StringInfoData cols;
	int			n = 0;

	initStringInfo(&cols);
	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	attno = index->rd_index->indkey.values[i];
		const char *name = attno > 0 ? get_attname(RelationGetRelid(heap), attno, true) : NULL;

		if (name != NULL && sift_col_volatile_name(name))
			appendStringInfo(&cols, "%s%s", n++ ? ", " : "", name);
	}
	if (n == 0)
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": an UPDATE of %s is not a HOT update, since the index covers %s", RelationGetRelationName(index), cols.data,
					n == 1 ? "it" : "them"),
			 errdetail("Each such update writes a new row version into every index of the table (measured on a 13-column index: 0%% HOT updates against 28%%, 23.7k against 38.7k updates a second, 2,175 against 1,493 bytes of WAL an update), and the index keeps the old one until a merge or a REINDEX."),
			 errhint("Leave the column out of the index if it is rewritten far more often than it is aggregated; keep it if its aggregates are the reason for the index.")));
}
