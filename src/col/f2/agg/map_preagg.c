/*
 * col/f2/agg/map_preagg.c - SiftAggV6 answering granules of a map or an array from the cubes of its elements (col/f2/agg/map_preagg.h).
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement or the keys of one granule, not the rows of the table */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/group/int.h"
#include "agg/unnest/jsonb.h"
#include "agg/vexpr/str.h"
#include "col/f2/f2.h"
#include "col/f2/agg/map_preagg.h"
#include "col/f2/agg/preagg.h"
#include "col/f2/preagg/delta.h"
#include "core/memtrack.h"

#define MAP_ROWS_ONLY	(-1)
#define MAP_DIM_COUNT(d)	(-2 - (d))	/* the count of the elements whose dimension d is not NULL */

/* What the statement makes of the cubes of one part: the cube of the elements and where each key, test and aggregate is in it */
typedef struct MapBind
{
	uint64		gen;
	bool		bound;
	bool		usable;
	const SiftPreaggPart *pp;
	int			cube;
	int			kdim[SIFT_PREAGG_MAX_DIMS];	/* of each key the dimension of the cube */
	int			tdim;			/* the dimension the tests read, -1: none */
	int		   *meas;			/* of each aggregate the measure of the cube it reads, MAP_ROWS_ONLY or MAP_DIM_COUNT */
	SiftKeyDict dict[SIFT_PREAGG_MAX_DIMS];	/* of each dimension of strings */
} MapBind;

/* A batch of the keys of the cells */
typedef struct MapKeys
{
	SiftVec		vec[SIFT_PREAGG_MAX_DIMS];
	SiftVec    *vp[SIFT_PREAGG_MAX_DIMS];
	void	   *data[SIFT_PREAGG_MAX_DIMS];
	uint64		valid[SIFT_PREAGG_MAX_DIMS][SIFT_VEC_WORDS];
} MapKeys;

struct SiftV6MapPreagg
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6Vet  *vet;
	SiftV6ScanStats *stats;
	int			tslot;			/* the column of the element all the tests read, -1: none */
	SiftClause	ewhere;
	bool		has_ewhere;
	int			ecol[SIFT_V6_MAX_FCOLS];
	int			nevec;
	SiftVec		evin[SIFT_V6_MAX_FCOLS];
	SiftVec    *evp[SIFT_V6_MAX_FCOLS];
	void	   *keybuf;
	uint64		keyvalid[SIFT_VEC_WORDS];
	MapKeys    *mk;
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

/* The aggregate reads the count of the elements, the count of a column of them or the value of a numeric one */
static bool
agg_shape_ok(const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);

	if (a->fclause != 0 || a->nxcols != 0 || a->arg.derive > 0)
		return false;
	if (a->kind == SV6_COUNT_STAR)
		return true;
	if (a->arg.src != SV6SRC_ELEM)
		return false;
	if (a->kind == SV6_COUNT)
		return true;
	if (sift_agg_exact_float && cls == SV6C_F64 && (a->kind == SV6_SUM || a->kind == SV6_AVG))
		return false;
	return (a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX) && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64);
}

/* A key is a column of the element as it is: a string, or an integer of an array */
static bool
key_ok(const SiftV6Col *k)
{
	const SiftV6Class cls = sift_v6_class(k->typid);

	return k->src == SV6SRC_ELEM && k->derive == 0 && (cls == SV6C_TEXT || cls == SV6C_I32 || cls == SV6C_I64);
}

/* where_preds: every qual of the table is a predicate of the pull, which the zones of a granule may say every row passes */
static bool
spec_ok(const SiftV6Spec *spec, bool where_preds)
{
	const SiftV6Ext *x = spec->ext;

	if (spec->nsets != 0 || spec->nbins != 0 || spec->nderive != 0 || spec->nfilters != 0 || spec->nfuncs != 0 || spec->text_queries != NIL || spec->jsonqual != NIL ||
		spec->vqual != NIL || spec->pgexpr != NIL || spec->genref != NIL || spec->constant_groups || spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1 ||
		spec->nkeys > SIFT_PREAGG_MAX_DIMS || spec->is_table || spec->tid_range || spec->indexqual != NIL)
		return false;
	if (x == NULL || !x->unnest || x->ndims != 0 || x->nrcol != 0 || x->ntests != 0 || x->resid != NIL || (x->where != NIL && !where_preds) || x->elem.left || x->elem.semi ||
		(!elem_fn_map(x->elem.fn) && x->elem.fn != SV6E_RECORDSET && x->elem.fn != SV6E_ELEMS_TEXT && x->elem.fn != SV6E_MEMBERS))
		return false;
	for (int c = 0; c < x->elem.ncols; c++)
		if (x->elem.col_kind[c])
			return false;
	for (int i = 0; i < spec->nkeys; i++)
		if (!key_ok(&spec->key[i]))
			return false;
	return true;
}

/* The column of the element all the tests read, -1: none, -2: more than one */
static int
ewhere_slot(const SiftV6MapPreagg *m)
{
	int			slot = -1;

	for (int v = 0; v < m->nevec; v++)
	{
		if (slot >= 0 && slot != m->ecol[v])
			return -2;
		slot = m->ecol[v];
	}
	return slot;
}

SiftV6MapPreagg *
sift_v6_map_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6Vet *vet, SiftV6ScanStats *stats, bool where_preds)
{
	SiftV6MapPreagg *m;

	if (!spec_ok(spec, where_preds))
		return NULL;
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_shape_ok(&spec->agg[j]))
			return NULL;
	m = palloc0(sizeof(SiftV6MapPreagg));
	*m = (SiftV6MapPreagg) {.spec = spec,.group = g,.vet = vet,.stats = stats,.tslot = -1};
	if (spec->ext->elem_where != NIL)
	{
		if (!sift_v6_unnest_elem_clause(CurrentMemoryContext, spec->ext->elem_where, &m->ewhere, m->ecol, &m->nevec) || (m->tslot = ewhere_slot(m)) == -2)
			return NULL;
		m->has_ewhere = true;
	}
	m->keybuf = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	m->mk = palloc0(sizeof(MapKeys));
	for (int i = 0; i < spec->nkeys; i++)
		m->mk->data[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	m->pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 map preagg part");
	m->gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v6 map preagg granule");
	return m;
}

void
sift_v6_map_preagg_set_group(SiftV6MapPreagg *m, SiftV6Group *g)
{
	m->group = g;
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

/* The dimension of the cube that column slot of the element is, -1: none */
static int
slot_dim(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, int slot)
{
	for (int d = 0; d < cd->ndims; d++)
		if (cd->dim_leaf[d] == m->spec->ext->elem.col_leaf[slot])
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
		const int	d = a->kind == SV6_COUNT ? slot_dim(m, cd, a->arg.slot) : -1;
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

/* Every key and the column of the tests are dimensions of the cube, as the statement reads them: a string a dimension of codes, an integer one of integers */
static bool
bind_dims(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, MapBind *pb)
{
	const uint8 kind = elem_fn_map(m->spec->ext->elem.fn) ? SIFT_PCUBE_ELEMENTS : SIFT_PCUBE_KIND;

	if ((cd->flags & SIFT_PCUBE_KIND) != kind)
		return false;
	for (int i = 0; i < m->spec->nkeys; i++)
	{
		const bool	text = sift_v6_class(m->spec->key[i].typid) == SV6C_TEXT;

		if ((pb->kdim[i] = slot_dim(m, cd, m->spec->key[i].slot)) < 0 || cd->dim_kind[pb->kdim[i]] != (text ? SIFT_PDIM_DICT32 : SIFT_PDIM_INT64))
			return false;
	}
	pb->tdim = m->tslot >= 0 ? slot_dim(m, cd, m->tslot) : -1;
	return m->tslot < 0 || (pb->tdim >= 0 && cd->dim_kind[pb->tdim] == SIFT_PDIM_DICT32);
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
			memcpy(pb->kdim, try.kdim, sizeof(pb->kdim));
			memcpy(pb->dict, try.dict, sizeof(pb->dict));
			memcpy(pb->meas, try.meas, m->spec->naggs * sizeof(int));
			pb->tdim = try.tdim;
			pb->cube = k;
			pb->usable = true;
		}
	}
	MemoryContextSwitchTo(old);
}

/* ---- the cells of a granule ---- */

static const char *
key_bytes(const SiftKeyDict *dict, uint32 code, uint32 *len)
{
	*len = dict->d.off[code + 1] - dict->d.off[code];
	return dict->d.bytes + dict->d.off[code];
}

/* Whether each cell of the granule passes the tests of the statement on the column of the element they read: every cell when it has none */
static void
cells_pass(SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran, bool *pass)
{
	const MapBind *pb = &m->pb;
	const int	d = pb->tdim;

	for (uint32 at = 0; at < gran->ncells; at += SIFT_VEC_LEN)
	{
		const uint32 cnt = Min(SIFT_VEC_LEN, gran->ncells - at);
		uint64		keep[SIFT_VEC_WORDS];

		if (!m->has_ewhere)
		{
			memset(pass + at, true, cnt);
			continue;
		}
		memset(m->keyvalid, 0, sizeof(m->keyvalid));
		for (uint32 i = 0; i < cnt; i++)
		{
			const SiftPreaggGroupHdr *grp = sift_pag_group(cd, gran->cells, at + i);
			uint32		len = 0;
			const char *bytes = ((grp->dim_null_mask >> d) & 1) ? NULL : key_bytes(&pb->dict[d], (uint32) grp->keys[d], &len);

			((SiftStrView *) m->keybuf)[i] = bytes != NULL ? sift_vstr_make(bytes, len) : (SiftStrView) {0};
			m->keyvalid[i >> 6] |= (uint64) (bytes != NULL) << (i & 63);
		}
		for (int v = 0; v < m->nevec; v++)
		{
			m->evin[v] = (SiftVec) {.data = m->keybuf,.valid = m->keyvalid,.len = (uint16) cnt,.all_valid = false,.dense = true,.type = SVEC_STRVIEW};
			m->evp[v] = &m->evin[v];
		}
		sift_clause_eval(&m->ewhere, m->evp, cnt, keep);
		for (uint32 i = 0; i < cnt; i++)
			pass[at + i] = (keep[i >> 6] >> (i & 63)) & 1;
	}
}

static void
keys_reset(SiftV6MapPreagg *m)
{
	MapKeys    *mk = m->mk;

	for (int i = 0; i < m->spec->nkeys; i++)
	{
		const int	d = m->pb.kdim[i];
		const bool	text = sift_v6_class(m->spec->key[i].typid) == SV6C_TEXT;

		memset(mk->valid[i], 0, sizeof(mk->valid[i]));
		mk->vec[i] = (SiftVec) {.data = mk->data[i],.valid = mk->valid[i],.dense = true,.type = text ? SVEC_DICT32 : SVEC_I64,.dict = text ? &m->pb.dict[d].d : NULL};
		mk->vp[i] = &mk->vec[i];
	}
}

static void
keys_put(SiftV6MapPreagg *m, const SiftPreaggGroupHdr *grp, uint16 n)
{
	MapKeys    *mk = m->mk;

	for (int i = 0; i < m->spec->nkeys; i++)
	{
		const int	d = m->pb.kdim[i];
		const bool	null = (grp->dim_null_mask >> d) & 1;

		if (mk->vec[i].type == SVEC_DICT32)
			((uint32 *) mk->data[i])[n] = null ? 0 : (uint32) grp->keys[d];
		else
			((int64 *) mk->data[i])[n] = null ? 0 : grp->keys[d];
		mk->valid[i][n >> 6] |= (uint64) !null << (n & 63);
	}
}

static void
merge_batch(SiftV6MapPreagg *m, const V6Acc *acc, uint16 n)
{
	for (int i = 0; i < m->spec->nkeys; i++)
		m->mk->vec[i].len = n;
	sift_v6_group_merge_cells(m->group, m->spec->nkeys > 0 ? m->mk->vp : NULL, acc, n);
	keys_reset(m);
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

/* The cells that pass folded into the groups, a batch of keys at a time */
static void
fold_cells(SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran, const bool *pass)
{
	const int	naggs = m->spec->naggs;
	V6Acc	   *acc = palloc(SIFT_VEC_LEN * naggs * sizeof(V6Acc));
	uint16		n = 0;

	keys_reset(m);
	for (uint32 c = 0; c < gran->ncells; c++)
	{
		const SiftPreaggGroupHdr *grp = sift_pag_group(cd, gran->cells, c);

		if (grp->row_count == 0 || !pass[c])
			continue;
		for (int j = 0; j < naggs; j++)
			cell_acc(m, cd, grp, j, &acc[n * naggs + j]);
		keys_put(m, grp, n);
		if (++n == SIFT_VEC_LEN)
		{
			merge_batch(m, acc, n);
			n = 0;
		}
	}
	if (n > 0)
		merge_batch(m, acc, n);
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

	if (!(o->desc->ext_flags & SIFT_PART_EXT_PREAGG) || !(o->gd->flags & SIFT_GDESC_HAS_PREAGG) || o->patch != NULL || !sift_v6_group_takes_cells(m->group) ||
		(m->spec->ext->where != NIL && !o->rows_pass))
		return false;
	if (!pb->bound || pb->gen != o->desc->part_gen)
		bind_part(m, o);
	if (!pb->usable || !sift_v6_preagg_rows_known(m->vet, o) || sift_preagg_dead_rows(o, dead) != 0)
		return false;
	MemoryContextReset(m->gcx);
	old = MemoryContextSwitchTo(m->gcx);
	sift_preagg_gran_read(o->rel, pb->pp, pb->cube, o->g, &gran);
	if (gran.ncells > 0)
	{
		bool	   *pass = palloc(gran.ncells);

		cells_pass(m, &pb->pp->cube[pb->cube], &gran, pass);
		fold_cells(m, &pb->pp->cube[pb->cube], &gran, pass);
		done = true;
	}
	MemoryContextSwitchTo(old);
	m->stats->grans_preagg += done;
	return done;
}
