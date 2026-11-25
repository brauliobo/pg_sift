/*
 * col/f2/scope/probe.c - the pairs (member, scope key) of a scope for a build, held as the keys of the scope and not as its pairs. One pass over the btree of the pairs, which is led by the
 * member, counts the members and the pairs and meets the distinct keys; a member is then one probe of the btree for its keys (scope_v6/meta.h, SiftScopePairProbe), so a build holds what
 * follows the keys of the scope, whatever the number of its pairs. A scope of two paths, or one whose member is a text, varchar or uuid column (its members are codes of a map held in
 * memory), keeps its pairs in memory (sift_f2s_load_pairs).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/fmgroids.h"

#include "col/f2/part/idx.h"
#include "col/f2/scope/int.h"
#include "col/f2/scope/mem.h"
#include "col/scope/scope.h"
#include "col/scope_v6/probe.h"
#include "core/cmp.h"
#include "core/compat.h"
#include "core/memtrack.h"

typedef struct IntOps
{
	RegProcedure eq;
	RegProcedure ge;
	RegProcedure le;
	int64		min;
	int64		max;
} IntOps;

static IntOps
int_ops(Oid type)
{
	switch (type)
	{
		case INT2OID:
			return (IntOps) {F_INT2EQ, F_INT2GE, F_INT2LE, PG_INT16_MIN, PG_INT16_MAX};
		case INT4OID:
			return (IntOps) {F_INT4EQ, F_INT4GE, F_INT4LE, PG_INT32_MIN, PG_INT32_MAX};
		default:
			return (IntOps) {F_INT8EQ, F_INT8GE, F_INT8LE, PG_INT64_MIN, PG_INT64_MAX};
	}
}

static Datum
int_datum(Oid type, int64 v)
{
	return type == INT2OID ? Int16GetDatum((int16) v) : type == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

typedef struct PairSource
{
	Relation	rel;
	Relation	ix;
	IndexScanDesc scan;			/* one key: the member */
	IndexScanDesc range;		/* two keys: the least and the greatest member */
	TupleTableSlot *slot;
	AttrNumber	key;
	Oid			mtype;
	Oid			ktype;
	IntOps		ops;
	SiftKeyMap *km;
	bool		multiset;
	MemoryContext cx;
	int64	   *keys;
	uint32		capkeys;
} PairSource;

static void
keys_add(PairSource *s, uint32 n, int64 key)
{
	if (n == s->capkeys)
	{
		s->capkeys = Max(16, s->capkeys * 2);
		s->keys = s->keys == NULL ? MemoryContextAlloc(s->cx, sizeof(int64) * s->capkeys) : repalloc(s->keys, sizeof(int64) * s->capkeys);
	}
	s->keys[n] = key;
}

/* The keys ascending, one for each pair of a multiset and one for each distinct key of a set: the new count */
static uint32
keys_settle(int64 *keys, uint32 n, bool multiset, bool *repeat)
{
	uint32		u = 0;

	qsort(keys, n, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < n; i++)
	{
		if (i > 0 && keys[i] == keys[i - 1])
			*repeat |= multiset;
		if (multiset || i == 0 || keys[i] != keys[i - 1])
			keys[u++] = keys[i];
	}
	return u;
}

static uint32
source_member_keys(void *arg, int64 member, const int64 **keys)
{
	PairSource *s = arg;
	ScanKeyData skey;
	uint32		n = 0;
	bool		repeat = false;
	MemoryContext old = MemoryContextSwitchTo(s->cx);

	if (member >= s->ops.min && member <= s->ops.max)
	{
		ScanKeyEntryInitialize(&skey, 0, 1, BTEqualStrategyNumber, InvalidOid, InvalidOid, s->ops.eq, int_datum(s->mtype, member));
		index_rescan(s->scan, &skey, 1, NULL, 0);
		while (index_getnext_slot(s->scan, ForwardScanDirection, s->slot))
		{
			bool		isnull;
			const Datum key = slot_getattr(s->slot, s->key, &isnull);

			if (!isnull)
				keys_add(s, n++, sift_scope_key_code(s->ktype, s->km, key));
		}
	}
	MemoryContextSwitchTo(old);
	n = keys_settle(s->keys, n, s->multiset, &repeat);
	*keys = s->keys;
	return n;
}

static bool
source_range_hits(void *arg, int64 lo, int64 hi)
{
	PairSource *s = arg;
	ScanKeyData skey[2];
	MemoryContext old;
	bool		hit;

	if (lo > hi || hi < s->ops.min || lo > s->ops.max)
		return false;
	ScanKeyEntryInitialize(&skey[0], 0, 1, BTGreaterEqualStrategyNumber, InvalidOid, InvalidOid, s->ops.ge, int_datum(s->mtype, Max(lo, s->ops.min)));
	ScanKeyEntryInitialize(&skey[1], 0, 1, BTLessEqualStrategyNumber, InvalidOid, InvalidOid, s->ops.le, int_datum(s->mtype, Min(hi, s->ops.max)));
	old = MemoryContextSwitchTo(s->cx);
	index_rescan(s->range, skey, 2, NULL, 0);
	hit = index_getnext_slot(s->range, ForwardScanDirection, s->slot);
	MemoryContextSwitchTo(old);
	return hit;
}

static void
source_close(void *arg)
{
	PairSource *s = arg;

	ExecDropSingleTupleTableSlot(s->slot);
	index_endscan(s->scan);
	index_endscan(s->range);
	index_close(s->ix, NoLock);
	table_close(s->rel, NoLock);
	if (s->keys != NULL)
		pfree(s->keys);
	pfree(s);
}

/* What one pass over the pairs finds: the distinct keys, the totals, the members that have one key or more, a member at a time (the btree is led by the member) */
typedef struct Survey
{
	SiftU64Set	keys;
	SiftMemTracker *mem;
	uint32		check_at;
	int64		member;
	uint32		nrun;
	uint32		nmembers;
	uint32		npairs;
	SiftScopePairProbe *probe;
	PairSource *s;
} Survey;

static void
survey_run(Survey *sv)
{
	uint32		n = keys_settle(sv->s->keys, sv->nrun, sv->s->multiset, &sv->probe->repeat);

	sv->nmembers++;
	sv->npairs += n;
	sv->probe->nsingle += n == 1;
	if (n > 1 && !sv->probe->has_multi)
	{
		sv->probe->has_multi = true;
		sv->probe->first_multi = sv->member;
	}
	for (uint32 i = 0; i < n; i++)
		sift_u64_set_add(&sv->keys, (uint64) sv->s->keys[i]);
	if (sv->keys.count >= sv->check_at)
	{
		sv->check_at *= 2;
		if (sift_mem_would_exceed(sv->mem, (Size) sv->keys.count * 4 * sizeof(uint64)))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the keys of the scope do not fit in maintenance_work_mem"), errhint("Raise maintenance_work_mem.")));
	}
	sv->nrun = 0;
}

/* The rows of the btree in its order: sv NULL, the keys are only met, so that a writer's map holds every one of them */
static void
survey_pass(PairSource *s, Oid btree, AttrNumber member, Snapshot snap, Survey *sv)
{
	Relation	ix = index_open(btree, AccessShareLock);
	IndexScanDesc scan = sift_index_beginscan(s->rel, ix, snap, 0);
	TupleTableSlot *slot = table_slot_create(s->rel, NULL);

	index_rescan(scan, NULL, 0, NULL, 0);
	while (index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		bool		nm,
					nk;
		const Datum m = slot_getattr(slot, member, &nm),
					k = slot_getattr(slot, s->key, &nk);
		int64		kcode,
					mcode;

		if (nm || nk)
			continue;
		kcode = sift_scope_key_code(s->ktype, s->km, k);
		if (sv == NULL)
			continue;
		mcode = sift_scope_key_code(s->mtype, NULL, m);
		if (sv->nrun > 0 && mcode != sv->member)
			survey_run(sv);
		sv->member = mcode;
		keys_add(s, sv->nrun++, kcode);
	}
	if (sv != NULL && sv->nrun > 0)
		survey_run(sv);
	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(ix, NoLock);
}

static PairSource *
source_open(Relation rel, Oid btree, AttrNumber member, AttrNumber key, SiftKeyMap *km, bool multiset, Snapshot snap)
{
	PairSource *s = palloc0(sizeof(PairSource));

	s->rel = rel;
	s->ix = index_open(btree, AccessShareLock);
	s->key = key;
	s->mtype = TupleDescAttr(rel->rd_att, member - 1)->atttypid;
	s->ktype = TupleDescAttr(rel->rd_att, key - 1)->atttypid;
	s->ops = int_ops(s->mtype);
	s->km = km;
	s->multiset = multiset;
	s->cx = CurrentMemoryContext;
	s->scan = sift_index_beginscan(rel, s->ix, snap, 1);
	s->range = sift_index_beginscan(rel, s->ix, snap, 2);
	s->slot = table_slot_create(rel, NULL);
	return s;
}

static int64 *
set_keys(const SiftU64Set *set, uint32 *n)
{
	int64	   *keys = palloc(sizeof(int64) * Max(set->count, 1));

	*n = 0;
	for (uint32 i = 0; i <= set->mask; i++)
		if (set->keys[i] != 0)
			keys[(*n)++] = (int64) set->keys[i];
	if (set->has_zero)
		keys[(*n)++] = 0;
	qsort(keys, *n, sizeof(int64), sift_cmp_i64);
	return keys;
}

static bool
member_leads(Oid btree, AttrNumber member)
{
	Relation	ix = index_open(btree, AccessShareLock);
	bool		leads = ix->rd_index->indkey.values[0] == member;

	index_close(ix, NoLock);
	return leads;
}

SiftScopePairMap *
sift_f2s_build_pairs(Relation heap, Relation index, const SiftF2ScopeCfg *sc, Snapshot snap, SiftKeyMap *km)
{
	Relation	rel;
	AttrNumber	member,
				key;
	const Oid	btree = sift_scope_relation_index(heap, index, sc->spec, &rel, &member, &key);
	PairSource *s;
	Survey		sv = {0};
	SiftMemTracker mem;
	SiftScopePairProbe *probe;
	SiftScopePairMap *pm;
	MemoryContext old;
	int64	   *keys;
	uint32		nkeys;

	if (sc->spec->joined != NULL || sift_f2s_mem_map(sc->mem) != NULL || !sift_f2s_int_type(TupleDescAttr(rel->rd_att, member - 1)->atttypid) ||
		!sift_f2s_pair_types(TupleDescAttr(rel->rd_att, member - 1)->atttypid, TupleDescAttr(rel->rd_att, key - 1)->atttypid) ||
		!member_leads(btree, member))
	{
		table_close(rel, NoLock);
		return sift_f2s_load_pairs(heap, index, sc, snap, km, true);
	}
	s = source_open(rel, btree, member, key, km, sift_scope_pairs_multiset(index, sc->spec), snap);
	if (km != NULL)
	{
		sift_keymap_collect(km, true);
		survey_pass(s, btree, member, snap, NULL);
		sift_keymap_collect(km, false);
	}
	probe = palloc0(sizeof(SiftScopePairProbe));
	sift_mem_init(&mem, "scope pair keys", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 1, 1);
	old = MemoryContextSwitchTo(mem.root_cx);
	sift_u64_set_init(&sv.keys, 16);
	MemoryContextSwitchTo(old);
	sv.mem = &mem;
	sv.check_at = 1024;
	sv.probe = probe;
	sv.s = s;
	old = MemoryContextSwitchTo(mem.root_cx);
	survey_pass(s, btree, member, snap, &sv);
	MemoryContextSwitchTo(old);
	keys = set_keys(&sv.keys, &nkeys);
	sift_mem_end(&mem);
	probe->arg = s;
	probe->member_keys = source_member_keys;
	probe->range_hits = source_range_hits;
	probe->close = source_close;
	pm = sift_spm_probed(keys, nkeys, sv.nmembers, sv.npairs, probe);
	return pm;
}
