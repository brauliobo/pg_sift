/*
 * col/v1/reader_meta.c - what the planner reads of a Format 1 index through the unit source: the leaves of its layout and its statistics, from the
 * leaf list and the zones and dictionaries of its segments (col/v1/reader.h).
 */
#include "postgres.h"

#include "utils/rel.h"

#include "col/am.h"
#include "col/col.h"
#include "col/pg.h"
#include "col/scope/scope.h"
#include "col/v1/reader.h"

/* a head of two or three lanes (a numeric of more than 18 digits or of any other shape, an interval) or a uuid, bytea or other varlena image:
 * read through a dictionary of its datums */
static bool
leaf_wide(const SiftColDef *def)
{
	return def->lane == 0 && (def->lanes > 0 || sift_col_def_is_image(def));
}

static SiftSrcVecType
leaf_vtype(const SiftColDef *def)
{
	if (leaf_wide(def))
		return SCV_CODE;
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return (def->type == SIFT_COLT_INT8 || def->type == SIFT_COLT_TIMESTAMP || def->type == SIFT_COLT_TIMESTAMPTZ || def->type == SIFT_COLT_NUMERIC ||
					def->type == SIFT_COLT_MONEY || def->type == SIFT_COLT_TIME) ? SCV_I64 : SCV_I32;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return SCV_F64;
		case SIFT_COLS_STR:
			return SCV_CODE;
		default:
			SIFT_INVARIANT(false, "sift: leaf of storage %d has no column vector type", def->store);
	}
	return SCV_I32;
}

/* A leaf the aggregate scan does not read: a lane of a wide value and the heads that are not one machine word; with no SQL type no expression matches it. money is one int64, read as such. */
static bool
leaf_hidden(const SiftColDef *def)
{
	return (def->lane > 0 && !sift_col_def_arrlen_lane(def)) || (def->flags & SIFT_COLD_FOUND);
}

/* "a.b" below the index attribute `att` of a leaf path "att.a.b" / "att->a->b" / "att.a[]" / "att.*": the keys, as String nodes */
static List *
path_below(const char *path, const char *att)
{
	List	   *keys = NIL;
	size_t		alen = strlen(att);
	const char *p = path + alen;
	StringInfoData key;

	if (*path == '\0')
		return NIL;
	SIFT_INVARIANT(strncmp(path, att, alen) == 0, "sift: leaf path \"%s\" does not start with its attribute \"%s\"", path, att);
	initStringInfo(&key);
	while (*p)
	{
		if (*p == '.' || (p[0] == '-' && p[1] == '>') || *p == '[' || *p == '#')
		{
			if (key.len)
				keys = lappend(keys, makeString(pstrdup(key.data)));
			resetStringInfo(&key);
			if (*p == '[' || *p == '#')
				break;
			p += (*p == '.') ? 1 : 2;
		}
		else if (*p == '*')
			break;
		else
			appendStringInfoChar(&key, *p++);
	}
	if (key.len)
		keys = lappend(keys, makeString(pstrdup(key.data)));
	return keys;
}

/* the field name of a child of a group: after "[]." (list) or the member / value of a map; "#present" for the leaf of the document itself */
static void
child_name(const char *path, bool is_map, bool is_key, char *name)
{
	const char *f = is_map ? NULL : strstr(path, "[].");

	if (is_map)
		strlcpy(name, is_key ? "key" : "value", NAMEDATALEN);
	else if (f)
		strlcpy(name, f + 3, NAMEDATALEN);
	else
		strlcpy(name, strstr(path, "#present") ? "#present" : "", NAMEDATALEN);	/* the leaf of a jsonb document, true or NULL */
}

/* The leaf of column def i of a shredded attribute: its place in the group tree, its path below the attribute, its kind and vector type, its SQL type in desc */
void
sift_src_def_leaf(const SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l)
{
	const SiftColDef *def = &cols[i];
	const char *att = NameStr(TupleDescAttr(desc, def->attno - 1)->attname);
	bool		is_map = def->parent >= 0 && strstr(cols[def->parent].path, ".*") != NULL;

	l->id = def->id;
	l->attno = def->attno;
	l->parent = def->parent;
	l->typid = leaf_hidden(def) ? InvalidOid : sift_col_def_typid(desc, def);
	l->arrhead = sift_col_def_arrlen_head(def);
	l->noimage = l->arrhead && (def->flags & SIFT_COLD_NOIMAGE);
	l->arrlen = sift_col_def_arrlen_lane(def) ? def->lane : 0;
	l->wide = leaf_wide(def);
	l->shape = sift_col_def_shape(def);
	l->wtypid = sift_col_def_lane_tested(def) ? sift_col_def_typid(desc, def) : InvalidOid;
	l->scale = def->scale;
	l->lossy = (def->flags & SIFT_COLD_LOSSY) != 0;
	l->astext = (def->flags & SIFT_COLD_ASTEXT) != 0;
	if (def->kind == SIFT_COLK_GROUP)
	{
		l->kind = strstr(def->path, ".*") ? SCL_OBJECT : SCL_ARRAY;
		l->vtype = SCV_I32;
		l->path = path_below(def->path, att);
	}
	else
	{
		l->kind = SCL_SCALAR;
		l->vtype = def->store == SIFT_COLS_VAR || (def->lane > 0 && i >= def->lane && sift_col_def_shape(&cols[i - def->lane])) ? SCV_I64 : leaf_vtype(def);	/* the lanes of a range or network are 64-bit positions whatever the head's type */
		l->lossy |= def->store == SIFT_COLS_VAR;
		if (def->parent < 0 && !(def->flags & SIFT_COLD_DERIVED))
			l->path = path_below(def->path, att);
		else if (def->flags & SIFT_COLD_SCOPE)
			l->path = list_make1(makeString(pstrdup(def->path)));	/* "<key_name>#scope": no jsonb key of the member's column is read by it */
		child_name(def->path, is_map, strstr(def->path, "#key") != NULL, l->name);
	}
}

SiftSrcLayout *
sift_v1_idx_layout(Relation index)
{
	SiftColIndexMeta m = *sift_col_index_defs(index);	/* a copy: the cached state it points into is freed when reading an index expression reloads the entry */
	const SiftColIndexMeta *meta = &m;
	TupleDesc	tupdesc = RelationGetDescr(index);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));

	lay->heapoid = meta->heapoid;
	lay->natts = meta->natts;
	for (int a = 1; a <= meta->natts; a++)
		lay->attexprs = lappend(lay->attexprs, sift_col_att_expr(index, a));
	lay->nleaves = meta->ncols;
	lay->leaf = palloc0(sizeof(SiftSrcLeaf) * meta->ncols);
	for (int i = 0; i < meta->ncols; i++)
	{
		sift_src_def_leaf(meta->cols, i, tupdesc, &lay->leaf[i]);
		elog(DEBUG1, "sift_aggcol: leaf %d path \"%s\" attno %d parent %d kind %d", i, meta->cols[i].path, meta->cols[i].attno, meta->cols[i].parent,
			 lay->leaf[i].kind);
	}
	return lay;
}

/* A leaf with a value in every row that is the group of the second order of a leaf holding every row: ORDER BY it reads those orders group after group (gord_bygroup) */
static void
stats_bygroup(SiftSrcStats *st, const SiftColLeafStats *ls, int ncols)
{
	for (int j = 0; j < ncols && j < SIFT_COLSRC_MAX_LEAVES; j++)
	{
		int			gi = ls[j].gord_gcol;

		if (ls[j].gord_grp_rows > 0 && gi >= 0 && gi < ncols && gi < SIFT_COLSRC_MAX_LEAVES && ls[gi].nnull == 0 && ls[j].gord_grp_rows + ls[j].gord_free_rows >= 0.999 * ls[gi].nvals)
		{
			st->gord_bygroup[gi] = true;
			/* the segments without the second order are read in memory (sift_col_gorder_open): the planner prices their rows as the walk of the group leaf's own array would */
			st->gord_free[gi] = Max(st->gord_free[gi], ls[j].gord_free_rows);
		}
	}
}

/*
 * The per-leaf numbers of SiftSrcStats from the zones and dictionaries, counted over the non-NULL values
 * of the leaf (elements for a child). ndistinct: a text leaf with no inline unit is its largest segment dictionary, exact for one segment
 * and a lower bound for several that hold different values; an integer leaf is bounded by its range (max - min + 1); any leaf by its
 * non-NULL values. min and max: integer leaves, as double. elems_per_row: a group's total count over its rows.
 */
void
sift_src_leaf_stats(const SiftColDef *cols, int ncols, const SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st)
{
	for (int i = 0; i < ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		const SiftColDef *def = &cols[i];
		double		nonnull = ls[i].nvals - ls[i].nnull;

		st->ovf[i] = (ls[i].has_ovf || wovf[i]) && def->type != SIFT_COLT_NUMERIC;
		st->ovf_units[i] = st->ovf[i] ? Max(ls[i].novf, 1.0) / Max(ls[i].nunits, 1.0) : 0;
		st->ovf_frac[i] = ls[i].ovf_vals > 0 ? ls[i].ovf_slots / ls[i].ovf_vals : -1;
		st->nokinds[i] = ls[i].nokinds;
		st->null_frac[i] = ls[i].nvals > 0 ? Min(ls[i].nnull / ls[i].nvals, 1.0) : 0;
		st->rootless[i] = ls[i].nvals > 0 ? ls[i].rootless * ls[i].nnull / ls[i].nvals : ls[i].rootless;
		st->jnull[i] = ls[i].has_jnull || ls[i].nokinds || wjnull[i];
		if (def->kind == SIFT_COLK_GROUP)
		{
			st->elems_per_row[i] = ls[i].has_sum && ls[i].nvals > 0 ? ls[i].sum / ls[i].nvals : 0;
			continue;
		}
		if (ls[i].point_units > 0 && ls[i].nunits > 0)
			st->point_cover[i] = Min(ls[i].point_units / ls[i].nunits, 1.0);
		if (ls[i].set_units > 0)
		{
			st->set_cover[i] = ls[i].set_units / ls[i].set_sampled;
			st->set_keys[i] = ls[i].set_keys / ls[i].set_units;
		}
		st->head_cover[i] = ls[i].nunits > 0 ? Min(ls[i].head_units / ls[i].nunits, 1.0) : 0;
		st->gord_free[i] = ls[i].gord_rows + ls[i].gord_ends_rows > 0 ? ls[i].gord_free_rows : 0;
		st->gord_cover[i] = ls[i].nvals > 0 ? Min((ls[i].gord_rows - ls[i].gord_grp_rows + ls[i].gord_ends_rows + st->gord_free[i]) / ls[i].nvals, 1.0) : 0;
		st->gord_coll[i] = ls[i].gord_coll;
		st->gord_group[i] = ls[i].nvals > 0 && ls[i].gord_grp_rows > 0 && ls[i].gord_grp_rows + ls[i].gord_free_rows >= 0.9 * ls[i].nvals ? ls[i].gord_gcol : -1;
		st->ndistinct[i] = nonnull;
		if (def->store == SIFT_COLS_STR && !ls[i].inl)
			st->ndistinct[i] = Min(nonnull, ls[i].dict_n);
		else if (def->store == SIFT_COLS_INT && ls[i].has_range && nonnull > 0 && !leaf_wide(def))
		{
			st->ndistinct[i] = Min(nonnull, (double) ls[i].max - (double) ls[i].min + 1);
			st->min[i] = (double) ls[i].min;
			st->max[i] = (double) ls[i].max;
			st->zone_span[i] = Min(ls[i].span / Max(ls[i].nzone, 1.0) / ((double) ls[i].max - (double) ls[i].min + 1.0), 1.0);
		}
	}	/* an element that overflowed has its row read again: the share of rows is the elements' times those of a row */
	for (int i = 0; i < ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (cols[i].parent >= 0 && st->ovf_frac[i] > 0)
			st->ovf_frac[i] = Min(1.0, st->ovf_frac[i] * Max(st->elems_per_row[cols[i].parent], 1.0));
	stats_bygroup(st, ls, ncols);
}

/* A copy of the sample of the unit zones the planner may read (the memo of the backend can be replaced while the plan is made); only the integer scalar leaves that are not wide. nunits: the units of the family */
const SiftSrcZones *
sift_v1_idx_zones(const SiftColZones *zs, const SiftColIndexMeta *meta, double nunits)
{
	SiftSrcZones *z;

	if (zs->n == 0 || nunits <= 0)
		return NULL;
	z = palloc0(sizeof(SiftSrcZones));
	z->n = zs->n;
	z->covered = Min((double) zs->units / nunits, 1.0);
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (zs->lo[i] != NULL && !leaf_wide(&meta->cols[i]))
		{
			z->lo[i] = memcpy(palloc(zs->n * sizeof(int64)), zs->lo[i], zs->n * sizeof(int64));
			z->hi[i] = memcpy(palloc(zs->n * sizeof(int64)), zs->hi[i], zs->n * sizeof(int64));
		}
	return z;
}

void
sift_v1_idx_stats(Relation index, SiftSrcStats *st)
{
	SiftColLeafMemo memo;
	SiftColIndexMeta *meta = sift_col_index_meta(index, &memo);
	bool		wovf[SIFT_COLSRC_MAX_LEAVES],
				wjnull[SIFT_COLSRC_MAX_LEAVES];

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->bytes = meta->stats.bytes;
	st->keyed_frac = meta->stats.keyed_frac;
	st->allvisible_frac = meta->stats.allvisible_frac;
	/* a unit answers from its pre-aggregates when every page it has rows on is all-visible: the superblocks that are, a keyed unit (it spans the heap) only when all are */
	st->exact_frac = (1.0 - st->keyed_frac) * meta->stats.clean_frac + (meta->stats.clean_frac >= 1.0 ? st->keyed_frac : 0.0);
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		wovf[i] = (meta->stats.wbuf_ovf & SIFT_COL_OVF_BIT(i)) != 0;
		wjnull[i] = (meta->stats.wbuf_jnull & SIFT_COL_OVF_BIT(i)) != 0;
	}
	sift_src_leaf_stats(meta->cols, meta->ncols, memo.leaves, wovf, wjnull, st);
	st->zones = sift_v1_idx_zones(memo.zones, meta, meta->stats.nunits);
	st->miss_pending = true;
}

/*
 * The statistics of the scope units of the index, the fields a read of them prices by its units (docs/COL-SCOPE.md): its units are cut by the scope key and the time, so their zones are narrow where
 * the entity units' are wide, and the sections they hold (the histograms, rollups) are those of the units written for the scope, which a member's entity unit may have where they have not. Every unit
 * of a scope is keyed: it answers from its pre-aggregates only where the whole heap is all-visible.
 */
bool
sift_v1_idx_scope_stats(Relation index, SiftSrcStats *st)
{
	SiftColLeafMemo memo;
	SiftColIndexMeta *meta = sift_col_index_meta(index, &memo);
	bool		wovf[SIFT_COLSRC_MAX_LEAVES] = {false},
				wjnull[SIFT_COLSRC_MAX_LEAVES] = {false};

	if (memo.scope_leaves == NULL)
		return false;
	memset(st, 0, sizeof(*st));
	st->scope = true;
	/* a unit cut by time holds the rows of a slice of time, the pages of a slice of the heap: the share of the heap's clean superblocks is the share of the units whose pages are all-visible (a heap of rows
	 * that arrive out of time order makes it an estimate, and the read proves each unit); without a time cut a unit may hold any page and answers only from a clean heap */
	st->exact_frac = sift_scope_time_leaf(meta->cols, meta->ncols) >= 0 || meta->stats.clean_frac >= 1.0 ? meta->stats.clean_frac : 0.0;
	sift_src_leaf_stats(meta->cols, meta->ncols, memo.scope_leaves, wovf, wjnull, st);
	st->zones = sift_v1_idx_zones(memo.scope_zones, meta, memo.scope_zones->units);
	st->miss_pending = true;
	return true;
}
