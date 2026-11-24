/*
 * col/am/options_read.c - how an index reads its reloptions (am_col_amoptions put them in the rd_options of the index) and the checks of an ALTER of them
 */
#include "postgres.h"
#include "access/reloptions.h"
#include "col/am/am_int.h"
#include "col/am/conn_route.h"
#include "col/v1/conn.h"
#include "col/scope.h"
#include "core/lane.h"
#include "core/ops.h"

const SiftColOptions *
sift_col_options(Relation index)
{
	static const SiftColOptions defaults = {.write_buffer_limit = SIFT_DEFAULT_WRITE_BUFFER_LIMIT, .dead_rewrite_pct = SIFT_COL_DEAD_AUTO, .rollup_on_demand = true, .vacuum_truncate = true, .cluster_advice = true, .dict_lookup = SIFT_COL_DICT_AUTO, .cube_minmax = true, .key_table = true, .superunit = SIFT_SUPER_DEFAULT_FAN};

	return index->rd_options ? (const SiftColOptions *) index->rd_options : &defaults;
}

COL_OPT_STR(sift_col_options_paths_raw, paths, "")

/* 'auto' (decided at the build) or 'auto: spec' (decided by an earlier build); a spec starts with a key and a type, so it never reads like this */
/* the option is empty or a bare 'auto' on an index with a jsonb column: its paths are read from the documents at the next build */
bool
sift_col_paths_redecide(Relation index)
{
	const char *opt = sift_col_options_paths_raw(index);

	if (opt[0] != '\0' && !(sift_col_paths_auto(opt) && opt[4] == '\0'))
		return false;
	for (int i = 0; i < IndexRelationGetNumberOfAttributes(index); i++)
		if (TupleDescAttr(RelationGetDescr(index), i)->atttypid == JSONBOID)
			return true;
	return false;
}

/* paths is empty or a bare 'auto': no leaf of a jsonb column is chosen yet */
bool
sift_col_paths_unresolved(Relation index)
{
	const char *opt = sift_col_options_paths_raw(index);

	return opt[0] == '\0' || strcmp(opt, "auto") == 0;
}

bool
sift_col_paths_auto(const char *opt)
{
	return strncmp(opt, "auto", 4) == 0 && (opt[4] == '\0' || opt[4] == ':');
}

/* The spec the shredder parses: the option, or what follows 'auto:' ("" while 'auto' has not been decided) */
const char *
sift_col_options_paths(Relation index)
{
	const char *opt = sift_col_options_paths_raw(index);

	if (!sift_col_paths_auto(opt))
		return opt;
	return opt[4] == ':' ? opt + 5 + strspn(opt + 5, " ") : "";
}

COL_OPT_STR(sift_col_options_compression, compression, "")

COL_OPT_STR(sift_col_options_preagg, preagg, "")

/*
 * No key column: the triggers of each row give the address of the version it ends (a column cannot be called ctid). That is track_changes = ctid, and every tracking of an index of
 * a format that follows addresses (format major 2): it is on unless the option says off
 */
bool
sift_col_track_ctid(Relation index)
{
	const char *rk = sift_col_options_row_key(index);

	return sift_col_ops(index)->tracks_addresses ? strcmp(rk, "off") != 0 : strcmp(rk, "ctid") == 0;
}

/*
 * An ALTER INDEX is checked by building the state the new options give; an index that has segments written under the leaf list of the old options (stale) is not left with the
 * new ones, so that it goes on serving while its replacement is built beside it (CREATE INDEX CONCURRENTLY, which writes the stamp of its scope as a REINDEX does). An index with
 * no segment has nothing to write again: its first write takes the options. paths = 'auto' without a spec (or RESET (paths)) on an index with a jsonb column has no leaf list until
 * the next build reads the documents; the segments an index of that kind holds all have the paths of an earlier build, so it is refused as any other change of the leaf list.
 */
void
sift_col_alter_check(Relation index)
{
	if (!sift_col_is_index(index))
		return;
	if (sift_col_ops(index)->format_major == 1 && sift_col_paths_redecide(index))
	{
		SiftDirSnapshot *snap;

		snap = sift_dir_read(index);
		if (sift_segs_have_layout(snap->segs, snap->nsegs))
			sift_lane_alter_refuse(index, "column definitions");
		pfree(snap);
		return;
	}
	(void) sift_col_rel(index);
	sift_col_row_key_check(index);
	sift_scope_journal_alter(index);
	sift_col_track_reconcile(index);
	sift_conn_route_alter(index);
	sift_scope_alter(index);
	if (sift_col_rel(index)->stale)
		sift_lane_alter_refuse(index, "column definitions");
}


COL_OPT_STR(sift_col_options_key_columns, key_columns, NULL)

COL_OPT_STR(sift_col_options_count_columns, count_columns, "")

COL_OPT_STR(sift_col_options_point_tid_columns, point_tid_columns, "")

COL_OPT_STR(sift_col_options_key_order, key_order, "")

/* whether the key_order is what a build chose ('auto: ...'), decided again by the next one (keycols/gorder.c) */
bool
sift_col_options_key_order_auto(Relation index)
{
	return strncmp(sift_col_options_key_order(index), "auto:", 5) == 0;
}

/* whether key_order is a bare 'auto': the build of an index of format major 2 chooses it from the rows it reads */
bool
sift_col_key_order_auto_pending(Relation index)
{
	return strcmp(sift_col_options_key_order(index), "auto") == 0;
}

/* the column that track_changes names (am_col_amoptions took the quotes out of it) */
const char *
sift_col_options_row_key(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->track_changes ? (const char *) o + o->track_changes : "auto";
}

COL_OPT_STR(sift_col_options_connect, connect, "")

COL_OPT_STR(sift_col_options_scope, scope, "")

/* whether the user wrote a key_order that names an order: what the build chose with key_cut ('auto: a, b' in key_order) is decided again with it */
bool
sift_col_options_key_order_user(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->ko_last && !o->ko_auto;
}

/* the group of a key_order of the user's, 'group, key' (NULL for any other): the column a build with no key_cut cuts the units by */
const char *
sift_col_options_key_order_group(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	if (o->ko_group == 0 || o->ko_auto)
		return NULL;
	return (const char *) o + o->ko_group;
}

/* the leaves the option names: 'auto: a, b' is the choice of a build, 'auto' and 'off' name none */
const char *
sift_col_options_key_cut(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);
	const char *v = o->key_cut ? (const char *) o + o->key_cut : o->ko_key ? (const char *) o + o->ko_key : "";

	if (strncmp(v, "auto:", 5) == 0)
		return v + 5;
	return strcmp(v, "auto") == 0 || strcmp(v, "off") == 0 ? "" : v;
}

/* whether a build decides key_cut: the option is not set, or holds a choice of an earlier build */
bool
sift_col_options_key_cut_auto(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return (o->key_cut == 0 && o->ko_key == 0) || (o->key_cut != 0 && strncmp((const char *) o + o->key_cut, "auto", 4) == 0);
}
