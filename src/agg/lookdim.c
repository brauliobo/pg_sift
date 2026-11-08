/*
 * agg/lookdim.c - the dimension map: the classes of the keys of a joined table, kept in shared memory for every backend.
 *
 * A join whose table is a dimension (one int4 key, a few columns the query reads, a few distinct tuples of them) is answered from a class of
 * each key and one row for each class (map_load_dense, agg/lookcol.c), which is read from the leaves of a sift_col index of the table, not from
 * its heap. The first backend that needs the map reads the leaves and keeps the classes in a dynamic shared memory segment (sift.agg_dimmap_kb,
 * the directory and its eviction are agg/lookshm.c's); every later statement of any backend attaches the segment and reads it in place, so the
 * load of the table, which is a statement's cost in proportion to the rows of the dimension, is paid once for all of them. A map that does not
 * fit the budget is not kept: the statement loads the classes itself, as it does without a map.
 *
 * What a map is valid for is decided by agg/dimgen.c: the state of the dimension it was read at, compared with the state now.
 */
#include "postgres.h"

#include "miscadmin.h"

#include "agg/agg_int.h"
#include "agg/lookup/int.h"

/*
 * The classes of the keys of the dimension of the join of combo ci, in cxt: from the shared memory when a backend kept them for the state the
 * dimension is in, else read from the leaves now and kept. NULL when the join is not one the executor takes as classes, the dimension cannot
 * be vouched for, or its rows are not classes of their keys (a key twice, too many tuples): the caller loads the table as before.
 */
MapTab *
lookdim_load(AggExec *ex, int ci, Relation rel, MemoryContext cxt)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	LookCols	need = {.n = j->ncol},
				have;
	DimStamp   *before,
			   *after;
	char	   *key;
	MapTab	   *m;
	bool		large;

	if (ex->first_peek || lookshm_bytes(LOOK_DIM) == 0 || lookup_keys_only(ex->spec, ex->lay, ci, ex->nbase) || !sift_agg_dense_join(ex->spec, ci, ex->lay) ||
		!map_load_dense_possible(rel, j) || (before = dimgen_take(rel)) == NULL)
		return NULL;
	for (int c = 0; c < j->ncol; c++)
	{
		need.att[c] = j->colatt[c];
		need.type[c] = j->coltype[c];
	}
	key = psprintf("%u|%s", GetUserId(), shared_map_key(ex, ci, true));
	if ((m = lookshm_find(LOOK_DIM, key, dimgen_finger(before), &need, &have, cxt)) != NULL ||
		(lookshm_claim(LOOK_DIM, key, cxt) && (m = lookshm_find(LOOK_DIM, key, dimgen_finger(before), &need, &have, cxt)) != NULL))
	{
		ex->ndim_shared++;
		return map_view(m, &have, j, cxt);
	}
	m = map_new(j, false, cxt);
	if (!map_load_dense(ex, m, j, cb, rel))
	{
		lookshm_unclaim(LOOK_DIM, key);
		return NULL;
	}
	map_index_rows(m);
	ex->ndim_built++;
	if ((after = dimgen_take(rel)) != NULL && dimgen_same(before, after))
		lookshm_keep(LOOK_DIM, key, dimgen_finger(before), m, &need, &large);
	lookshm_unclaim(LOOK_DIM, key);
	return m;
}
