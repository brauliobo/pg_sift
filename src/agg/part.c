/*
 * agg/part.c - the partitions of a partitioned table as one column source (agg/part.h, docs/AGG.md "Partitioned tables").
 *
 * The wrapper opens the source of every partition (sift_colsrc_open_part) and is a provider of its own behind the seam of agg/colsrc.h. Its
 * units are those of the partitions one after the other: begin cuts the range of superblocks it is given into the ranges of the partitions it
 * overlaps (the superblocks of the partitions are numbered one after the other) and starts them one at a time, and every call about the
 * unit the source is at goes to the partition that returned it. Nothing is merged: the dictionaries of the units are their own already, and the
 * executor translates by value. The one thing the wrapper changes of what a partition says is the id of a dictionary (SiftSrcDict.id): a
 * partition numbers its dictionaries by itself, so two of them can hold the same id for different dictionaries.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/fmgroids.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/typcache.h"

#include "agg/part.h"
#include "core/cmp.h"

#define PART_DICT_ID_BASE	UINT64CONST(0x6000000000000000)	/* no id of a provider has these bits: the engine's counters and pointers are below, its flags above */

/* the id the wrapper gave the dictionary of a slot (a leaf, a histogram or a rollup dimension) last, with the partition and the id it came from */
typedef struct PartId
{
	int			child;
	const void *in;
	const void *out;
} PartId;

typedef struct PartSource
{
	int			n;
	Relation   *heap;
	Relation   *index;			/* NULL until open_rest, as src, for a partition the scan reads: the pruned ones are never opened (the first's always is: the layout) */
	SiftSrcSource **src;
	Oid		   *indexoid;
	Snapshot	snapshot;
	MemoryContext mcxt;
	bool		opened;			/* open_rest ran: the partitions can no longer be pruned */
	uint32	   *off;			/* n + 1: the first superblock of each partition, the total last */
	bool		have_off;
	bool	   *skip;			/* sift_aggpart_prune: the partitions the scan does not read (no superblocks), NULL: all are read */
	int			cur;			/* the partition of the unit the source is at */
	int			next;			/* the first partition begin has not started */
	uint32		lo,
				hi;				/* the superblocks of the begin */
	const SiftSrcPrune *prune;
	int			nprune;
	SiftSrcUnit cunit;			/* the unit as the partition returned it */
	int			nleaves;
	PartId	   *id;
	SiftSrcDict *dict;
	uint64		idseq;
} PartSource;

/* slots of dictionaries: the leaves, the dimensions of a histogram, the dimensions of a rollup */
#define SLOT_HIST(ps, leaf)		((ps)->nleaves + (leaf))
#define SLOT_ROLLUP(ps, j)		((ps)->nleaves + SIFT_COLSRC_MAX_LEAVES + (j))
#define NSLOTS(nleaves)			((nleaves) + SIFT_COLSRC_MAX_LEAVES + SIFT_SRC_MAX_RDIMS)

static inline PartSource *
ps_of(void *impl)
{
	return impl;
}

static inline SiftSrcSource *
cur_of(void *impl)
{
	PartSource *ps = impl;

	return ps->src[ps->cur];
}

static inline const SiftSrcUnit *
unit_of(void *impl)
{
	return &ps_of(impl)->cunit;
}

/* what PostgreSQL checks for a query on a partitioned table: its privileges and row security, not the partitions' */
static void
check_access(Relation parent)
{
	Oid			relid = RelationGetRelid(parent);

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(parent));
	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col indexes cannot be used on table \"%s\", which has row-level security", RelationGetRelationName(parent))));
}

static const SiftSrcProvider part_provider;

static void
open_part(PartSource *ps, int i)
{
	if (ps->src[i] != NULL)
		return;
	ps->index[i] = index_open(ps->indexoid[i], AccessShareLock);
	ps->src[i] = sift_colsrc_open_part(ps->heap[i], ps->index[i], ps->snapshot, ps->mcxt);
}

/* the sources of the partitions the scan reads: opened at the first use (the pruned ones are not opened at all) */
static void
open_rest(PartSource *ps)
{
	if (ps->opened)
		return;
	ps->opened = true;
	for (int i = 0; i < ps->n; i++)
		if (ps->skip == NULL || !ps->skip[i])
			open_part(ps, i);
}

SiftSrcSource *
sift_aggpart_open(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt)
{
	PartSource *ps = MemoryContextAllocZero(mcxt, sizeof(PartSource));
	SiftSrcSource *out = MemoryContextAlloc(mcxt, sizeof(SiftSrcSource));

	if (n <= 0 || n > SIFT_AGG_MAX_PARTS)
		elog(ERROR, "sift: %d partitions in a scan, from 1 to %d", n, SIFT_AGG_MAX_PARTS);
	check_access(parent);
	ps->n = n;
	ps->snapshot = snapshot;
	ps->mcxt = mcxt;
	ps->heap = MemoryContextAlloc(mcxt, n * sizeof(Relation));
	ps->index = MemoryContextAllocZero(mcxt, n * sizeof(Relation));
	ps->src = MemoryContextAllocZero(mcxt, n * sizeof(SiftSrcSource *));
	ps->indexoid = MemoryContextAlloc(mcxt, n * sizeof(Oid));
	memcpy(ps->indexoid, indexes, n * sizeof(Oid));
	ps->off = MemoryContextAllocZero(mcxt, (n + 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		ps->heap[i] = table_open(heaps[i], AccessShareLock);
	open_part(ps, 0);
	ps->nleaves = sift_colsrc_source_layout(ps->src[0])->nleaves;
	ps->id = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(PartId));
	ps->dict = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(SiftSrcDict));
	ps->cur = 0;
	out->prov = &part_provider;
	out->impl = ps;
	return out;
}

static void
part_close(void *impl)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
	{
		if (ps->src[i])
		{
			sift_colsrc_close(ps->src[i]);
			index_close(ps->index[i], NoLock);
		}
		table_close(ps->heap[i], NoLock);
	}
}

static const SiftSrcLayout *
part_source_layout(const void *impl)
{
	const PartSource *ps = impl;

	return sift_colsrc_source_layout(ps->src[0]);
}

/* the superblocks the partitions have now, counted once: the units are numbered by them for as long as the source is open */
static void
count_sbs(PartSource *ps)
{
	uint64		total = 0;

	if (ps->have_off)
		return;
	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
	{
		ps->off[i] = (uint32) total;
		if (ps->skip == NULL || !ps->skip[i])
			total += sift_colsrc_nsb(ps->src[i]);
	}
	if (total > PG_UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the partitions of a scan hold more superblocks than the scan can number")));
	ps->off[ps->n] = (uint32) total;
	ps->have_off = true;
}

static uint32
part_nsb(void *impl)
{
	PartSource *ps = impl;

	count_sbs(ps);
	return ps->off[ps->n];
}

void
sift_aggpart_offsets(SiftSrcSource *src, uint32 *off)
{
	PartSource *ps = src->impl;

	count_sbs(ps);
	memcpy(off, ps->off, (ps->n + 1) * sizeof(uint32));
}

void
sift_aggpart_set_offsets(SiftSrcSource *src, const uint32 *off)
{
	PartSource *ps = src->impl;

	memcpy(ps->off, off, (ps->n + 1) * sizeof(uint32));
	ps->have_off = true;
	/* the partitions without superblocks (pruned, or empty) are never read: not opened */
	ps->skip = palloc0(ps->n * sizeof(bool));
	for (int i = 0; i < ps->n; i++)
		ps->skip[i] = off[i + 1] == off[i];
}

typedef struct QualVars
{
	Oid			part;
	Oid			parent;
	bool		ok;
} QualVars;

/* a column of the partition's bound as the column of the partitioned table (the attribute numbers differ after a dropped column) */
static Node *
qual_vars_mutator(Node *n, QualVars *qv)
{
	Var		   *v;
	AttrNumber	att;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, qual_vars_mutator, qv);
	v = copyObject((Var *) n);
	att = get_attnum(qv->parent, get_attname(qv->part, v->varattno, false));
	if (att == InvalidAttrNumber)
		qv->ok = false;
	v->varattno = att;
	return (Node *) v;
}

/* `col = value` or `col = ANY (array)`: the column, the operand and the operator, whatever the operand is (the planner asks it too) */
bool
sift_aggpart_key_clause(Node *cl, Var **var, Node **operand, Oid *opno, Oid *collid, bool *any)
{
	List	   *args;
	Node	   *l,
			   *r;

	if (IsA(cl, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) cl;

		args = op->args;
		*opno = op->opno;
		*collid = op->inputcollid;
		*any = false;
	}
	else if (IsA(cl, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) cl)->useOr)
	{
		ScalarArrayOpExpr *op = (ScalarArrayOpExpr *) cl;

		args = op->args;
		*opno = op->opno;
		*collid = op->inputcollid;
		*any = true;
	}
	else
		return false;
	if (list_length(args) != 2)
		return false;
	l = linitial(args);
	r = lsecond(args);
	if (!*any && IsA(r, Var) && !IsA(l, Var))
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (!IsA(l, Var) || ((Var *) l)->varlevelsup != 0 || contain_var_clause(r))
		return false;
	*var = (Var *) l;
	*operand = r;
	return true;
}

/* the values a condition leaves a column of the partitioned table (a NULL matches nothing: n can be 0) */
typedef struct KeyVals
{
	AttrNumber	attno;
	Oid			typid;
	Oid			opno;			/* the operator of the condition and its collation: a hash partitioning takes it only if it is the equality of its operator family */
	Oid			collid;
	int			n;
	Datum	   *val;
} KeyVals;

static KeyVals *
key_vals(Node *cl)
{
	Var		   *v;
	Node	   *operand;
	Oid			opno,
				collid;
	bool		any;
	Const	   *c;
	KeyVals    *kv;

	if (!sift_aggpart_key_clause(cl, &v, &operand, &opno, &collid, &any) || !IsA(operand, Const))
		return NULL;
	c = (Const *) operand;
	kv = palloc0(sizeof(KeyVals));
	kv->attno = v->varattno;
	kv->typid = v->vartype;
	kv->opno = opno;
	kv->collid = collid;
	if (c->constisnull)
		return kv;
	if (!any)
	{
		if (c->consttype != v->vartype)
			return NULL;
		kv->val = palloc(sizeof(Datum));
		kv->val[0] = c->constvalue;
		kv->n = 1;
	}
	else
	{
		Oid			et = get_element_type(c->consttype);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		if (et != v->vartype)
			return NULL;
		get_typlenbyvalalign(et, &len, &byval, &align);
		deconstruct_array(DatumGetArrayTypeP(c->constvalue), et, len, byval, align, &elems, &nulls, &nelems);
		kv->val = palloc(Max(nelems, 1) * sizeof(Datum));
		for (int i = 0; i < nelems; i++)
			if (!nulls[i])
				kv->val[kv->n++] = elems[i];
	}
	return kv;
}

#define HASH_MAX_COMBOS		4096	/* the combinations of values a hash partition is tested with: a larger product keeps the partition */

/*
 * Can a row whose key columns take the values of the sets (KeyVals of the conditions) be in the partition of the hash level f
 * (satisfies_hash_partition(table, modulus, remainder, columns ..))? The remainder of each combination of values is computed as PostgreSQL
 * computes it for a row, with the support function of the level's partition key. A column without a set, or too many combinations: yes.
 */
static bool
hash_may_hold(FuncExpr *f, List *sets)
{
	Relation	level = relation_open(DatumGetObjectId(((Const *) linitial(f->args))->constvalue), AccessShareLock);
	PartitionKey key = RelationGetPartitionKey(level);
	int			modulus = DatumGetInt32(((Const *) lsecond(f->args))->constvalue),
				remainder = DatumGetInt32(((Const *) lthird(f->args))->constvalue);
	KeyVals    *kv[PARTITION_MAX_KEYS] = {NULL};
	int			idx[PARTITION_MAX_KEYS] = {0};
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS] = {false};
	uint64		combos = 1;
	bool		known = true,
				may;

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
			relation_close(level, NoLock);
			return false;
		}
		else
			combos *= kv[j]->n;
	}
	if (!known || combos > HASH_MAX_COMBOS)
	{
		relation_close(level, NoLock);
		return true;
	}
	may = false;
	for (;;)
	{
		int			j;

		for (j = 0; j < key->partnatts; j++)
			values[j] = kv[j]->val[idx[j]];
		if (compute_partition_hash_value(key->partnatts, key->partsupfunc, key->partcollation, values, isnull) % modulus == remainder)
		{
			may = true;
			break;
		}
		for (j = 0; j < key->partnatts && ++idx[j] == kv[j]->n; j++)
			idx[j] = 0;
		if (j == key->partnatts)
			break;
	}
	relation_close(level, NoLock);
	return may;
}

/* is the partition out of reach of the rows the sets of values describe: one of its hash levels has no remainder for them? */
static bool
hash_refuted(List *bound, List *sets)
{
	foreach_ptr(Node, q, bound)
		if (IsA(q, FuncExpr) && ((FuncExpr *) q)->funcid == F_SATISFIES_HASH_PARTITION && !hash_may_hold((FuncExpr *) q, sets))
			return true;
	return false;
}

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

/* a relation changed (a partition, an ancestor, the table): the bounds are read again, as PostgreSQL does for its own copy; dropped by the next part_bound, not under a caller that still reads one */
static void
bound_inval(Datum arg, Oid relid)
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

static const BoundEntry *
part_bound(Relation part, Oid parent)
{
	BoundKey	key = {RelationGetRelid(part), parent};
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
	fresh.key = key;
	fresh.bound = (List *) qual_vars_mutator((Node *) RelationGetPartitionQual(part), &qv);
	fresh.ok = qv.ok;
	if (bound_stale)			/* the lookups above invalidated: the bound is as of before it, the next call reads it again */
		return (const BoundEntry *) memcpy(palloc(sizeof(fresh)), &fresh, sizeof(fresh));
	if (bound_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(BoundKey), .entrysize = sizeof(BoundEntry)};

		if (bound_cx == NULL)
		{
			bound_cx = AllocSetContextCreate(CacheMemoryContext, "sift cache partition bounds", ALLOCSET_DEFAULT_SIZES);
			CacheRegisterRelcacheCallback(bound_inval, (Datum) 0);
		}
		ctl.hcxt = bound_cx;
		bound_cache = hash_create("sift partition bounds", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(bound_cache, &key, HASH_ENTER, &found);
	e->ok = fresh.ok;
	e->bound = bound_copy(fresh.bound);
	return e;
}

/* drops the partitions the conditions refute, or all of them (none); returns how many it dropped now */
static int
prune(PartSource *ps, Oid parent, List *conds, List *sets, bool none)
{
	int			dropped = 0;

	if (ps->opened)
		return 0;				/* the partitions are read already: the keys came too late */
	if (ps->skip == NULL)
		ps->skip = palloc0(ps->n * sizeof(bool));
	for (int i = 0; i < ps->n; i++)
	{
		const BoundEntry *b;

		if (ps->skip[i])
			continue;
		b = none ? NULL : part_bound(ps->heap[i], parent);
		if (none || (b->ok && b->bound != NIL && (predicate_refuted_by(b->bound, conds, false) || hash_refuted(b->bound, sets))))
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

#define KEYS_MAX_TESTS		20000	/* the keys times the partitions a list of keys is tested against exactly: more, and only the range of the keys is */

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
	qsort(sorted, n, sizeof(int64), sift_cmp_i64);
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
		if ((uint64) nk * ps->n <= KEYS_MAX_TESTS)
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

/* Starts the next partition that has superblocks in [lo, hi); false when there is none */
static bool
start_next(PartSource *ps)
{
	for (int i = ps->next; i < ps->n && ps->off[i] < ps->hi; i++)
	{
		uint32		from = Max(ps->lo, ps->off[i]),
					to = Min(ps->hi, ps->off[i + 1]);

		if (from >= to)
			continue;
		if (ps->cur >= 0)
			sift_colsrc_unpin(ps->src[ps->cur]);	/* a scan of many partitions would pin the pages of all of them */
		/* a worker has the offsets from the leader and opened the first partition only: the others when its items reach them */
		open_part(ps, i);
		ps->cur = i;
		ps->next = i + 1;
		sift_colsrc_begin(ps->src[i], from - ps->off[i], to - ps->off[i], ps->prune, ps->nprune);
		return true;
	}
	ps->next = ps->n;
	return false;
}

static void
part_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	PartSource *ps = impl;

	count_sbs(ps);
	ps->lo = sb_lo;
	ps->hi = sb_hi;
	ps->prune = prune;
	ps->nprune = nprune;
	ps->next = 0;
	while (ps->next < ps->n && ps->off[ps->next + 1] <= sb_lo)
		ps->next++;
	if (ps->cur >= 0)
		sift_colsrc_unpin(ps->src[ps->cur]);
	ps->cur = -1;
}

static bool
part_next(void *impl, SiftSrcUnit *unit)
{
	PartSource *ps = impl;

	for (;;)
	{
		if (ps->cur >= 0 && sift_colsrc_next(ps->src[ps->cur], &ps->cunit))
		{
			*unit = (SiftSrcUnit) {ps->off[ps->cur] + ps->cunit.sb, ps->cunit.nrow, ps};
			return true;
		}
		if (!start_next(ps))
			return false;
	}
}

/* the dictionary of a read as the wrapper presents it: the id is the partition's own, made unique among the partitions (a new dictionary gets a new id) */
static const SiftSrcDict *
presented(PartSource *ps, int slot, const SiftSrcDict *d)
{
	PartId	   *m = &ps->id[slot];

	if (d == NULL)
		return NULL;
	if (m->child != ps->cur + 1 || m->in != d->id)
	{
		m->child = ps->cur + 1;
		m->in = d->id;
		m->out = (const void *) (uintptr_t) (PART_DICT_ID_BASE + ++ps->idseq);
	}
	ps->dict[slot] = *d;
	ps->dict[slot].id = m->out;
	return &ps->dict[slot];
}

static void
part_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	sift_colsrc_read(cur_of(impl), unit_of(impl), leaf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	sift_colsrc_read_into(cur_of(impl), unit_of(impl), leaf, buf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	sift_colsrc_read_sel(cur_of(impl), unit_of(impl), leaf, bits, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static int64
part_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_leaf_n(cur_of(impl), unit_of(impl), leaf);
}

static void
part_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_live(cur_of(impl), unit_of(impl), bits);
}

static void
part_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_vet(cur_of(impl), unit_of(impl), bits);
}

static bool
part_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_vet_needed(cur_of(impl), unit_of(impl));
}

static void
part_counters(const void *impl, SiftSrcCounters *c)
{
	const PartSource *ps = impl;

	memset(c, 0, sizeof(*c));
	for (int i = 0; i < ps->n; i++)
	{
		SiftSrcCounters pc;

		if (ps->src[i] == NULL)
			continue;
		sift_colsrc_counters(ps->src[i], &pc);
		c->units += pc.units;
		c->units_pruned += pc.units_pruned;
		c->units_set_pruned += pc.units_set_pruned;
		c->rows += pc.rows;
		c->bytes += pc.bytes;
		c->rows_vetted += pc.rows_vetted;
		c->heap_pages += pc.heap_pages;
		c->rechecked += pc.rechecked;
		c->recheck_units += pc.recheck_units;
	}
}

static bool
part_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_overflow(cur_of(impl), unit_of(impl), leaf);
}

/* ---- the tests the partitions apply on what they store: every partition gets them ---- */

static void
part_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_prune_str(ps->src[i], t, nt);
}

static bool
part_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return sift_colsrc_filter_str(cur_of(impl), unit_of(impl), t, rows);
}

/* the partitions share one layout, so they take the same tests: the first's answer is everyone's */
static int
part_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	PartSource *ps = impl;
	int			n = 0;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			n = sift_colsrc_set_wide(ps->src[i], t, nt);
	return n;
}

static bool
part_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	return sift_colsrc_filter_wide(cur_of(impl), unit_of(impl), i, rows);
}

static uint64
part_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	PartSource *ps = impl;
	uint64		applied = 0;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			applied = sift_colsrc_set_filters(ps->src[i], f, nf);
	return applied;
}

static bool
part_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	return sift_colsrc_filter(cur_of(impl), unit_of(impl), bits);
}

static bool
part_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	return sift_colsrc_filter_types(cur_of(impl), unit_of(impl), leaf, types, bits);
}

static void
part_tolerate_ovf(void *impl, int leaf)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_tolerate_ovf(ps->src[i], leaf);
}

static void
part_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast)
{
	PartSource *ps = impl;

	open_rest(ps);
	for (int i = 0; i < ps->n; i++)
		if (ps->src[i])
			sift_colsrc_set_recheck(ps->src[i], leaf, mode, key, cast);
}

static void
part_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows)
{
	sift_colsrc_overflow_raise(cur_of(impl), unit_of(impl), rows);
}

static void
part_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	sift_colsrc_read_types(cur_of(impl), unit_of(impl), leaf, tags);
}

static void
part_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	sift_colsrc_read_jnull(cur_of(impl), unit_of(impl), leaf, bits);
}

/* ---- pre-aggregates: each unit answers from its partition ---- */

static bool
part_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_exact(cur_of(impl), unit_of(impl));
}

static bool
part_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return sift_colsrc_unit_agg(cur_of(impl), unit_of(impl), leaf, agg);
}

static bool
part_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	if (!sift_colsrc_unit_hist(cur_of(impl), unit_of(impl), dim, h))
		return false;
	h->dict = presented(impl, SLOT_HIST(ps_of(impl), dim), h->dict);
	return true;
}

static uint32
part_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	return sift_colsrc_unit_rollup_tuples(cur_of(impl), unit_of(impl), list);
}

static bool
part_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r)
{
	if (!sift_colsrc_unit_rollup(cur_of(impl), unit_of(impl), list, need, r))
		return false;
	for (int j = 0; j < r->ndims; j++)
		r->dict[j] = presented(impl, SLOT_ROLLUP(ps_of(impl), j), r->dict[j]);
	return true;
}

static bool
part_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_rollup_miss(cur_of(impl), unit_of(impl));
}

/* ---- the Vulkan path ---- */

static bool
part_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	return sift_colsrc_gpu_unit(cur_of(impl), unit_of(impl), g);
}

static bool
part_gpu_dense(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_gpu_dense(cur_of(impl), unit_of(impl), leaf);
}

static bool
part_gpu_chunk(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	return sift_colsrc_gpu_chunk(cur_of(impl), unit_of(impl), leaf, c);
}

static void
part_gpu_dict(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	sift_colsrc_gpu_dict(cur_of(impl), unit_of(impl), leaf, dict);
	*dict = *presented(impl, leaf, dict);
}

static uint64 *
part_recheck_rows(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_recheck_rows(cur_of(impl), unit_of(impl));
}

/*
 * No match set (select), no top-N (tids, zone, zone_range, read_rows): the planner never gives a partitioned table a search or a ranked scan.
 * The others are the partitions' own; a provider that lacks one of them has no partitions (sift_colsrc_can_part).
 */
static const SiftSrcProvider part_provider = {
	.close = part_close,
	.source_layout = part_source_layout,
	.nsb = part_nsb,
	.begin = part_begin,
	.next = part_next,
	.read = part_read,
	.live = part_live,
	.vet = part_vet,
	.vet_needed = part_vet_needed,
	.counters = part_counters,
	.leaf_n = part_leaf_n,
	.read_into = part_read_into,
	.overflow = part_overflow,
	.set_filters = part_set_filters,
	.filter = part_filter,
	.read_sel = part_read_sel,
	.filter_types = part_filter_types,
	.tolerate_ovf = part_tolerate_ovf,
	.set_recheck = part_set_recheck,
	.overflow_raise = part_overflow_raise,
	.recheck_rows = part_recheck_rows,
	.read_types = part_read_types,
	.read_jnull = part_read_jnull,
	.prune_str = part_prune_str,
	.filter_str = part_filter_str,
	.set_wide = part_set_wide,
	.filter_wide = part_filter_wide,
	.unit_exact = part_unit_exact,
	.unit_agg = part_unit_agg,
	.unit_hist = part_unit_hist,
	.unit_rollup_tuples = part_unit_rollup_tuples,
	.unit_rollup = part_unit_rollup,
	.unit_rollup_miss = part_unit_rollup_miss,
	.gpu_unit = part_gpu_unit,
	.gpu_dense = part_gpu_dense,
	.gpu_chunk = part_gpu_chunk,
	.gpu_dict = part_gpu_dict,
};

/* ---- planner side ---- */

bool
sift_aggpart_same_layout(const SiftSrcLayout *a, const SiftSrcLayout *b)
{
	if (a->is_table != b->is_table || a->natts != b->natts || a->nleaves != b->nleaves || !equal(a->attexprs, b->attexprs))
		return false;
	for (int i = 0; i < a->nleaves; i++)
	{
		const SiftSrcLeaf *x = &a->leaf[i],
				   *y = &b->leaf[i];

		if (x->id != y->id || x->attno != y->attno || x->parent != y->parent || x->kind != y->kind || x->vtype != y->vtype || x->typid != y->typid ||
			x->wtypid != y->wtypid || x->wide != y->wide || x->scale != y->scale || x->lossy != y->lossy || x->astext != y->astext ||
			!equal(x->path, y->path) || strcmp(x->name, y->name) != 0)
			return false;
	}
	return true;
}

/* the number of a leaf's values is a lower bound over several sources (the distinct values of a leaf may repeat among them), its range the union */
void
sift_aggpart_add_stats(SiftSrcStats *tot, const SiftSrcStats *s, int nleaves)
{
	double		w = tot->nrows + s->nrows;

	if (w > 0)
	{
		tot->allvisible_frac = (tot->allvisible_frac * tot->nrows + s->allvisible_frac * s->nrows) / w;
		tot->exact_frac = (tot->exact_frac * tot->nrows + s->exact_frac * s->nrows) / w;
		tot->rows_miss = (tot->rows_miss * tot->nrows + s->rows_miss * s->nrows) / w;
	}
	for (int i = 0; i < Min(nleaves, SIFT_COLSRC_MAX_LEAVES); i++)
	{
		if (w > 0)
		{
			tot->elems_per_row[i] = (tot->elems_per_row[i] * tot->nrows + s->elems_per_row[i] * s->nrows) / w;
			tot->dim_miss[i] = (tot->dim_miss[i] * tot->nrows + s->dim_miss[i] * s->nrows) / w;
			tot->mm_miss[i] = (tot->mm_miss[i] * tot->nrows + s->mm_miss[i] * s->nrows) / w;
			tot->roll_miss[i] = (tot->roll_miss[i] * tot->nrows + s->roll_miss[i] * s->nrows) / w;
		}
		if (tot->min[i] == 0 && tot->max[i] == 0)
		{
			tot->min[i] = s->min[i];
			tot->max[i] = s->max[i];
		}
		else if (s->min[i] != 0 || s->max[i] != 0)
		{
			tot->min[i] = Min(tot->min[i], s->min[i]);
			tot->max[i] = Max(tot->max[i], s->max[i]);
		}
		tot->ndistinct[i] = Max(tot->ndistinct[i], s->ndistinct[i]);
		tot->ovf[i] |= s->ovf[i];
		tot->nokinds[i] |= s->nokinds[i];
		tot->jnull[i] |= s->jnull[i];
	}
	tot->nrows += s->nrows;
	tot->nunits += s->nunits;
	tot->bytes += s->bytes;
}
