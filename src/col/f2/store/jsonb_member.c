/*
 * col/f2/store/jsonb_member.c - the paths below a jsonb document of an index or a table of format major 2 (SiftJsonbOps.member_leaf, path_declared): `doc -> 'a' ->> 'k'` reads the leaf the paths
 * option declares at `doc.a.k`, one value a row as a top-level key has, or the member `k` of a map `doc.a.*`: one of the elements of its group, which the scan finds by its key in each row.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/value.h"

#include "col/f2/store/ops_int.h"
#include "col/f2/store/store.h"

/* the path text the shredder gives a leaf of the keys of path below the attribute named name: name.k1.k2 */
static char *
path_text(const char *name, const List *path)
{
	StringInfoData b;

	initStringInfo(&b);
	appendStringInfoString(&b, name);
	foreach_node(String, k, path)
	{
		if (strpbrk(strVal(k), ".[]*#") != NULL)
			return NULL;
		appendStringInfo(&b, ".%s", strVal(k));
	}
	return b.data;
}

/* the leaf of attribute att whose path text is path followed by suffix, a top-level leaf of one value a row when scalar; -1: none */
static int
leaf_at(const SiftF2Store *st, int att, const List *path, const char *suffix, bool scalar)
{
	const SiftColSchema *sch = st->schema;
	const int	first = att >= 1 && att <= st->natts ? st->att_first[att] : -1;
	char	   *text = first >= 0 && sch->cols[first].name != NULL ? path_text(sch->cols[first].name, path) : NULL;
	const size_t len = text != NULL ? strlen(text) : 0;

	for (int c = first; text != NULL && c < sch->ncols && sch->cols[c].attno == att; c++)
	{
		const SiftColDef *d = &sch->cols[c];

		if (d->path != NULL && strncmp(d->path, text, len) == 0 && strcmp(d->path + len, suffix) == 0 && d->lane == 0 &&
			(!scalar || (d->kind == SIFT_COLK_SCALAR && d->parent < 0)))
			return c;
	}
	return -1;
}

static bool
float8_leaf(const SiftColDef *d)
{
	return d->store == SIFT_COLS_F8 && d->type == SIFT_COLT_FLOAT8;
}

/* a leaf of the numbers or the booleans of a path: float8, integers or bool */
static bool
value_leaf(const SiftColDef *d)
{
	return float8_leaf(d) || (d->store == SIFT_COLS_INT && ((d->type >= SIFT_COLT_INT2 && d->type <= SIFT_COLT_INT8) || d->type == SIFT_COLT_BOOL));
}

/* the leaf of the values of the map whose member the path names (`doc.a.*` for doc -> 'a' -> 'k'), when they are float8 and the keys are text; -1: none */
static int
map_value_leaf(const SiftF2Store *st, int att, const List *path)
{
	const int	group = leaf_at(st, att, list_copy_head(path, list_length(path) - 1), ".*", false);
	int			key = -1,
				value = -1;

	for (int c = group + 1; group >= 0 && c < st->schema->ncols; c++)
		if (st->schema->cols[c].parent == group && strstr(st->schema->cols[c].path, "#key") != NULL)
			key = c;
		else if (st->schema->cols[c].parent == group)
			value = c;
	return key >= 0 && value >= 0 && st->schema->cols[key].store == SIFT_COLS_STR && float8_leaf(&st->schema->cols[value]) ? value : -1;
}

int
sift_f2_jsonb_path_leaf(Oid relid, int att, const List *path)
{
	return leaf_at(sift_f2_store_from_oid(relid), att, path, "", true);
}

int
sift_f2_jsonb_member_leaf(Oid relid, int att, const List *path)
{
	const SiftF2Store *st = sift_f2_store_from_oid(relid);
	const int	leaf = leaf_at(st, att, path, "", true);

	if (leaf >= 0)
		return value_leaf(&st->schema->cols[leaf]) ? leaf : -1;
	return map_value_leaf(st, att, path);
}

/* the paths option declares the leaf at the path, its array, or the map whose member the path is */
bool
sift_f2_jsonb_path_declared(Oid relid, bool is_table, int att, const List *path)
{
	const SiftF2Store *st;

	if (is_table)
		return true;
	st = sift_f2_store_from_oid(relid);
	return leaf_at(st, att, path, "", false) >= 0 || leaf_at(st, att, path, "[]", false) >= 0 ||
		(path != NIL && leaf_at(st, att, list_copy_head(path, list_length(path) - 1), ".*", false) >= 0);
}
