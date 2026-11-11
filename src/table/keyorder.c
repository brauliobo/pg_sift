/*
 * table/keyorder.c - the key_order option of a sift_table: the columns the rewrites of the table sort its rows by (docs/COL-TAM.md section 8.2), and how far the
 * rows are from that order now
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/varlena.h"

#include "col/seg.h"
#include "table/table.h"
#include "table/seg_int.h"

/* The column names of spec, split as SQL splits identifiers (unquoted names fold to lower case) */
static List *
key_names(const char *spec)
{
	List	   *names = NIL;
	char	   *copy = pstrdup(spec);

	if (!SplitIdentifierString(copy, ',', &names) || names == NIL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("key_order is a list of column names: \"%s\"", spec),
						errhint("For example 'tenant_id, created_at'.")));
	return names;
}

char *
sift_tbl_key_order_check(Relation rel, const char *spec)
{
	List	   *names = key_names(spec);
	Bitmapset  *seen = NULL;
	StringInfoData out;
	ListCell   *lc;

	if (list_length(names) > SIFT_TBL_KEY_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("key_order has more than %d columns", SIFT_TBL_KEY_MAX),
						errhint("Only the leading columns decide the units that hold a key: list at most %d.", SIFT_TBL_KEY_MAX)));
	initStringInfo(&out);
	foreach(lc, names)
	{
		const char *name = lfirst(lc);
		AttrNumber	a = get_attnum(RelationGetRelid(rel), name);
		TypeCacheEntry *te;

		if (a <= 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" of table \"%s\" does not exist", name, RelationGetRelationName(rel))));
		if (bms_is_member(a, seen))
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("column \"%s\" is listed twice in key_order", name)));
		seen = bms_add_member(seen, a);
		te = lookup_type_cache(TupleDescAttr(RelationGetDescr(rel), a - 1)->atttypid, TYPECACHE_LT_OPR);
		if (!OidIsValid(te->lt_opr))
			ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%s\" has a type with no btree order and cannot be in key_order", name)));
		appendStringInfo(&out, "%s%s", out.len ? ", " : "", quote_identifier(name));
	}
	return out.data;
}

int
sift_tbl_key_order(Relation rel, const SiftTblRel *st, AttrNumber *att)
{
	const char *spec = sift_tbl_opt_str(st, SIFT_TBLOPT_KEY_ORDER);
	int			n = 0;
	ListCell   *lc;

	if (spec == NULL || spec[0] == '\0')
		return 0;
	foreach(lc, key_names(spec))
	{
		AttrNumber	a = get_attnum(RelationGetRelid(rel), lfirst(lc));

		if (a > 0 && n < SIFT_TBL_KEY_MAX)	/* a column dropped since leaves the order */
			att[n++] = a;
	}
	return n;
}

void
sift_tbl_key_state(Relation rel, SiftTblRel *st, SiftTblKeyState *ks)
{
	AttrNumber	att[SIFT_TBL_KEY_MAX];
	const SiftTblLayout *l;
	const SiftColDef *def;
	MemoryContext cx;
	MemoryContext old;
	SiftColSegment **seg;
	int			nseg = 0;
	double		point;

	*ks = (SiftTblKeyState) {0};
	if (sift_tbl_key_order(rel, st, att) == 0)
		return;
	sift_tbl_refresh(rel, st);
	l = layout_of(rel, st);
	def = &l->schema->cols[l->leaf[att[0]]];
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table key state", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	seg = palloc(Max(st->dir.nsegs, 1) * sizeof(SiftColSegment *));
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		if (e->kind != SIFT_SEG_IMMUTABLE || schema_of(rel, st, e)->hash != l->schema->hash)
			continue;
		seg[nseg] = sift_cseg_open(rel, e, cx);
		ks->units += seg[nseg]->nsb;
		ks->rows += sift_cseg_live_rows(seg[nseg]);
		nseg++;
	}
	if (def->store == SIFT_COLS_INT && def->kind == SIFT_COLK_SCALAR && def->lane == 0 && ks->units > 0 &&
		sift_col_zone_point(seg, nseg, l->leaf[att[0]], ks->units, &point))
	{
		ks->known = true;
		ks->units_per_key = point;
		ks->out_of_order_pct = ks->units > 1 ? Min(100.0, Max(0.0, 100.0 * (point - 1) / (ks->units - 1))) : 0;
	}
	for (int i = 0; i < nseg; i++)
		sift_cseg_close(seg[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static double
file_mb(Relation rel)
{
	return (double) RelationGetNumberOfBlocks(rel) * BLCKSZ / (1024.0 * 1024.0);
}

void
sift_tbl_key_order_notice(Relation rel, const char *spec)
{
	SiftTblKeyState ks;
	char	   *first = pstrdup(spec);

	sift_tbl_key_state(rel, sift_tbl_rel(rel), &ks);
	if (strchr(first, ',') != NULL)
		*strchr(first, ',') = '\0';
	if (!ks.known)
		ereport(NOTICE, (errmsg("sift_table \"%s\" is sorted by (%s) when VACUUM FULL rewrites it", RelationGetRelationName(rel), spec),
						 errdetail("Its present order is measured for an integer, date or timestamp first column once the table has column groups."),
						 errhint("VACUUM FULL sorts the table; the TIDs change and its indexes are rebuilt.")));
	else
		ereport(NOTICE, (errmsg("sift_table \"%s\" is sorted by (%s) when VACUUM FULL rewrites it", RelationGetRelationName(rel), spec),
						 errdetail("%.0f%% of its %lld rows are out of that order: an equality on %s reads %.1f of its %lld units, 1 when it is sorted; the rewrite writes %.1f MB.",
								   ks.out_of_order_pct, (long long) ks.rows, first, ks.units_per_key, (long long) ks.units, file_mb(rel)),
						 errhint("VACUUM FULL sorts the table; the TIDs change and its indexes are rebuilt.")));
}

/* VACUUM of a table with a key_order: says so once when its rows have drifted from it (5% of the units or more) */
void
sift_tbl_key_order_vacuum(Relation rel, SiftTblRel *st)
{
	SiftTblKeyState ks;
	char	   *spec;

	if (sift_tbl_opt_str(st, SIFT_TBLOPT_KEY_ORDER)[0] == '\0')
		return;
	sift_tbl_key_state(rel, st, &ks);
	if (!ks.known || ks.out_of_order_pct < SIFT_TBL_KEY_DECAY_PCT)
		return;
	spec = pstrdup(sift_tbl_opt_str(st, SIFT_TBLOPT_KEY_ORDER));
	ereport(NOTICE, (errmsg("sift_table \"%s\": %.0f%% of its rows are out of its key_order (%s)", RelationGetRelationName(rel), ks.out_of_order_pct, spec),
					 errdetail("An equality on the first column reads %.1f of its %lld units, 1 when it is sorted.", ks.units_per_key, (long long) ks.units),
					 errhint("VACUUM FULL sorts the table again: it rewrites %.1f MB and the indexes (new TIDs).", file_mb(rel))));
}
