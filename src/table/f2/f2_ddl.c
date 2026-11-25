/*
 * table/f2/f2_ddl.c - a sift_table follows ALTER TABLE without rewriting a block.
 *
 * Every part names its columns by the permanent attribute number of PostgreSQL (SEC_SCHEMA with SIFT_SCHEMA_EXT_ATTNUM), so a part written under an older tuple descriptor is read
 * through a map from the leaves of today to its chunks: a column added later is the constant of its fast default (getmissingattr) and costs no byte, a dropped column is a chunk
 * no map names, a rename changes nothing stored. A compaction rewrites an old part under the current schema. Also here: the leaves of a table, and the checks around the DDL.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"

#include "col/f2/f2_store_schema.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"
#include "core/damage.h"
#include "core/meta_peek.h"

static bool
is_time_type(Oid t)
{
	return t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static SiftPartColIdentity
identity_of(Relation rel, const SiftF2Store *store, int leaf)
{
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), store->leaf_attno[leaf] - 1);

	return (SiftPartColIdentity) {
		.attnum = store->leaf_attno[leaf],
		.phys_kind = (uint8) store->kinds[leaf],
		.phys_hint = store->hints[leaf],
		.phys_scale = store->scales[leaf],
		.flags = store->headerless && sift_f2_attr_bare_bytes(att) ? SIFT_COLID_HEADERLESS : 0,
	};
}

SiftPartColIdentity *
sift_tbl_f2_column_identities(Relation rel, const SiftF2Store *store, int *n)
{
	SiftPartColIdentity *ids = MemoryContextAlloc(store->cx, sizeof(SiftPartColIdentity) * Max(store->ncols, 1));

	for (int c = 0; c < store->ncols; c++)
		ids[c] = identity_of(rel, store, c);
	*n = store->ncols;
	return ids;
}

uint32
sift_tbl_f2_identities_hash(const SiftPartColIdentity *ids, int n)
{
	return DatumGetUInt32(hash_any((const unsigned char *) ids, n * sizeof(SiftPartColIdentity)));
}

/* ---- the leaves of a table ---- */

/* A Format 2.0 table made by this build says so in its metapage; one that has no block yet is made with the flags of the host; a Format 1 table (the source of sift.upgrade_table) has none */
static bool
native_strings(Relation rel)
{
	SiftFormatStatus status = sift_meta_peek_format(rel).status;

	return status == SIFT_FMT_EMPTY || (status != SIFT_FMT_V1 && (sift_col_meta_read(rel).common.compat_flags & SIFT_COMPAT_NATIVE_STRINGS) != 0);
}

static void
define_time_columns(Relation rel, SiftF2Store *store)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			t = 0;

	for (int a = 1; a <= desc->natts && t < SIFT_MANIFEST_TIME_COLS; a++)
	{
		Oid			type = TupleDescAttr(desc, a - 1)->atttypid;

		if (!sift_f2_attr_has_leaf(TupleDescAttr(desc, a - 1)) || !is_time_type(type))
			continue;
		store->spec.time_cols[t] = store->att_first[a] + 1;
		store->time_unit[t++] = type == DATEOID ? USECS_PER_DAY : 1;
	}
	store->spec.time_col = store->spec.time_cols[0];
}

static bool
has_partial_or_expr_unique(Relation rel)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));

		if (!HeapTupleIsValid(tup))
			continue;
		found = found || (((Form_pg_index) GETSTRUCT(tup))->indisunique &&
						  (!heap_attisnull(tup, Anum_pg_index_indpred, NULL) || !heap_attisnull(tup, Anum_pg_index_indexprs, NULL)));
		ReleaseSysCache(tup);
	}
	list_free(indexes);
	return found;
}

void
sift_tbl_f2_define_leaves(Relation rel, SiftF2Store *store)
{
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = store->cx;
	SiftTblF2Extra *x = MemoryContextAllocZero(cx, sizeof(SiftTblF2Extra));
	MemoryContext old;
	int			n = 0;

	store->host_data = x;
	store->natts = desc->natts;
	store->att_first = MemoryContextAlloc(cx, sizeof(int) * (desc->natts + 1));
	store->leaf_attno = MemoryContextAlloc(cx, sizeof(int16) * Max(desc->natts, 1));
	store->kinds = MemoryContextAlloc(cx, sizeof(SiftCellKind) * Max(desc->natts, 1));
	store->hints = MemoryContextAllocZero(cx, Max(desc->natts, 1));
	store->scales = MemoryContextAllocZero(cx, Max(desc->natts, 1));
	store->att_first[0] = -1;
	store->headerless = native_strings(rel);
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		store->att_first[a] = -1;
		if (!sift_f2_attr_has_leaf(att))
			continue;
		store->att_first[a] = n;
		store->leaf_attno[n] = a;
		store->kinds[n] = sift_f2_kind_of_attribute(att);
		if (sift_f2_attr_num64(att, &store->scales[n]))
			store->hints[n] = SIFT_CHINT_NUM64;
		else if (store->headerless && att->atttypid == UUIDOID)
			store->hints[n] = SIFT_CHINT_UUID;
		x->has_wide = x->has_wide || (store->kinds[n] == SIFT_CELL_STR && !att->attbyval && att->attlen == -1);
		n++;
		store->has_jsonb = store->has_jsonb || att->atttypid == JSONBOID;
	}
	if (n > SIFT_TBL_F2_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_TOO_MANY_COLUMNS), errmsg("table \"%s\" has %d columns, the most a sift_table keeps is %d", RelationGetRelationName(rel), n, SIFT_TBL_F2_MAX_COLS)));
	store->ncols = n;

	sift_tbl_f2_apply_key_order(rel, store);
	store->spec = (SiftPartSpec) {
		.ncols = n,.kinds = store->kinds,.nsort = store->nkey,.sort_cols = store->key_cols,.hints = store->hints,.scales = store->scales,
	};
	define_time_columns(rel, store);
	store->spec.col_ids = sift_tbl_f2_column_identities(rel, store, &store->spec.ncol_ids);
	store->schema_hash = sift_tbl_f2_identities_hash(store->spec.col_ids, store->spec.ncol_ids);
	sift_f2_store_hydrate(rel, store);
	sift_tbl_f2_defaults_attach(rel, store);
	old = MemoryContextSwitchTo(cx);
	x->key_attrs = bms_copy(RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY));
	x->hot_blocking_attrs = bms_copy(RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_HOT_BLOCKING));
	x->summarized_attrs = bms_copy(RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_SUMMARIZED));
	x->protected_attrs = sift_col_delta_protected_attrs(store);
	x->has_partial_or_expr_unique = has_partial_or_expr_unique(rel);
	MemoryContextSwitchTo(old);
}

/* The key order of the table a rewrite copies from: the new relation has neither its options nor its primary key yet, and its parts must sort as the old ones do */
void
sift_tbl_f2_adopt_layout(SiftF2Store *to, const SiftF2Store *from)
{
	to->nkey = from->nkey;
	memcpy(to->key_cols, from->key_cols, sizeof(to->key_cols));
	memcpy(to->spec.sort_flags, from->spec.sort_flags, sizeof(to->spec.sort_flags));
	to->spec.nsort = from->nkey;
}

/* ---- SEC_SCHEMA ---- */

static inline Size
align8(Size n)
{
	return (n + 7) & ~(Size) 7;
}

const SiftPartColIdentity *
sift_tbl_f2_schema_identities(const char *schema, uint64 len, int *n)
{
	uint16		hdr[2];
	Size		off;
	int			nsort;

	memcpy(hdr, schema, sizeof(hdr));
	if ((hdr[1] & SIFT_SCHEMA_EXT_ATTNUM) == 0)
		return NULL;
	nsort = hdr[1] & 0x00FF;
	off = ((4 + hdr[0] + 1) & ~(Size) 1) + (Size) nsort * 3;
	if ((hdr[1] & SIFT_SCHEMA_EXT_PROMO) != 0)
	{
		uint16		npromo;

		off = align8(off);
		memcpy(&npromo, schema + off, sizeof(npromo));
		off = align8(off + 2) + (Size) npromo * sizeof(SiftColPromo);
	}
	off = align8(off);
	if (off + (Size) hdr[0] * sizeof(SiftPartColIdentity) > len)
		SIFT_DAMAGE_OTHER_DATA(errmsg("the schema section of a part of a sift_table is shorter than its columns"));
	*n = hdr[0];
	return (const SiftPartColIdentity *) (schema + off);
}

/* ---- the map from the leaves of today to the chunks of a part ---- */

static void
set_missing(SiftF2ColMap *m, const SiftF2Store *store, Relation rel, int c, MemoryContext cx)
{
	int			a = store->leaf_attno[c];
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), a - 1);
	Datum		raw;
	bool		isnull;
	MemoryContext old = MemoryContextSwitchTo(cx);

	raw = getmissingattr(RelationGetDescr(rel), a, &isnull);
	m->mode[c] = SIFT_CMAP_MISSING;
	m->phys_col[c] = -1;
	m->phys_kind[c] = (uint8) store->kinds[c];
	m->has_missing = true;
	m->missing_val[c] = isnull ? raw : datumCopy(raw, att->attbyval, att->attlen);
	m->missing_null[c] = isnull;
	m->missing_cell[c].isnull = isnull;
	if (!isnull)
		sift_f2_cell_of_datum(store, att, m->missing_val[c], cx, false, &m->missing_cell[c]);
	MemoryContextSwitchTo(old);
}

static void
set_found(SiftF2ColMap *m, const SiftF2Store *store, const SiftPartColIdentity *id, int phys, int c)
{
	m->phys_col[c] = phys;
	m->leaf_of_phys[phys] = c;
	m->phys_kind[c] = id->phys_kind;
	m->old_scale[c] = id->phys_scale;
	m->mode[c] = id->phys_scale != store->scales[c] ? SIFT_CMAP_WIDEN_N64 : id->phys_hint != store->hints[c] ? SIFT_CMAP_RECAST : SIFT_CMAP_DIRECT;
}

SiftF2ColMap *
sift_tbl_f2_build_col_map(const SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx)
{
	uint64		len;
	char	   *schema = sift_part_read_section(rel, part, SEC_SCHEMA, &len);
	const SiftPartColIdentity *ids;
	int			nids;
	SiftF2ColMap *m;
	int16	   *part_attnums,
			   *phys;

	if (schema == NULL)
		return NULL;
	ids = sift_tbl_f2_schema_identities(schema, len, &nids);
	if (ids == NULL || sift_tbl_f2_identities_hash(ids, nids) == store->schema_hash)
		return NULL;
	part_attnums = palloc(sizeof(int16) * Max(nids, 1));
	phys = palloc(sizeof(int16) * Max(store->ncols, 1));
	for (int i = 0; i < nids; i++)
		part_attnums[i] = ids[i].attnum;
	sift_defaults_map_columns(part_attnums, nids, store->leaf_attno, store->ncols, phys);
	m = MemoryContextAllocZero(cx, sizeof(SiftF2ColMap));
	m->cur_ncols = store->ncols;
	m->phys_ncols = nids;
	m->leaf_of_phys = MemoryContextAlloc(cx, sizeof(int16) * Max(nids, 1));
	memset(m->leaf_of_phys, -1, sizeof(int16) * Max(nids, 1));
	m->phys_col = MemoryContextAlloc(cx, sizeof(int16) * store->ncols);
	m->mode = MemoryContextAlloc(cx, sizeof(SiftColMapMode) * store->ncols);
	m->old_scale = MemoryContextAllocZero(cx, store->ncols);
	m->phys_kind = MemoryContextAllocZero(cx, store->ncols);
	m->missing_val = MemoryContextAllocZero(cx, sizeof(Datum) * store->ncols);
	m->missing_null = MemoryContextAllocZero(cx, sizeof(bool) * store->ncols);
	m->missing_cell = MemoryContextAllocZero(cx, sizeof(SiftCell) * store->ncols);
	for (int c = 0; c < store->ncols; c++)
		if (phys[c] < 0)
			set_missing(m, store, rel, c, cx);
		else
			set_found(m, store, &ids[phys[c]], phys[c], c);
	pfree(part_attnums);
	pfree(phys);
	pfree(schema);
	return m;
}

/* ---- the fence and the checks of the DDL ---- */

void
sift_tbl_f2_flush_all_slabs(Relation rel)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftManifestRead mr;

	sift_stage_flush(rel);
	mr = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < mr.n; i++)
		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber)
			sift_l0_slab_flush(rel, mr.entries[i].part_id, mr.entries[i].key_lo, &store->spec);
}

void
sift_tbl_f2_ddl_prepare(Relation rel)
{
	sift_tbl_f2_flush_all_slabs(rel);
}

static const struct
{
	const char *name;
	size_t		off;
}			text_opts[] = {
	{"key_order", offsetof(SiftF2StoreOpts, key_order)}, {"scope", offsetof(SiftF2StoreOpts, scope)}, {"connect", offsetof(SiftF2StoreOpts, connect)},
	{"paths", offsetof(SiftF2StoreOpts, paths)}, {"preagg", offsetof(SiftF2StoreOpts, preagg)}, {"sketches", offsetof(SiftF2StoreOpts, sketches)},
	{"skip_index", offsetof(SiftF2StoreOpts, skip_index)}, {"compression", offsetof(SiftF2StoreOpts, compression)},
};

#define TEXT_OPTS	((int) lengthof(text_opts))
#define TEXT_OPT(o, i)	(*(char *const *) ((const char *) (o) + text_opts[i].off))

static void
check_drop_column(Relation rel, const SiftF2StoreOpts *o, const char *column)
{
	for (int i = 0; i < TEXT_OPTS; i++)
		if (sift_tbl_f2_option_names_column(TEXT_OPT(o, i), column))
			ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST), errmsg("cannot drop column \"%s\" of table \"%s\": the option %s names it", column, RelationGetRelationName(rel), text_opts[i].name),
							errhint("Update or reset the option first with SELECT sift.table_reset_option('%s'::regclass, '%s').", RelationGetRelationName(rel), text_opts[i].name)));
}

void
sift_tbl_f2_ddl_before(Relation rel, AlterTableStmt *stmt)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	bool		fence = false;
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		switch (cmd->subtype)
		{
			case AT_DropColumn:
				check_drop_column(rel, &store->opts, cmd->name);
				fence = true;
				break;
			case AT_AddColumn:
			case AT_AlterColumnType:
			case AT_SetTableSpace:
			case AT_SetLogged:
			case AT_SetUnLogged:
				fence = true;
				break;
			default:
				break;
		}
	}
	if (fence)
		sift_tbl_f2_ddl_prepare(rel);
}

void
sift_tbl_f2_guard_publication(Relation rel)
{
	(void) sift_tbl_check_publication(rel);
}

void
sift_tbl_f2_ddl_after(Relation rel, Node *parsetree)
{
	bool		moved = IsA(parsetree, AlterObjectSchemaStmt) && castNode(AlterObjectSchemaStmt, parsetree)->objectType == OBJECT_TABLE;

	if (IsA(parsetree, AlterTableStmt))
	{
		ListCell   *lc;

		foreach(lc, castNode(AlterTableStmt, parsetree)->cmds)
			moved = moved || lfirst_node(AlterTableCmd, lc)->subtype == AT_AttachPartition;
	}
	if (moved)
		sift_tbl_f2_guard_publication(rel);
}

/* The options of the tables are rows of a catalog of the extension that only its owner writes: the table owner who renames a column is let write the rows that name it */
static Oid
option_table_owner(void)
{
	Oid			relid = get_relname_relid("table_option_values", get_namespace_oid("sift", false));
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "cache lookup failed for relation %u", relid);
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	ReleaseSysCache(tup);
	return owner;
}

void
sift_tbl_f2_ddl_rename_column(Relation rel, const char *oldname, const char *newname)
{
	const SiftF2StoreOpts *o = &sift_f2_store_open(rel)->opts;
	Oid			argtypes[3] = {TEXTOID, OIDOID, TEXTOID};
	Oid			uid;
	int			sec;

	GetUserIdAndSecContext(&uid, &sec);
	SetUserIdAndSecContext(option_table_owner(), sec | SECURITY_LOCAL_USERID_CHANGE);
	SPI_connect();
	for (int i = 0; i < TEXT_OPTS; i++)
	{
		Datum		args[3];
		int			updated;

		if (!sift_tbl_f2_option_names_column(TEXT_OPT(o, i), oldname))
			continue;
		args[0] = CStringGetTextDatum(sift_tbl_f2_options_rename_column(TEXT_OPT(o, i), oldname, newname));
		args[1] = ObjectIdGetDatum(RelationGetRelid(rel));
		args[2] = CStringGetTextDatum(text_opts[i].name);
		updated = SPI_execute_with_args("UPDATE sift.table_option_values SET value = $1 WHERE rel::pg_catalog.oid = $2 AND name = $3", 3, argtypes, args, NULL, false, 0);
		SIFT_INVARIANT(updated == SPI_OK_UPDATE, "could not rename the column in the options of table \"%s\"", RelationGetRelationName(rel));
	}
	SPI_finish();
	SetUserIdAndSecContext(uid, sec);
	CacheInvalidateRelcacheByRelid(RelationGetRelid(rel));
}
