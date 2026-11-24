/*
 * col/f2/f2_connect.c - connect on an index of format major 2 (col/f2/f2_connect.h).
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "utils/lsyscache.h"

#include "col/am.h"
#include "col/am/conn_route.h"
#include "col/f2/f2_connect.h"
#include "col/f2/f2_journal.h"
#include "core/meta_peek.h"

/* Calls fn(parent table, parent index, spec) once for each parent index the option names (a parent two connections share is called once) */
static void
each_parent(Relation heap, Relation index, void (*fn) (Relation parent, Relation pindex, const SiftConnSpec *s))
{
	SiftConnSpecs *specs = sift_conn_specs(index);
	List	   *seen = NIL;

	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		Relation	parent;
		Oid			pidx = sift_conn_route_parent(heap, index, specs->c[c], &parent);

		if (!list_member_oid(seen, pidx))
		{
			Relation	pindex = index_open(pidx, ShareUpdateExclusiveLock);

			fn(parent, pindex, specs->c[c]);
			index_close(pindex, NoLock);
			seen = lappend_oid(seen, pidx);
		}
		table_close(parent, NoLock);
	}
	list_free(seen);
	if (specs != NULL)
		sift_conn_specs_free(specs);
}

/* An index of the parent that has not chosen its key gets the one the child joins by: the key journal records the keys of that column */
static void
give_key(Relation parent, Relation pindex, const SiftConnSpec *s)
{
	if (sift_col_row_key_on(pindex))
		return;
	sift_col_set_option(pindex, "track_changes", get_attname(RelationGetRelid(parent), get_attnum(RelationGetRelid(parent), s->parent_key), false));
	CommandCounterIncrement();
}

static void
enable_journal(Relation parent, Relation pindex, const SiftConnSpec *s)
{
	(void) sift_f2_journal_enable(parent, pindex);
}

void
sift_f2_connect_resolve(Relation heap, Relation index)
{
	if (strcmp(sift_col_options_connect(index), "auto") == 0)
		sift_conn_auto(heap, index);
	sift_conn_route_check_parents(heap, index, SIFT_FORMAT2_MAJOR);
	each_parent(heap, index, give_key);
	sift_conn_resolve(heap, index);
}

void
sift_f2_connect_build(Relation heap, Relation index, bool concurrent)
{
	Oid			origin = RelationGetRelid(index);
	bool		wanted = sift_conn_children(origin) != NIL;

	if (!wanted && (origin = sift_col_swap_origin(heap, index, true)) != InvalidOid)
		wanted = sift_conn_children(origin) != NIL;
	if (wanted && !concurrent && sift_col_track_on(index))
		(void) sift_f2_journal_make(heap, index);
	each_parent(heap, index, enable_journal);
}

void
sift_f2_connect_alter(Relation heap, Relation index)
{
	sift_f2_connect_resolve(heap, index);
	each_parent(heap, index, enable_journal);
}
