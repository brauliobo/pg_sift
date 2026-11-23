/*
 * col/am/rel.c - the relation of a column index as the executor sees it: its schema,
 * definitions and expressions read once into the relcache, the plan memo
 * and the hash a plan is keyed by
 */
#include "postgres.h"
#include "access/relation.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "col/f2/f2.h"
#include "col/v1/point.h"
#include "col/hll.h"
#include "col/v1/gorder.h"
#include "col/v1/setsum.h"
#include "core/limits.h"
#include "core/meta_peek.h"
#include "core/quarantine.h"
#include "col/am/am_int.h"
#include "core/cachebudget.h"
#include "col/v1/conn.h"
#include "col/scope.h"

Oid			sift_col_building = InvalidOid;

COL_OPT_STR_KO(sift_col_options_order_array_group, order_array_group, ko_group)

/* NULL while it is not set (the build chooses); '' is the user's none */
const char *
sift_col_options_order_array_columns(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->order_array_columns ? (const char *) o + o->order_array_columns : o->ko_last ? (const char *) o + o->ko_last : NULL;
}

COL_OPT_STR(sift_col_options_order_layout, order_layout, "")

COL_OPT_STR(sift_col_options_hll_columns, hll_columns, "")
COL_OPT_STR(sift_col_options_sketches, sketches, "")
COL_OPT_STR(sift_col_options_skip_index, skip_index, "")

COL_OPT_STR(sift_col_options_return_columns, return_columns, "")

COL_OPT_STR(sift_col_options_rollup_leaves, rollup_leaves, "")

COL_OPT_STR(sift_col_options_rollup_columns, rollup_columns, "")

COL_OPT_STR(sift_col_options_calendar_columns, calendar_columns, "")

/* The options of the pre-aggregates, orders and key cuts of format major 1; an index of format major 2 has none of them */
static void
parse_v5_options(Relation index, SiftColRel *r, SiftColDef *defs, char **names, int n, int natts, TupleDesc desc)
{
	const char *err = sift_cpagg_spec_parse(sift_col_options_preagg(index), r->schema, (const char *const *) names, &r->pagg);

	if (err)
		sift_col_option_error("preagg", sift_col_options_preagg(index), err);
	if ((err = sift_cpagg_rollup_apply(sift_col_options_rollup_leaves(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
		sift_col_option_error("rollup_leaves", sift_col_options_rollup_leaves(index), err);
	if ((err = sift_cpagg_rows_apply(sift_col_options_rollup_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
		sift_col_option_error("rollup_columns", sift_col_options_rollup_columns(index), err);
	if ((err = sift_cpagg_calendar_apply(sift_col_options_calendar_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
		sift_col_option_error("calendar_columns", sift_col_options_calendar_columns(index), err);
	r->pagg.minmax = sift_col_options(index)->cube_minmax;
	r->pagg.key_table = sift_col_options(index)->key_table;
	if ((err = sift_cset_keys_parse(sift_col_options_key_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
		sift_col_option_error("key_columns", sift_col_options_key_columns(index), err);
	r->pagg.super = sift_col_options(index)->superunit;
	for (int d = 0; r->nscope > 0 && r->pagg.super >= SIFT_SUPER_MIN_FAN && d < r->pagg.ndims; d++)
		r->pagg.dsets[r->pagg.dim[d].leaf] = !r->pagg.dim[d].cal && sift_cdsets_leaf_ok(r->schema, r->pagg.dim[d].leaf);
	if (sift_col_options(index)->ko_n > 2)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("key_order lists %d columns, an index of format major 1 takes at most two", sift_col_options(index)->ko_n),
						errhint("List 'group, key'.")));
	for (int i = 0; i < sift_col_options(index)->ko_n; i++)
		if (sift_col_options(index)->ko_flags[i] != 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("key_order orders a column descending or with its NULLs first, which an index of format major 1 cannot"),
							errhint("Take DESC and NULLS FIRST out of key_order.")));
	for (int k = 0; k < 2 && sift_col_options(index)->ko_lead; k++)
	{
		const SiftColOptions *ko = sift_col_options(index);
		const char *c = (const char *) ko + (k ? ko->ko_last : ko->ko_lead);
		int			i = 0;

		while (i < n && strcmp(names[i], c) != 0)
			i++;
		if (i == n)
			sift_col_option_error("key_order", sift_col_options_key_order(index), psprintf("column \"%s\" is not a column of the index", c));
	}
	if ((err = sift_cpagg_leaf_list(sift_cpagg_auto_list(sift_col_options_order_array_columns(index), &r->pagg.gorderauto), r->schema, (const char *const *) names, r->pagg.gorder, sift_cgord_leaf_array_ok,
									"not an integer, date, timestamp, numeric or text column")) != NULL)
		sift_col_option_error("order_array_columns", sift_col_options_order_array_columns(index), err);
	for (int i = 0; i < n; i++)
		if (r->pagg.gorder[i] && sift_cgord_leaf_text(&r->schema->cols[i]))
			r->pagg.gorder_coll[i] = TupleDescAttr(desc, defs[i].attno - 1)->attcollation;	/* deterministic: the operator class of a text leaf demands it */
	if ((err = sift_cgord_layout_parse(sift_col_options_order_layout(index), r->schema, (const char *const *) names, r->pagg.glayout)) != NULL)
		sift_col_option_error("order_layout", sift_col_options_order_layout(index), err);
	const char *glist = sift_cpagg_auto_list(sift_col_options_order_array_group(index), &r->pagg.gorder_group_auto);
	if (glist[0] != '\0' && strcmp(glist, "off") != 0)
	{
		bool		grp[SIFT_COL_MAX_COLS] = {false};
		int			ng = 0;
		const char *with = r->pagg.gorder_group_auto ? strchr(glist, ';') : NULL;
		const char *only;

		/* 'auto: g, a, b; c, d': the arrays that are grouped beside their plain order, after the ';' */
		if (with != NULL)
		{
			if ((err = sift_cpagg_leaf_list(with + 1, r->schema, (const char *const *) names, r->pagg.gorder_with, sift_cgord_leaf_ok, "not an integer, date or timestamp column")) != NULL)
				sift_col_option_error("order_array_group", sift_col_options_order_array_group(index), err);
			r->pagg.gorder_listed = true;
			glist = pnstrdup(glist, with - glist);
		}
		only = r->pagg.gorder_group_auto ? strchr(glist, ',') : NULL;

		/* 'auto: g, a, b': the arrays after the group keep the ends of the plain order beside the second order */
		if (only != NULL)
		{
			if ((err = sift_cpagg_leaf_list(only + 1, r->schema, (const char *const *) names, r->pagg.gorder_only, sift_cgord_leaf_ok, "not an integer, date or timestamp column")) != NULL)
				sift_col_option_error("order_array_group", sift_col_options_order_array_group(index), err);
			glist = pnstrdup(glist, only - glist);
		}
		if ((err = sift_cpagg_leaf_list(glist, r->schema, (const char *const *) names, grp, sift_cgord_leaf_ok,
										"not an integer, date or timestamp column")) != NULL)
			sift_col_option_error("order_array_group", sift_col_options_order_array_group(index), err);
		for (int i = 0; i < n; i++)
			if (grp[i])
			{
				if (r->pagg.gorder[i])
					sift_col_option_error("order_array_group", sift_col_options_order_array_group(index), psprintf("column \"%s\" is in order_array_columns", names[i]));
				r->pagg.gorder_group = i + 1;
				ng++;
			}
		if (ng != 1)
			sift_col_option_error("order_array_group", sift_col_options_order_array_group(index), psprintf("one column, not %d", ng));
	}
	/* the second order of the second key column grouped by the first is what an ordered index-only scan of (g, k) reads group after group, and it gives exact keys (amscan/ordered.c): whole chunks unless order_layout says otherwise */
	if (r->pagg.gorder_group > 0 && defs[r->pagg.gorder_group - 1].attno == 1)
		for (int i = 0; i < n; i++)
			if (defs[i].attno == 2 && r->pagg.glayout[i] == SIFT_CGORD_OL_AUTO)
				r->pagg.glayout[i] = SIFT_CGORD_OL_WHOLE;
	if ((err = sift_hll_spec_parse(sift_col_options_hll_columns(index), r->schema, (const char *const *) names, r->pagg.hll)) != NULL)
		sift_col_option_error("hll_columns", sift_col_options_hll_columns(index), err);
	if ((err = sift_hll_spec_parse(sift_f2_kll_hll_columns(sift_col_options_sketches(index)), r->schema, (const char *const *) names, r->pagg.hll)) != NULL)
		sift_col_option_error("sketches", sift_col_options_sketches(index), err);
	for (int i = 0; i < n; i++)
		if (r->pagg.hll[i])
		{
			Oid			t = TupleDescAttr(desc, defs[i].attno - 1)->atttypid;

			if (t != INT2OID && t != INT4OID && t != INT8OID && t != DATEOID && t != TIMESTAMPOID && t != TIMESTAMPTZOID && t != TEXTOID && t != VARCHAROID && t != UUIDOID)
				sift_col_option_error("hll_columns", sift_col_options_hll_columns(index), psprintf("\"%s\" is a %s column, whose hash a unit cannot compute (char(n) and name hash differently from text)", names[i], format_type_be(t)));
			if ((t == TEXTOID || t == VARCHAROID) && !get_collation_isdeterministic(TupleDescAttr(desc, defs[i].attno - 1)->attcollation))
				sift_col_option_error("hll_columns", sift_col_options_hll_columns(index), psprintf("\"%s\" has a nondeterministic collation, whose hash is not that of its bytes", names[i]));
			/* a column the build chose for its key list (key_columns 'auto') gives it up: a leaf has one blob a unit in the set section, and the user asked for the sketch */
			if (r->pagg.set[i] && r->pagg.keyauto)
				r->pagg.set[i] = r->pagg.pres[i] = false;
			else if (r->pagg.set[i])
				sift_col_option_error("hll_columns", sift_col_options_hll_columns(index), psprintf("\"%s\" is in key_columns, which keeps its distinct keys exactly", names[i]));
		}
	if ((err = sift_cpoint_spec_parse(sift_col_options_point_tid_columns(index), r->schema, (const char *const *) names, r->pagg.point_tid)) != NULL)
		sift_col_option_error("point_tid_columns", sift_col_options_point_tid_columns(index), err);
	if ((err = sift_cpoint_spec_parse(sift_col_options_count_columns(index), r->schema, (const char *const *) names, r->pagg.count)) != NULL)
		sift_col_option_error("count_columns", sift_col_options_count_columns(index), err);
	for (int i = 0; i < n; i++)
		if (r->pagg.count[i] && r->schema->cols[i].store != SIFT_COLS_STR)
			sift_col_option_error("count_columns", sift_col_options_count_columns(index), psprintf("\"%s\" is not a text column", names[i]));
	r->pagg.dict_hash = sift_col_options(index)->dict_lookup != SIFT_COL_DICT_OFF;
	for (int i = 0; i < n; i++)
		r->pagg.lookup[i] = sift_col_options(index)->dict_lookup == SIFT_COL_DICT_ON || r->pagg.count[i] ||
			(sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO && (r->pagg.set[i] || r->pagg.point_tid[i]));
	for (int a = 1; a <= natts; a++)
		if (r->ret[a] && sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO)
			r->pagg.lookup[r->first[a]] = true;
	/* a text leaf whose dictionary is read in place (and would have its order, whatever it holds) also counts the rows of each string, unless the option says off */
	if (strcmp(sift_col_options_count_columns(index), "off") != 0)
		for (int i = 0; i < n; i++)
			if (r->schema->cols[i].store == SIFT_COLS_STR && r->schema->cols[i].type == SIFT_COLT_TEXT && r->pagg.lookup[i])
				r->pagg.count[i] = true;
	{
		for (const char *p = sift_col_options_key_cut(index); *p != '\0'; p += *p == ',')
		{
			char	   *tok = sift_cpagg_name_take(&p, ",");
			int			id = -1;

			for (int i = 0; i < n && id < 0; i++)
				if (strcmp(names[i], tok) == 0)
					id = i;
			if (id < 0 || r->ncut == SIFT_COL_CUT_MAX || !(sift_crun_leaf_ok(r->schema, &r->schema->cols[id]) || sift_col_def_shape_start(r->schema->cols, id)))
				sift_col_option_error("key_cut", sift_col_options_key_cut(index),
									  psprintf("\"%s\" is not an integer, date, timestamp, numeric or float column of the index (at most %d)", tok, SIFT_COL_CUT_MAX));
			r->cut[r->ncut++] = id;
			pfree(tok);
		}
	}
}

/* The cached state of the index, in mcxt (the relcache entry's own in rd_indexcxt, a copy of a caller's in its context) */
static SiftColRel *
build_rel_with(Relation index, MemoryContext mcxt, uint32 present)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColRel *r = palloc0(sizeof(SiftColRel));
	TupleDesc	desc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	const SiftColFormatOps *ops = sift_col_ops_of_major(sift_meta_resolve_major(index));
	bool		f2 = ops->format_major == 2;
	int			max_leaves = f2 ? (int) sift_limit(f2_leaves, SIFT_F2_MAX_COLS) : (int) sift_limit(col_leaves, SIFT_COL_MAX_COLS);
	SiftColDef *defs = palloc0(max_leaves * sizeof(SiftColDef));
	char	  **names;
	int			n = 0;

	r->hdr.ops = ops;
	if (index->rd_index->indnatts != natts)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes do not support INCLUDE columns"),
						errhint("List the columns in the index key: a sift_col index stores all of them.")));
	r->natts = natts;
	r->first = palloc((natts + 1) * sizeof(int));
	r->nleaves = palloc((natts + 1) * sizeof(int));
	r->json = palloc0((natts + 1) * sizeof(bool));
	r->name = palloc0((natts + 1) * sizeof(bool));
	r->shred = sift_col_shred_create(mcxt, index, present);
	for (int a = 1; a <= natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		bool		isjson = att->atttypid == JSONBOID;
		int			kshred = r->shred ? sift_col_shred_nleaves(r->shred, a) : 0,
					k0 = 0,
					k;
		SiftColDef	lanes[SIFT_COL_MAX_LANES];

		if (!isjson)
		{
			k0 = sift_col_att_leaves(att, lanes, sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_ON ? 0 :
									 kshred > 0 && !sift_col_shred_keeps_image(r->shred, a) ? SIFT_COL_ARR_NOIMAGE : SIFT_COL_ARR_LANES);
			if (kshred > 0)
				sift_col_shred_set_base(r->shred, a, k0);
		}
		k = k0 + kshred;
		if (isjson && k == 0 && f2 && sift_col_paths_unresolved(index))
			r->paths_pending = true;
		else if (isjson && k == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the jsonb column \"%s\" of a sift_col index needs paths", NameStr(att->attname)),
					 errhint("Declare them with the paths option: WITH (paths = '%s.* float4'), or REINDEX to read them from the documents.", NameStr(att->attname))));
		if (n + k > max_leaves && r->shred && sift_col_shred_drop_present(r->shred, a))
			k--;
		if (n + k > max_leaves)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("a sift_col index holds at most %d columns", max_leaves),
							errdetail("Column \"%s\" brings the index to %d columns and paths.", NameStr(att->attname), n + k),
							errhint("Leave out the columns the queries do not read, or create several indexes: the planner picks among them.")));
		r->first[a] = n;
		r->json[a] = isjson;
		r->name[a] = att->atttypid == NAMEOID;
		r->nleaves[a] = k;
		for (int i = 0; i < k; i++)
		{
			SiftColDef *d = &defs[n + i];

			if (i >= k0)
				sift_col_shred_leaf(r->shred, a, i - k0, d);
			else
				*d = lanes[i];
			d->id = n + i;
			d->attno = a;
			d->name = NameStr(att->attname);
			d->parent = d->parent < 0 ? -1 : n + d->parent;
		}
		n += k;
	}
	r->conn_first = n;
	n = sift_conn_leaves(index, defs, n, r->conn_found, &r->nconn);
	r->scope_first = n;
	n = sift_scope_leaves(index, defs, n, r->scope_leaf, &r->nscope, r->scope_cols);
	r->ret = palloc0((natts + 1) * sizeof(bool));
	{
		const char *err = am_return_walk(sift_col_options_return_columns(index), r, desc);

		if (err)
			sift_col_option_error("return_columns", sift_col_options_return_columns(index), err);
		for (int a = 1; a <= natts; a++)
			if (r->ret[a] && index->rd_index->indkey.values[a - 1] == 0)
				sift_col_option_error("return_columns", sift_col_options_return_columns(index), psprintf("\"%s\" is an expression, not a column", NameStr(TupleDescAttr(desc, a - 1)->attname)));
	}
	r->schema = sift_cschema_create(mcxt, defs, n);
	r->policy = palloc(n * sizeof(SiftCcPolicy));
	names = palloc(n * sizeof(char *));
	for (int i = 0; i < n; i++)
	{
		const char *name = defs[i].path[0] ? defs[i].path : NameStr(TupleDescAttr(desc, defs[i].attno - 1)->attname);
		const char *err = sift_cc_policy_for_column(sift_col_options_compression(index), name, &r->policy[i]);

		names[i] = pstrdup(name);
		if (err)
			sift_col_option_error("compression", sift_col_options_compression(index), err);
	}
	if (!f2)
		parse_v5_options(index, r, defs, names, n, natts, desc);
	if (sift_col_row_key_on(index))
	{
		const char *rk = sift_col_options_row_key(index);
		int			id = -1;

		for (int i = 0; i < n && id < 0; i++)
			if (strcmp(names[i], rk) == 0)
				id = i;
		if (id < 0)
			sift_col_option_error("track_changes", rk, psprintf("\"%s\" is not a column of the index", rk));
		for (int a = 1; a <= natts; a++)
			if (r->first[a] == id)
				r->rowkey_attno = a;
		r->rowkey = true;
	}
	r->track = r->rowkey || sift_col_track_ctid(index);
	r->meta = (SiftColIndexMeta) {.indexoid = RelationGetRelid(index), .heapoid = index->rd_index->indrelid, .natts = natts,
	.ncols = n,.cols = r->schema->cols};
	/* mcxt is the context of the index in the relcache for as long as the entry lives: the leaf definitions and names the parsers read are theirs only here */
	for (int i = 0; i < n; i++)
		pfree(names[i]);
	for (int i = r->conn_first; i < n; i++)
		pfree((char *) defs[i].path);
	pfree(names);
	pfree(defs);
	MemoryContextSwitchTo(old);
	return r;
}

/* The hash of the leaf layout the index was written for and its state flags, from its metapage (false while there is none yet) */
static bool
stored_layout(Relation index, bool f2, uint32 *hash, uint32 *flags)
{
	if (f2)
	{
		SiftColMetaPageData meta;

		if (index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX || RelationGetNumberOfBlocks(index) == 0)
			return false;
		(void) sift_col_open_meta(index, &meta);
		*hash = meta.schema_hash;
		*flags = (meta.state_flags & SIFT_META_FLAG_QUARANTINED) ? SIFT_META_QUARANTINED : 0;
		return true;
	}
	return sift_store_meta_peek(index, hash, flags) && (*flags & SIFT_COL_META_FLAG);
}

/*
 * The jsonb columns of an index with paths whose column of the table may be NULL have a presence leaf ("<column>#present"). CREATE INDEX and REINDEX write it for
 * those columns; an index that exists has the layout its metapage hash says, whatever the table says now: one built before the leaf has none, and ALTER TABLE ..
 * SET NOT NULL or DROP NOT NULL does not change it. So the layout is the first of the nullable columns, none and every jsonb column whose schema hash is the stored one
 * (a metapage that has none yet, the index being built, takes the first); no match is left to the check that refuses the index with the REINDEX hint.
 */
static SiftColRel *
build_rel(Relation index, MemoryContext mcxt)
{
	uint32		layout[3] = {sift_col_present_attrs(index, SIFT_COL_PRESENT_NULLABLE), 0, sift_col_present_attrs(index, SIFT_COL_PRESENT_ALL)};
	SiftColRel *r = build_rel_with(index, mcxt, layout[0]);
	uint32		hash,
				flags;
	bool		stored = RelationGetRelid(index) != sift_col_building && stored_layout(index, r->hdr.ops->format_major == 2, &hash, &flags);

	if (stored && r->schema->hash != hash && (layout[0] != layout[1] || layout[0] != layout[2]))
		for (int i = 1; i < 3; i++)
		{
			SiftColRel *o;

			if (layout[i] == layout[0] || (i == 2 && layout[2] == layout[1]))
				continue;
			o = build_rel_with(index, mcxt, layout[i]);
			if (o->schema->hash == hash)
			{
				r = o;
				break;
			}
		}
	r->stale = stored && r->schema->hash != hash;
	r->out = stored && (flags & SIFT_META_QUARANTINED);
	return r;
}

/*
 * Do the schemas have the same leaves? The path of a leaf that is kept under the name of a column (a jsonb path, the lengths of an array, the flag of a connection) begins with the name
 * of the attribute of the leaf in desc (the index), and a column that is renamed keeps its leaves: the segments written before keep the name they had, so the path of old must end as the
 * path of cur does after that name (col/names.c).
 */
bool
sift_cschema_same_leaves(const SiftColSchema *old, const SiftColSchema *cur, TupleDesc desc)
{
	if (old->ncols != cur->ncols)
		return false;
	for (int i = 0; i < cur->ncols; i++)
	{
		const SiftColDef *a = &old->cols[i],
				   *b = &cur->cols[i];

		if (a->id != b->id || a->parent != b->parent || a->kind != b->kind || a->store != b->store || a->flags != b->flags || a->attno != b->attno || a->type != b->type ||
			a->scale != b->scale || a->lanes != b->lanes || a->lane != b->lane)
			return false;
		if (strcmp(a->path, b->path) != 0)
		{
			const char *name = b->attno > 0 ? NameStr(TupleDescAttr(desc, b->attno - 1)->attname) : "";
			size_t		nlen = strlen(name),
						tail = strlen(b->path) - nlen;

			if (strncmp(b->path, name, nlen) != 0 || strlen(a->path) <= tail || strcmp(a->path + strlen(a->path) - tail, b->path + nlen) != 0)
				return false;
		}
	}
	return true;
}

/*
 * Do all the immutable segments of the index have the leaves of the schema s, whatever the columns were called when they were written (sift_cschema_same_leaves)? The schema of a
 * segment is its own, in its header.
 */
static bool
segments_have_leaves(Relation index, const SiftColSchema *s)
{
	SiftDirSnapshot *dir;
	bool		same = true;

	dir = sift_dir_read(index);
	for (int i = 0; i < dir->nsegs && same; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);

			same = sift_cschema_same_leaves(seg->schema, s, RelationGetDescr(index));
			sift_cseg_close(seg);
		}
	pfree(dir);
	return same;
}

/*
 * A column of the index was renamed and the options and attribute of the index say so (col/names.c). The leaves that are kept under the name of the column (the paths of a jsonb column,
 * the lengths of an array, the flag of a connection) are named after it in the schema the options give now, and the metapage has the hash of the schema the segments were written for.
 * The leaves are the same: when every segment has the leaves of the schema the options give, the metapage takes its hash and the segments keep the names they were written with. True
 * when the metapage names the leaves of the options now (it did, or it was written), false when the segments lack them: the index stays as stale as it was, a read says so and asks
 * for the REINDEX.
 */
bool
sift_col_rekey(Relation index)
{
	uint32		layout[3] = {sift_col_present_attrs(index, SIFT_COL_PRESENT_NULLABLE), 0, sift_col_present_attrs(index, SIFT_COL_PRESENT_ALL)};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift col rekey", ALLOCSET_SMALL_SIZES);
	const SiftColSchema *schema[3];
	uint32		hash,
				flags;
	bool		rekeyed = true;

	if (sift_store_meta_peek(index, &hash, &flags) && (flags & SIFT_COL_META_FLAG))
	{
		for (int i = 0; i < 3; i++)
			schema[i] = build_rel_with(index, cx, layout[i])->schema;
		if (schema[0]->hash != hash && schema[1]->hash != hash && schema[2]->hash != hash)
		{
			rekeyed = false;
			for (int i = 0; i < 3 && !rekeyed; i++)
				if (segments_have_leaves(index, schema[i]))
				{
					sift_store_rekey_layout(index, schema[i]->hash);
					rekeyed = true;
				}
		}
	}
	MemoryContextDelete(cx);
	CacheInvalidateRelcache(index);
	return rekeyed;
}

/* Are the units of the sift_col index cut by the order key? (the planner keeps a ctid range, which selects units by heap superblock, on the normal plan for such an index) */
bool
sift_col_index_keyed(Oid indexoid)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	bool		keyed = sift_col_is_index(index) && sift_col_rel(index)->ncut > 0;

	index_close(index, AccessShareLock);
	return keyed;
}

/* Were the segments of the index written under another leaf list than its options give (ALTER INDEX of a scope or a connection): a scan refuses it until REINDEX */
bool
sift_col_index_stale(Oid indexoid)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	bool		stale = sift_col_is_index(index) && sift_col_rel(index)->stale;

	index_close(index, AccessShareLock);
	return stale;
}

/* The index with another file (the one REINDEX replaced): the relation entry over that file, with a state of its own; closed with sift_col_rel_file_view_close */
Relation
sift_col_rel_file_view(Relation index, const RelFileLocator *loc)
{
	Relation	view = palloc(sizeof(RelationData));

	memcpy(view, index, sizeof(RelationData));
	view->rd_locator = *loc;
	view->rd_smgr = NULL;
	view->rd_amcache = NULL;
	view->pgstat_enabled = false;
	view->pgstat_info = NULL;
	view->rd_indexcxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col replaced file", ALLOCSET_SMALL_SIZES);
	return view;
}

void
sift_col_rel_file_view_close(Relation view)
{
	RelationCloseSmgr(view);
	MemoryContextDelete(view->rd_indexcxt);
	pfree(view);
}

SiftColRel *
sift_col_rel(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = build_rel(index, index->rd_indexcxt);
	return index->rd_amcache;
}

/* The table of the format of the relation: that of its relcache state, read from block 0 until the state is made */
const SiftColFormatOps *
sift_col_ops(Relation index)
{
	if (index->rd_amcache != NULL)
		return ((SiftColRelHeader *) index->rd_amcache)->ops;
	return sift_col_ops_of_major(sift_meta_resolve_major(index));
}

uint16
sift_col_format_major(Relation index)
{
	return sift_col_ops(index)->format_major;
}

static uint16
oid_format(Oid indexoid, bool serving)
{
	Relation	index = try_relation_open(indexoid, AccessShareLock);
	uint16		major = 0;

	if (index == NULL)
		return 0;
	if (index->rd_rel->relam == sift_col_am_oid() && !(serving && (!index->rd_index->indisvalid || sift_index_quarantined(index))))
		major = sift_col_format_major(index);
	relation_close(index, AccessShareLock);
	return major;
}

uint16
sift_col_index_oid_format(Oid indexoid)
{
	return oid_format(indexoid, false);
}

uint16
sift_col_index_oid_engine(Oid indexoid)
{
	return oid_format(indexoid, true);
}

/* The memory of one inserted row, in the context that holds the state: a backend that only reads an index never pays its first block */
MemoryContext
sift_col_rel_rowcxt(SiftColRel *r)
{
	if (r->rowcxt == NULL)
		r->rowcxt = AllocSetContextCreate(GetMemoryChunkContext(r), "sift cache col insert", ALLOCSET_SMALL_SIZES);
	return r->rowcxt;
}

SiftColRel *
sift_col_rel_private(Relation index, MemoryContext mcxt)
{
	SiftColRel *r = build_rel(index, mcxt);

#ifdef SIFT_TESTING
	sift_col_test_inval(index);	/* building the state looks up the catalogs: what the caller took from the options before it is then freed */
#endif
	return r;
}

/*
 * A sibling index of the table, read for what its options and catalog rows declare: the rules of a build ask whether another index shares a column, an order or a key. A REINDEX of the
 * sibling holds AccessExclusiveLock on it from its start to its end, so a lock on it made two REINDEX of two indexes of one table wait for each other (each holds its own and asks for the
 * other's). The caller holds a lock on the table, which keeps every DDL that drops or alters an index out, and the rows are the committed ones whatever the REINDEX does; its metapage is
 * not read (the file a REINDEX replaces), which is why sift_col_peer_rel gives the declared layout and not the stored one.
 */
Relation
sift_col_peer_open(Oid indexoid)
{
	Relation	peer = RelationIdGetRelation(indexoid);

	SIFT_INVARIANT(RelationIsValid(peer), "sift: index %u of a locked table does not exist", indexoid);
	return peer;
}

void
sift_col_peer_close(Relation peer)
{
	RelationClose(peer);
}

SiftColRel *
sift_col_peer_rel(Relation peer, MemoryContext mcxt)
{
	return build_rel_with(peer, mcxt, sift_col_present_attrs(peer, SIFT_COL_PRESENT_NULLABLE));
}

#ifdef SIFT_TESTING
void
sift_col_test_inval(Relation index)
{
	if (!sift_test_col_rel_inval)
		return;
	/* the reload of the entry that a lock taken during the scan may cause frees the cached state and the options: first overwrite what they hold */
	memset(sift_col_rel(index), 0x7f, sizeof(SiftColRel));
	if (index->rd_options)
	{
		Size		sz = VARSIZE(index->rd_options);

		pfree(index->rd_options);
		index->rd_options = NULL;
		/* takes the chunk just freed, so the options the reload parses cannot reuse it under the string a caller kept */
		memset(MemoryContextAlloc(CacheMemoryContext, sz), 0x7f, sz);
	}
	RelationCacheInvalidateEntry(RelationGetRelid(index));
}
#endif

const SiftColIndexMeta *
sift_col_index_defs(Relation index)
{
	/* check-memory: allow hands out &r->meta at once; the callers copy it before any lock (sift_col_index_meta, col_layout) */
	SiftColRel *r = sift_col_rel(index);

	r->meta.valid = index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive;
	r->meta.partial = !heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL);
	return &r->meta;
}

/* The name of a leaf: its path (a jsonb path, a lane), else the column it comes from; leaves are not index attributes, there are more of them than columns */
const char *
sift_col_leaf_name(Relation index, const SiftColDef *def)
{
	return def->path[0] ? def->path : NameStr(TupleDescAttr(RelationGetDescr(index), def->attno - 1)->attname);
}

Node *
sift_col_att_expr(Relation index, int attno)
{
	AttrNumber	heapatt = index->rd_index->indkey.values[attno - 1];

#ifdef SIFT_TESTING
	sift_col_test_inval(index);	/* reading the index expressions can take a lock that reloads the entry (and frees its cached state) */
#endif

	if (heapatt != 0)
	{
		Oid			typid = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypid;
		int32		typmod = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypmod;

		return (Node *) makeVar(1, heapatt, typid, typmod, TupleDescAttr(RelationGetDescr(index), attno - 1)->attcollation, 0);
	}
	{
		List	   *exprs = RelationGetIndexExpressions(index);
		int			k = 0;

		for (int i = 0; i < attno - 1; i++)
			if (index->rd_index->indkey.values[i] == 0)
				k++;
		return (Node *) copyObject(list_nth(exprs, k));
	}
}

static uint64 leaf_plan_seq;

static MemoryContext plan_memo;

bool
sift_col_plan_begin(void)
{
	if (am_leaf_plan != 0)
		return false;
	am_leaf_plan = ++leaf_plan_seq;
	return true;
}

void
sift_col_plan_end(bool started)
{
	if (!started)
		return;
	am_leaf_plan = 0;
	if (plan_memo != NULL)
	{
		MemoryContextDelete(plan_memo);
		plan_memo = NULL;
	}
}

/* memory that lives as long as the planning that is open (NULL outside one; deleted when it ends, a context that stays is a leak at backend exit), and the number of that planning: what a caller keeps in it is stale when the number changes */
MemoryContext
sift_col_plan_memo(uint64 *plan)
{
	if (am_leaf_plan == 0)
		return NULL;
	if (plan_memo == NULL)
		plan_memo = sift_cache_create_context("sift planning memo");
	*plan = am_leaf_plan;
	return plan_memo;
}

uint32
sift_col_pagg_hash(Relation index)
{
	const char *a = sift_col_options_preagg(index),
			   *b = sift_col_options_rollup_leaves(index),
			   *c = sift_col_options_rollup_columns(index),
			   *d = sift_col_options_calendar_columns(index);

	return hash_bytes((const unsigned char *) a, strlen(a)) ^ (hash_bytes((const unsigned char *) b, strlen(b)) * 31) ^
		(hash_bytes((const unsigned char *) c, strlen(c)) * 17) ^ (hash_bytes((const unsigned char *) d, strlen(d)) * 13) ^
		(sift_col_options(index)->cube_minmax ? 0x9e3779b9 : 0) ^ (sift_col_options(index)->key_table ? 0x7f4a7c15 : 0) ^ ((uint32) sift_col_format_minor * 0x85ebca6b);
}
