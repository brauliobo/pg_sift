/*
 * col/f2/agg/map_preagg.c - SiftAggV6 answering granules of a map or an array from the cubes of its elements (col/f2/agg/map_preagg.h).
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement or the keys of one granule, not the rows of the table */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/group/int.h"
#include "agg/unnest/cells.h"
#include "agg/vexpr/str.h"
#include "col/f2/f2.h"
#include "col/f2/agg/map_preagg.h"
#include "col/f2/agg/preagg.h"
#include "col/f2/agg/unnest/int.h"
#include "col/f2/preagg/delta.h"

#define MAP_ROWS_ONLY	(-1)
#define MAP_DIM_COUNT(d)	(-2 - (d))	/* the count of the elements whose dimension d is not NULL */

/* What the statement makes of the cubes of one part: the cube of the elements and where each dimension and aggregate is in it */
typedef struct MapBind
{
	uint64		gen;
	bool		bound;
	bool		usable;
	const SiftPreaggPart *pp;
	int			cube;
	int			cdim[SIFT_V6_XELEM_COLS];	/* of each element column the statement reads as a dimension, the dimension of the cube */
	int		   *meas;			/* of each aggregate the measure of the cube it reads, MAP_ROWS_ONLY or MAP_DIM_COUNT */
	SiftKeyDict dict[SIFT_PREAGG_MAX_DIMS];	/* of each dimension of strings */
} MapBind;

struct SiftV6MapPreagg
{
	UnRun	   *u;
	const SiftV6Spec *spec;
	uint64		dims;			/* bit c: element column c is a dimension the statement reads */
	SiftVec		ev[SIFT_V6_XELEM_COLS];	/* the element columns of a batch of cells */
	uint64		keep[SIFT_VEC_WORDS + 1];
	V6Acc	   *acc;
	MemoryContext pcx;			/* what the part in hand is bound to */
	MemoryContext gcx;			/* what a granule reads */
	MapBind		pb;
};

/* The function takes apart a map (the cube of its members) or an array of objects or strings (a cube of SIFT_PCUBE_LIST) */
static bool
elem_fn_map(SiftV6ElemFn fn)
{
	return fn >= SV6E_EACH;
}

static bool
fn_ok(SiftV6ElemFn fn)
{
	return elem_fn_map(fn) || fn == SV6E_RECORDSET || fn == SV6E_ELEMS_TEXT || fn == SV6E_MEMBERS;
}

SiftV6MapPreagg *
sift_v6_map_preagg_begin(UnRun *u)
{
	const SiftV6Spec *spec = u->spec;
	SiftV6MapPreagg *m;
	uint64		dims;

	if (!sift_v6_elem_cells_ok(spec, u->eev, &dims) || !fn_ok(u->x->elem.fn) || (u->x->where != NIL && !u->where_preds))
		return NULL;
	m = palloc0(sizeof(SiftV6MapPreagg));
	*m = (SiftV6MapPreagg) {.u = u,.spec = spec,.dims = dims};
	for (int c = 0; c < u->nel; c++)
		m->ev[c] = (SiftVec) {.data = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * Max(sizeof(SiftStrView), sizeof(int64)) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0),
		.valid = palloc0((SIFT_VEC_WORDS + 1) * sizeof(uint64)),.dense = true,.type = u->st[0].etype[c]};
	m->acc = palloc(SIFT_VEC_LEN * spec->naggs * sizeof(V6Acc));
	m->pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 map preagg part");
	m->gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 map preagg granule");
	return m;
}

void
sift_v6_map_preagg_end(SiftV6MapPreagg *m)
{
	MemoryContextDelete(m->pcx);
	MemoryContextDelete(m->gcx);
}

/* ---- the cube of a part ---- */

static int
find_meas(const SiftPreaggCubeDesc *cd, int leaf)
{
	for (int i = 0; i < cd->nmeasures; i++)
		if (cd->meas_leaf[i] == leaf)
			return i;
	return -1;
}

/* The dimension of the cube that element column c is, -1: none */
static int
col_dim(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, int c)
{
	for (int d = 0; d < cd->ndims; d++)
		if (cd->dim_leaf[d] == m->spec->ext->elem.col_leaf[c])
			return d;
	return -1;
}

/* Where each aggregate is in the cube; false when one reads what the cube does not keep */
static bool
bind_aggs(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, int *meas)
{
	const SiftV6XElem *e = &m->spec->ext->elem;

	for (int j = 0; j < m->spec->naggs; j++)
	{
		const SiftV6Agg *a = &m->spec->agg[j];
		const int	d = a->kind == SV6_COUNT ? col_dim(m, cd, a->arg.slot) : -1;
		int			i;

		if (a->kind == SV6_COUNT_STAR)
			meas[j] = MAP_ROWS_ONLY;
		else if (d >= 0)
			meas[j] = MAP_DIM_COUNT(d);
		else if ((i = find_meas(cd, e->col_leaf[a->arg.slot])) < 0 ||
				 (a->kind != SV6_COUNT && (cd->meas_kind[i] >= SIFT_PMEAS_F4) != (sift_v6_class(a->arg.typid) == SV6C_F64)))
			return false;
		else
			meas[j] = i;
	}
	return true;
}

/* Every element column the statement reads as a dimension is one of the cube, as the statement reads it: a string a dimension of codes, an integer one of integers */
static bool
bind_dims(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, MapBind *pb)
{
	const uint8 kind = elem_fn_map(m->spec->ext->elem.fn) ? SIFT_PCUBE_ELEMENTS : SIFT_PCUBE_KIND;

	if ((cd->flags & SIFT_PCUBE_KIND) != kind)
		return false;
	for (int c = 0; c < m->u->nel; c++)
		if ((m->dims >> c) & 1)
		{
			const SiftVecType want = m->ev[c].type == SVEC_STRVIEW ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64;

			if ((pb->cdim[c] = col_dim(m, cd, c)) < 0 || cd->dim_kind[pb->cdim[c]] != want || (m->ev[c].type != SVEC_STRVIEW && m->ev[c].type != SVEC_I64))
				return false;
		}
	return true;
}

/* The dictionaries of the dimensions of strings of the cube; false when the part keeps none of one */
static bool
bind_dicts(const SiftF2GranOffer *o, const SiftPreaggCubeDesc *cd, MapBind *pb)
{
	for (int d = 0; d < cd->ndims; d++)
	{
		const SiftPartDict *pd = cd->dim_kind[d] == SIFT_PDIM_DICT32 ? sift_part_dict_find(sift_part_dicts(o->rel, o->part), cd->dim_leaf[d], SIFT_DICT_KIND_STR) : NULL;

		if (cd->dim_kind[d] == SIFT_PDIM_DICT32 && pd == NULL)
			return false;
		if (pd != NULL)
			pb->dict[d] = (SiftKeyDict) {.d = {.n = pd->n,.off = pd->off,.bytes = pd->bytes,.part = true},.gen = sift_v6_dict_gen(o->rel, o->desc->part_gen)};
	}
	return true;
}

/* The cube of the part that holds the statement with the fewest dimensions */
static void
bind_part(SiftV6MapPreagg *m, const SiftF2GranOffer *o)
{
	MapBind    *pb = &m->pb;
	MapBind		try;
	int			best = SIFT_PREAGG_MAX_DIMS + 1;
	MemoryContext old;

	MemoryContextReset(m->pcx);
	old = MemoryContextSwitchTo(m->pcx);
	*pb = (MapBind) {.gen = o->desc->part_gen,.bound = true};
	try.meas = palloc(m->spec->naggs * sizeof(int));
	pb->meas = palloc(m->spec->naggs * sizeof(int));
	pb->pp = sift_preagg_part_open(o->rel, o->part);
	for (int k = 0; pb->pp != NULL && k < pb->pp->hdr.ncubes; k++)
	{
		const SiftPreaggCubeDesc *cd = &pb->pp->cube[k];

		if (cd->ndims < best && bind_dims(m, cd, &try) && bind_aggs(m, cd, try.meas) && bind_dicts(o, cd, &try))
		{
			best = cd->ndims;
			memcpy(pb->cdim, try.cdim, sizeof(pb->cdim));
			memcpy(pb->dict, try.dict, sizeof(pb->dict));
			memcpy(pb->meas, try.meas, m->spec->naggs * sizeof(int));
			pb->cube = k;
			pb->usable = true;
		}
	}
	MemoryContextSwitchTo(old);
}

/* ---- the cells of a granule ---- */

/* The values of the dimensions of cell grp as row n of the element columns */
static void
cell_dims(SiftV6MapPreagg *m, const SiftPreaggGroupHdr *grp, uint32 n)
{
	const MapBind *pb = &m->pb;

	for (int c = 0; c < m->u->nel; c++)
		if ((m->dims >> c) & 1)
		{
			const int	d = pb->cdim[c];
			const bool	null = (grp->dim_null_mask >> d) & 1;

			if (null)
				continue;
			if (m->ev[c].type == SVEC_STRVIEW)
			{
				const SiftKeyDict *dict = &pb->dict[d];
				const uint32 code = (uint32) grp->keys[d];

				((SiftStrView *) m->ev[c].data)[n] = sift_vstr_make(dict->d.bytes + dict->d.off[code], dict->d.off[code + 1] - dict->d.off[code]);
			}
			else
				((int64 *) m->ev[c].data)[n] = grp->keys[d];
			m->ev[c].valid[n >> 6] |= UINT64CONST(1) << (n & 63);
		}
}

/* The state of aggregate j from cell grp */
static void
cell_acc(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGroupHdr *grp, int j, V6Acc *out)
{
	const int	meas = m->pb.meas[j];

	*out = (V6Acc) {0};
	if (meas == MAP_ROWS_ONLY)
		out->n = grp->row_count;
	else if (meas < MAP_ROWS_ONLY)
		out->n = ((grp->dim_null_mask >> (MAP_DIM_COUNT(0) - meas)) & 1) ? 0 : grp->row_count;
	else
		sift_v6_preagg_meas_acc(&m->spec->agg[j], sift_pag_measure(cd, grp, meas), out);
}

/* The n cells in the element columns and the states, through the quals of the element, on to the groups */
static void
emit_batch(SiftV6MapPreagg *m, uint32 n)
{
	memset(m->keep, 0, sizeof(m->keep));
	if (m->u->eev != NULL)
		sift_v6_elem_eval(m->u->eev, m->ev, 0, n, NULL, m->keep);
	else
		for (uint32 i = 0; i < n; i++)
			m->keep[i >> 6] |= UINT64CONST(1) << (i & 63);
	for (int c = 0; c < m->u->nel; c++)
		m->ev[c].len = (uint16) n;
	sift_f2_unnest_emit_cells(m->u, m->ev, m->keep, m->acc, n);
}

static void
batch_reset(SiftV6MapPreagg *m)
{
	for (int c = 0; c < m->u->nel; c++)
		memset(m->ev[c].valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
}

/* The cells of the granule that hold elements, a batch at a time */
static void
fold_cells(SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran)
{
	const int	naggs = m->spec->naggs;
	uint32		n = 0;

	batch_reset(m);
	for (uint32 c = 0; c < gran->ncells; c++)
	{
		const SiftPreaggGroupHdr *grp = sift_pag_group(cd, gran->cells, c);

		if (grp->row_count == 0)
			continue;
		cell_dims(m, grp, n);
		for (int j = 0; j < naggs; j++)
			cell_acc(m, cd, grp, j, &m->acc[n * naggs + j]);
		if (++n == SIFT_VEC_LEN)
		{
			emit_batch(m, n);
			batch_reset(m);
			n = 0;
		}
	}
	if (n > 0)
		emit_batch(m, n);
}

bool
sift_v6_map_preagg_try_granule(void *arg, const SiftF2GranOffer *o)
{
	SiftV6MapPreagg *m = arg;
	const MapBind *pb = &m->pb;
	uint16		dead[SIFT_PREAGG_DEAD_MAX];
	MemoryContext old;
	SiftPreaggGran gran;
	bool		done = false;

	if (!(o->desc->ext_flags & SIFT_PART_EXT_PREAGG) || !(o->gd->flags & SIFT_GDESC_HAS_PREAGG) || o->patch != NULL || !sift_v6_group_takes_cells(m->u->pipe->group) ||
		(m->spec->ext->where != NIL && !o->rows_pass))
		return false;
	if (!pb->bound || pb->gen != o->desc->part_gen)
		bind_part(m, o);
	if (!pb->usable || !sift_v6_preagg_rows_known(&m->u->vet, o) || sift_preagg_dead_rows(o, dead) != 0)
		return false;
	MemoryContextReset(m->gcx);
	old = MemoryContextSwitchTo(m->gcx);
	sift_preagg_gran_read(o->rel, pb->pp, pb->cube, o->g, &gran);
	if (gran.ncells > 0)
	{
		fold_cells(m, &pb->pp->cube[pb->cube], &gran);
		done = true;
	}
	MemoryContextSwitchTo(old);
	m->u->stats->grans_preagg += done;
	return done;
}
