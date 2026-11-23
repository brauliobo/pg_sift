/*
 * agg/v6/v6_win_par.c - the partition regroup of v6_win_par.h.
 */
/* check-memory: allow-allocs the rows are held up to max_bytes, which the planner takes from work_mem; the arrays follow its rows and partitions */
#include "core/host.h"

#include "agg/v6/v6_batch.h"
#include "agg/v6/v6_win_par.h"

#define RG_FIRST_ROWS		SIFT_VEC_LEN
#define RG_FIRST_SLOTS		1024
#define RG_MAX_ROWS			(MaxAllocSize / sizeof(SiftI128))
#define RG_MAX_GROUPS		(MaxAllocSize / (sizeof(int64) * SIFT_WIN_MAX_KEYS))
#define RG_GROUP_BYTE		(sizeof(int64) * SIFT_WIN_MAX_KEYS + 2 * sizeof(uint32) + sizeof(uint8))
#define RG_GROUP_BYTES(np)	((uint64) (np) * RG_GROUP_BYTE)

typedef struct Regroup
{
	SiftV6Source src;			/* first: the source handed out */
	SiftV6Source *in;
	MemoryContext cx;
	SiftWinRegroupSpec spec;
	size_t		esz[SIFT_V6_MAX_COLS];
	uint64		row_bytes;
	bool		drained;
	uint32		n;
	uint32		cap;
	char	   *data[SIFT_V6_MAX_COLS];
	uint8	   *valid[SIFT_V6_MAX_COLS];
	uint64	   *tag;
	uint32	   *gid;			/* of each row: its partition */
	uint32		ngroups;
	uint32		gcap;
	int64	   *gkey;			/* of each partition: its key values (0 for a NULL one) */
	uint8	   *gnull;			/* and the bit of each NULL key */
	uint32	   *slot;			/* the hash table over the keys: partition number + 1, 0 empty */
	uint32		nslot;
	uint32	   *order;			/* the rows by partition */
	uint32		pos;
	char	   *obuf[SIFT_V6_MAX_COLS];
	uint64		ovalid[SIFT_V6_MAX_COLS][SIFT_VEC_WORDS];
	SiftVec		ovec[SIFT_V6_MAX_COLS];
	uint64	   *otag;
} Regroup;

bool
sift_win_regroup_type_ok(SiftVecType t)
{
	return t == SVEC_I32 || t == SVEC_I64 || t == SVEC_F64 || t == SVEC_NUM64 || t == SVEC_NUM128;
}

static size_t
elem_size(SiftVecType t)
{
	return t == SVEC_I32 ? sizeof(int32) : t == SVEC_NUM128 ? sizeof(SiftI128) : sizeof(int64);
}

static inline void
copy_elem(char *dst, uint32 di, const char *src, uint32 si, size_t esz)
{
	if (esz == sizeof(int32))
		((int32 *) dst)[di] = ((const int32 *) src)[si];
	else if (esz == sizeof(int64))
		((int64 *) dst)[di] = ((const int64 *) src)[si];
	else
		memcpy(dst + (size_t) di * esz, src + (size_t) si * esz, esz);
}

static uint64
row_bytes_of(const SiftVecType *type, int ncols)
{
	uint64		b = sizeof(uint64) + 2 * sizeof(uint32);

	for (int c = 0; c < ncols; c++)
		b += elem_size(type[c]) + 1;
	return b;
}

bool
sift_win_regroup_fits(const SiftV6Input *input, const List *vars, double rows, double groups, uint64 max_bytes)
{
	SiftV6Input in = *input;
	SiftVecType type[SIFT_V6_MAX_COLS];

	foreach_ptr(Var, v, (List *) vars)
		sift_v6_input_add_var(&in, v);
	for (int i = 0; i < in.ncols; i++)
	{
		if (!sift_win_regroup_type_ok(in.col[i].type))
			return false;
		type[i] = in.col[i].type;
	}
	return rows * row_bytes_of(type, in.ncols) + groups * RG_GROUP_BYTE <= max_bytes * 0.9;
}

static void
over_budget(void)
{
	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the rows of a window do not fit in work_mem to be put together by partition"),
					errhint("Increase work_mem, or filter the rows of the window.")));
}

static void
grow_rows(Regroup *g)
{
	const uint64 room = g->spec.max_bytes > RG_GROUP_BYTES(g->gcap) ? (g->spec.max_bytes - RG_GROUP_BYTES(g->gcap)) / g->row_bytes : 0;
	const uint32 cap = (uint32) Min(Min(room, RG_MAX_ROWS), g->cap == 0 ? RG_FIRST_ROWS : (uint64) g->cap * 2);

	if (cap <= g->cap)
		over_budget();
	for (int c = 0; c < g->spec.ncols; c++)
	{
		g->data[c] = g->cap == 0 ? MemoryContextAlloc(g->cx, (Size) cap * g->esz[c]) : repalloc(g->data[c], (Size) cap * g->esz[c]);
		g->valid[c] = g->cap == 0 ? MemoryContextAlloc(g->cx, cap) : repalloc(g->valid[c], cap);
	}
	g->tag = g->cap == 0 ? MemoryContextAlloc(g->cx, (Size) cap * sizeof(uint64)) : repalloc(g->tag, (Size) cap * sizeof(uint64));
	g->gid = g->cap == 0 ? MemoryContextAlloc(g->cx, (Size) cap * sizeof(uint32)) : repalloc(g->gid, (Size) cap * sizeof(uint32));
	g->cap = cap;
}

static inline uint64
mix(uint64 h, uint64 v)
{
	h ^= v + UINT64CONST(0x9E3779B97F4A7C15) + (h << 6) + (h >> 2);
	h *= UINT64CONST(0xFF51AFD7ED558CCD);
	return h ^ (h >> 32);
}

static inline uint64
key_hash(const int64 *key, uint8 nulls, int n)
{
	uint64		h = nulls;

	for (int k = 0; k < n; k++)
		h = mix(h, (uint64) key[k]);
	return h;
}

static void
rehash(Regroup *g, uint32 nslot)
{
	if (g->slot != NULL)
		pfree(g->slot);
	g->slot = MemoryContextAllocZero(g->cx, (Size) nslot * sizeof(uint32));
	g->nslot = nslot;
	for (uint32 id = 0; id < g->ngroups; id++)
	{
		uint32		i = key_hash(&g->gkey[(Size) id * g->spec.n_part], g->gnull[id], g->spec.n_part) & (nslot - 1);

		while (g->slot[i] != 0)
			i = (i + 1) & (nslot - 1);
		g->slot[i] = id + 1;
	}
}

static void
grow_groups(Regroup *g)
{
	const uint64 used = (uint64) g->cap * g->row_bytes;
	const uint64 room = g->spec.max_bytes > used ? (g->spec.max_bytes - used) / RG_GROUP_BYTE : 0;
	const uint32 gcap = (uint32) Min(Min(room, RG_MAX_GROUPS), g->gcap == 0 ? RG_FIRST_SLOTS : (uint64) g->gcap * 2);

	if (gcap <= g->gcap)
		over_budget();
	g->gkey = g->gcap == 0 ? MemoryContextAlloc(g->cx, (Size) gcap * g->spec.n_part * sizeof(int64)) : repalloc(g->gkey, (Size) gcap * g->spec.n_part * sizeof(int64));
	g->gnull = g->gcap == 0 ? MemoryContextAlloc(g->cx, gcap) : repalloc(g->gnull, gcap);
	g->gcap = gcap;
}

/* the partition of the key (the number of a new one when the key has not been seen) */
static uint32
group_of(Regroup *g, const int64 *key, uint8 nulls)
{
	const int	np = g->spec.n_part;
	uint32		i = key_hash(key, nulls, np) & (g->nslot - 1);

	for (uint32 id; (id = g->slot[i]) != 0; i = (i + 1) & (g->nslot - 1))
		if (g->gnull[id - 1] == nulls && memcmp(&g->gkey[(Size) (id - 1) * np], key, np * sizeof(int64)) == 0)
			return id - 1;
	if (g->ngroups == g->gcap)
		grow_groups(g);
	memcpy(&g->gkey[(Size) g->ngroups * np], key, np * sizeof(int64));
	g->gnull[g->ngroups] = nulls;
	g->slot[i] = ++g->ngroups;
	if ((uint64) g->ngroups * 2 > g->nslot)
		rehash(g, g->nslot * 2);
	return g->ngroups - 1;
}

static inline int64
int_at(const SiftVec *v, uint32 r)
{
	return v->type == SVEC_I32 ? ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
}

static void
append_row(Regroup *g, const SiftV6Batch *b, uint32 r)
{
	int64		key[SIFT_WIN_MAX_KEYS] = {0};
	uint8		nulls = 0;

	if (g->n == g->cap)
		grow_rows(g);
	for (int c = 0; c < g->spec.ncols; c++)
	{
		const SiftVec *v = &b->col[c];

		copy_elem(g->data[c], g->n, v->data, r, g->esz[c]);
		g->valid[c][g->n] = sift_vec_valid_at(v, r);
	}
	for (int k = 0; k < g->spec.n_part; k++)
	{
		const SiftVec *v = &b->col[g->spec.part_slot[k]];

		if (sift_vec_valid_at(v, r))
			key[k] = int_at(v, r);
		else
			nulls |= (uint8) (1 << k);
	}
	g->tag[g->n] = b->tag[r];
	g->gid[g->n] = group_of(g, key, nulls);
	g->n++;
}

/* the rows by partition number, the rows of a partition in the order they came */
static void
sort_rows(Regroup *g)
{
	uint32	   *next = MemoryContextAllocZero(g->cx, ((Size) g->ngroups + 1) * sizeof(uint32));

	for (uint32 i = 0; i < g->n; i++)
		next[g->gid[i] + 1]++;
	for (uint32 p = 0; p < g->ngroups; p++)
		next[p + 1] += next[p];
	g->order = MemoryContextAlloc(g->cx, (Size) Max(g->n, 1) * sizeof(uint32));
	for (uint32 i = 0; i < g->n; i++)
		g->order[next[g->gid[i]]++] = i;
	pfree(next);
	pfree(g->slot);
	g->slot = NULL;
	if (g->gid != NULL)
		pfree(g->gid);
	if (g->gkey != NULL)
	{
		pfree(g->gkey);
		pfree(g->gnull);
	}
	g->gid = NULL;
	g->gkey = NULL;
	g->gnull = NULL;
}

static void
drain(Regroup *g)
{
	SiftV6Batch b;

	while (g->in->next(g->in, &b))
	{
		SIFT_INVARIANT(b.tag != NULL, "the rows of a window have no tag");
		for (uint32 r = 0; r < b.n; r++)
			if (b.mask == NULL || ((b.mask[r >> 6] >> (r & 63)) & 1))
				append_row(g, &b, r);
	}
	sort_rows(g);
	g->drained = true;
}

static bool
rg_next(SiftV6Source *s, SiftV6Batch *out)
{
	Regroup    *g = (Regroup *) s;
	uint32		m;

	if (!g->drained)
		drain(g);
	if (g->pos >= g->n)
		return false;
	m = Min(SIFT_VEC_LEN, g->n - g->pos);
	for (int c = 0; c < g->spec.ncols; c++)
	{
		uint64	   *words = g->ovalid[c];
		bool		all_valid = true;

		memset(words, 0, sizeof(g->ovalid[c]));
		for (uint32 i = 0; i < m; i++)
		{
			const uint32 row = g->order[g->pos + i];
			const bool	valid = g->valid[c][row];

			copy_elem(g->obuf[c], i, g->data[c], row, g->esz[c]);
			words[i >> 6] |= (uint64) valid << (i & 63);
			all_valid &= valid;
		}
		g->ovec[c] = (SiftVec) {.data = g->obuf[c],.valid = all_valid ? NULL : words,.len = (uint16) m,.all_valid = all_valid,.dense = true,.type = g->spec.type[c],.scale = g->spec.scale[c]};
	}
	for (uint32 i = 0; i < m; i++)
		g->otag[i] = g->tag[g->order[g->pos + i]];
	g->pos += m;
	*out = (SiftV6Batch) {.n = (uint16) m,.col = g->ovec,.tag = g->otag};
	return true;
}

static bool
rg_fetch(SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	Regroup    *g = (Regroup *) s;

	return g->in->fetch(g->in, tag, attnos, n, values, isnull);
}

static void
rg_close(SiftV6Source *s)
{
	Regroup    *g = (Regroup *) s;

	g->in->close(g->in);
	if (g->cap > 0)
	{
		for (int c = 0; c < g->spec.ncols; c++)
		{
			pfree(g->data[c]);
			pfree(g->valid[c]);
		}
		pfree(g->tag);
	}
	if (g->order != NULL)
		pfree(g->order);
	g->cap = 0;
	g->order = NULL;
}

SiftV6Source *
sift_win_regroup_open(MemoryContext cx, SiftV6Source *in, const SiftWinRegroupSpec *spec)
{
	Regroup    *g = MemoryContextAllocZero(cx, sizeof(Regroup));

	SIFT_INVARIANT(spec->ncols <= SIFT_V6_MAX_COLS && spec->n_part >= 1 && spec->n_part <= SIFT_WIN_MAX_KEYS, "window: too many columns or keys to put together");
	g->in = in;
	g->cx = cx;
	g->spec = *spec;
	g->row_bytes = row_bytes_of(spec->type, spec->ncols);
	for (int c = 0; c < spec->ncols; c++)
	{
		SIFT_INVARIANT(sift_win_regroup_type_ok(spec->type[c]), "window: a column of a type the partition regroup does not hold");
		g->esz[c] = elem_size(spec->type[c]);
		g->obuf[c] = MemoryContextAllocAligned(cx, g->esz[c] * SIFT_VEC_LEN + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	g->otag = MemoryContextAlloc(cx, SIFT_VEC_LEN * sizeof(uint64));
	rehash(g, RG_FIRST_SLOTS);
	g->src = (SiftV6Source) {.next = rg_next,.skip = NULL,.fetch = rg_fetch,.close = rg_close};
	return &g->src;
}

SiftV6Source *
sift_win_regroup_over(MemoryContext cx, SiftV6Source *src, const SiftV6Input *in, int n_part, const int *part_slot, uint64 max_bytes)
{
	SiftWinRegroupSpec rg = {.ncols = in->ncols,.n_part = n_part,.max_bytes = max_bytes};

	for (int i = 0; i < in->ncols; i++)
	{
		rg.type[i] = in->col[i].type;
		rg.scale[i] = in->col[i].scale;
	}
	memcpy(rg.part_slot, part_slot, n_part * sizeof(int));
	return sift_win_regroup_open(cx, src, &rg);
}
