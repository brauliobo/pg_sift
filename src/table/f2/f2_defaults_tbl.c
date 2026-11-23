/*
 * table/f2/f2_defaults_tbl.c - the fast defaults and the dropped attributes of a sift_table as SEC_SCHEMA_DEFAULTS (col/f2/f2_defaults.h).
 *
 * ALTER TABLE ADD COLUMN ... DEFAULT <constant> and DROP COLUMN change the catalog and no part: a part written before the column reads it as the constant of attmissingval (or null), a dropped
 * column is a chunk that no current column names, and the next compaction rewrites the part under the tuple descriptor of the day (col/f2/compact.c, through sift_tbl_f2_build_col_remap). Each part
 * written afterwards carries the defaults and the dropped attributes it was written under, stamped in its SEC_SCHEMA, so that a part names what its neighbours may not have been written with.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/part_format.h"
#include "table/f2/f2_tbl.h"
#include "core/damage.h"

StaticAssertDecl(SEC_SCHEMA_DEFAULTS == SEC_SCHEMA_DEFAULTS_TAG, "SEC_SCHEMA_DEFAULTS is section 0x0305");

static SiftDefaultKind
default_kind(SiftCellKind kind)
{
	return kind == SIFT_CELL_INT ? SIFT_DEFAULT_INT : kind == SIFT_CELL_F8 ? SIFT_DEFAULT_F8 : SIFT_DEFAULT_STR;
}

static SiftDefaultsStatus
add_missing(const SiftF2Store *store, SiftSchemaDefaults *d, TupleDesc desc, int attnum, MemoryContext tmp)
{
	Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
	SiftDefaultVal dv = {.attnum = attnum,.kind = default_kind(sift_f2_kind_of_attribute(att))};
	Datum		value = getmissingattr(desc, attnum, &dv.isnull);

	if (!dv.isnull)
	{
		SiftCell	cell = {0};

		sift_f2_cell_of_datum(store, att, value, tmp, false, &cell);
		dv.i = cell.i;
		dv.f = cell.f;
		dv.s = (const uint8 *) cell.s;
		dv.slen = cell.slen;
	}
	return sift_defaults_add(d, &dv);
}

static SiftSchemaDefaults *
defaults_of_desc(Relation rel, const SiftF2Store *store)
{
	TupleDesc	desc = RelationGetDescr(rel);
	SiftSchemaDefaults *d = sift_defaults_new();
	/* MEM[MEM-TBL-17]: transient table defaults decode ctx (ALLOCSET_SMALL_SIZES) */
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift table defaults", ALLOCSET_SMALL_SIZES);

	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		SiftDefaultsStatus st = SIFT_DEFAULTS_OK;

		if (att->attisdropped)
			st = sift_defaults_add_dropped(d, a);
		else if (att->atthasmissing && sift_f2_attr_has_leaf(att))
			st = add_missing(store, d, desc, a, tmp);
		if (st != SIFT_DEFAULTS_OK)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the default of column %d of sift_table \"%s\" cannot be kept in its parts: %s", a, RelationGetRelationName(rel),
																			sift_defaults_status_text(st)),
							errhint("Set the default on a column of fewer or smaller defaults, or rewrite the table with VACUUM FULL so that no part keeps an old one.")));
	}
	MemoryContextDelete(tmp);
	return d;
}

void
sift_tbl_f2_defaults_attach(Relation rel, SiftF2Store *store)
{
	MemoryContext old = MemoryContextSwitchTo(store->cx);
	SiftSchemaDefaults *d = defaults_of_desc(rel, store);
	uint8	   *bytes;
	uint64		len;

	if (d->n > 0 || d->ndropped > 0)
	{
		if (sift_defaults_encode(d, &bytes, &len) != SIFT_DEFAULTS_OK)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the defaults of sift_table \"%s\" are larger than a part keeps", RelationGetRelationName(rel)),
							errhint("Drop a column that has a large default, or rewrite the table with VACUUM FULL so that no part keeps the old ones.")));
		sift_tbl_extra(store)->defaults = d;
		store->spec.defaults = bytes;
		store->spec.defaults_len = len;
		store->spec.defaults_stamp = d->stamp;
	}
	else
		sift_defaults_free(d);
	MemoryContextSwitchTo(old);
}

SiftSchemaDefaults *
sift_tbl_f2_part_defaults(Relation rel, const SiftPart *part)
{
	uint64		len;
	char	   *bytes = sift_part_read_section(rel, part, SEC_SCHEMA_DEFAULTS, &len);
	SiftSchemaDefaults *d;
	SiftDefaultsStatus st;

	if (bytes == NULL)
		return NULL;
	st = sift_defaults_decode((const uint8 *) bytes, len, &d);
	pfree(bytes);
	if (st != SIFT_DEFAULTS_OK)
		SIFT_DAMAGE_OTHER_DATA(errmsg("the defaults of a part of sift_table \"%s\" are damaged: %s", RelationGetRelationName(rel), sift_defaults_status_text(st)));
	return d;
}

void
sift_tbl_f2_defaults_verify(Relation rel, const SiftF2Store *store, const SiftPart *part)
{
	SiftSchemaDefaults *pd = sift_tbl_f2_part_defaults(rel, part);
	SiftSchemaDefaults none = {0};
	const SiftSchemaDefaults *cat = sift_tbl_extra(store)->defaults;
	int16		bad;

	if (pd == NULL)
		return;
	if (sift_defaults_check(pd, cat != NULL ? cat : &none, &bad) != SIFT_DEFAULTS_OK)
		SIFT_DAMAGE_OTHER_DATA(errmsg("column %d of sift_table \"%s\" has another fast default or another dropped state than a part was written under", bad, RelationGetRelationName(rel)),
							   errhint("Do not change pg_attribute.attmissingval of a sift_table by hand; a rewrite of the table (VACUUM FULL) makes every part agree again."));
	sift_defaults_free(pd);
}

const SiftColRemap *
sift_tbl_f2_build_col_remap(const SiftF2Store *store, Relation rel, const SiftPart *part, MemoryContext cx)
{
	SiftF2ColMap *m = sift_tbl_f2_build_col_map(store, rel, part, cx);
	SiftColRemap *r;

	if (m == NULL)
		return NULL;
	sift_tbl_f2_defaults_verify(rel, store, part);
	for (int c = 0; c < m->cur_ncols; c++)
		if (m->mode[c] == SIFT_CMAP_WIDEN_N64)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a part of sift_table \"%s\" holds column %d at another scale than the table has now", RelationGetRelationName(rel), store->leaf_attno[c]),
							errhint("Rewrite the table with VACUUM FULL.")));
	r = MemoryContextAlloc(cx, sizeof(SiftColRemap));
	*r = (SiftColRemap) {.ncols = m->cur_ncols,.phys = m->phys_col,.missing = m->missing_cell};
	return r;
}

void
sift_tbl_f2_col_synth(const SiftF2Store *store, int leaf, SiftSynth *out)
{
	const SiftSchemaDefaults *d = sift_tbl_extra(store)->defaults;

	sift_defaults_synth(d != NULL ? sift_defaults_find(d, store->leaf_attno[leaf]) : NULL, out);
}
