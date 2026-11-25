/*
 * col/v1_engine/v1_store_ops_jsonb.c - SiftJsonbOps of Format 1 (core/store_ops.h).
 *
 * key_leaf, table_key_leaf: the shredded scalar leaf of a top-level key of a document whose chunks keep the JSON type of each row (a path declared with a type or as text), -1 for any other key. member_leaf: the leaf of
 * the float8 values of the map `doc.a.*` that a member `doc -> 'a' ->> 'k'` is read from (v1_store_ops_member.c), -1 for any other path.
 * path_declared: the paths option of the index declares the path of a document attribute.
 * unnest_run: the aggregate scan of a LATERAL unnest of an array or a map column, of the keys of a map and of the tests of the elements of a row (v1_store_ops_unnest.c).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"

#include "col/v1/v1_store_ops.h"
#include "table/v1/v1_tbl_ops.h"

static bool
typed_leaf(Oid typid)
{
	return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID || typid == TEXTOID;
}

static int
key_leaf(Oid relid, bool is_table, int att, const char *key, int len)
{
	SiftV1Rel	r;
	const SiftSrcLayout *lay;
	int			found = -1;

	if (!is_table && get_rel_relkind(relid) != RELKIND_INDEX)
		return -1;
	r = sift_v1_rel_open(relid, is_table);
	lay = sift_v1_reader_of(is_table)->layout(r.rel);
	for (int i = 0; i < lay->nleaves && found < 0; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' && typed_leaf(l->typid) && list_length(l->path) == 1 &&
			strncmp(strVal(linitial(l->path)), key, len) == 0 && strVal(linitial(l->path))[len] == '\0')
			found = i;
	}
	sift_v1_rel_close(&r);
	return found;
}

static int
jsonb_member_leaf(Oid indexoid, int att, const List *path)
{
	SiftV1Rel	r;
	int			leaf;

	if (get_rel_relkind(indexoid) != RELKIND_INDEX)
		return -1;
	r = sift_v1_rel_open(indexoid, false);
	leaf = sift_v1_member_value_leaf(sift_v1_idx_reader.layout(r.rel), att, path);
	sift_v1_rel_close(&r);
	return leaf;
}

static int
jsonb_key_leaf(Oid indexoid, int att, const char *key, int len)
{
	return key_leaf(indexoid, false, att, key, len);
}

static int
jsonb_table_key_leaf(Oid relid, int att, const char *key, int len)
{
	return key_leaf(relid, true, att, key, len);
}

/* the leaf is at the path, or is the object of free keys (`path.*`) whose member the path is */
static bool
leaf_declares(const SiftSrcLeaf *l, int att, const List *path)
{
	if (l->attno != att || l->parent >= 0)
		return false;
	if (l->kind == SCL_OBJECT)
		return list_length(path) == list_length(l->path) + 1 && equal(list_copy_head(path, list_length(l->path)), l->path);
	return equal(l->path, path);
}

static bool
jsonb_path_declared(Oid relid, bool is_table, int att, const List *path)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = sift_v1_reader_of(is_table)->layout(r.rel);
	bool		found = false;

	for (int i = 0; i < lay->nleaves && !found; i++)
		found = leaf_declares(&lay->leaf[i], att, path);
	sift_v1_rel_close(&r);
	return found;
}

static bool
jsonb_types_exact(Oid relid, bool is_table, int leaf, uint32 types)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftV1ReaderOps *reader = sift_v1_reader_of(is_table);
	SiftSrcStats st;
	bool		exact;

	reader->stats(r.rel, &st);
	exact = !st.nokinds[leaf] || !sift_src_types_need_kinds(types >> 1, reader->layout(r.rel)->leaf[leaf].astext);
	sift_v1_rel_close(&r);
	return exact;
}

const SiftJsonbOps v1_jsonb_ops = {
	.key_leaf = jsonb_key_leaf,
	.member_leaf = jsonb_member_leaf,
	.table_key_leaf = jsonb_table_key_leaf,
	.types_exact = jsonb_types_exact,
	.path_declared = jsonb_path_declared,
	.unnest_run = sift_v1_unnest_run
};
