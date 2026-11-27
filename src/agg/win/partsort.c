/*
 * agg/win/partsort.c - the partition-local sort of v6_win_partsort.h.
 */
/* check-memory: allow-allocs a partition is held up to max_bytes, which the planner takes from work_mem; the arrays follow its rows */
#include "core/host.h"

#include <math.h>

#include "agg/batch.h"
#include "agg/win/partsort.h"
#include "agg/win/spill.h"
#include "core/memtrack.h"

#define PS_CHUNK	65536
#define PS_MIN_CAP	256

typedef struct PsCol
{
	SiftVecType type;
	size_t		esz;
	char	   *data;
	uint8	   *valid;			/* of each row: SIFT_CELL_STATE_* */
} PsCol;

typedef struct PsChunk
{
	struct PsChunk *next;
	size_t		used;
	char		bytes[PS_CHUNK];
} PsChunk;

typedef struct PartSort
{
	SiftV6Source src;			/* first: the source handed out */
	SiftV6Source *in;
	MemoryContext cx;
	SiftWinPsSpec spec;
	SiftV6Batch ib;
	uint16		ipos;
	bool		ib_live;
	bool		eof;
	PsCol		col[SIFT_V6_MAX_COLS];
	uint64	   *tag;
	uint32		n;
	uint32		cap;
	uint64		bytes;
	PsChunk    *chunks;
	SiftWinSpill *sp;			/* the partition read is more than max_bytes: its rows are here, and one (the first) is held */
	MemoryContext bcx;			/* what a batch of the spill points to */
	uint8		ostate[SIFT_V6_MAX_COLS][SIFT_VEC_LEN];
	int64		speer[SIFT_WIN_MAX_KEYS][SIFT_VEC_LEN];
	uint32	   *perm;
	int64	   *peer[SIFT_WIN_MAX_KEYS];
	uint32		opos;
	int64		part_no;
	SiftVec		ovec[SIFT_V6_MAX_COLS + 1 + SIFT_WIN_MAX_KEYS];
	char	   *obuf[SIFT_V6_MAX_COLS + 1 + SIFT_WIN_MAX_KEYS];
	uint64		ovalid[SIFT_V6_MAX_COLS + 1 + SIFT_WIN_MAX_KEYS][SIFT_VEC_WORDS];
	uint64		onan[SIFT_V6_MAX_COLS + 1 + SIFT_WIN_MAX_KEYS][SIFT_VEC_WORDS];
	uint64		otag[SIFT_VEC_LEN];
} PartSort;

size_t
sift_win_ps_elem_size(SiftVecType t)
{
	switch (t)
	{
		case SVEC_I32:
			return 4;
		case SVEC_I64:
		case SVEC_F64:
		case SVEC_NUM64:
			return 8;
		case SVEC_NUM128:
			return sizeof(SiftI128);
		default:
			return sizeof(SiftStrView);
	}
}

bool
sift_win_ps_type_ok(SiftVecType t)
{
	return t == SVEC_I32 || t == SVEC_I64 || t == SVEC_F64 || t == SVEC_NUM64 || t == SVEC_NUM128 || t == SVEC_STRVIEW;
}

static const char *
sv_bytes(const SiftStrView *v)
{
	return v->len > 12 ? v->u.ptr : (const char *) v->prefix;
}

static int
sv_cmp(const SiftStrView *a, const SiftStrView *b)
{
	const uint32 m = Min(a->len, b->len);
	const int	r = m == 0 ? 0 : memcmp(sv_bytes(a), sv_bytes(b), m);

	return r != 0 ? r : (a->len < b->len ? -1 : a->len > b->len);
}

static int
f8_cmp(double a, double b)
{
	const bool	an = isnan(a),
				bn = isnan(b);

	if (an || bn)
		return an == bn ? 0 : (an ? 1 : -1);
	return a < b ? -1 : a > b;
}

/* the order of the cells i and j of a column: the values only, both valid */
static int
cell_cmp(const PsCol *c, uint32 i, uint32 j)
{
	switch (c->type)
	{
		case SVEC_I32:
		{
			const int32 *p = (const int32 *) c->data;

			return p[i] < p[j] ? -1 : p[i] > p[j];
		}
		case SVEC_F64:
		{
			const double *p = (const double *) c->data;

			return f8_cmp(p[i], p[j]);
		}
		case SVEC_NUM128:
		{
			const SiftI128 *p = (const SiftI128 *) c->data;
			const __int128 x = (__int128) (((unsigned __int128) (uint64) p[i].hi << 64) | p[i].lo),
						y = (__int128) (((unsigned __int128) (uint64) p[j].hi << 64) | p[j].lo);

			return x < y ? -1 : x > y;
		}
		case SVEC_STRVIEW:
			return sv_cmp(&((const SiftStrView *) c->data)[i], &((const SiftStrView *) c->data)[j]);
		default:
		{
			const int64 *p = (const int64 *) c->data;

			return p[i] < p[j] ? -1 : p[i] > p[j];
		}
	}
}

static int
key_cmp(const PartSort *ps, const SiftWinPsKey *k, uint32 i, uint32 j)
{
	const PsCol *c = &ps->col[k->slot];
	const uint8	si = c->valid[i],
				sj = c->valid[j];
	int			r;

	if (si == SIFT_CELL_STATE_NULL || sj == SIFT_CELL_STATE_NULL)
		return si == sj ? 0 : ((si == SIFT_CELL_STATE_NULL) == k->nulls_first ? -1 : 1);
	r = si != sj ? (si == SIFT_CELL_STATE_NAN ? 1 : -1) : si == SIFT_CELL_STATE_NAN ? 0 : cell_cmp(c, i, j);
	return k->desc ? -r : r;
}

static int
row_cmp(const void *a, const void *b, void *arg)
{
	const PartSort *ps = arg;
	const uint32 i = *(const uint32 *) a,
				j = *(const uint32 *) b;

	for (int k = 0; k < ps->spec.n_ord; k++)
	{
		const int	r = key_cmp(ps, &ps->spec.ord[k], i, j);

		if (r != 0)
			return r;
	}
	return i < j ? -1 : i > j;
}

/* the arrays of the rows at cap rows */
static void
resize(PartSort *ps, uint32 cap)
{
	for (int c = 0; c < ps->spec.ncols; c++)
	{
		PsCol	   *pc = &ps->col[c];

		pc->data = ps->cap == 0 ? MemoryContextAlloc(ps->cx, (Size) pc->esz * cap) : repalloc(pc->data, (Size) pc->esz * cap);
		pc->valid = ps->cap == 0 ? MemoryContextAlloc(ps->cx, cap) : repalloc(pc->valid, cap);
	}
	ps->tag = ps->cap == 0 ? MemoryContextAlloc(ps->cx, sizeof(uint64) * cap) : repalloc(ps->tag, sizeof(uint64) * cap);
	ps->perm = ps->cap == 0 ? MemoryContextAlloc(ps->cx, sizeof(uint32) * cap) : repalloc(ps->perm, sizeof(uint32) * cap);
	for (int k = 0; k < ps->spec.n_ord; k++)
		ps->peer[k] = ps->cap == 0 ? MemoryContextAlloc(ps->cx, sizeof(int64) * cap) : repalloc(ps->peer[k], sizeof(int64) * cap);
	ps->cap = cap;
}

static void
grow(PartSort *ps, uint32 need)
{
	uint32		cap = Max(ps->cap, PS_MIN_CAP);

	if (need <= ps->cap)
		return;
	while (cap < need)
		cap *= 2;
	resize(ps, cap);
}

/* the arrays are full and twice their rows would make one of them more than MaxAllocSize */
static bool
arrays_full(const PartSort *ps)
{
	uint64		esz = sizeof(int64);

	if (ps->n < ps->cap)
		return false;
	for (int c = 0; c < ps->spec.ncols; c++)
		esz = Max(esz, ps->col[c].esz);
	return (uint64) ps->cap * 2 * esz > MaxAllocSize;
}

static char *
arena_copy(PartSort *ps, const char *p, uint32 len)
{
	PsChunk    *ch = ps->chunks;

	if (ch == NULL || ch->used + len > PS_CHUNK)
	{
		ch = MemoryContextAlloc(ps->cx, sift_alloc_grow(0, Max(sizeof(PsChunk), offsetof(PsChunk, bytes) + (Size) len), "a value of a row of a window"));
		ch->next = ps->chunks;
		ch->used = 0;
		ps->chunks = ch;
	}
	memcpy(ch->bytes + ch->used, p, len);
	ch->used += len;
	return ch->bytes + ch->used - len;
}

static void
free_chunks(PsChunk *ch)
{
	while (ch != NULL)
	{
		PsChunk    *next = ch->next;

		pfree(ch);
		ch = next;
	}
}

static void
release_chunks(PartSort *ps)
{
	free_chunks(ps->chunks);
	ps->chunks = NULL;
}

static inline bool
batch_active(const SiftV6Batch *b, uint32 r)
{
	return b->mask == NULL || ((b->mask[r >> 6] >> (r & 63)) & 1);
}

/* the row r of the input batch is in the partition being read, whatever its columns; compared by the partition keys against the first row held */
static bool
same_partition(const PartSort *ps, uint32 r)
{
	for (int k = 0; k < ps->spec.n_part; k++)
	{
		const SiftVec *v = &ps->ib.col[ps->spec.part_slot[k]];
		const PsCol *c = &ps->col[ps->spec.part_slot[k]];
		const uint8	state = sift_vec_state_at(v, r);

		if (state != c->valid[0])
			return false;
		if (state != SIFT_CELL_STATE_VALUE)
			continue;
		switch (v->type)
		{
			case SVEC_I32:
				if (((const int32 *) v->data)[r] != ((const int32 *) c->data)[0])
					return false;
				break;
			case SVEC_F64:
				if (f8_cmp(((const double *) v->data)[r], ((const double *) c->data)[0]) != 0)
					return false;
				break;
			case SVEC_NUM128:
				if (memcmp(&((const SiftI128 *) v->data)[r], c->data, sizeof(SiftI128)) != 0)
					return false;
				break;
			case SVEC_STRVIEW:
				if (sv_cmp(&((const SiftStrView *) v->data)[r], (const SiftStrView *) c->data) != 0)
					return false;
				break;
			default:
				if (((const int64 *) v->data)[r] != ((const int64 *) c->data)[0])
					return false;
				break;
		}
	}
	return true;
}

static void
put_batch_row(PartSort *ps, uint32 r)
{
	uint8		state[SIFT_V6_MAX_COLS];
	const char *cell[SIFT_V6_MAX_COLS];

	for (int c = 0; c < ps->spec.ncols; c++)
	{
		state[c] = sift_vec_state_at(&ps->ib.col[c], r);
		cell[c] = (const char *) ps->ib.col[c].data + (size_t) r * ps->col[c].esz;
	}
	sift_win_spill_put(ps->sp, ps->ib.tag != NULL ? ps->ib.tag[r] : 0, state, cell);
}

static void
put_held_row(PartSort *ps, uint32 i)
{
	uint8		state[SIFT_V6_MAX_COLS];
	const char *cell[SIFT_V6_MAX_COLS];

	for (int c = 0; c < ps->spec.ncols; c++)
	{
		state[c] = ps->col[c].valid[i];
		cell[c] = ps->col[c].data + (size_t) i * ps->col[c].esz;
	}
	sift_win_spill_put(ps->sp, ps->tag[i], state, cell);
}

/* the rows taken stay, in their order, and the strings of the others are freed */
static void
retain(PartSort *ps, const uint8 *take)
{
	PsChunk    *old = ps->chunks;
	const uint32 before = ps->n;
	uint32		o = 0;

	ps->chunks = NULL;
	for (uint32 i = 0; i < before; i++)
	{
		if (!take[i])
			continue;
		for (int c = 0; c < ps->spec.ncols; c++)
		{
			PsCol	   *pc = &ps->col[c];

			memmove(pc->data + o * pc->esz, pc->data + i * pc->esz, pc->esz);
			pc->valid[o] = pc->valid[i];
			if (pc->type == SVEC_STRVIEW && pc->valid[o] == SIFT_CELL_STATE_VALUE && ((SiftStrView *) pc->data)[o].len > 12)
				((SiftStrView *) pc->data)[o].u.ptr = arena_copy(ps, ((SiftStrView *) pc->data)[o].u.ptr, ((SiftStrView *) pc->data)[o].len);
		}
		ps->tag[o++] = ps->tag[i];
	}
	free_chunks(old);
	ps->bytes = ps->bytes / before * o;
	ps->n = o;
}

/* the partition is more than max_bytes: its rows go to the sort, of which the first row stays for same_partition() in arrays of the least rows */
static void
start_spill(PartSort *ps)
{
	uint8	   *take = MemoryContextAllocZero(ps->cx, ps->n);

	ps->sp = sift_win_spill_begin(ps->cx, &ps->spec, ps->spec.max_bytes);
	for (uint32 i = 0; i < ps->n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		put_held_row(ps, i);
	}
	take[0] = 1;
	retain(ps, take);
	pfree(take);
	if (ps->cap > PS_MIN_CAP)
		resize(ps, PS_MIN_CAP);
}

/* the rows held are past 2 * keep + 1, which is computed without overflow for any keep a run condition gives */
static inline bool
over_keep(const PartSort *ps)
{
	return ps->spec.keep > 0 && ps->n / 2 > ps->spec.keep;
}

static void
add_row(PartSort *ps, uint32 r)
{
	const uint32 i = ps->n;

	if (ps->sp != NULL)
	{
		put_batch_row(ps, r);
		return;
	}
	grow(ps, i + 1);
	for (int c = 0; c < ps->spec.ncols; c++)
	{
		const SiftVec *v = &ps->ib.col[c];
		PsCol	   *pc = &ps->col[c];
		const uint8	state = sift_vec_state_at(v, r);

		pc->valid[i] = state;
		memset(pc->data + i * pc->esz, 0, pc->esz);
		if (state != SIFT_CELL_STATE_VALUE)
			continue;
		memcpy(pc->data + i * pc->esz, (const char *) v->data + (size_t) r * pc->esz, pc->esz);
		if (pc->type == SVEC_STRVIEW)
		{
			SiftStrView *sv = &((SiftStrView *) pc->data)[i];

			if (sv->len > 12)
				sv->u.ptr = arena_copy(ps, sv->u.ptr, sv->len);
			ps->bytes += sv->len > 12 ? sv->len : 0;
		}
		ps->bytes += pc->esz + 1;
	}
	ps->tag[i] = ps->ib.tag != NULL ? ps->ib.tag[r] : 0;
	ps->bytes += sizeof(uint64) + sizeof(uint32) + sizeof(int64) * ps->spec.n_ord;
	ps->n++;
	if ((ps->bytes > ps->spec.max_bytes || arrays_full(ps)) && !over_keep(ps))
		start_spill(ps);
}

static void
sort_rows(PartSort *ps)
{
	for (uint32 i = 0; i < ps->n; i++)
		ps->perm[i] = i;
	if (ps->spec.n_ord > 0 && ps->n > 1)
		qsort_arg(ps->perm, ps->n, sizeof(uint32), row_cmp, ps);
}

/* more than 2 * keep rows are held: the best keep stay, in row order of the input, so that the ties the sort breaks by that order keep falling the same way */
static void
compact(PartSort *ps)
{
	uint8	   *take = MemoryContextAllocZero(ps->cx, ps->n);

	sort_rows(ps);
	for (uint32 i = 0; i < (uint32) ps->spec.keep; i++)
		take[ps->perm[i]] = 1;
	retain(ps, take);
	pfree(take);
}

/* reads the rows of the next partition; false when the input is at its end */
static bool
fill_partition(PartSort *ps)
{
	ps->n = 0;
	ps->bytes = 0;
	release_chunks(ps);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (!ps->ib_live || ps->ipos >= ps->ib.n)
		{
			ps->ib_live = !ps->eof && ps->in->next(ps->in, &ps->ib);
			ps->ipos = 0;
			if (!ps->ib_live)
			{
				ps->eof = true;
				break;
			}
		}
		while (ps->ipos < ps->ib.n)
		{
			const uint32 r = ps->ipos;

			if (!batch_active(&ps->ib, r))
			{
				ps->ipos++;
				continue;
			}
			if (ps->n > 0 && !same_partition(ps, r))
				return true;
			add_row(ps, r);
			ps->ipos++;
			if (over_keep(ps))
				compact(ps);
		}
	}
	return ps->n > 0;
}

static void
peers_of(PartSort *ps)
{
	for (int k = 0; k < ps->spec.n_ord; k++)
	{
		int64		no = 0;

		for (uint32 i = 0; i < ps->n; i++)
		{
			CHECK_FOR_INTERRUPTS();
			if (i > 0)
			{
				const SiftWinPsKey *key = &ps->spec.ord[k];
				const PsCol *c = &ps->col[key->slot];
				const uint32 a = ps->perm[i - 1],
							b = ps->perm[i];
				const uint8	sa = c->valid[a];

				if (sa != c->valid[b] || (sa == SIFT_CELL_STATE_VALUE && cell_cmp(c, a, b) != 0))
					no++;
			}
			ps->peer[k][i] = no;
		}
	}
}

static void
seal_column(PartSort *ps, int c, uint16 m)
{
	uint64	   *valid = ps->ovalid[c];
	bool		has_nan = false;

	memset(valid, 0, sizeof(ps->ovalid[c]));
	memset(ps->onan[c], 0, sizeof(ps->onan[c]));
	for (uint16 i = 0; i < m; i++)
	{
		sift_vec_state_put(valid, ps->onan[c], i, ps->ostate[c][i]);
		has_nan |= ps->ostate[c][i] == SIFT_CELL_STATE_NAN;
	}
	ps->ovec[c] = (SiftVec) {.data = ps->obuf[c],.valid = valid,.nan = has_nan ? ps->onan[c] : NULL,.len = m,.all_valid = false,.dense = true,.type = ps->col[c].type,.scale = ps->spec.scale[c]};
}

static void
gather_column(PartSort *ps, int c, uint32 from, uint16 m)
{
	const PsCol *pc = &ps->col[c];

	for (uint16 i = 0; i < m; i++)
	{
		const uint32 row = ps->perm[from + i];

		memcpy(ps->obuf[c] + (size_t) i * pc->esz, pc->data + (size_t) row * pc->esz, pc->esz);
		ps->ostate[c][i] = pc->valid[row];
	}
	seal_column(ps, c, m);
}

/* the numbers of the partition (peers: NULL) or of the changes of an order key, of the m rows of a batch */
static void
synthetic_column(PartSort *ps, int slot, uint16 m, const int64 *peers)
{
	int64	   *out = (int64 *) ps->obuf[slot];

	for (uint16 i = 0; i < m; i++)
		out[i] = peers == NULL ? ps->part_no : peers[i];
	ps->ovec[slot] = (SiftVec) {.data = out,.valid = NULL,.len = m,.all_valid = true,.dense = true,.type = SVEC_I64};
}

static uint16
memory_batch(PartSort *ps)
{
	const uint16 m = (uint16) Min(SIFT_VEC_LEN, ps->n - ps->opos);

	for (int c = 0; c < ps->spec.ncols; c++)
		gather_column(ps, c, ps->opos, m);
	for (int k = 0; k < ps->spec.n_ord; k++)
		synthetic_column(ps, sift_win_ps_peer_slot(&ps->spec, k), m, ps->peer[k] + ps->opos);
	for (uint16 i = 0; i < m; i++)
		ps->otag[i] = ps->tag[ps->perm[ps->opos + i]];
	ps->opos += m;
	return m;
}

static uint16
spill_batch(PartSort *ps)
{
	SiftWinSpillOut o = {.cx = ps->bcx,.tag = ps->otag};
	uint16		m;

	MemoryContextReset(ps->bcx);
	for (int c = 0; c < ps->spec.ncols; c++)
	{
		o.col[c] = ps->obuf[c];
		o.state[c] = ps->ostate[c];
	}
	for (int k = 0; k < ps->spec.n_ord; k++)
		o.peer[k] = ps->speer[k];
	m = sift_win_spill_read(ps->sp, SIFT_VEC_LEN, &o);
	for (int c = 0; c < ps->spec.ncols; c++)
		seal_column(ps, c, m);
	for (int k = 0; k < ps->spec.n_ord; k++)
		synthetic_column(ps, sift_win_ps_peer_slot(&ps->spec, k), m, ps->speer[k]);
	return m;
}

static void
end_spill(PartSort *ps)
{
	sift_win_spill_end(ps->sp);
	ps->sp = NULL;
	ps->n = 0;
	ps->opos = 0;
}

static bool
next_partition(PartSort *ps)
{
	if (!fill_partition(ps))
		return false;
	if (ps->sp != NULL)
		sift_win_spill_sort(ps->sp);
	else
	{
		sort_rows(ps);
		peers_of(ps);
	}
	ps->part_no++;
	ps->opos = 0;
	return true;
}

static bool
ps_next(SiftV6Source *s, SiftV6Batch *out)
{
	PartSort   *ps = (PartSort *) s;
	uint16		m;

	for (;;)
	{
		if (ps->sp != NULL)
		{
			m = spill_batch(ps);
			if (m > 0)
				break;
			end_spill(ps);
		}
		else if (ps->opos < ps->n)
		{
			m = memory_batch(ps);
			break;
		}
		if (!next_partition(ps))
			return false;
	}
	synthetic_column(ps, sift_win_ps_part_slot(&ps->spec), m, NULL);
	*out = (SiftV6Batch) {.n = m,.col = ps->ovec,.masks = NULL,.mask = NULL,.tag = ps->otag};
	return true;
}

static bool
ps_fetch(SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	PartSort   *ps = (PartSort *) s;

	return ps->in->fetch(ps->in, tag, attnos, n, values, isnull);
}

static void
ps_close(SiftV6Source *s)
{
	PartSort   *ps = (PartSort *) s;

	ps->in->close(ps->in);
	if (ps->sp != NULL)
		sift_win_spill_end(ps->sp);
	MemoryContextDelete(ps->bcx);
	release_chunks(ps);
	if (ps->cap > 0)
	{
		for (int c = 0; c < ps->spec.ncols; c++)
		{
			pfree(ps->col[c].data);
			pfree(ps->col[c].valid);
		}
		pfree(ps->tag);
		pfree(ps->perm);
		for (int k = 0; k < ps->spec.n_ord; k++)
			pfree(ps->peer[k]);
	}
	for (int i = 0; i < ps->spec.ncols + 1 + ps->spec.n_ord; i++)
		pfree(ps->obuf[i]);
	pfree(ps);
}

SiftV6Source *
sift_win_partsort_open(MemoryContext cx, SiftV6Source *in, const SiftWinPsSpec *spec)
{
	PartSort   *ps = MemoryContextAllocZero(cx, sizeof(PartSort));

	SIFT_INVARIANT(spec->ncols <= SIFT_V6_MAX_COLS && spec->n_ord <= SIFT_WIN_MAX_KEYS && spec->n_part <= SIFT_WIN_MAX_KEYS, "window: too many columns or keys to sort");
	ps->in = in;
	ps->cx = cx;
	ps->spec = *spec;
	ps->bcx = AllocSetContextCreate(cx, "sift window spill batch", ALLOCSET_SMALL_SIZES);
	for (int c = 0; c < spec->ncols; c++)
	{
		SIFT_INVARIANT(sift_win_ps_type_ok(spec->type[c]), "window: a column of a type the partition sort does not hold");
		ps->col[c] = (PsCol) {.type = spec->type[c],.esz = sift_win_ps_elem_size(spec->type[c])};
	}
	for (int i = 0; i < spec->ncols + 1 + spec->n_ord; i++)
	{
		const size_t esz = i < spec->ncols ? ps->col[i].esz : sizeof(int64);

		ps->obuf[i] = MemoryContextAllocAligned(cx, esz * SIFT_VEC_LEN + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	ps->src = (SiftV6Source) {.next = ps_next,.skip = NULL,.fetch = ps_fetch,.close = ps_close};
	return &ps->src;
}
