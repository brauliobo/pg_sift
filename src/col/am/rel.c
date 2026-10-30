/*
 * col/am/rel.c - the relation of a column index as the executor sees it: its schema,
 * definitions and expressions read once into the relcache, the plan memo
 * and the hash a plan is keyed by
 */
#include "postgres.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "col/point.h"
#include "col/hll.h"
#include "col/gorder.h"
#include "col/setsum.h"
#include "core/limits.h"
#include "col/am/am_int.h"

COL_OPT_STR(sift_col_options_order_columns, order_columns, "")

COL_OPT_STR(sift_col_options_order_array_group, order_array_group, "")

COL_OPT_STR(sift_col_options_order_array_columns, order_array_columns, NULL)

COL_OPT_STR(sift_col_options_hll_columns, hll_columns, "")

COL_OPT_STR(sift_col_options_return_columns, return_columns, "")

COL_OPT_STR(sift_col_options_rollup, rollup, "")

COL_OPT_STR(sift_col_options_rollup_dims, rollup_dims, "")

COL_OPT_STR(sift_col_options_calendar_columns, calendar_columns, "")

/* The cached state of the index, in mcxt (the relcache entry's own in rd_indexcxt, a copy of a caller's in its context) */
static SiftColRel *
build_rel(Relation index, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColRel *r = palloc0(sizeof(SiftColRel));
	TupleDesc	desc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	SiftColDef *defs = palloc0(SIFT_COL_MAX_COLS * sizeof(SiftColDef));
	char	  **names;
	int			n = 0;

	if (index->rd_index->indnatts != natts)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes do not support INCLUDE columns"),
						errhint("List the columns in the index key: a sift_col index stores all of them.")));
	r->natts = natts;
	r->first = palloc((natts + 1) * sizeof(int));
	r->nleaves = palloc((natts + 1) * sizeof(int));
	r->json = palloc0((natts + 1) * sizeof(bool));
	r->name = palloc0((natts + 1) * sizeof(bool));
	r->shred = sift_col_shred_create(mcxt, index);
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
		if (isjson && k == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the jsonb column \"%s\" of a sift_col index needs paths", NameStr(att->attname)),
					 errhint("Declare them with the paths option: WITH (paths = '%s.* float4'), or REINDEX to read them from the documents.", NameStr(att->attname))));
		if (n + k > sift_limit(col_leaves, SIFT_COL_MAX_COLS))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("a sift_col index holds at most %d columns", (int) sift_limit(col_leaves, SIFT_COL_MAX_COLS)),
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
			d->parent = d->parent < 0 ? -1 : n + d->parent;
		}
		n += k;
	}
	r->ret = palloc0((natts + 1) * sizeof(bool));
	{
		const char *err = am_return_walk(sift_col_options_return_columns(index), r, desc);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid return_columns option: %s", err)));
		for (int a = 1; a <= natts; a++)
			if (r->ret[a] && index->rd_index->indkey.values[a - 1] == 0)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid return_columns option: \"%s\" is an expression, not a column", NameStr(TupleDescAttr(desc, a - 1)->attname))));
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
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option: %s", err)));
	}
	{
		const char *err = sift_cpagg_spec_parse(sift_col_options_preagg(index), r->schema, (const char *const *) names, &r->pagg);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid preagg option: %s", err)));
		if ((err = sift_cpagg_rollup_apply(sift_col_options_rollup(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup option: %s", err)));
		if ((err = sift_cpagg_rows_apply(sift_col_options_rollup_dims(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup_dims option: %s", err)));
		if ((err = sift_cpagg_calendar_apply(sift_col_options_calendar_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid calendar_columns option: %s", err)));
		r->pagg.minmax = sift_col_options(index)->cube_minmax;
		r->pagg.key_table = sift_col_options(index)->key_table;
		if ((err = sift_cset_keys_parse(sift_col_options_key_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_columns option: %s", err)));
		r->pagg.super = sift_col_options(index)->superunit;
		if ((err = sift_crun_spec_parse(sift_col_options_order_columns(index), r->schema, (const char *const *) names, r->pagg.run)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_columns option: %s", err)));
		if ((err = sift_cpagg_leaf_list(sift_cpagg_auto_list(sift_col_options_order_array_columns(index), &r->pagg.gorderauto), r->schema, (const char *const *) names, r->pagg.gorder, sift_cgord_leaf_array_ok,
										"not an integer, date, timestamp or text column")) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_columns option: %s", err)));
		for (int i = 0; i < n; i++)
			if (r->pagg.gorder[i] && sift_cgord_leaf_text(&r->schema->cols[i]))
				r->pagg.gorder_coll[i] = TupleDescAttr(desc, defs[i].attno - 1)->attcollation;	/* deterministic: the operator class of a text leaf demands it */
		const char *glist = sift_cpagg_auto_list(sift_col_options_order_array_group(index), &r->pagg.gorder_group_auto);
		if (glist[0] != '\0' && strcmp(glist, "off") != 0)
		{
			bool		grp[SIFT_COL_MAX_COLS] = {false};
			int			ng = 0;

			if ((err = sift_cpagg_leaf_list(glist, r->schema, (const char *const *) names, grp, sift_cgord_leaf_ok,
											"not an integer, date or timestamp column")) != NULL)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: %s", err)));
			for (int i = 0; i < n; i++)
				if (grp[i])
				{
					if (r->pagg.gorder[i])
						ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: column \"%s\" is in order_array_columns", names[i])));
					r->pagg.gorder_group = i + 1;
					ng++;
				}
			if (ng != 1)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: one column, not %d", ng)));
		}
		for (int i = 0; i < n; i++)
			if (r->pagg.gorder[i] && !r->pagg.run[i] && r->pagg.gorderauto)
				;
			else if (r->pagg.gorder[i] && !r->pagg.run[i] && !sift_cgord_leaf_text(&r->schema->cols[i]))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_columns option: column \"%s\" is not in order_columns", names[i]),
								errhint("The global order extends the sorted run of a column: add it to order_columns.")));
		if ((err = sift_hll_spec_parse(sift_col_options_hll_columns(index), r->schema, (const char *const *) names, r->pagg.hll)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: %s", err)));
		for (int i = 0; i < n; i++)
			if (r->pagg.hll[i])
			{
				Oid			t = TupleDescAttr(desc, defs[i].attno - 1)->atttypid;

				if (t != INT2OID && t != INT4OID && t != INT8OID && t != DATEOID && t != TIMESTAMPOID && t != TIMESTAMPTZOID && t != TEXTOID && t != VARCHAROID && t != UUIDOID)
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" is a %s column, whose hash a unit cannot compute (char(n) and name hash differently from text)", names[i], format_type_be(t))));
				if ((t == TEXTOID || t == VARCHAROID) && !get_collation_isdeterministic(TupleDescAttr(desc, defs[i].attno - 1)->attcollation))
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" has a nondeterministic collation, whose hash is not that of its bytes", names[i])));
				/* a column the build chose for its key list (key_columns 'auto') gives it up: a leaf has one blob a unit in the set section, and the user asked for the sketch */
				if (r->pagg.set[i] && r->pagg.keyauto)
					r->pagg.set[i] = r->pagg.pres[i] = false;
				else if (r->pagg.set[i])
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" is in key_columns, which keeps its distinct keys exactly", names[i])));
			}
		if ((err = sift_cpoint_spec_parse(sift_col_options_point_columns(index), r->schema, (const char *const *) names, r->pagg.point)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_columns option: %s", err)));
		if ((err = sift_cpoint_spec_parse(sift_col_options_point_tid_columns(index), r->schema, (const char *const *) names, r->pagg.point_tid)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_tid_columns option: %s", err)));
		if ((err = sift_cpoint_spec_parse(sift_col_options_count_columns(index), r->schema, (const char *const *) names, r->pagg.count)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid count_columns option: %s", err)));
		for (int i = 0; i < n; i++)
			if (r->pagg.count[i] && r->schema->cols[i].store != SIFT_COLS_STR)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid count_columns option: \"%s\" is not a text column", names[i])));
		r->pagg.dict_hash = sift_col_options(index)->dict_lookup != SIFT_COL_DICT_OFF;
		for (int i = 0; i < n; i++)
			r->pagg.lookup[i] = sift_col_options(index)->dict_lookup == SIFT_COL_DICT_ON || r->pagg.count[i] ||
				(sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO && (r->pagg.set[i] || r->pagg.point[i] || r->pagg.point_tid[i]));
		for (int a = 1; a <= natts; a++)
			if (r->ret[a] && sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO)
				r->pagg.lookup[r->first[a]] = true;
		/* a text leaf whose dictionary is read in place (and would have its order, whatever it holds) also counts the rows of each string, unless the option says off */
		if (strcmp(sift_col_options_count_columns(index), "off") != 0)
			for (int i = 0; i < n; i++)
				if (r->schema->cols[i].store == SIFT_COLS_STR && r->schema->cols[i].type == SIFT_COLT_TEXT && r->pagg.lookup[i])
					r->pagg.count[i] = true;
		{
			char	   *list = pstrdup(sift_col_options_key_cut(index)),
					   *save = NULL;

			for (char *tok = strtok_r(list, ", ", &save); tok != NULL; tok = strtok_r(NULL, ", ", &save))
			{
				int			id = -1;

				for (int i = 0; i < n && id < 0; i++)
					if (strcmp(names[i], tok) == 0)
						id = i;
				if (id < 0 || r->ncut == SIFT_COL_CUT_MAX || !(sift_crun_leaf_ok(r->schema, &r->schema->cols[id]) || sift_col_def_shape_start(r->schema->cols, id)))
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_cut option: \"%s\" is not an integer, date, timestamp, numeric or float column of the index (at most %d)", tok, SIFT_COL_CUT_MAX)));
				r->cut[r->ncut++] = id;
			}
			r->cut_rows = sift_col_options(index)->key_cut_rows;
		}
	}
	r->meta = (SiftColIndexMeta) {.indexoid = RelationGetRelid(index), .heapoid = index->rd_index->indrelid, .natts = natts,
	.ncols = n,.cols = r->schema->cols};
	r->rowcxt = AllocSetContextCreate(mcxt, "sift cache col insert", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	return r;
}

/* Are the units of the sift_col index cut by the order key? (the planner keeps a ctid range, which selects units by heap superblock, on the normal plan for such an index) */
bool
sift_col_index_keyed(Oid indexoid)
{
	Relation	index = index_open(indexoid, NoLock);
	bool		keyed = sift_col_is_index(index) && sift_col_rel(index)->ncut > 0;

	index_close(index, NoLock);
	return keyed;
}

SiftColRel *
sift_col_rel(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = build_rel(index, index->rd_indexcxt);
	return index->rd_amcache;
}

SiftColRel *
sift_col_rel_private(Relation index, MemoryContext mcxt)
{
	return build_rel(index, mcxt);
}
#ifdef SIFT_TESTING
void
sift_col_test_inval(Relation index)
{
	if (!sift_test_col_rel_inval)
		return;
	/* the reload of the entry that a lock taken during the scan may cause frees the cached state: first overwrite what it holds */
	memset(sift_col_rel(index), 0x7f, sizeof(SiftColRel));
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
		plan_memo = AllocSetContextCreate(TopMemoryContext, "sift planning memo", ALLOCSET_DEFAULT_SIZES);
	*plan = am_leaf_plan;
	return plan_memo;
}

uint32
sift_col_pagg_hash(Relation index)
{
	const char *a = sift_col_options_preagg(index),
			   *b = sift_col_options_rollup(index),
			   *c = sift_col_options_rollup_dims(index),
			   *d = sift_col_options_calendar_columns(index);

	return hash_bytes((const unsigned char *) a, strlen(a)) ^ (hash_bytes((const unsigned char *) b, strlen(b)) * 31) ^
		(hash_bytes((const unsigned char *) c, strlen(c)) * 17) ^ (hash_bytes((const unsigned char *) d, strlen(d)) * 13) ^
		(sift_col_options(index)->cube_minmax ? 0x9e3779b9 : 0) ^ (sift_col_options(index)->key_table ? 0x7f4a7c15 : 0) ^ ((uint32) sift_col_format_minor * 0x85ebca6b);
}
