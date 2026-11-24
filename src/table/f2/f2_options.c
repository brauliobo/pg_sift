/*
 * table/f2/f2_options.c - the options of a sift_table of format major 2: the ten that are documented (the five of the storage and the five of the analytics that sift_col has
 * under the same names and grammar), and the thirteen of Format 1 that are still accepted so that the scripts of a table that was upgraded keep running. A table access method has
 * no reloptions before PostgreSQL 20: the rows are in sift.table_option_values.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#include "col/codec.h"
#include "col/f2/preagg_v2.h"
#include "table/f2/f2_tbl.h"

const char *const sift_tbl_f2_option_names[] = {
	"compression", "key_order", "paths", "dead_rewrite_pct", "auto_defrag", "scope", "connect", "preagg", "sketches", "skip_index",
};

const char *const sift_tbl_f2_hidden_option_names[] = {
	"write_buffer_limit", "wide_rows", "inline_promote", "async_promote", "backpressure", "hot_update", "bulk_groups", "dict_lookup", "key_table",
	"auto_defrag_free_pct", "auto_defrag_budget", "auto_defrag_coalesce", "auto_defrag_indexes",
};

typedef enum OptKind
{
	OPT_TEXT,
	OPT_BOOL,
	OPT_PCT,
	OPT_SIZE
} OptKind;

static const OptKind hidden_kinds[] = {
	OPT_SIZE, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_BOOL, OPT_PCT, OPT_SIZE, OPT_BOOL, OPT_BOOL,
};

static const OptKind documented_kinds[] = {
	OPT_TEXT, OPT_TEXT, OPT_TEXT, OPT_PCT, OPT_BOOL, OPT_TEXT, OPT_TEXT, OPT_TEXT, OPT_TEXT, OPT_TEXT,
};

StaticAssertDecl(lengthof(sift_tbl_f2_option_names) == SIFT_TBL_F2_OPTIONS, "ten documented sift_table options");
StaticAssertDecl(lengthof(sift_tbl_f2_hidden_option_names) == SIFT_TBL_F2_HIDDEN_OPTIONS, "thirteen hidden Format 1 options");
StaticAssertDecl(lengthof(hidden_kinds) == SIFT_TBL_F2_HIDDEN_OPTIONS && lengthof(documented_kinds) == SIFT_TBL_F2_OPTIONS, "a kind for every option");

int
sift_tbl_f2_option_count(void)
{
	return lengthof(sift_tbl_f2_option_names);
}

int
sift_tbl_f2_hidden_option_count(void)
{
	return lengthof(sift_tbl_f2_hidden_option_names);
}

static int
find_name(const char *const *names, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (strcmp(names[i], name) == 0)
			return i;
	return -1;
}

static bool
parse_pct(const char *value, int lo, int hi)
{
	char	   *end;
	long		v = strtol(value, &end, 10);

	return *value != '\0' && *end == '\0' && v >= lo && v <= hi;
}

static bool
value_ok(OptKind kind, const char *value)
{
	bool		b;

	switch (kind)
	{
		case OPT_TEXT:
			return *value != '\0';
		case OPT_BOOL:
			return parse_bool(value, &b);
		case OPT_PCT:
			return parse_pct(value, 0, 100);
		case OPT_SIZE:
			return parse_pct(value, 0, INT_MAX);
	}
	return false;
}

static bool
policy_ok(const char *policy)
{
	SiftCcPolicy p;

	return sift_cc_policy_parse(policy, &p) == NULL;
}

/* An item of the list: a policy, "column=policy" as sift_col reads it, or "column:policy" as Format 1 did */
static bool
compression_item_ok(const char *item)
{
	const char *eq = strchr(item, '=');
	const char *colon = strchr(item, ':');

	if (eq != NULL)
		return eq > item && policy_ok(eq + 1);
	return policy_ok(item) || (colon != NULL && colon > item && policy_ok(colon + 1));
}

/* "policy" or "policy; column=policy; ..." (the grammar of sift_col, col/codec.h), or the comma list of Format 1 */
static bool
compression_ok(const char *value)
{
	List	   *items;
	ListCell   *lc;
	char	   *copy = pstrdup(value);
	bool		ok = SplitIdentifierString(copy, strchr(value, ';') != NULL ? ';' : ',', &items);

	if (!ok)
		return false;
	foreach(lc, items)
		ok = ok && compression_item_ok(lfirst(lc));
	list_free(items);
	pfree(copy);
	return ok;
}

static bool
sift_tbl_f2_option_check(const char *name, const char *value, const char **why)
{
	int			i = find_name(sift_tbl_f2_option_names, lengthof(sift_tbl_f2_option_names), name);
	int			h = find_name(sift_tbl_f2_hidden_option_names, lengthof(sift_tbl_f2_hidden_option_names), name);

	if (i < 0 && h < 0)
	{
		*why = "no such option";
		return false;
	}
	if (!value_ok(i >= 0 ? documented_kinds[i] : hidden_kinds[h], value))
	{
		*why = "the value does not fit the type of the option";
		return false;
	}
	if (i == 0 && !compression_ok(value))
	{
		*why = "compression is a policy (auto, none, lz4[:level], zstd[:level]) or a list \"policy; column=policy; ...\"";
		return false;
	}
	return true;
}

Datum
sift_tbl_f2_option_names_array(void)
{
	Datum		names[SIFT_TBL_F2_OPTIONS];

	for (int i = 0; i < SIFT_TBL_F2_OPTIONS; i++)
		names[i] = CStringGetTextDatum(sift_tbl_f2_option_names[i]);
	return PointerGetDatum(construct_array_builtin(names, SIFT_TBL_F2_OPTIONS, TEXTOID));
}

/* What a table that sets nothing has: the two options that a setting of the cluster stands for follow it, the others are empty (the analytics options) or ignored (those of Format 1); a table of Format 1 follows no setting */
char *
sift_tbl_f2_option_default(const char *name, const char **setting)
{
	*setting = NULL;
	if (strcmp(name, "dead_rewrite_pct") == 0)
	{
		*setting = "sift.dead_rewrite_pct";
		return psprintf("%d", sift_dead_rewrite_pct);
	}
	if (strcmp(name, "auto_defrag") == 0)
	{
		*setting = "sift.async_maintenance";
		return pstrdup(sift_async_maintenance ? "on" : "off");
	}
	return pstrdup("");
}

bool
sift_tbl_f2_option_known(const char *name)
{
	return find_name(sift_tbl_f2_option_names, lengthof(sift_tbl_f2_option_names), name) >= 0 ||
		find_name(sift_tbl_f2_hidden_option_names, lengthof(sift_tbl_f2_hidden_option_names), name) >= 0;
}

/* The cubes of preagg name columns of the table */
static bool
preagg_ok(Relation rel, const char *value, const char **why)
{
	SiftPreaggDecl decl;
	bool		off;

	*why = sift_preagg_decl_parse(value, sift_f2_store_open(rel)->schema, true, &decl, &off);
	return *why == NULL;
}

bool
sift_tbl_f2_option_check_set(Relation rel, const char *name, const char *value, const char **why)
{
	if (!sift_tbl_f2_option_check(name, value, why))
		return false;
	if (strcmp(name, "preagg") == 0 && !preagg_ok(rel, value, why))
		return false;
	if (find_name(sift_tbl_f2_hidden_option_names, lengthof(sift_tbl_f2_hidden_option_names), name) >= 0 && !IsBinaryUpgrade)
		ereport(NOTICE, (errmsg("option \"%s\" of sift_table \"%s\" is an option of Format 1 that Format 2.0 ignores", name, RelationGetRelationName(rel)),
						 errhint("It is kept so that scripts written for pg_sift 5.1 run; take it out of them.")));
	return true;
}

/* ---- the rows of sift.table_option_values ---- */

static Oid
config_relid(void)
{
	Oid			nsp = get_namespace_oid("sift", true);

	return OidIsValid(nsp) ? get_relname_relid("table_option_values", nsp) : InvalidOid;
}

static void
store_value(SiftF2StoreOpts *o, MemoryContext cx, const char *name, const char *value)
{
	int			i = find_name(sift_tbl_f2_option_names, lengthof(sift_tbl_f2_option_names), name);
	bool		b;

	switch (i)
	{
		case 0:
			o->compression = MemoryContextStrdup(cx, value);
			break;
		case 1:
			o->key_order = MemoryContextStrdup(cx, value);
			break;
		case 2:
			o->paths = MemoryContextStrdup(cx, value);
			break;
		case 3:
			o->dead_rewrite_pct = atoi(value);
			break;
		case 4:
			if (parse_bool(value, &b))
				o->auto_maint = b;
			break;
		case 5:
			o->scope = MemoryContextStrdup(cx, value);
			break;
		case 6:
			o->connect = MemoryContextStrdup(cx, value);
			break;
		case 7:
			o->preagg = MemoryContextStrdup(cx, value);
			break;
		case 8:
			o->sketches = MemoryContextStrdup(cx, value);
			break;
		case 9:
			o->skip_index = MemoryContextStrdup(cx, value);
			break;
		default:
			break;
	}
}

static void
read_rows(Relation rel, SiftF2StoreOpts *o, MemoryContext cx)
{
	Oid			cfgid = config_relid();
	Relation	cfg;
	List	   *indexes;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	if (!OidIsValid(cfgid))
		return;
	cfg = table_open(cfgid, AccessShareLock);
	indexes = RelationGetIndexList(cfg);
	SIFT_INVARIANT(indexes != NIL, "sift.table_option_values has no index");
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(cfg, linitial_oid(indexes), true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		char	   *name = TextDatumGetCString(heap_getattr(tup, 2, RelationGetDescr(cfg), &isnull));
		char	   *value = TextDatumGetCString(heap_getattr(tup, 3, RelationGetDescr(cfg), &isnull));

		store_value(o, cx, name, value);
	}
	systable_endscan(scan);
	list_free(indexes);
	table_close(cfg, AccessShareLock);
}

void
sift_tbl_f2_load_options(Relation rel, SiftF2Store *store, MemoryContext cx)
{
	SiftF2StoreOpts *o = &store->opts;

	o->key_order = o->paths = o->compression = o->scope = o->connect = o->preagg = o->sketches = o->skip_index = MemoryContextStrdup(cx, "");
	o->auto_maint = true;
	o->vacuum_truncate = true;
	o->track_changes = true;
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP && RelationGetNumberOfBlocks(rel) == 0)
		sift_tbl_f2_init_layout(rel);
	read_rows(rel, o, cx);
	sift_tbl_f2_define_leaves(rel, store);
}

/* ---- the names of columns in an option ---- */

typedef struct Token
{
	const char *start;
	int			len;
	bool		quoted;
} Token;

static bool
next_identifier(const char **p, Token *t)
{
	const char *s = *p;

	while (*s != '\0')
	{
		if (*s == '"')
		{
			const char *e = s + 1;

			while (*e != '\0' && !(*e == '"' && e[1] != '"'))
				e += (*e == '"') ? 2 : 1;
			*t = (Token) {s, (int) (e - s) + (*e == '"'), true};
			*p = *e == '"' ? e + 1 : e;
			return true;
		}
		if (isalpha((unsigned char) *s) || *s == '_')
		{
			const char *e = s;

			while (isalnum((unsigned char) *e) || *e == '_' || *e == '$')
				e++;
			*t = (Token) {s, (int) (e - s), false};
			*p = e;
			return true;
		}
		s++;
	}
	return false;
}

static bool
token_is(const Token *t, const char *name)
{
	if (t->quoted)
	{
		char	   *plain = pnstrdup(t->start + 1, t->len - 2);
		bool		same = strcmp(plain, name) == 0;

		pfree(plain);
		return same;
	}
	return (int) strlen(name) == t->len && pg_strncasecmp(t->start, name, t->len) == 0;
}

bool
sift_tbl_f2_option_names_column(const char *spec, const char *colname)
{
	Token		t;

	for (const char *p = spec; next_identifier(&p, &t);)
		if (token_is(&t, colname))
			return true;
	return false;
}

char *
sift_tbl_f2_options_rename_column(const char *spec, const char *oldname, const char *newname)
{
	StringInfoData out;
	Token		t;
	const char *p = spec,
			   *copied = spec;

	initStringInfo(&out);
	while (next_identifier(&p, &t))
	{
		if (!token_is(&t, oldname))
			continue;
		appendBinaryStringInfo(&out, copied, t.start - copied);
		appendStringInfoString(&out, quote_identifier(newname));
		copied = t.start + t.len;
	}
	appendStringInfoString(&out, copied);
	return out.data;
}

/* ---- key_order ---- */

static bool
leaf_sortable(Form_pg_attribute att)
{
	return sift_f2_attr_has_leaf(att) && att->attbyval && att->atttypid != FLOAT4OID && att->atttypid != FLOAT8OID;
}

static int
auto_key_attr(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Oid			pk;

	list_free(RelationGetIndexList(rel));
	pk = rel->rd_pkindex;
	if (OidIsValid(pk))
	{
		Relation	idx = index_open(pk, AccessShareLock);
		int			first = idx->rd_index->indkey.values[0];

		index_close(idx, AccessShareLock);
		if (first > 0 && leaf_sortable(TupleDescAttr(desc, first - 1)))
			return first;
	}
	for (int a = 1; a <= desc->natts; a++)
	{
		Oid			t = TupleDescAttr(desc, a - 1)->atttypid;

		if (sift_f2_attr_has_leaf(TupleDescAttr(desc, a - 1)) && (t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID))
			return a;
	}
	return 0;
}

static void
add_key(SiftF2Store *store, int attno, uint8 flags)
{
	if (store->nkey >= SIFT_KEY_ORDER_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("key_order names more than %d columns", SIFT_KEY_ORDER_MAX),
						errhint("Name at most %d columns in key_order.", SIFT_KEY_ORDER_MAX)));
	store->spec.sort_flags[store->nkey] = flags;
	store->key_cols[store->nkey++] = store->att_first[attno];
}

void
sift_tbl_f2_apply_key_order(Relation rel, SiftF2Store *store)
{
	const char *spec = store->opts.key_order;
	Token		t;
	const char *p = spec;

	store->nkey = 0;
	if (strcmp(spec, "off") == 0)
		return;
	if (*spec == '\0' || strcmp(spec, "auto") == 0)
	{
		int			a = auto_key_attr(rel);

		if (a > 0)
			add_key(store, a, 0);
		return;
	}
	while (next_identifier(&p, &t))
	{
		AttrNumber	a;
		char	   *name;

		if (token_is(&t, "desc") || token_is(&t, "asc") || token_is(&t, "nulls") || token_is(&t, "first") || token_is(&t, "last"))
		{
			if (store->nkey > 0 && token_is(&t, "desc"))
				store->spec.sort_flags[store->nkey - 1] |= SIFT_KEY_DESC;
			if (store->nkey > 0 && token_is(&t, "first"))
				store->spec.sort_flags[store->nkey - 1] |= SIFT_KEY_NULLS_FIRST;
			continue;
		}
		name = t.quoted ? pnstrdup(t.start + 1, t.len - 2) : pnstrdup(t.start, t.len);
		a = get_attnum(RelationGetRelid(rel), name);
		if (a > 0 && sift_f2_attr_is_virtual(TupleDescAttr(RelationGetDescr(rel), a - 1)))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("key_order names the column \"%s\", a virtual generated column, which table \"%s\" does not store", name, RelationGetRelationName(rel)),
							errhint("Name a stored column in key_order.")));
		if (a <= 0 || store->att_first[a] < 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("key_order names the column \"%s\", which table \"%s\" does not have", name, RelationGetRelationName(rel))));
		add_key(store, a, 0);
	}
}
