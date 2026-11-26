/*
 * col/f2/agg/map_preagg.c - SiftAggV6 answering granules of a map from the cubes of its members (col/f2/agg/map_preagg.h).
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

/* What the statement makes of the cubes of one part: the cube of the members of its map and the measure each aggregate reads */
typedef struct MapBind
{
	uint64		gen;
	bool		bound;
	bool		usable;
	const SiftPreaggPart *pp;
	int			cube;
	int		   *meas;			/* of each aggregate the measure of the cube it reads, MAP_ROWS_ONLY: the count of the members */
	SiftKeyDict dict;
} MapBind;

struct SiftV6MapPreagg
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6Vet  *vet;
	SiftV6ScanStats *stats;
	SiftClause	ewhere;
	bool		has_ewhere;
	int			ecol[SIFT_V6_MAX_FCOLS];
	int			nevec;
	SiftVec		evin[SIFT_V6_MAX_FCOLS];
	SiftVec    *evp[SIFT_V6_MAX_FCOLS];
	void	   *keybuf;
	uint64		keyvalid[SIFT_VEC_WORDS];
	MemoryContext pcx;			/* what the part in hand is bound to */
	MemoryContext gcx;			/* what a granule reads */
	MapBind		pb;
};

/* The aggregate reads the count of the members (count(*), count(key)) or a value of them */
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
		return a->arg.slot <= 1;
	if (sift_agg_exact_float && cls == SV6C_F64 && (a->kind == SV6_SUM || a->kind == SV6_AVG))
		return false;
	return a->arg.slot == 1 && (a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX) && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_F64);
}

static bool
spec_ok(const SiftV6Spec *spec)
{
	const SiftV6Ext *x = spec->ext;

	if (spec->nsets != 0 || spec->nbins != 0 || spec->nderive != 0 || spec->nfilters != 0 || spec->nfuncs != 0 || spec->text_queries != NIL || spec->jsonqual != NIL ||
		spec->vqual != NIL || spec->pgexpr != NIL || spec->genref != NIL || spec->constant_groups || spec->lock_k != 0 || spec->rows || spec->affine || spec->naggs < 1 ||
		spec->nkeys > 1 || spec->is_table)
		return false;
	if (x == NULL || !x->unnest || x->ndims != 0 || x->nrcol != 0 || x->resid != NIL || x->where != NIL || x->heap_where != NIL || x->elem.left || x->elem.semi || x->elem.fn < SV6E_EACH)
		return false;
	if (spec->nkeys == 1 && (spec->key[0].src != SV6SRC_ELEM || spec->key[0].slot != 0 || spec->key[0].derive > 0 || sift_v6_class(spec->key[0].typid) != SV6C_TEXT))
		return false;
	for (int j = 0; j < spec->naggs; j++)
		if (!agg_shape_ok(&spec->agg[j]))
			return false;
	return true;
}

SiftV6MapPreagg *
sift_v6_map_preagg_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6Vet *vet, SiftV6ScanStats *stats)
{
	SiftV6MapPreagg *m;

	if (!spec_ok(spec))
		return NULL;
	m = palloc0(sizeof(SiftV6MapPreagg));
	m->spec = spec;
	m->group = g;
	m->vet = vet;
	m->stats = stats;
	if (spec->ext->elem_where != NIL)
	{
		if (!sift_v6_unnest_elem_clause(CurrentMemoryContext, spec->ext->elem_where, &m->ewhere, m->ecol, &m->nevec))
			return NULL;
		for (int v = 0; v < m->nevec; v++)
			if (m->ecol[v] != 0)
				return NULL;
		m->has_ewhere = true;
	}
	m->keybuf = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
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

/* Where each aggregate is in the cube of the members of the map; false when one reads what the cube does not keep */
static bool
bind_aggs(const SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, int *meas)
{
	const SiftV6XElem *e = &m->spec->ext->elem;

	for (int j = 0; j < m->spec->naggs; j++)
	{
		const SiftV6Agg *a = &m->spec->agg[j];
		int			i;

		if (a->kind == SV6_COUNT_STAR || a->arg.slot == 0)
			meas[j] = MAP_ROWS_ONLY;
		else if ((i = find_meas(cd, e->col_leaf[1])) < 0 ||
				 (a->kind != SV6_COUNT && (cd->meas_kind[i] >= SIFT_PMEAS_F4) != (sift_v6_class(a->arg.typid) == SV6C_F64)))
			return false;
		else
			meas[j] = i;
	}
	return true;
}

static void
bind_part(SiftV6MapPreagg *m, const SiftF2GranOffer *o)
{
	MapBind    *pb = &m->pb;
	const SiftV6XElem *e = &m->spec->ext->elem;
	MemoryContext old;

	MemoryContextReset(m->pcx);
	old = MemoryContextSwitchTo(m->pcx);
	*pb = (MapBind) {.gen = o->desc->part_gen,.bound = true};
	pb->meas = palloc(m->spec->naggs * sizeof(int));
	pb->pp = sift_preagg_part_open(o->rel, o->part);
	for (int k = 0; pb->pp != NULL && !pb->usable && k < pb->pp->hdr.ncubes; k++)
	{
		const SiftPreaggCubeDesc *cd = &pb->pp->cube[k];

		if ((cd->flags & SIFT_PCUBE_ELEMENTS) && cd->dim_leaf[0] == e->col_leaf[0] && bind_aggs(m, cd, pb->meas))
		{
			const SiftPartDict *pd = sift_part_dict_find(sift_part_dicts(o->rel, o->part), cd->dim_leaf[0], SIFT_DICT_KIND_STR);

			pb->cube = k;
			pb->usable = pd != NULL;
			if (pd != NULL)
				pb->dict = (SiftKeyDict) {.d = {.n = pd->n,.off = pd->off,.bytes = pd->bytes,.part = true},.gen = sift_v6_dict_gen(o->rel, o->desc->part_gen)};
		}
	}
	MemoryContextSwitchTo(old);
}

/* ---- the cells of a granule ---- */

static const char *
key_bytes(const MapBind *pb, uint32 code, uint32 *len)
{
	*len = pb->dict.d.off[code + 1] - pb->dict.d.off[code];
	return pb->dict.d.bytes + pb->dict.d.off[code];
}

/* Whether each cell of the granule passes the tests of the statement on the key of the member: every cell when it has none */
static void
cells_pass(SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran, bool *pass)
{
	const MapBind *pb = &m->pb;

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
			const char *bytes = (grp->dim_null_mask & 1) ? NULL : key_bytes(pb, (uint32) grp->keys[0], &len);

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
merge_batch(SiftV6MapPreagg *m, SiftVec *key, const V6Acc *acc, uint16 n)
{
	SiftVec    *keys[1] = {key};

	key->len = n;
	key->all_valid = true;
	sift_v6_group_merge_cells(m->group, m->spec->nkeys > 0 ? keys : NULL, acc, n);
}

/* The cells that pass folded into the groups, a batch of keys at a time */
static void
fold_cells(SiftV6MapPreagg *m, const SiftPreaggCubeDesc *cd, const SiftPreaggGran *gran, const bool *pass)
{
	const MapBind *pb = &m->pb;
	const int	naggs = m->spec->naggs;
	uint32	   *codes = palloc(SIFT_VEC_LEN * sizeof(uint32) + SIFT_VEC_PAD);
	V6Acc	   *acc = palloc(SIFT_VEC_LEN * naggs * sizeof(V6Acc));
	SiftVec		key = {.data = codes,.dense = true,.type = SVEC_DICT32,.dict = &pb->dict.d};
	uint16		n = 0;

	for (uint32 c = 0; c < gran->ncells; c++)
	{
		const SiftPreaggGroupHdr *grp = sift_pag_group(cd, gran->cells, c);

		if (grp->row_count == 0 || !pass[c])
			continue;
		for (int j = 0; j < naggs; j++)
		{
			V6Acc	   *out = &acc[n * naggs + j];

			*out = (V6Acc) {0};
			if (pb->meas[j] == MAP_ROWS_ONLY)
				out->n = grp->row_count;
			else
				sift_v6_preagg_meas_acc(&m->spec->agg[j], sift_pag_measure(cd, grp, pb->meas[j]), out);
		}
		codes[n++] = (uint32) grp->keys[0];
		if (n == SIFT_VEC_LEN)
		{
			merge_batch(m, &key, acc, n);
			n = 0;
		}
	}
	if (n > 0)
		merge_batch(m, &key, acc, n);
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

	if (!(o->desc->ext_flags & SIFT_PART_EXT_PREAGG) || !(o->gd->flags & SIFT_GDESC_HAS_PREAGG) || o->patched)
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
