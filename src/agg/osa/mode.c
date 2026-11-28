/*
 * agg/osa/mode.c - mode() as a count of the (group, value) pairs (agg/osa/mode.h)
 */
/* check-memory: allow-allocs the hash grows inside a context of its own, bounded by its share of work_mem; past it the pairs move to the sort of the aggregate */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"
#include "utils/typcache.h"

#include "agg/gen/sort.h"
#include "agg/osa/mode.h"
#include "core/memtrack.h"

#define MODE_FIRST_SLOTS	1024

typedef struct ModeEntry
{
	uint32		sid;
	uint32		hash;
	int64		count;
	Datum		value;
} ModeEntry;

struct V6Mode
{
	MemoryContext cx;
	uint64		limit;			/* the bytes the context may hold */
	Oid			typid;
	int16		typlen;
	bool		typbyval;
	SortSupportData order;		/* the order of the call: the first of the values counted as often wins */
	ModeEntry  *entry;
	uint32		nentry;
	uint32		cap;			/* entries the array holds */
	uint32	   *slot;			/* the entry + 1 of each slot of the hash, 0: empty */
	uint32		mask;
};

/* a type whose values are equal exactly when their images are (float8: but for the two zeros), in the order of its default btree class */
static bool
countable(Oid typid, Oid collid, Oid sortop)
{
	TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT8OID:
			break;
		case TEXTOID:
		case VARCHAROID:
			if (!OidIsValid(collid) || !get_collation_isdeterministic(collid))
				return false;
			break;
		default:
			return false;
	}
	return sortop == tc->lt_opr || sortop == tc->gt_opr;
}

V6Mode *
v6_mode_new(const V6GenAgg *ga, MemoryContext cx, int mem_kb)
{
	const Aggref *ref = ga->ref;
	const SortGroupClause *sgc;
	Oid			typid;
	V6Mode	   *m;

	if (ref->aggfnoid != F_MODE || ga->nargs != 1 || !ga->arg[0].varies)
		return NULL;
	sgc = linitial(ref->aggorder);
	typid = ga->arg[0].typid;
	if (!countable(typid, ref->inputcollid, sgc->sortop))
		return NULL;
	m = MemoryContextAllocZero(cx, sizeof(V6Mode));
	m->cx = sift_mem_allocset_create(cx, "sift agg v6 mode");
	m->limit = (uint64) mem_kb * 1024;
	m->typid = typid;
	get_typlenbyval(typid, &m->typlen, &m->typbyval);
	m->order.ssup_cxt = cx;
	m->order.ssup_collation = ref->inputcollid;
	PrepareSortSupportFromOrderingOp(sgc->sortop, &m->order);
	m->cap = MODE_FIRST_SLOTS / 2;
	m->mask = MODE_FIRST_SLOTS - 1;
	m->entry = MemoryContextAlloc(m->cx, m->cap * sizeof(ModeEntry));
	m->slot = MemoryContextAllocZero(m->cx, MODE_FIRST_SLOTS * sizeof(uint32));
	return m;
}

void
v6_mode_free(V6Mode *m)
{
	MemoryContextDelete(m->cx);
	pfree(m);
}

/* a float8 zero of either sign, and a NaN of either sign, is the one value its equality makes it */
static Datum
canonical(const V6Mode *m, Datum v)
{
	if (m->typid != FLOAT8OID)
		return v;
	if (isnan(DatumGetFloat8(v)))
		return Float8GetDatum(get_float8_nan());
	return DatumGetFloat8(v) == 0.0 ? Float8GetDatum(0.0) : v;
}

static bool
same_value(const V6Mode *m, Datum a, Datum b)
{
	return datum_image_eq(canonical(m, a), canonical(m, b), m->typbyval, m->typlen);
}

static uint32
pair_hash(const V6Mode *m, uint32 sid, Datum v)
{
	return hash_combine(murmurhash32(sid), datum_image_hash(canonical(m, v), m->typbyval, m->typlen));
}

/* the hash at twice the entries: their slots again */
static void
grow(V6Mode *m)
{
	const uint32 nslot = (m->mask + 1) * 2;

	m->cap = nslot / 2;
	m->entry = repalloc(m->entry, m->cap * sizeof(ModeEntry));
	pfree(m->slot);
	m->slot = MemoryContextAllocZero(m->cx, nslot * sizeof(uint32));
	m->mask = nslot - 1;
	for (uint32 e = 0; e < m->nentry; e++)
	{
		uint32		at = m->entry[e].hash & m->mask;

		while (m->slot[at] != 0)
			at = (at + 1) & m->mask;
		m->slot[at] = e + 1;
	}
}

/* false when the hash has no room for one more pair */
static bool
count_pair(V6Mode *m, uint32 sid, Datum v)
{
	const uint32 h = pair_hash(m, sid, v);
	uint32		at = h & m->mask;
	MemoryContext old;

	for (; m->slot[at] != 0; at = (at + 1) & m->mask)
	{
		ModeEntry  *e = &m->entry[m->slot[at] - 1];

		if (e->hash == h && e->sid == sid && same_value(m, e->value, v))
		{
			e->count++;
			return true;
		}
	}
	if (m->nentry == m->cap)
	{
		if (MemoryContextMemAllocated(m->cx, true) * 2 > m->limit || (uint64) (m->mask + 1) * sizeof(ModeEntry) > MaxAllocSize)
			return false;
		grow(m);
		return count_pair(m, sid, v);
	}
	if (MemoryContextMemAllocated(m->cx, true) > m->limit)
		return false;
	old = MemoryContextSwitchTo(m->cx);
	m->entry[m->nentry] = (ModeEntry) {.sid = sid,.hash = h,.count = 1,.value = datumCopy(v, m->typbyval, m->typlen)};
	MemoryContextSwitchTo(old);
	m->slot[at] = ++m->nentry;
	return true;
}

/* every pair counted so far to the sort of the aggregate, as often as it was counted */
static void
move_to_sort(V6GenAgg *ga)
{
	V6Mode	   *m = ga->mode;
	const bool	isnull = false;

	elog(DEBUG1, "sift: mode() moved to a tuplesort");
	for (uint32 e = 0; e < m->nentry; e++)
		for (int64 i = 0; i < m->entry[e].count; i++)
		{
			CHECK_FOR_INTERRUPTS();
			v6_gen_sort_put(ga->sort, m->entry[e].sid, &m->entry[e].value, &isnull);
		}
	v6_mode_free(m);
	ga->mode = NULL;
}

bool
v6_mode_take(V6GenAgg *ga, uint32 sid, Datum value)
{
	if (count_pair(ga->mode, sid, value))
		return true;
	move_to_sort(ga);
	return false;
}

void
v6_mode_each(const V6Mode *m, V6PairFn fn, void *arg)
{
	for (uint32 e = 0; e < m->nentry; e++)
		fn(arg, m->entry[e].sid, m->entry[e].value, m->entry[e].count);
}

void
v6_mode_finish(V6Gen *gen, V6GenAgg *ga, V6Acc **accs, uint32 nsid)
{
	V6Mode	   *m = ga->mode;
	/* check-memory: allow one entry number for each group the scan met, which the group table holds as well */
	uint32	   *best = palloc0(Max(nsid, 1) * sizeof(uint32));
	MemoryContext old;

	for (uint32 i = 0; i < m->nentry; i++)
	{
		const ModeEntry *e = &m->entry[i];
		const ModeEntry *b = best[e->sid] != 0 ? &m->entry[best[e->sid] - 1] : NULL;

		CHECK_FOR_INTERRUPTS();
		if (b == NULL || e->count > b->count || (e->count == b->count && ApplySortComparator(e->value, false, b->value, false, &m->order) < 0))
			best[e->sid] = i + 1;
	}
	old = MemoryContextSwitchTo(gen->res);
	for (uint32 sid = 0; sid < nsid; sid++)
	{
		V6Acc	   *acc = accs[sid];

		Assert(acc != NULL && best[sid] != 0);
		acc->v0 = (uint64) datumCopy(m->entry[best[sid] - 1].value, m->typbyval, m->typlen);
		acc->n = (acc->n & V6GEN_INIT) | V6GEN_DONE;
	}
	MemoryContextSwitchTo(old);
	pfree(best);
}
