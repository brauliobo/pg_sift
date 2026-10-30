/*
 * agg/part/prune.c - the partitions a query can skip
 * The bounds of the partitions in a cache, the range and the hash tests of a key clause
 * against them, and the pruning of the partitions by key or by a list of keys.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "core/cmp.h"
#include "core/limits.h"
#include "agg/part/int.h"

#define HASH_MAX_COMBOS		1048576	/* the combinations of values the remainders of one hash level are computed for: a larger product keeps the level's partitions */

/* the remainders a hash level (its parent and modulus) gives the combinations of the values the conditions leave its key columns: NULL rem, every one */
typedef struct HashLevel
{
	Oid			parent;
	int			modulus;
	int			nrem;
	uint64	   *rem;			/* sorted, distinct */
} HashLevel;

typedef struct BoundKey
{
	Oid			part;
	Oid			parent;
} BoundKey;

typedef struct BoundEntry
{
	BoundKey	key;
	bool		ok;
	List	   *bound;
} BoundEntry;

static MemoryContext bound_cx;
static HTAB *bound_cache;
static bool bound_stale;

/* the range of values a column can take under the conditions that compare it with constants */
typedef struct KeyRange
{
	bool		have_lo,
				have_hi;
	Datum		lo,
				hi;
} KeyRange;

#define KEYS_MAX_TESTS		20000	/* the keys times the partitions a list of keys is tested against exactly: more, and only the range of the keys is */

/*
 * The remainder of each combination of values the sets (KeyVals of the conditions) leave the key columns of the hash level f
 * (satisfies_hash_partition(table, modulus, remainder, columns ..)), computed as PostgreSQL computes it for a row, with the support function of
 * the level's partition key: once for the level, so that testing a partition is a search (O(values + partitions), not O(values x partitions)).
 * A column without a set, or too many combinations: every remainder is possible (rem NULL).
 */
static const HashLevel *
hash_level(FuncExpr *f, List *sets, List **levels)
{
	Oid			parent = DatumGetObjectId(((Const *) linitial(f->args))->constvalue);
	int			modulus = DatumGetInt32(((Const *) lsecond(f->args))->constvalue);
	Relation	level;
	PartitionKey key;
	KeyVals    *kv[PARTITION_MAX_KEYS] = {NULL};
	int			idx[PARTITION_MAX_KEYS] = {0};
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS] = {false};
	uint64		combos = 1;
	bool		known = true;
	HashLevel  *hl;
	uint64	   *rem;
	int			nrem = 0;

	foreach_ptr(HashLevel, h, *levels)
		if (h->parent == parent && h->modulus == modulus)
			return h;
	hl = palloc0(sizeof(HashLevel));
	hl->parent = parent;
	hl->modulus = modulus;
	*levels = lappend(*levels, hl);
	level = relation_open(parent, AccessShareLock);
	key = RelationGetPartitionKey(level);
	for (int j = 0; j < key->partnatts; j++)
	{
		Node	   *a = list_nth(f->args, 3 + j);

		if (IsA(a, Var))
			foreach_ptr(KeyVals, s, sets)
				if (s->attno == ((Var *) a)->varattno && s->typid == key->parttypid[j] && s->collid == key->partcollation[j] &&
					get_op_opfamily_strategy(s->opno, key->partopfamily[j]) == HTEqualStrategyNumber && (kv[j] == NULL || s->n < kv[j]->n))
					kv[j] = s;
		if (kv[j] == NULL)
			known = false;
		else if (kv[j]->n == 0)
		{
			relation_close(level, NoLock);	/* a NULL or an empty list matches nothing: no remainder */
			hl->rem = palloc(sizeof(uint64));
			return hl;
		}
		else if (known)
		{
			combos *= kv[j]->n;
			known = combos <= (uint64) sift_limit(hash_part_combos, HASH_MAX_COMBOS);
		}
	}
	if (!known)
	{
		relation_close(level, NoLock);
		return hl;
	}
	rem = palloc(combos * sizeof(uint64));
	for (;;)
	{
		int			j;

		for (j = 0; j < key->partnatts; j++)
			values[j] = kv[j]->val[idx[j]];
		rem[nrem++] = compute_partition_hash_value(key->partnatts, key->partsupfunc, key->partcollation, values, isnull) % modulus;
		for (j = 0; j < key->partnatts && ++idx[j] == kv[j]->n; j++)
			idx[j] = 0;
		if (j == key->partnatts)
			break;
	}
	relation_close(level, NoLock);
	sift_sort_u64(rem, nrem);
	{
		int			k = 0;

		for (int i = 0; i < nrem; i++)
			if (i == 0 || rem[i] != rem[i - 1])
				rem[k++] = rem[i];
		nrem = k;
	}
	hl->rem = rem;
	hl->nrem = nrem;
	return hl;
}

/* is the partition out of reach of the rows the sets of values describe: one of its hash levels has no remainder for them? */
static bool
hash_refuted(List *bound, List *sets, List **levels)
{
	foreach_ptr(Node, q, bound)
		if (IsA(q, FuncExpr) && ((FuncExpr *) q)->funcid == F_SATISFIES_HASH_PARTITION)
		{
			FuncExpr   *f = (FuncExpr *) q;
			const HashLevel *hl = hash_level(f, sets, levels);
			uint64		r = DatumGetInt32(((Const *) lthird(f->args))->constvalue);

			if (hl->rem != NULL && bsearch(&r, hl->rem, hl->nrem, sizeof(uint64), sift_cmp_u64) == NULL)
				return true;
		}
	return false;
}

/*
 * A row of pg_class changed (a partition attached or detached, a column added, a table rewritten): the bounds are read again, as PostgreSQL does for its own
 * copy; dropped by the next part_bound, not under a caller that still reads one. A syscache callback, not a relcache one: a backend has ten relcache
 * callback slots for all its libraries, and the callbacks of the syscaches have sixty-four.
 */
static void
bound_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	bound_stale = true;
}

/*
 * The bound of a partition in the terms of the columns of the table, kept for the backend: every execution of a generic plan prunes the
 * partitions again, and translating the bounds of thousands of them was most of its start. A relation cache invalidation drops them all.
 * The entry is valid until the next call.
 */
static List *
bound_copy(List *l)
{
	MemoryContext old = MemoryContextSwitchTo(bound_cx);
	List	   *c = copyObject(l);

	MemoryContextSwitchTo(old);
	return c;
}

/* the heap of partition i is opened for its bound the first time only: the bound of a partition the backend has read is in the cache, and needs no relation */
static const BoundEntry *
part_bound(PartSource *ps, int i, Oid parent)
{
	BoundKey	key = {ps->heapoid[i], parent};
	BoundEntry *e;
	BoundEntry	fresh;
	QualVars	qv = {key.part, parent, true};
	bool		found;

	if (bound_stale && bound_cache != NULL)
		MemoryContextReset(bound_cx);
	if (bound_stale)
	{
		bound_cache = NULL;
		bound_stale = false;
	}
	if (bound_cache != NULL && (e = hash_search(bound_cache, &key, HASH_FIND, NULL)) != NULL)
		return e;
	if (ps->heap[i] == NULL)
		ps->heap[i] = table_open(key.part, AccessShareLock);
	fresh.key = key;
	fresh.bound = (List *) qual_vars_mutator((Node *) RelationGetPartitionQual(ps->heap[i]), &qv);
	fresh.ok = qv.ok;
	if (bound_stale)			/* the lookups above invalidated: the bound is as of before it, the next call reads it again */
		return (const BoundEntry *) memcpy(palloc(sizeof(fresh)), &fresh, sizeof(fresh));
	if (bound_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(BoundKey), .entrysize = sizeof(BoundEntry)};

		if (bound_cx == NULL)
		{
			bound_cx = AllocSetContextCreate(CacheMemoryContext, "sift cache partition bounds", ALLOCSET_DEFAULT_SIZES);
			CacheRegisterSyscacheCallback(RELOID, bound_inval, (Datum) 0);
		}
		ctl.hcxt = bound_cx;
		bound_cache = hash_create("sift partition bounds", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(bound_cache, &key, HASH_ENTER, &found);
	e->ok = fresh.ok;
	e->bound = bound_copy(fresh.bound);
	return e;
}

static int
key_cmp(PartitionKey key, Datum a, Datum b)
{
	return DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0], key->partcollation[0], a, b));
}

/* the bound of one side: the smaller of the upper bounds, the larger of the lower ones */
static void
range_narrow(PartitionKey key, KeyRange *r, bool upper, Datum d)
{
	bool	   *have = upper ? &r->have_hi : &r->have_lo;
	Datum	   *cur = upper ? &r->hi : &r->lo;

	if (!*have || (key_cmp(key, d, *cur) < 0) == upper)
		*cur = d;
	*have = true;
}

/* what the conditions `key op const` and `key = ANY (const array)`, with the operators of the key's operator family, leave the single column of a range key */
static KeyRange
key_range(PartitionKey key, List *conds)
{
	KeyRange	r = {0};

	foreach_ptr(Node, cl, conds)
	{
		List	   *args;
		Oid			opno,
					collid;
		bool		any = IsA(cl, ScalarArrayOpExpr);
		Node	   *l,
				   *x;
		Const	   *c;
		int			strat;

		if (IsA(cl, OpExpr))
		{
			args = ((OpExpr *) cl)->args;
			opno = ((OpExpr *) cl)->opno;
			collid = ((OpExpr *) cl)->inputcollid;
		}
		else if (any && ((ScalarArrayOpExpr *) cl)->useOr)
		{
			args = ((ScalarArrayOpExpr *) cl)->args;
			opno = ((ScalarArrayOpExpr *) cl)->opno;
			collid = ((ScalarArrayOpExpr *) cl)->inputcollid;
		}
		else
			continue;
		if (list_length(args) != 2 || collid != key->partcollation[0])
			continue;
		l = linitial(args);
		x = lsecond(args);
		if (!IsA(l, Var) || ((Var *) l)->varlevelsup != 0 || ((Var *) l)->varattno != key->partattrs[0] || !IsA(x, Const) || ((Const *) x)->constisnull)
			continue;
		c = (Const *) x;
		strat = get_op_opfamily_strategy(opno, key->partopfamily[0]);
		if (any)
		{
			int16		len;
			bool		byval;
			char		align;
			Datum	   *elems;
			bool	   *nulls;
			int			nelems;
			Datum		lowest = 0,
						highest = 0;
			bool		some = false;

			if (strat != BTEqualStrategyNumber || get_element_type(c->consttype) != key->parttypid[0])
				continue;
			get_typlenbyvalalign(key->parttypid[0], &len, &byval, &align);
			deconstruct_array(DatumGetArrayTypeP(c->constvalue), key->parttypid[0], len, byval, align, &elems, &nulls, &nelems);
			for (int i = 0; i < nelems; i++)
				if (!nulls[i])
				{
					if (!some || key_cmp(key, elems[i], lowest) < 0)
						lowest = elems[i];
					if (!some || key_cmp(key, elems[i], highest) > 0)
						highest = elems[i];
					some = true;
				}
			if (some)
			{
				range_narrow(key, &r, false, lowest);
				range_narrow(key, &r, true, highest);
			}
			continue;
		}
		if (c->consttype != key->parttypid[0] || ((Var *) l)->vartype != key->parttypid[0])
			continue;
		if (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber || strat == BTEqualStrategyNumber)
			range_narrow(key, &r, true, c->constvalue);
		if (strat == BTGreaterStrategyNumber || strat == BTGreaterEqualStrategyNumber || strat == BTEqualStrategyNumber)
			range_narrow(key, &r, false, c->constvalue);
	}
	return r;
}

/*
 * The partitions of a table split by range on one column that its rows can be in when the conditions bound that column, from the bounds
 * PostgreSQL keeps (two bisections): the partitions between the bounds' places, and the default partition, which holds what no other does.
 * False when the bounds do not decide (another strategy or several columns, no condition on the column).
 */
static bool
range_window(Relation parent, List *conds, PartitionDesc *pd, Bitmapset **window)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	PartitionBoundInfo bi;
	KeyRange	r;
	int			first = 0;
	int			last;
	bool		eq;

	if (key->strategy != PARTITION_STRATEGY_RANGE || key->partnatts != 1 || key->partattrs[0] == 0)
		return false;
	*pd = RelationGetPartitionDesc(parent, false);
	bi = (*pd)->boundinfo;
	r = key_range(key, conds);
	if (bi == NULL || (!r.have_lo && !r.have_hi))
		return false;
	last = bi->ndatums;
	if (r.have_lo)
		first = partition_range_datum_bsearch(key->partsupfunc, key->partcollation, bi, 1, &r.lo, &eq) + 1;
	if (r.have_hi)
		last = partition_range_datum_bsearch(key->partsupfunc, key->partcollation, bi, 1, &r.hi, &eq) + 1;
	*window = NULL;
	for (int k = first; k <= last; k++)
		if (bi->indexes[k] >= 0)
			*window = bms_add_member(*window, bi->indexes[k]);
	if (bi->default_index >= 0)
		*window = bms_add_member(*window, bi->default_index);
	return true;
}

/*
 * Is partition i, the next of the table's own partitions in the order of their bounds (pd->oids; the partitions of a partitioned partition stand
 * for it and are not found), outside the window? *next is where the next one is looked for.
 */
static bool
outside_window(PartSource *ps, int i, PartitionDesc pd, const Bitmapset *window, int *next)
{
	int			j = *next;

	while (j < pd->nparts && pd->oids[j] != ps->heapoid[i] && !pd->is_leaf[j])
		j++;
	if (j >= pd->nparts || pd->oids[j] != ps->heapoid[i])
		return false;
	*next = j + 1;
	return !bms_is_member(j, window);
}

/* drops the partitions the conditions refute, or all of them (none); returns how many it dropped now */
int
prune(PartSource *ps, Oid parent, List *conds, List *sets, bool none)
{
	int			dropped = 0;
	List	   *levels = NIL;
	int			next = 0;
	PartitionDesc pd = NULL;
	Bitmapset  *window = NULL;
	bool		bounded;

	if (ps->opened)
		return 0;				/* the partitions are read already: the keys came too late */
	if (ps->skip == NULL)
		ps->skip = palloc0(ps->n * sizeof(bool));
	bounded = !none && range_window(ps->parent, conds, &pd, &window);
	for (int i = 0; i < ps->n; i++)
	{
		const BoundEntry *b;

		if (ps->skip[i])
			continue;
		if (bounded && outside_window(ps, i, pd, window, &next))
		{
			ps->skip[i] = true;
			dropped++;
			continue;
		}
		b = none ? NULL : part_bound(ps, i, parent);
		if (none || (b->ok && b->bound != NIL && (predicate_refuted_by(b->bound, conds, false) || hash_refuted(b->bound, sets, &levels))))
		{
			ps->skip[i] = true;
			dropped++;
		}
	}
	return dropped;
}

/*
 * The partitions whose bounds the conditions refute (they hold the partitioned table's columns, with varno 1 and constants for the operands the
 * query evaluated when it started) are not read: they have no superblocks. Before the first sift_colsrc_nsb. PostgreSQL's own test of a
 * partition against the conditions, so a partition it cannot refute (a default partition) stays; the remainder of a hash partition is tested
 * with the values an equality or `= ANY` leaves the key columns. Returns how many are dropped.
 */
int
sift_aggpart_prune(SiftSrcSource *src, Oid parent, List *conds)
{
	List	   *sets = NIL;

	if (conds == NIL)
		return 0;
	foreach_ptr(Node, cl, conds)
	{
		KeyVals    *kv = key_vals(cl);

		if (kv)
			sets = lappend(sets, kv);
	}
	return prune(src->impl, parent, conds, sets, false);
}

static Datum
key_datum(int16 len, int64 v)
{
	return len == 8 ? Int64GetDatum(v) : len == 4 ? Int32GetDatum((int32) v) : Int16GetDatum((int16) v);
}

/*
 * The partitions that none of the keys can be in (the keys a join needs of the rows, a column of the partitioned table: col is its Var as the
 * first partition has it) are not read. A range or list bound is refuted by the range of the keys and, when they are few enough, by each of
 * them; a hash remainder by the remainders of the keys; no key at all refutes every partition. Returns how many are dropped.
 */
int
sift_aggpart_prune_keys(SiftSrcSource *src, Oid parent, const Node *col, const int64 *vals, int n)
{
	PartSource *ps = src->impl;
	Var		   *v;
	AttrNumber	att;
	TypeCacheEntry *tc;
	int16		len;
	bool		byval;
	char		align;
	Datum	   *keys;
	int			nk = 0;
	KeyVals		kv;
	List	   *conds = NIL;
	int64	   *sorted;

	if (!IsA(col, Var))
		return 0;
	v = (Var *) col;
	get_typlenbyvalalign(v->vartype, &len, &byval, &align);
	att = get_attnum(parent, get_attname(RelationGetRelid(ps->heap[0]), v->varattno, false));
	tc = lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR | TYPECACHE_BTREE_OPFAMILY);
	if (!byval || (len != 2 && len != 4 && len != 8) || att == InvalidAttrNumber || !OidIsValid(tc->eq_opr) || !OidIsValid(tc->btree_opf))
		return 0;
	if (n == 0)
		return prune(ps, parent, NIL, NIL, true);
	sorted = palloc(n * sizeof(int64));
	memcpy(sorted, vals, n * sizeof(int64));
	sift_sort_i64(sorted, n);
	keys = palloc(n * sizeof(Datum));
	for (int i = 0; i < n; i++)
		if (i == 0 || sorted[i] != sorted[i - 1])
			keys[nk++] = key_datum(len, sorted[i]);
	kv = (KeyVals) {att, v->vartype, tc->eq_opr, InvalidOid, nk, keys};
	{
		Var		   *col1 = makeVar(1, att, v->vartype, v->vartypmod, InvalidOid, 0);
		Oid			ge = get_opfamily_member(tc->btree_opf, v->vartype, v->vartype, BTGreaterEqualStrategyNumber),
					le = get_opfamily_member(tc->btree_opf, v->vartype, v->vartype, BTLessEqualStrategyNumber);

		if (OidIsValid(ge) && OidIsValid(le))
		{
			conds = list_make2(make_opclause(ge, BOOLOID, false, (Expr *) col1, (Expr *) makeConst(v->vartype, -1, InvalidOid, len, keys[0], false, true), InvalidOid, InvalidOid),
							   make_opclause(le, BOOLOID, false, (Expr *) col1, (Expr *) makeConst(v->vartype, -1, InvalidOid, len, keys[nk - 1], false, true), InvalidOid, InvalidOid));
			foreach_ptr(OpExpr, op, conds)
				set_opfuncid(op);
		}
		if ((uint64) nk * ps->n <= (uint64) sift_limit(part_key_tests, KEYS_MAX_TESTS))
		{
			ScalarArrayOpExpr *any = makeNode(ScalarArrayOpExpr);

			any->opno = tc->eq_opr;
			any->opfuncid = get_opcode(tc->eq_opr);
			any->useOr = true;
			any->inputcollid = InvalidOid;
			any->args = list_make2(col1, makeConst(get_array_type(v->vartype), -1, InvalidOid, -1,
												   PointerGetDatum(construct_array(keys, nk, v->vartype, len, byval, align)), false, false));
			any->location = -1;
			conds = lappend(conds, any);
		}
	}
	return prune(ps, parent, conds, list_make1(&kv), false);
}
