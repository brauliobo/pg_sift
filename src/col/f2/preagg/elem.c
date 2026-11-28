/*
 * col/f2/preagg/elem.c - which cubes of the elements of its maps and arrays a part gets (col/f2/preagg/elem.h).
 */
/* check-memory: allow-allocs the keys of the elements of the first granule of a part, once per plan */
#include "core/host.h"

#include "col/f2/preagg/elem.h"
#include "core/hash.h"
#include "port/pg_bitutils.h"

#define PAG_MAP_GROUPS			256		/* the most keys a granule has in the cube of the elements of a map or an array */

/* The measure a value leaf of a map or an element is: an integer or a float8 */
static bool
map_measure(const SiftColIn *c, SiftPreaggMeasKind *kind)
{
	if (c->key_leaf || c->astext || c->hint != SIFT_CHINT_AUTO || c->promo != NULL)
		return false;
	*kind = c->kind == SIFT_CELL_F8 ? SIFT_PMEAS_F8 : SIFT_PMEAS_INT64;
	return c->kind == SIFT_CELL_F8 || c->kind == SIFT_CELL_INT;
}

/* The group of the arrays a leaf is an element of (its lengths, a column of the rows) is a map: one of its leaves is the `#key` leaf */
static bool
map_group(const SiftRows *rows, int group1)
{
	for (int c = 0; rows->map_key != NULL && c < rows->ncols; c++)
		if (rows->map_key[c] && rows->cols[c].group1 == group1)
			return true;
	return false;
}

/* The group of the elements is an array of objects or strings that is a column of the rows */
static bool
list_group(const SiftRows *rows, int group1)
{
	return group1 != 0 && rows->cols[group1 - 1].group1 == 0 && !map_group(rows, group1);
}

/* The flags of the cube of the elements a string leaf groups (the `#key` leaf of a map, or a leaf of an array of objects or of strings that is a column of the rows), 0: it groups none */
static uint8
elem_cube_flags(const SiftRows *rows, const SiftDictEnc *const *dicts, int c)
{
	const SiftColIn *key = &rows->cols[c];

	if (key->kind != SIFT_CELL_STR || key->group1 == 0 || !sift_pag_dict_codes_kept(key, dicts != NULL ? dicts[c] : NULL))
		return 0;
	if (rows->map_key != NULL && rows->map_key[c])
		return SIFT_PCUBE_ELEMENTS;
	return list_group(rows, key->group1) ? SIFT_PCUBE_KIND : 0;
}

/* The elements of the arrays of the first n rows */
static int
elem_values(const SiftRows *rows, int group1, int n)
{
	const SiftColIn *len = &rows->cols[group1 - 1];

	return (int) (len->eoff[n] - len->eoff[0]);
}

/* An integer leaf of an array that is a dimension of its elements: a plain integer of few values */
static bool
int_dim(const SiftRows *rows, int c)
{
	const SiftColIn *col = &rows->cols[c];
	SiftPreaggMeasKind kind;
	uint32		nd;

	if (col->kind != SIFT_CELL_INT || !list_group(rows, col->group1) || !map_measure(col, &kind))
		return false;
	nd = sift_pag_sample_distinct(col, elem_values(rows, col->group1, rows->nrows), PAG_MAP_GROUPS);
	return nd > 0 && nd < PAG_MAP_GROUPS;
}

static bool
listed(const int *v, int n, int leaf)
{
	for (int i = 0; i < n; i++)
		if (v[i] == leaf)
			return true;
	return false;
}

/* The leaf may be a measure of the elements of group1: the rollup of the option preagg names it, or names no measure of those elements */
static bool
declared_meas(const SiftRows *rows, int group1, int leaf)
{
	const SiftPreaggDecl *dc = rows->preagg_decl;
	bool		any = false;

	for (int i = 0; dc != NULL && i < dc->nrmeas; i++)
	{
		if (dc->rmeas[i] == leaf)
			return true;
		any |= dc->rmeas[i] < rows->ncols && rows->cols[dc->rmeas[i]].group1 == group1;
	}
	return !any;
}

/* A cube of the elements of dims of at most groups cells a granule, whose measures are the other numeric leaves of the same elements (the declared ones) */
static void
add_cube(const SiftRows *rows, SiftPreaggPlan *plan, uint8 flags, const int *dims, const uint8 *dkind, int ndims, uint32 groups)
{
	SiftPreaggCubeDesc *cd = &plan->cube[plan->ncubes++];
	const int	group1 = rows->cols[dims[0]].group1;

	*cd = (SiftPreaggCubeDesc) {.ndims = (uint8) ndims,.flags = flags,.max_groups_per_gran = groups};
	for (int d = 0; d < ndims; d++)
	{
		cd->dim_leaf[d] = (uint16) dims[d];
		cd->dim_kind[d] = dkind[d];
	}
	for (int v = 0; v < rows->ncols && cd->nmeasures < SIFT_PREAGG_MAX_MEAS; v++)
	{
		SiftPreaggMeasKind kind;

		if (!listed(dims, ndims, v) && rows->cols[v].group1 == group1 && map_measure(&rows->cols[v], &kind) && declared_meas(rows, group1, v))
		{
			cd->meas_leaf[cd->nmeasures] = (uint16) v;
			cd->meas_kind[cd->nmeasures++] = (uint8) kind;
		}
	}
}

/* The key of value r of a dimension: the code of a string, the integer, -1 for NULL */
static int64
elem_key(const SiftColIn *col, const SiftDictEnc *dict, int r)
{
	if (col->isnull != NULL && col->isnull[r])
		return -1;
	return col->kind == SIFT_CELL_STR ? (int64) (col->dcode != NULL ? col->dcode[r] : dict->row_codes[r]) : col->i[r];
}

/* The groups of the dimensions together fit a cube of the elements of the first granule of the rows of fewer than cap cells */
static bool
joint_fits(const SiftRows *rows, const SiftDictEnc *const *dicts, const int *dims, int ndims, uint32 cap)
{
	const int	n = elem_values(rows, rows->cols[dims[0]].group1, Min(rows->nrows, SIFT_GRANULE_ROWS));
	const uint32 size = pg_nextpower2_32(cap * 2);
	uint64	   *seen = palloc0(size * sizeof(uint64));
	uint32		groups = 0;

	for (int r = 0; r < n && groups < cap; r++)
	{
		uint64		h = 0;
		uint32		at;

		for (int d = 0; d < ndims; d++)
			h = sift_fmix64(h ^ (uint64) elem_key(&rows->cols[dims[d]], dicts != NULL ? dicts[dims[d]] : NULL, r)) + d;
		h |= 1;
		for (at = (uint32) h & (size - 1); seen[at] != 0 && seen[at] != h; at = (at + 1) & (size - 1))
			;
		groups += seen[at] == 0;
		seen[at] = h;
	}
	pfree(seen);
	return groups < cap;
}

/* The leaf is a dimension the rollup of the option preagg declares */
static bool
declared_dim(const SiftRows *rows, int c)
{
	const SiftPreaggDecl *dc = rows->preagg_decl;

	return dc != NULL && listed(dc->rdim, dc->nrdims, c);
}

/* The option preagg declares dimensions of the elements of the list of lengths column l */
static bool
list_declared(const SiftRows *rows, int l)
{
	for (int c = 0; c < rows->ncols; c++)
		if (rows->cols[c].group1 == l + 1 && declared_dim(rows, c))
			return true;
	return false;
}

/* Of the array of objects of lengths column l: the dimensions its cubes group by, the declared ones when the rollup names some, of what kind each is */
static int
list_dims(const SiftRows *rows, const SiftDictEnc *const *dicts, int l, bool declared, int *dims, uint8 *dkind)
{
	int			nd = 0;

	for (int c = 0; c < rows->ncols && nd < SIFT_PREAGG_MAX_DIMS; c++)
	{
		const bool	str = rows->cols[c].group1 == l + 1 && elem_cube_flags(rows, dicts, c) != 0;
		SiftPreaggMeasKind kind;

		if (declared && !declared_dim(rows, c))
			continue;
		if (str || (rows->cols[c].group1 == l + 1 && (declared ? rows->cols[c].kind == SIFT_CELL_INT && map_measure(&rows->cols[c], &kind) : int_dim(rows, c))))
		{
			dims[nd] = c;
			dkind[nd++] = str ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64;
		}
	}
	return nd;
}

/* Of the array of objects of lengths column l: the cube of its dimensions together, which answers each of them too, else one of each integer dimension; a declared list the cube of its
 * dimensions together when their groups fit, then one of each integer dimension (those of strings are sift_pag_add_elem_cubes's), with no byte budget */
static void
add_list_cubes(const SiftRows *rows, const SiftDictEnc *const *dicts, SiftPreaggPlan *plan, int l)
{
	const bool	declared = list_declared(rows, l);
	const uint32 groups = declared ? SIFT_PAG_DECLARED_GROUPS : PAG_MAP_GROUPS;
	int			dims[SIFT_PREAGG_MAX_DIMS];
	uint8		dkind[SIFT_PREAGG_MAX_DIMS];
	const int	nd = list_dims(rows, dicts, l, declared, dims, dkind);
	const bool	joint = nd > 1 && plan->ncubes < SIFT_PREAGG_MAX_CUBES && joint_fits(rows, dicts, dims, nd, groups);

	if (joint)
		add_cube(rows, plan, SIFT_PCUBE_KIND, dims, dkind, nd, groups);
	for (int d = 0; (declared || !joint) && d < nd && plan->ncubes < SIFT_PREAGG_MAX_CUBES; d++)
		if (dkind[d] == SIFT_PDIM_INT64)
			add_cube(rows, plan, SIFT_PCUBE_KIND, &dims[d], &dkind[d], 1, groups);
}

void
sift_pag_add_elem_cubes(const SiftRows *rows, const SiftDictEnc *const *dicts, SiftPreaggPlan *plan)
{
	const uint8 dict32 = SIFT_PDIM_DICT32;

	for (int c = 0; c < rows->ncols && plan->ncubes < SIFT_PREAGG_MAX_CUBES; c++)
	{
		const uint8 flags = elem_cube_flags(rows, dicts, c);

		if (flags != 0 && (flags != SIFT_PCUBE_KIND || !list_declared(rows, rows->cols[c].group1 - 1) || declared_dim(rows, c)))
			add_cube(rows, plan, flags, &c, &dict32, 1, declared_dim(rows, c) ? SIFT_PAG_DECLARED_GROUPS : PAG_MAP_GROUPS);
	}
	for (int l = 0; l < rows->ncols; l++)
		if (rows->cols[l].hint == SIFT_CHINT_LIST && list_group(rows, l + 1))
			add_list_cubes(rows, dicts, plan, l);
}
