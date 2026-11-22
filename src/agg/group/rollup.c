/*
 * agg/group/rollup.c - the grouping sets of agg/group/rollup.h. A set is a node: a table of groups keyed by the int64 value of each key column of the set
 * (the columns outside the set are 0 and not NULL, the NULLs of the set's own columns are in the null flags of the key), each group holding in the state
 * block of its slot the count(*) and, for each value column, the count, the 128-bit sum, the minimum and the maximum. The nodes are sorted by the size of
 * their set, largest first, so that the set a node is built from is always before it.
 */
#include "postgres.h"

#include <stdalign.h>

#include "agg/group/dist.h"
#include "agg/group/rollup.h"
#include "agg/group/spill.h"
#include "agg/group/state.h"
#include "core/popcount.h"

#define VAL_BYTES	((uint32) sizeof(SiftRollupVal))
#define HDR_BYTES	16
#define DIST_OFF	8

typedef struct RollNode
{
	uint64		mask;
	int			parent;			/* the node it is built from, -1 for a node fed from the rows */
	bool		grand;			/* mask == 0: one accumulator, no table */
	SiftSaltedHT ht;
	uint8	   *grand_acc;
	uint64		dist_bytes;		/* the cells and headers of every SiftDistSet of the node */
	SiftGroupSpill *spill;		/* the groups that left the table, once it outgrew its share of the memory: ht then gathers the rows that follow */
} RollNode;

struct SiftRollupTree
{
	SiftRollupSpec spec;
	MemoryContext cxt;
	uint64		max_bytes;
	uint16		n_nodes;
	uint16		n_leaves;
	bool		post;
	uint32		stride;
	RollNode   *nodes;
	uint16		node_of_set[SIFT_ROLLUP_MAX_SETS];
	int64	   *kcol;			/* the taken rows of the batch: n_keys then n_vals columns of SIFT_VEC_LEN int64, and their validity */
	uint64	   *kvalid;
	uint64	   *khash;
	uint16	   *sel;
	uint16		cn;
	SiftRadixShape shape;		/* the records of the groups of a node that spilled */
	uint32		spill_cap;		/* the slots of the table of a node that spilled */
	uint32		folds;
};

#define KCOL(t, c)	((t)->kcol + (size_t) (c) * SIFT_VEC_LEN)
#define KVAL(t, c)	((t)->kvalid + (size_t) (c) * SIFT_VEC_WORDS)
#define KHASH(t, k)	((t)->khash + (size_t) (k) * SIFT_VEC_LEN)
#define BIT(m, i)	(((m)[(i) >> 6] >> ((i) & 63)) & 1)

static inline int64 *
acc_rows(uint8 *blk)
{
	return (int64 *) blk;
}

static inline SiftRollupVal *
acc_val(uint8 *blk, int v)
{
	return (SiftRollupVal *) (blk + HDR_BYTES + (size_t) v * VAL_BYTES);
}

static void
acc_add(SiftRollupTree *t, RollNode *nd, uint8 *blk, uint16 i)
{
	(*acc_rows(blk))++;
	for (int v = 0; v < t->spec.n_vals; v++)
	{
		SiftRollupVal *a = acc_val(blk, v);
		const int64 x = KCOL(t, t->spec.n_keys + v)[i];

		if (!BIT(KVAL(t, t->spec.n_keys + v), i))
			continue;
		if (a->nvals == 0)
			a->min = a->max = x;
		else
		{
			a->min = Min(a->min, x);
			a->max = Max(a->max, x);
		}
		a->nvals++;
		sift_i128_add(&a->sum, sift_i128_of_i64(x));
		if (v == 0 && t->spec.count_distinct)
		{
			SiftDistSet **set = (SiftDistSet **) (blk + DIST_OFF);
			const uint64 before = *set ? sift_dist_bytes(*set) : 0;

			if (!*set)
			{
				MemoryContext old = MemoryContextSwitchTo(t->cxt);

				*set = sift_dist_new();
				MemoryContextSwitchTo(old);
			}
			sift_dist_add(*set, (uint64) x);
			nd->dist_bytes += sift_dist_bytes(*set) - before;
		}
	}
}

static void
acc_merge(const SiftRollupTree *t, uint8 *dst, const uint8 *src)
{
	*acc_rows(dst) += *(const int64 *) src;
	for (int v = 0; v < t->spec.n_vals; v++)
	{
		SiftRollupVal *d = acc_val(dst, v);
		const SiftRollupVal *s = (const SiftRollupVal *) (src + HDR_BYTES + (size_t) v * VAL_BYTES);

		if (s->nvals == 0)
			continue;
		if (d->nvals == 0)
		{
			d->min = s->min;
			d->max = s->max;
		}
		else
		{
			d->min = Min(d->min, s->min);
			d->max = Max(d->max, s->max);
		}
		d->nvals += s->nvals;
		sift_i128_add(&d->sum, s->sum);
	}
}

static int
nbits(uint64 m)
{
	return (int) sift_pc64(m);
}

static void
shape_merge(const SiftRadixShape *sh, SiftAggSlot64 *dst, uint8 *dst_ext, const SiftAggSlot64 *src, const uint8 *src_ext)
{
	acc_merge(sh->ctx, dst_ext, src_ext);
}

/* The slots of the table that gathers the groups of a node that spilled: all the nodes may spill, each within a quarter of the budget shared by them */
static uint32
spill_cap_for(uint64 max_bytes, uint16 n_nodes, uint32 stride, uint8 n_keys)
{
	const uint64 slot_bytes = sizeof(SiftAggSlot64) + 1 + stride + n_keys * 8;
	const uint64 fit = max_bytes / (4 * Max(n_nodes, 1) * slot_bytes);

	return (uint32) Max(SIFT_HT_MIN_SLOTS, Min(SIFT_SPILL_LOCAL_CAP, fit ? pg_prevpower2_64(fit) : 0));
}

SiftRollupTree *
sift_rollup_create(MemoryContext cxt, const SiftRollupSpec *spec, uint64 max_bytes)
{
	SiftRollupTree *t = MemoryContextAllocAligned(cxt, sizeof(SiftRollupTree), alignof(SiftI128), MCXT_ALLOC_ZERO);
	MemoryContext old;
	const int	ncols = spec->n_keys + spec->n_vals;
	uint64		masks[SIFT_ROLLUP_MAX_SETS];
	uint16		n = 0;

	Assert(spec->n_keys <= SIFT_ROLLUP_MAX_KEYS && spec->n_vals <= SIFT_ROLLUP_MAX_VALS && spec->n_sets <= SIFT_ROLLUP_MAX_SETS);
	t->spec = *spec;
	t->cxt = cxt;
	t->max_bytes = max_bytes;
	t->stride = HDR_BYTES + spec->n_vals * VAL_BYTES;
	t->post = !spec->count_distinct && !spec->force_prehash;
	t->shape = (SiftRadixShape) {.wide_key = true,.ext_stride = t->stride,.ext_dist_off = DIST_OFF,.ctx = t,.merge = shape_merge};
	for (int s = 0; s < spec->n_sets; s++)
	{
		int			j = 0;

		while (j < n && masks[j] != spec->set_masks[s])
			j++;
		if (j == n)
		{
			SIFT_INVARIANT(n < lengthof(masks), "sift: rollup has more distinct grouping sets than SIFT_ROLLUP_MAX_SETS");
			masks[n++] = spec->set_masks[s];
		}
	}
	/* the largest sets first, the others in the order given */
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && nbits(masks[b]) > nbits(masks[b - 1]); b--)
		{
			const uint64 x = masks[b];

			masks[b] = masks[b - 1];
			masks[b - 1] = x;
		}
	t->n_nodes = n;
	t->spill_cap = spill_cap_for(max_bytes, n, t->stride, spec->n_keys);
	t->nodes = MemoryContextAllocAligned(cxt, (Size) n * sizeof(RollNode), alignof(SiftI128), MCXT_ALLOC_ZERO);
	old = MemoryContextSwitchTo(cxt);
	for (int u = 0; u < n; u++)
	{
		RollNode   *nd = &t->nodes[u];
		int			best = -1;

		nd->mask = masks[u];
		nd->grand = nd->mask == 0;
		for (int p = 0; p < u; p++)
			if ((masks[p] & masks[u]) == masks[u] && masks[p] != masks[u] && (best < 0 || nbits(masks[p]) < nbits(masks[best])))
				best = p;
		nd->parent = t->post ? best : -1;
		t->n_leaves += nd->parent < 0;
		if (nd->grand)
			nd->grand_acc = MemoryContextAllocAligned(cxt, t->stride, 64, MCXT_ALLOC_ZERO);
		else
			sift_ht_init_ex(&nd->ht, SIFT_HT_MIN_SLOTS, true, t->stride, DIST_OFF);
	}
	MemoryContextSwitchTo(old);
	for (int s = 0; s < spec->n_sets; s++)
		for (int u = 0; u < n; u++)
			if (masks[u] == spec->set_masks[s])
				t->node_of_set[s] = (uint16) u;
	t->kcol = MemoryContextAllocAligned(cxt, (Size) ncols * SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	t->kvalid = MemoryContextAllocAligned(cxt, (Size) ncols * SIFT_VEC_WORDS * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	t->khash = MemoryContextAllocAligned(cxt, (Size) spec->n_keys * SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	t->sel = MemoryContextAllocAligned(cxt, (SIFT_VEC_LEN + SIFT_SEL_SLACK) * 2, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	return t;
}

void
sift_rollup_free(SiftRollupTree *t)
{
	for (int u = 0; u < t->n_nodes; u++)
		if (!t->nodes[u].grand)
		{
			if (t->nodes[u].spill)
				sift_gspill_free(t->nodes[u].spill);
			sift_ht_destroy(&t->nodes[u].ht);
		}
		else
		{
			SiftDistSet *set = *(SiftDistSet **) (t->nodes[u].grand_acc + DIST_OFF);

			if (set)
				sift_dist_free(set);
			pfree(t->nodes[u].grand_acc);
		}
	pfree(t->nodes);
	pfree(t->kcol);
	pfree(t->kvalid);
	pfree(t->khash);
	pfree(t->sel);
	pfree(t);
}

void
sift_rollup_shape(const SiftRollupTree *t, uint16 *n_nodes, uint16 *n_leaves, bool *post_rollup)
{
	*n_nodes = t->n_nodes;
	*n_leaves = t->n_leaves;
	*post_rollup = t->post;
}

static uint64
node_bytes(const RollNode *nd)
{
	return nd->dist_bytes + (nd->grand ? 0 : sift_ht_bytes(&nd->ht));
}

static uint64
tree_bytes(const SiftRollupTree *t)
{
	uint64		b = 0;

	for (int u = 0; u < t->n_nodes; u++)
		b += node_bytes(&t->nodes[u]);
	return b;
}

static bool
over_soft_limit(const SiftRollupTree *t)
{
	return tree_bytes(t) * 100 >= t->max_bytes * SIFT_SPILL_SOFT_PERCENT;
}

/* The distinct values of the grand total are one set: it has no table to spill, and no way to be smaller */
static void
check_grand(const SiftRollupTree *t)
{
	for (int u = 0; u < t->n_nodes; u++)
		if (t->nodes[u].grand && t->nodes[u].dist_bytes > t->max_bytes)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the groups of the grouping sets take more than %lu bytes", (unsigned long) t->max_bytes),
							errhint("Raise work_mem.")));
}

/* The groups of the table to the files, and the table empty: the dist sets are freed with it */
static void
flush_node(RollNode *nd)
{
	sift_gspill_put_ht(nd->spill, &nd->ht);
	sift_ht_clear(&nd->ht);
	nd->dist_bytes = 0;
}

/* The node outgrew its share: its groups go to the files, and its table is the small one that gathers the rows that follow, flushed whenever it is full */
static void
spill_node(SiftRollupTree *t, RollNode *nd)
{
	MemoryContext old = MemoryContextSwitchTo(t->cxt);

	nd->spill = sift_gspill_new(&t->shape);
	sift_gspill_put_ht(nd->spill, &nd->ht);
	sift_ht_destroy(&nd->ht);
	sift_ht_init_ex(&nd->ht, t->spill_cap, true, t->stride, DIST_OFF);
	nd->dist_bytes = 0;
	MemoryContextSwitchTo(old);
}

/* Room in the table of a node that spilled for the group the next upsert may add */
static void
spilled_room(const SiftRollupTree *t, RollNode *nd)
{
	if (nd->ht.count >= nd->ht.grow_at || nd->dist_bytes >= t->max_bytes / 8)
		flush_node(nd);
}

/* The node that holds the most and has not spilled, when that is more than the table it would be left with */
static RollNode *
biggest_in_memory(SiftRollupTree *t)
{
	RollNode   *best = NULL;

	for (int u = 0; u < t->n_nodes; u++)
	{
		RollNode   *nd = &t->nodes[u];

		if (!nd->grand && !nd->spill && node_bytes(nd) > (uint64) t->spill_cap * (sizeof(SiftAggSlot64) + 1 + t->stride) &&
			(!best || node_bytes(nd) > node_bytes(best)))
			best = nd;
	}
	return best;
}

/* The tiers: the nodes grow in memory, and while the tree holds more than SIFT_SPILL_SOFT_PERCENT of its budget the biggest one goes to the files */
static void
make_room(SiftRollupTree *t)
{
	RollNode   *big;

	while (over_soft_limit(t) && (big = biggest_in_memory(t)) != NULL)
		spill_node(t, big);
	check_grand(t);
}

static void
take_col(SiftRollupTree *t, int c, const SiftVec *v, const uint64 *mask)
{
	int64	   *d = KCOL(t, c);
	uint64	   *dv = KVAL(t, c);

	memset(dv, 0, SIFT_VEC_WORDS * 8);
	for (uint16 j = 0; j < t->cn; j++)
	{
		const uint32 r = mask ? t->sel[j] : j;
		const bool	valid = v->all_valid || BIT(v->valid, r);

		d[j] = valid ? (v->type == SVEC_I32 ? (int64) ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r]) : 0;
		dv[j >> 6] |= (uint64) valid << (j & 63);
	}
}

/* the rows of the batch into the table of a node: one upsert a row, the hash of its set from the hashes of its key columns */
static void
feed_node(SiftRollupTree *t, RollNode *nd)
{
	const int	nk = t->spec.n_keys;

	for (uint16 i = 0; i < t->cn; i++)
	{
		int64		key[SIFT_ROLLUP_MAX_KEYS];
		uint64		nulls = 0,
					h = 0;
		uint8	   *blk;
		bool		found;
		bool		first = true;

		memset(key, 0, sizeof(key));
		for (int k = 0; k < nk; k++)
		{
			if (!((nd->mask >> k) & 1))
				continue;
			if (BIT(KVAL(t, k), i))
				key[k] = KCOL(t, k)[i];
			else
				nulls |= UINT64CONST(1) << k;
			h = first ? KHASH(t, k)[i] : sift_aggk6_hash1(((h << 5) | (h >> 59)) ^ KHASH(t, k)[i]);
			first = false;
		}
		if (nd->grand)
			blk = nd->grand_acc;
		else
		{
			SiftAggSlot64 *s;

			if (nd->spill)
				spilled_room(t, nd);
			s = sift_ht_upsert_wide(&nd->ht, h, key, nk * 8, nulls, &found);
			blk = sift_ht_ext(&nd->ht, s);
		}
		acc_add(t, nd, blk, i);
	}
}

void
sift_rollup_feed(SiftRollupTree *t, const SiftRollupBatch *b)
{
	const int	nk = t->spec.n_keys;

	Assert(b->n <= SIFT_VEC_LEN);
	t->cn = b->mask ? sift_aggk6->mask_to_sel16(t->sel, b->mask, b->n) : b->n;
	if (!t->cn)
		return;
	for (int k = 0; k < nk; k++)
		take_col(t, k, b->keys[k], b->mask);
	for (int v = 0; v < t->spec.n_vals; v++)
		take_col(t, nk + v, b->vals[v], b->mask);
	for (int k = 0; k < nk; k++)
		sift_aggk6->hash_i64((uint64 *) KHASH(t, k), (const int64 *) KCOL(t, k), NULL, t->cn, false);
	for (int u = 0; u < t->n_nodes; u++)
		if (t->nodes[u].parent < 0)
			feed_node(t, &t->nodes[u]);
	make_room(t);
}

/* The node's groups as final tables, one when it is in memory and the merged partitions of its files when it spilled */
typedef void (*NodeVisit) (SiftRollupTree *t, void *arg, SiftSaltedHT *ht);

typedef struct NodeScan
{
	SiftRollupTree *t;
	NodeVisit	visit;
	void	   *arg;
} NodeScan;

static void
node_scan_cb(void *arg, SiftSaltedHT *ht)
{
	NodeScan   *ns = arg;

	ns->visit(ns->t, ns->arg, ht);
}

/* A table merged from the files may take what the nodes that are in memory leave of the budget, and a quarter of it at least */
static uint64
merge_budget(const SiftRollupTree *t)
{
	const uint64 used = tree_bytes(t);

	return Max(t->max_bytes > used ? t->max_bytes - used : 0, t->max_bytes / 4);
}

static void
node_scan(SiftRollupTree *t, RollNode *nd, NodeVisit visit, void *arg)
{
	NodeScan	ns = {t, visit, arg};

	if (!nd->spill)
	{
		visit(t, arg, &nd->ht);
		return;
	}
	flush_node(nd);
	sift_gspill_scan(nd->spill, merge_budget(t), node_scan_cb, &ns);
}

/* One group of a node into the node u built from it: the key of u, the state merged */
typedef struct FoldCtx
{
	RollNode   *u;
	const RollNode *p;
} FoldCtx;

static void
fold_table(SiftRollupTree *t, void *arg, SiftSaltedHT *pht)
{
	const FoldCtx *f = arg;
	RollNode   *u = f->u;
	const int	nk = t->spec.n_keys;
	SiftAggSlot64 *s;

	SIFT_HT_FOREACH(pht, s)
	{
		uint32		len;
		const int64 *pk = (const int64 *) sift_ht_wide_key(pht, s, &len);
		int64		key[SIFT_ROLLUP_MAX_KEYS];
		const uint64 nulls = s->key.wide.key_nulls & u->mask;
		uint8	   *dst;

		for (int k = 0; k < nk; k++)
			key[k] = ((u->mask >> k) & 1) ? pk[k] : 0;
		if (u->grand)
			dst = u->grand_acc;
		else
		{
			const uint64 h = sift_aggk6_hash_bytes(key, nk * 8) ^ sift_aggk6_hash1(nulls);
			bool		found;

			if (u->spill)
				spilled_room(t, u);
			else if (++t->folds % 128 == 0 && over_soft_limit(t))
				spill_node(t, u);
			dst = sift_ht_ext(&u->ht, sift_ht_upsert_wide(&u->ht, h, key, nk * 8, nulls, &found));
		}
		acc_merge(t, dst, sift_ht_ext(pht, s));
	}
}

/* the groups of the node p, projected to the columns of the node u, into the table of u */
static void
build_from(SiftRollupTree *t, RollNode *u, RollNode *p)
{
	FoldCtx		f = {u, p};

	if (p->grand)
		return;
	node_scan(t, p, fold_table, &f);
}

void
sift_rollup_finish(SiftRollupTree *t)
{
	for (int u = 0; u < t->n_nodes; u++)
		if (t->nodes[u].parent >= 0)
		{
			build_from(t, &t->nodes[u], &t->nodes[t->nodes[u].parent]);
			make_room(t);
		}
}

static void
emit_group(const SiftRollupTree *t, uint16 set, const RollNode *nd, const int64 *key, uint64 nulls, uint8 *blk, SiftRollupEmit emit, void *arg)
{
	SiftRollupGroup g;
	const uint64 all = t->spec.n_keys == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << t->spec.n_keys) - 1;

	memset(&g, 0, sizeof(g));
	g.set = set;
	g.omitted = ~nd->mask & all;
	g.key_nulls = nulls | g.omitted;
	for (int k = 0; k < t->spec.n_keys; k++)
		g.key[k] = ((nd->mask >> k) & 1) ? key[k] : 0;
	g.rows = *acc_rows(blk);
	for (int v = 0; v < t->spec.n_vals; v++)
		g.val[v] = *acc_val(blk, v);
	if (t->spec.count_distinct && *(SiftDistSet **) (blk + DIST_OFF))
		g.distinct = sift_dist_count(*(SiftDistSet **) (blk + DIST_OFF));
	emit(arg, &g);
}

typedef struct EmitCtx
{
	uint16		set;
	const RollNode *nd;
	SiftRollupEmit emit;
	void	   *arg;
} EmitCtx;

static void
emit_table(SiftRollupTree *t, void *arg, SiftSaltedHT *ht)
{
	const EmitCtx *e = arg;
	SiftAggSlot64 *slot;

	SIFT_HT_FOREACH(ht, slot)
	{
		uint32		len;

		emit_group(t, e->set, e->nd, (const int64 *) sift_ht_wide_key(ht, slot, &len), slot->key.wide.key_nulls, sift_ht_ext(ht, slot), e->emit,
				   e->arg);
	}
}

void
sift_rollup_emit(SiftRollupTree *t, SiftRollupEmit emit, void *arg)
{
	for (int s = 0; s < t->spec.n_sets; s++)
	{
		RollNode   *nd = &t->nodes[t->node_of_set[s]];
		EmitCtx		e = {(uint16) s, nd, emit, arg};

		if (nd->grand)
			emit_group(t, (uint16) s, nd, NULL, 0, nd->grand_acc, emit, arg);
		else
			node_scan(t, nd, emit_table, &e);
	}
}

void
sift_rollup_spill_stats(const SiftRollupTree *t, uint16 *nodes_spilled, uint64 *bytes_spilled, uint32 *depth)
{
	*nodes_spilled = 0;
	*bytes_spilled = 0;
	*depth = 0;
	for (int u = 0; u < t->n_nodes; u++)
		if (t->nodes[u].spill)
		{
			(*nodes_spilled)++;
			*bytes_spilled += sift_gspill_bytes(t->nodes[u].spill);
			*depth = Max(*depth, sift_gspill_depth(t->nodes[u].spill));
		}
}
