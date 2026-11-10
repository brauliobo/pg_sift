/*
 * col/build/choices.c - what CREATE INDEX and REINDEX say about a sift_col index, and sift.index_choices(), sift.help()
 *
 * The build makes its choices in many places (key_cut, order arrays, key_columns, track_changes ...), each written into an option of the index. Nothing is said where a
 * choice is made: when the build ends, derive() reads the finished index (its options, the statistics of its segments, the catalog) and says all of it in ONE NOTICE: the
 * count of what it chose and the values, one line, and where to read the rest. sift.index_choices(index) runs the same derivation later, and adds the reason, the cost and the
 * command that turns each choice off from the table docs/messages.tsv generates (choices_gen.h), so a text is written once, there.
 */
#include "postgres.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_constraint.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "col/build/build_int.h"
#include "col/build/choices_gen.h"
#include "agg/refuse.h"
#include "col/scope.h"

#define SUMMARY_LINE	100		/* characters of the first line of the summary */
#define DETAIL_LINE		112		/* ... and of a line of its values (two lines) */
#define VALUE_MAX		48		/* characters of one value in it: the rest is in sift.index_choices */

typedef struct Found
{
	const SiftMsgRow *row;
	char	   *value;
	char	   *cost;			/* measured on this index, or NULL */
} Found;

typedef struct Derive
{
	Relation	heap;			/* NULL: only what the index says by itself (the limits that a refresh reports) */
	Relation	index;
	int			kinds;			/* SIFT_CHOICE_* wanted */
	bool		build;			/* the statement that built the index: its key_cut verdict is at hand, and nothing is decided again */
	List	   *out;
	const SiftColLeafStats *ls;
	SiftColIndexMeta defs;		/* a copy: the cached one is freed when a lock reloads the index entry */
	int			minor;			/* the column format minor the choices need, 0: none that an older library refuses */
} Derive;

static const SiftMsgRow *
msg_row(const char *id)
{
	for (int i = 0; i < lengthof(sift_msg_rows); i++)
		if (strcmp(sift_msg_rows[i].id, id) == 0)
			return &sift_msg_rows[i];
	SIFT_INVARIANT(false, "docs/messages.tsv has no row \"%s\"", id);
	return NULL;
}

static int
kind_bit(const char *kind)
{
	return strcmp(kind, "auto") == 0 ? SIFT_CHOICE_AUTO : strcmp(kind, "advice") == 0 ? SIFT_CHOICE_ADVICE : strcmp(kind, "limit") == 0 ? SIFT_CHOICE_CAP : SIFT_CHOICE_ACTION;
}

static void
add(Derive *d, const char *id, char *value, char *cost)
{
	const SiftMsgRow *r = msg_row(id);
	Found	   *f;

	if (!(d->kinds & kind_bit(r->kind)))
		return;
	f = palloc(sizeof(Found));
	f->row = r;
	f->value = value;
	f->cost = cost;
	d->out = lappend(d->out, f);
}

static void
add_from_caps(void *ctx, const char *id, const char *value)
{
	add(ctx, id, pstrdup(value), NULL);
}

static const SiftColLeafStats *
stats(Derive *d)
{
	if (d->ls == NULL)
	{
#ifdef SIFT_TESTING
		sift_col_test_inval(d->index);	/* reading the leaf statistics can take a lock that reloads the entry */
#endif
		d->ls = sift_col_leaf_stats(d->index);
	}
	return d->ls;
}

/* 'auto: a, b' gives 'a, b'; anything else, and an 'auto' that chose nothing, NULL */
static const char *
after_auto(const char *opt)
{
	if (opt == NULL || strncmp(opt, "auto:", 5) != 0)
		return NULL;
	opt += 5 + strspn(opt + 5, " ");
	return *opt ? opt : NULL;
}

/* The first release that reads a column format minor, as scripts/upgrade_check.sql lists it: the segments of 4.0 are minor 40 and up, the others of 3.7 are 33 to 39 */
static const char *
minor_lib(int minor)
{
	return minor >= SIFT_COL_FORMAT_SCOPE ? "5.0" : minor >= SIFT_COL_FORMAT_SEEKORD ? "4.3" : minor >= SIFT_COL_FORMAT_ENDORD ? "4.0" : "3.7";
}

static void
need_minor(Derive *d, int minor)
{
	d->minor = Max(d->minor, minor);
}

/* The index under the name it will have: REINDEX CONCURRENTLY builds "name_ccnew" and renames it */
const char *
sift_col_final_name(Relation index)
{
	const char *name = RelationGetRelationName(index);
	const char *p = strstr(name, "_ccnew");

	if (p != NULL && p[6 + strspn(p + 6, "0123456789")] == '\0')
		return pnstrdup(name, p - name);
	return name;
}

/* ---- the choices, one function each: id and function in the table at the end of the section (scripts/gen-messages.sh reads the ids) ---- */

static void
d_key_order(Derive *d)
{
	const char *ko = sift_col_options_key_order(d->index);

	if (after_auto(ko))
	{
		const char *scope = d->heap == NULL ? NULL : sift_col_scope_key(d->heap, d->index, false);
		size_t		n = scope == NULL ? 0 : strlen(scope);

		/* the group of 'group, key' is the scope key of a table shaped like a copy */
		add(d, n > 0 && strncmp(after_auto(ko), scope, n) == 0 && after_auto(ko)[n] == ',' ? "key-order-scope" : "key-order-auto", pstrdup(after_auto(ko)), NULL);
	}
	else if (ko[0] != '\0' && strcmp(ko, "off") != 0)
		add(d, "key-order", pstrdup(ko), NULL);
}

static void
d_array_lengths(Derive *d)
{
	StringInfoData names;

	if (sift_col_options(d->index)->array_lengths != SIFT_COL_ARRLEN_ON)
		return;
	initStringInfo(&names);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(d->index); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(d->index), i);

		if (sift_col_is_array_type(att->atttypid))
			appendStringInfo(&names, "%s%s", names.len ? ", " : "", NameStr(att->attname));
	}
	if (names.len > 0)
		add(d, "array-lengths", names.data, NULL);
}

/* paths = 'auto: spec': the element tokens of array columns ("name[] type", "+name[] type") are the elements of arrays, the others the paths of jsonb columns */
static void
d_paths(Derive *d)
{
	const char *raw = sift_col_options_paths_raw(d->index);
	char	   *spec,
			   *tok,
			   *save = NULL;
	StringInfoData json,
				arrs;

	if (!sift_col_paths_auto(raw) || raw[4] != ':')
		return;
	spec = pstrdup(sift_col_options_paths(d->index));
	initStringInfo(&json);
	initStringInfo(&arrs);
	for (tok = strtok_r(spec, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
	{
		const char *name = tok + strspn(tok, " +");
		const char *br = strstr(name, "[] ");
		StringInfo	to = br != NULL && strchr(name, '.') == NULL && strchr(name, ':') == NULL ? &arrs : &json;

		appendStringInfo(to, "%s%.*s", to->len ? ", " : "", to == &arrs ? (int) (br - name) : (int) strlen(name), name);
	}
	if (json.len > 0)
		add(d, "jsonb-paths", json.data, NULL);
	if (arrs.len > 0)
		add(d, "array-elements", arrs.data, NULL);
}

static void
d_key_columns(Derive *d)
{
	if (after_auto(sift_col_options_key_columns(d->index)))
		add(d, "key-lists", pstrdup(after_auto(sift_col_options_key_columns(d->index))), NULL);
}

static void
d_preagg(Derive *d)
{
	if (after_auto(sift_col_options_preagg(d->index)))
		add(d, "cube-ranking", pstrdup(after_auto(sift_col_options_preagg(d->index))), NULL);
}

static void
d_rollup(Derive *d)
{
	if (after_auto(sift_col_options_rollup_columns(d->index)))
		add(d, "rows-rollup", pstrdup(after_auto(sift_col_options_rollup_columns(d->index))), NULL);
}

static void
d_key_cut(Derive *d)
{
	const char *kc = sift_col_options_key_cut(d->index);

	kc += strspn(kc, " ");
	if (sift_col_options_key_cut_auto(d->index) && *kc)	/* a bare 'auto' chose no key */
		add(d, "key-cut-chosen", pstrdup(kc), NULL);
}

/* "a, b, c" without the column name (the key of track_changes joins the directory of heap addresses by itself, and is said as track_changes) */
static char *
list_without(const char *list, const char *name)
{
	StringInfoData out;
	char	   *copy = pstrdup(list),
			   *tok,
			   *save = NULL;

	initStringInfo(&out);
	for (tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
	{
		tok += strspn(tok, " ");
		if (strcmp(tok, name) != 0)
			appendStringInfo(&out, "%s%s", out.len ? ", " : "", tok);
	}
	return out.data;
}

static void
d_point_tid(Derive *d)
{
	const char *opt = sift_col_options_point_tid_columns(d->index);
	bool		autolist;
	const char *list = sift_cpagg_auto_list(opt, &autolist);
	char	   *shown;

	if (opt[0] == '\0' || strcmp(opt, "off") == 0)
		return;
	list += strspn(list, " ");
	shown = sift_col_row_key_on(d->index) ? list_without(list, sift_col_options_row_key(d->index)) : pstrdup(list);
	if (shown[0] == '\0')
		return;
	if (autolist)
	{
		add(d, "point-directory-auto", shown, NULL);
		return;
	}
	if (sift_col_options_key_cut(d->index)[0] != '\0')
	{
		add(d, "point-tid-key-cut", shown, NULL);
		return;
	}
	for (int c = 0; c < d->defs.ncols; c++)
		if (stats(d)[c].point_tid_units > 0)
		{
			need_minor(d, SIFT_COL_FORMAT_EXACTSLOT);
			add(d, "point-tid-explicit", shown, NULL);
			return;
		}
}

/* The column format minor the complete order arrays of an index have (a compact one is newer than the others) */
static int
order_minor(Relation index, const SiftColLeafStats *ls, int ncols)
{
	for (int c = 0; c < ncols; c++)
		if ((ls[c].gord_rows > 0 || ls[c].gord_ends_rows > 0) && sift_col_gorder_chunks(index, c, SIFT_CGORD_SEEK | SIFT_CGORD_POS, false) > 0)
			return SIFT_COL_FORMAT_SEEKORD;
	for (int c = 0; c < ncols; c++)
		if (ls[c].gord_perm > 0)
			return SIFT_COL_FORMAT_TEXTPERM;
	for (int c = 0; c < ncols; c++)
		if (ls[c].gord_compact_blocks > 0)
			return SIFT_COL_FORMAT_ORDLAYOUT;
	return SIFT_COL_FORMAT_ADDRPACK;
}

/* The columns whose sorted keys and heap addresses the segments hold, whole or as the ends of the order, their bytes a row and the second order by a group */
static void
d_order_arrays(Derive *d)
{
	const SiftColIndexMeta *defs = &d->defs;
	const char *opt = sift_col_options_order_array_columns(d->index);
	const SiftColLeafStats *ls;
	bool		autolist,
				gauto;
	const char *glist = pstrdup(sift_cpagg_auto_list(sift_col_options_order_array_group(d->index), &gauto));
	StringInfoData names;
	double		rows = 0,
				bytes = 0;
	int			n = 0,
				ne = 0,
				minor;

	(void) sift_cpagg_auto_list(opt, &autolist);
	if (opt == NULL || strcmp(opt, "off") == 0 || (opt[0] == '\0' && !autolist))
		return;
	ls = stats(d);
	initStringInfo(&names);
	for (int c = 0; c < defs->ncols; c++)
	{
		if (ls[c].gord_rows > 0)
		{
			rows += ls[c].gord_rows;
			bytes += ls[c].gord_bytes;
			n++;
		}
		if (ls[c].gord_rows > 0 || ls[c].gord_ends_rows > 0)
			appendStringInfo(&names, "%s%s%s", names.len ? ", " : "", sift_col_leaf_name(d->index, &defs->cols[c]), ls[c].gord_rows > 0 ? "" : " (ends only)");
		ne += ls[c].gord_ends_rows > 0;
	}
	if (n + ne == 0)
		return;
	minor = order_minor(d->index, ls, defs->ncols);
	need_minor(d, ne > 0 ? Max(minor, SIFT_COL_FORMAT_ENDORD) : minor);
	while (*glist == ' ')
		glist++;
	if (gauto && glist[0] != '\0')
	{
		const char *end = strpbrk(glist, ",;");

		int			len = end ? (int) (end - glist) : (int) strlen(glist);

		appendStringInfo(&names, " (second order by %.*s: %s)", len, glist,
						 d->heap ? sift_col_group_reason(d->heap, d->index, pnstrdup(glist, len)) : "hint");
	}
	add(d, autolist ? "order-array-auto" : "order-array-listed", names.data,
		n > 0 ? psprintf("%.1f bytes a row, %.1f MB together", bytes / rows, bytes / (1024.0 * 1024.0)) : NULL);
}

/* The layout of the order array of each column: the share of its chunks that are compact and its bytes a row; said when the option asks for one or the build kept a compact chunk */
static void
d_order_layout(Derive *d)
{
	const SiftColIndexMeta *defs = &d->defs;
	const char *opt = sift_col_options_order_array_columns(d->index);
	const SiftColLeafStats *ls;
	StringInfoData out,
				cost;
	bool		any = false;

	if (opt == NULL || opt[0] == '\0' || strcmp(opt, "off") == 0)
		return;
	ls = stats(d);
	initStringInfo(&out);
	initStringInfo(&cost);
	for (int c = 0; c < defs->ncols; c++)
	{
		const char *name = sift_col_leaf_name(d->index, &defs->cols[c]);

		if (ls[c].gord_rows <= 0)
			continue;
		any = any || ls[c].gord_compact_blocks > 0;
		if (ls[c].gord_compact_blocks > 0)
			appendStringInfo(&out, "%s%s compact in %.0f%% of chunks", out.len ? ", " : "", name, 100.0 * sift_col_gorder_chunks(d->index, c, SIFT_CGORD_COMPACT, true) / ls[c].gord_chunks);
		else
			appendStringInfo(&out, "%s%s exact", out.len ? ", " : "", name);
		appendStringInfo(&cost, "%s%s %.1f bytes a row", cost.len ? ", " : "", name, ls[c].gord_bytes / ls[c].gord_rows);
	}
	if (out.len == 0 || !(any || sift_col_options_order_layout(d->index)[0] != '\0'))
		return;
	if (any)
		need_minor(d, SIFT_COL_FORMAT_ORDLAYOUT);
	add(d, "order-layout", out.data, cost.data);
}

/* track_changes: the key the triggers tell the index about, why there is none, or that the log is not complete after a concurrent build */
static void
d_track(Derive *d)
{
	const char *rk = pstrdup(sift_col_options_row_key(d->index));
	AttrNumber	pk;
	const char *why;

	if (sift_col_track_on(d->index))
	{
		add(d, sift_col_track_ctid(d->index) ? "sift-col-index-installed-row-triggers" : "sift-col-index-installed-two-statement-triggers", pstrdup(rk), NULL);
		if (sift_col_track_incomplete(d->index))
			add(d, "track-changes-concurrent", pstrdup(rk), NULL);
	}
	else if (d->heap != NULL && strcmp(rk, "auto") == 0 && (why = sift_col_row_key_why(d->heap, d->index, &pk)) != NULL)
		add(d, "sift-col-index-does-not-track-changes", pstrdup(why), NULL);
}

static void
d_key_cut_advice(Derive *d)
{
	const char *keys;

	if (d->heap == NULL)
		return;
	keys = d->build ? sift_col_key_cut_advised(d->index) : sift_col_key_cut_advised_now(d->heap, d->index);
	if (keys == NULL)
		keys = sift_col_key_cut_note(d->index);
	if (keys != NULL)
		add(d, "key-cut-advised", pstrdup(keys), NULL);
}

static void
d_key_cut_hot(Derive *d)
{
	const char *why = d->heap == NULL ? NULL : sift_col_key_cut_hot(d->heap, d->index, d->build);

	if (why != NULL)
		add(d, "key-cut-hot-value", pstrdup(why), NULL);
}

/* The keys of a wildcard path (m.*) past the dictionary of the shredder leave the key and its values as overflow, and a query that reads the leaf keeps the normal plan */
static void
d_key_overflow(Derive *d)
{
	const SiftColIndexMeta *defs = &d->defs;
	const SiftColLeafStats *ls = NULL;
	StringInfoData names;

	initStringInfo(&names);
	for (int c = 0; c < defs->ncols; c++)
	{
		const SiftColDef *def = &defs->cols[c];
		size_t		n = def->path ? strlen(def->path) : 0;

		if (def->parent < 0 || n <= 4 || strcmp(def->path + n - 4, "#key") != 0)
			continue;
		if (ls == NULL)
			ls = stats(d);
		if (ls[c].novf > 0)
			appendStringInfo(&names, "%s%.*s", names.len ? ", " : "", (int) (n - 4), def->path);
	}
	if (names.len > 0)
		add(d, "wildcard-keys-overflow", names.data, NULL);
}

/*
 * The columns of the foreign keys of the table are the keys a join from it reads (a fact table's keys to its dimensions). When one is not a column of the index, an
 * aggregate scan that joins the table to the referenced one keeps the normal plan (join_condition) and a lookup join cannot filter the table by the keys of the
 * rows its filters keep. The column adds a few bytes a row, more than a default may add, so it is advice.
 */
static void
d_foreign_keys(Derive *d)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;
	StringInfoData names;
	Bitmapset  *seen = NULL;

	if (d->heap == NULL)
		return;
	conrel = table_open(ConstraintRelationId, AccessShareLock);
	initStringInfo(&names);
	ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(d->heap)));
	scan = systable_beginscan(conrel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		bool		isnull;
		Datum		datum;
		ArrayType  *arr;
		int16	   *att;

		if (con->contype != CONSTRAINT_FOREIGN)
			continue;
		datum = heap_getattr(tup, Anum_pg_constraint_conkey, RelationGetDescr(conrel), &isnull);
		if (isnull)
			continue;
		arr = DatumGetArrayTypeP(datum);
		att = (int16 *) ARR_DATA_PTR(arr);
		for (int k = 0; k < ARR_DIMS(arr)[0]; k++)
		{
			bool		listed = false;

			for (int i = 0; i < d->index->rd_index->indnatts; i++)
				listed |= d->index->rd_index->indkey.values[i] == att[k];
			if (listed || bms_is_member(att[k], seen))
				continue;
			seen = bms_add_member(seen, att[k]);
			appendStringInfo(&names, "%s%s", names.len ? ", " : "", quote_identifier(get_attname(RelationGetRelid(d->heap), att[k], false)));
		}
	}
	systable_endscan(scan);
	table_close(conrel, AccessShareLock);
	if (names.len > 0)
		add(d, "foreign-key-not-indexed", names.data, NULL);
}

/*
 * A column the index covers cannot be changed by a HOT update: every UPDATE of it writes a new row version into every index of the table, and the index keeps the
 * old one until the next merge. Measured on the 13-column index of the web API (updated_at and a counter covered): 0% HOT updates against 28% without the index,
 * 23.7k against 38.7k updates a second, 2,175 against 1,493 bytes of WAL an update. The columns that look rewritten often (a timestamp of the last change, a counter, a version) are named once; leaving one out is the
 * choice that keeps updates HOT, covering it the choice that keeps its aggregates.
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

static void
d_hot_updates(Derive *d)
{
	StringInfoData cols;

	if (d->heap == NULL)
		return;
	initStringInfo(&cols);
	for (int i = 0; i < d->index->rd_index->indnatts; i++)
	{
		AttrNumber	attno = d->index->rd_index->indkey.values[i];
		const char *name = attno > 0 ? get_attname(RelationGetRelid(d->heap), attno, true) : NULL;

		if (name != NULL && sift_col_volatile_name(name))
			appendStringInfo(&cols, "%s%s", cols.len ? ", " : "", name);
	}
	if (cols.len > 0)
		add(d, "hot-update-columns", cols.data, NULL);
}

static bool
point_columns_set(Relation index)
{
	return *sift_col_options_point_columns(index) != '\0';
}

/* An option that 4.3 deprecated is accepted and ignored by the build, which decides by itself; each row says it once and the release that removes the option */
static void
d_deprecated(Derive *d)
{
	static const struct
	{
		const char *id;
		bool		(*set) (Relation index);
		const char *value;
	} opts[] = {
		{"point-columns-deprecated", point_columns_set, "now point_tid_columns, removed in 4.4"},
		{"order-columns-deprecated", sift_col_options_order_columns_option, "ignored, removed in 4.4"},
	};

	if (sift_col_legacy_options)
		return;
	for (int i = 0; i < lengthof(opts); i++)
		if (opts[i].set(d->index))
			add(d, opts[i].id, pstrdup(opts[i].value), NULL);
	/* the directory of heap addresses that point_columns now asks for is a newer column format than the directory of rows */
	if (point_columns_set(d->index))
		for (int c = 0; c < d->defs.ncols; c++)
			if (stats(d)[c].point_tid_units > 0)
				need_minor(d, SIFT_COL_FORMAT_EXACTSLOT);
}

static void
d_caps(Derive *d)
{
	if (d->kinds & SIFT_CHOICE_CAP)
		sift_col_caps(d->index, add_from_caps, d);
}

/* One function derives several rows (the formula's list of heap addresses, the list you wrote, the list on a key-cut index): it has an entry for each, and runs once */
static const struct
{
	const char *id;
	void		(*fn) (Derive *d);
} derive_fn[] = {
	{"key-order", d_key_order},
	{"key-order-auto", d_key_order},
	{"key-order-scope", d_key_order},
	{"array-lengths", d_array_lengths},
	{"jsonb-paths", d_paths},
	{"array-elements", d_paths},
	{"key-lists", d_key_columns},
	{"cube-ranking", d_preagg},
	{"rows-rollup", d_rollup},
	{"key-cut-chosen", d_key_cut},
	{"point-directory-auto", d_point_tid},
	{"point-tid-explicit", d_point_tid},
	{"point-tid-key-cut", d_point_tid},
	{"order-array-auto", d_order_arrays},
	{"order-array-listed", d_order_arrays},
	{"order-layout", d_order_layout},
	{"sift-col-index-installed-two-statement-triggers", d_track},
	{"sift-col-index-installed-row-triggers", d_track},
	{"sift-col-index-does-not-track-changes", d_track},
	{"track-changes-concurrent", d_track},
	{"key-cut-advised", d_key_cut_advice},
	{"key-cut-hot-value", d_key_cut_hot},
	{"wildcard-keys-overflow", d_key_overflow},
	{"foreign-key-not-indexed", d_foreign_keys},
	{"hot-update-columns", d_hot_updates},
	{"point-columns-deprecated", d_deprecated},
	{"order-columns-deprecated", d_deprecated},
	{"cube-dim-no-histogram", d_caps},
	{"keys-bloom-filter", d_caps},
	{"keys-no-summary", d_caps},
	{"keys-no-list", d_caps},
	{"rollup-not-written", d_caps},
	{"cube-columns-left-out", d_caps},
};

static List *
derive(Relation heap, Relation index, int kinds, bool build, int *minor)
{
	Derive		d = {.heap = heap, .index = index, .kinds = kinds, .build = build, .defs = *sift_col_index_defs(index)};

	for (int i = 0; i < lengthof(derive_fn); i++)
	{
		bool		ran = false;

		for (int j = 0; j < i; j++)
			ran |= derive_fn[j].fn == derive_fn[i].fn;
		if (!ran)
			derive_fn[i].fn(&d);
	}
	if (sift_scope_specs(index) != NULL)
		need_minor(&d, SIFT_COL_FORMAT_SCOPE);
	if (minor)
		*minor = d.minor;
	return d.out;
}

/* ---- the summary ---- */

/* A value of at most VALUE_MAX characters: a list is cut at a comma and says how many names it leaves out */
static char *
short_value(const char *v)
{
	const char *cut = NULL;
	int			left = 0;

	if (strlen(v) <= VALUE_MAX)
		return pstrdup(v);
	for (const char *p = v; p - v < VALUE_MAX - 12; p++)
		if (p[0] == ',' && p[1] == ' ')
			cut = p;
	if (cut == NULL)
	{
		int			n = VALUE_MAX - 9;

		while (n > 0 && v[n] != ' ')
			n--;
		return psprintf("%.*s and more", n > 0 ? n : VALUE_MAX - 9, v);
	}
	for (const char *p = cut; *p; p++)
		left += *p == ',';
	return psprintf("%.*s, and %d more", (int) (cut - v), v, left);
}

/* "a, b" of the distinct names of the rows of one kind */
static char *
kind_names(List *found, int bit)
{
	StringInfoData out;
	List	   *seen = NIL;
	ListCell   *lc;

	initStringInfo(&out);
	foreach(lc, found)
	{
		const Found *f = lfirst(lc);
		bool		dup = false;
		ListCell   *s;

		if (kind_bit(f->row->kind) != bit)
			continue;
		foreach(s, seen)
			dup |= strcmp(lfirst(s), f->row->choice) == 0;
		if (dup)
			continue;
		seen = lappend(seen, (char *) f->row->choice);
		appendStringInfo(&out, "%s%s", out.len ? ", " : "", f->row->choice);
	}
	return out.data;
}

static int
kind_count(List *found, int bit)
{
	int			n = 0;
	ListCell   *lc;

	foreach(lc, found)
		n += kind_bit(((Found *) lfirst(lc))->row->kind) == bit;
	return n;
}

static bool
same_option(const char *a, const char *b)
{
	return a == b || (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/* Do two indexes cut, order and roll up their rows the same way? Then the structures that follow from the rows (order arrays, key lists, pre-aggregates) are the same bytes in both */
static bool
same_layout(Relation a, Relation b)
{
	return same_option(sift_col_options_key_order(a), sift_col_options_key_order(b)) && same_option(sift_col_options_key_cut(a), sift_col_options_key_cut(b)) &&
		same_option(sift_col_options_rollup_columns(a), sift_col_options_rollup_columns(b)) &&
		same_option(sift_col_options_order_array_columns(a), sift_col_options_order_array_columns(b)) &&
		same_option(sift_col_options_order_array_group(a), sift_col_options_order_array_group(b)) &&
		same_option(sift_col_options_order_layout(a), sift_col_options_order_layout(b));
}

/* The columns both indexes list, as "a, b, c" (NULL when none) */
static char *
shared_columns(Relation heap, Relation a, Relation b)
{
	StringInfoData names;

	initStringInfo(&names);
	for (int i = 0; i < a->rd_index->indnatts; i++)
	{
		AttrNumber	att = a->rd_index->indkey.values[i];
		bool		listed = false;

		for (int j = 0; att > 0 && j < b->rd_index->indnatts; j++)
			listed |= b->rd_index->indkey.values[j] == att;
		if (listed)
			appendStringInfo(&names, "%s%s", names.len ? ", " : "", quote_identifier(get_attname(RelationGetRelid(heap), att, false)));
	}
	return names.len ? names.data : NULL;
}

/*
 * The same structures twice: another sift_col index of the table with the same layout and the same predicate that lists some of the columns of this one stores their leaves
 * again, and one index over both lists reads each column at the cost it has in either. Said once for a build (the first such index), advice only.
 */
static void
say_same_layout(Relation heap, Relation index)
{
	const char *name = sift_col_final_name(index);

	foreach_oid(oid, RelationGetIndexList(heap))
	{
		Relation	other;
		char	   *cols = NULL;

		if (oid == RelationGetRelid(index))
			continue;
		other = index_open(oid, AccessShareLock);
		if (other->rd_rel->relam == index->rd_rel->relam && other->rd_index->indisvalid && strcmp(sift_col_final_name(other), name) != 0 &&
			equal(RelationGetIndexPredicate(other), RelationGetIndexPredicate(index)) && same_layout(other, index))
			cols = shared_columns(heap, other, index);
		if (cols != NULL)
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\" stores columns %s of index \"%s\", which has the same layout", name, cols, RelationGetRelationName(other)),
					 errhint("One index over the union of their columns is smaller at the same read cost: create it, drop both.")));
		index_close(other, AccessShareLock);
		if (cols != NULL)
			return;
	}
}

/*
 * The one NOTICE of a statement: "6 automatic choices (names) and 1 advice (names)", the values on at most two lines, and where the rest is read. client_min_messages = warning
 * hides it; the choices are made either way. A format that an older library cannot read, and a log that is not complete, are said apart and short.
 */
void
sift_col_choices_say(Relation heap, Relation index, int kinds, bool build)
{
	int			minor = 0;
	List	   *found = derive(heap, index, kinds, build, &minor);
	StringInfoData head,
				line,
				detail;
	int			lines = 1,
				shown = 0,
				total = 0;
	bool		full = false;
	ListCell   *lc;
	static const struct
	{
		int			bit;
		const char *one;
		const char *many;
		const char *label;
	} k[] = {
		{SIFT_CHOICE_AUTO, "automatic choice", "automatic choices", ""},
		{SIFT_CHOICE_ADVICE, "advice", "advice", "advice: "},
		{SIFT_CHOICE_CAP, "limit", "limits", "limit: "},
	};
	const char *name = sift_col_final_name(index);

	if (build)
		say_same_layout(heap, index);
	foreach(lc, found)
		total += kind_bit(((Found *) lfirst(lc))->row->kind) != SIFT_CHOICE_ACTION;
	if (total == 0)
		return;
	initStringInfo(&head);
	for (int i = 0; i < lengthof(k); i++)
	{
		int			n = kind_count(found, k[i].bit);

		if (n > 0)
			appendStringInfo(&head, "%s%d %s (%s)", head.len ? ", " : "", n, n == 1 ? k[i].one : k[i].many, kind_names(found, k[i].bit));
	}
	if (strlen(name) + head.len > SUMMARY_LINE - 20)
	{
		resetStringInfo(&head);
		for (int i = 0; i < lengthof(k); i++)
		{
			int			n = kind_count(found, k[i].bit);

			if (n > 0)
				appendStringInfo(&head, "%s%d %s", head.len ? ", " : "", n, n == 1 ? k[i].one : k[i].many);
		}
	}
	initStringInfo(&detail);
	initStringInfo(&line);
	/* advice and limits first: what may need an action is never the part that "and more" hides */
	for (int j = 0; j < lengthof(k); j++)
		foreach(lc, found)
		{
			const Found *f = lfirst(lc);
			const int	i = (j + 1) % lengthof(k);
			char	   *entry;

			if (kind_bit(f->row->kind) != k[i].bit || full)
				continue;
			entry = psprintf("%s%s = %s", k[i].label, f->row->choice, short_value(f->value));
			if (line.len > 0 && line.len + 2 + strlen(entry) > DETAIL_LINE)
			{
				if (lines == 2)
				{
					full = true;
					continue;
				}
				appendStringInfo(&detail, "%s;\n", line.data);
				resetStringInfo(&line);
				lines++;
			}
			appendStringInfo(&line, "%s%s", line.len ? "; " : "", entry);
			shown++;
		}
	if (shown < total)
		appendStringInfo(&line, "; and %d more", total - shown);
	appendStringInfoString(&detail, line.data);
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": %s", name, head.data),
			 errdetail_internal("%s", detail.data),
			 errhint("Reasons, costs and how to turn each off: SELECT * FROM sift.index_choices('%s');", name)));
	if (minor >= SIFT_COL_FORMAT_SCOPE && (kinds & SIFT_CHOICE_AUTO))
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\" has a scope: column format minor %d, which a library before %s cannot read", name, minor, minor_lib(minor)),
				 errhint("The scope option needs this format: build the index without it for the older library.")));
	else if (minor > 0 && (kinds & SIFT_CHOICE_AUTO))
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\" is column format minor %d, which a library before %s cannot read", name, minor, minor_lib(minor)),
				 errhint("SET sift.col_format_minor = 14 before the build to write a format the older library reads.")));
}

/* ---- the SQL functions ---- */

PG_FUNCTION_INFO_V1(sift_col_index_choices);

/* sift.index_choices(index): the choices, advice and limits of the index, with the reason, the cost measured on it, the typical cost and the command that turns each off */
Datum
sift_col_index_choices(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index = sift_col_open_index(relid, AccessShareLock);
	Relation	heap;
	ListCell   *lc;
	const char *name;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	InitMaterializedSRF(fcinfo, 0);
	name = quote_identifier(RelationGetRelationName(index));
	foreach(lc, derive(heap, index, SIFT_CHOICE_ALL, false, NULL))
	{
		const Found *f = lfirst(lc);
		Datum		v[9];
		bool		nulls[9] = {false};

		v[0] = CStringGetTextDatum(f->row->choice);
		v[1] = CStringGetTextDatum(f->row->kind);
		v[2] = CStringGetTextDatum(f->value);
		v[3] = f->cost ? CStringGetTextDatum(f->cost) : (Datum) 0;
		nulls[3] = f->cost == NULL;
		v[4] = f->row->typical[0] ? CStringGetTextDatum(f->row->typical) : (Datum) 0;
		nulls[4] = f->row->typical[0] == '\0';
		v[5] = CStringGetTextDatum(f->row->why);
		v[6] = CStringGetTextDatum(psprintf(f->row->off, name, name));
		v[7] = CStringGetTextDatum(f->row->id);
		v[8] = CStringGetTextDatum(f->row->doc);
		tuplestore_putvalues(((ReturnSetInfo *) fcinfo->resultinfo)->setResult, ((ReturnSetInfo *) fcinfo->resultinfo)->setDesc, v, nulls);
	}
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(sift_help);

static void
help_row(ReturnSetInfo *rsi, const char *const *cells, int n)
{
	Datum		v[10];
	bool		nulls[10];

	for (int c = 0; c < n; c++)
	{
		nulls[c] = cells[c][0] == '\0';
		v[c] = nulls[c] ? (Datum) 0 : CStringGetTextDatum(cells[c]);
	}
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
}

/*
 * sift.help(slug): the row of docs/messages.tsv of a message or choice, or the registry row of a refusal slug of sift.explain_why (kind refusal: the reason is the message,
 * the action the hint); the rows of docs/messages.tsv without a slug (the refusal slugs are in sift.refusal_slugs())
 */
Datum
sift_help(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		all = PG_ARGISNULL(0);
	const char *slug = all ? "" : text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		found = all;
	const char *rslug,
			   *text,
			   *action;
	bool		limit;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < lengthof(sift_msg_rows); i++)
	{
		const SiftMsgRow *r = &sift_msg_rows[i];
		const char *cells[] = {r->id, r->kind, r->level, r->message, r->detail, r->hint, r->decision, r->why, r->off[0] ? psprintf(r->off, "<index>", "<index>") : "", r->doc};

		if (!all && strcmp(r->id, slug) != 0)
			continue;
		found = true;
		help_row(rsi, cells, lengthof(cells));
	}
	for (int i = 1; !all && sift_agg_refusal_row(i, &rslug, &text, &action, &limit); i++)
	{
		const char *cells[] = {rslug, "refusal", "", text, "", action, "",
			limit ? "The query has more of something than a fixed limit of the scan." : "The scan does not answer this shape of query.", "", "USAGE.md#why-a-query-kept-the-normal-plan"};

		if (strcmp(rslug, slug) != 0)
			continue;
		found = true;
		help_row(rsi, cells, lengthof(cells));
	}
	if (!found)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("no message, choice or refusal \"%s\"", slug),
						errhint("SELECT id FROM sift.help() lists the messages and choices, SELECT slug FROM sift.refusal_slugs() the refusals.")));
	PG_RETURN_NULL();
}
