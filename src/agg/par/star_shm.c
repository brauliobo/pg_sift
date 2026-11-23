/*
 * agg/par/star_shm.c - the dimensions of a star join of agg/par/star_shm.h.
 */
#include "postgres.h"

#include "executor/nodeHash.h"
#include "utils/wait_event.h"

#include "agg/par/star_shm.h"

#define ALIGN_UP(x)		TYPEALIGN(SIFT_PAR_SHM_ALIGN, (x))
#define BLOCK_ROWS		SIFT_DIM_BLOCK_ROWS
#define N_BLOCKS(rows)	(((rows) + BLOCK_ROWS - 1) / BLOCK_ROWS)

StaticAssertDecl(SIFT_DIM_BLOCK_ROWS % 64 == 0 && SIFT_DIM_BLOCK_ROWS <= SIFT_VEC_LEN, "a block of rows has whole words of validity bits and is one batch");

static bool
has_sip(SiftDimJoinKind kind)
{
	return kind == SDIM_INNER || kind == SDIM_SEMI;
}

/* The parent's columns are copied into the rows of the dimension: the parent has one row for a key, and gives its columns */
static bool
flattens(const SiftStarDimSpec *dims, int d)
{
	const int	p = dims[d].parent;

	return p >= 0 && dims[p].is_unique_key && (dims[p].kind == SDIM_INNER || dims[p].kind == SDIM_LEFT);
}

static int
n_cols_of(const SiftStarDimSpec *dims, int d)
{
	return dims[d].n_pay_cols + (flattens(dims, d) ? n_cols_of(dims, dims[d].parent) : 0);
}

/* A column of the dimension is NULL for a row whose parent has none: the columns of a LEFT parent, and the ones it has that are */
static bool
col_nullable(const SiftStarDimSpec *dims, int d, int c)
{
	const int	p = dims[d].parent;

	if (c < dims[d].n_pay_cols)
		return false;
	return dims[p].kind == SDIM_LEFT || col_nullable(dims, p, c - dims[d].n_pay_cols);
}

static uint32
sip_blocks(const SiftStarDimSpec *dims, int d)
{
	return sift_sip_blocks_for(dims[d].raw_rows, sift_sip_budget_bytes());
}

/* MEM[MEM-AGG-19]: star join DSM sized for the unfiltered rows of each dimension (raw_rows times 8 bytes per payload column plus 9 bytes of keys and pass flags; workers read int64 payload slots so narrowing the payload is a layout change not made here) */
/* Where everything is: filled into b when it is not NULL, and the total bytes */
static Size
layout(const SiftStarDimSpec *dims, int n, uint32 n_participants, SiftStarDimBundle *b)
{
	Size		at = ALIGN_UP(sizeof(SiftStarDimBundle));

	if (n < 1 || n > SIFT_STAR_MAX_DIMS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a join of %d dimensions is not a star join of at most %d", n, SIFT_STAR_MAX_DIMS), errhint("Join fewer dimensions in one star.")));
	for (int d = 0; d < n; d++)
	{
		const SiftStarDimSpec *s = &dims[d];
		const int	cols = n_cols_of(dims, d);
		const uint32 raw = s->raw_rows;
		SiftStarDimSlot *o = b ? &b->dims[d] : NULL;
		Size		pay_at;

		if (s->parent >= 0 && !dims[s->parent].is_unique_key && (dims[s->parent].kind == SDIM_INNER || dims[s->parent].kind == SDIM_LEFT))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a snowflake dimension that points to a parent with no unique key joins it as semi or anti"), errhint("Make the parent key unique.")));
		if (s->parent >= d || cols > SIFT_STAR_MAX_PAY_COLS)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a dimension of a star join has the parent after it or more than %d columns", SIFT_STAR_MAX_PAY_COLS), errhint("List the parent before the dimension and carry fewer columns.")));
		if (o)
		{
			o->join_kind = s->kind;
			o->parent_dim_id = s->parent;
			o->fact_joined = s->fact_joined;
			o->is_unique_key = s->is_unique_key;
			o->push_sip = s->fact_joined && has_sip(s->kind);
			o->fact_key_col = s->fact_key_col;
			o->n_own_cols = s->n_pay_cols;
			o->raw_rows = raw;
			if (s->parent >= 0 && flattens(dims, d))
				b->dims[s->parent].flattened_into_child = true;
			o->keys_off = at;
		}
		at += ALIGN_UP(mul_size(sizeof(int64), raw));
		if (o)
			o->pass_off = at;
		at += ALIGN_UP(raw);
		if (o)
			o->queue_off = at;
		at += ALIGN_UP(sift_morsel_size(1));
		if (o)
			o->payload_off = at;
		pay_at = ALIGN_UP(sizeof(SiftDimPayloadShm));
		for (int c = 0; c < cols; c++)
		{
			const bool	nullable = s->parent >= 0 && col_nullable(dims, d, c);

			if (o)
			{
				SiftDimPayColMeta *m = &((SiftDimPayloadShm *) ((char *) b + at))->cols[c];

				m->type = SVEC_I64;
				m->nullable = nullable;
				m->data_off = pay_at;
			}
			pay_at += ALIGN_UP(mul_size(sizeof(int64), raw));
			if (o)
				((SiftDimPayloadShm *) ((char *) b + at))->cols[c].valid_off = nullable ? pay_at : 0;
			if (nullable)
				pay_at += ALIGN_UP(mul_size(sizeof(uint64), (raw + 63) / 64));
		}
		at += pay_at;
		if (o)
			o->sip_off = at;
		if (has_sip(s->kind))
			at += ALIGN_UP(sift_sip_size(sip_blocks(dims, d)));
		if (o)
			o->dim_shm_off = at;
		at += sift_dim_shm_size_upto(raw, n_participants);
		at = ALIGN_UP(at);
	}
	return at;
}

Size
sift_star_size(const SiftStarDimSpec *dims, int n, uint32 n_participants)
{
	return add_size(layout(dims, n, n_participants, NULL), SIFT_PAR_SHM_ALIGN);
}

bool
sift_star_fits(const SiftStarDimSpec *dims, int n, uint32 n_participants)
{
	return layout(dims, n, n_participants, NULL) <= get_hash_memory_limit() / 2;
}

SiftStarDimBundle *
sift_star_init(void *raw, const SiftStarDimSpec *dims, int n, uint32 n_participants)
{
	SiftStarDimBundle *b = (SiftStarDimBundle *) TYPEALIGN(SIFT_PAR_SHM_ALIGN, raw);

	memset(b, 0, sizeof(*b));
	b->magic = SIFT_STAR_MAGIC;
	b->n_dims = n;
	b->n_participants = n_participants;
	pg_atomic_init_u32(&b->any_empty_inner, 0);
	pg_atomic_init_u32(&b->next_slot, 0);
	BarrierInit(&b->barrier, 0);
	b->total_bytes = layout(dims, n, n_participants, b);
	for (int d = 0; d < n; d++)
	{
		SiftStarDimSlot *s = &b->dims[d];
		SiftDimPayloadShm *p = (SiftDimPayloadShm *) ((char *) b + s->payload_off);
		const uint32 granules = N_BLOCKS(s->raw_rows);
		const int	cols = n_cols_of(dims, d);

		p->nrows = s->raw_rows;
		p->n_cols = cols;
		for (int c = 0; c < cols; c++)
			if (p->cols[c].valid_off)
				memset((char *) p + p->cols[c].valid_off, 0, sizeof(uint64) * ((s->raw_rows + 63) / 64));
		sift_morsel_init((SiftMorselQueue *) ((char *) b + s->queue_off), 1, &granules, n_participants);
		if (has_sip(s->join_kind))
			sift_sip_init_at((char *) b + s->sip_off, SVEC_I64, sip_blocks(dims, d));
	}
	return b;
}

static uint32
barrier_event(void)
{
	static uint32 ev = 0;

	if (ev == 0)
		ev = WaitEventExtensionNew("SiftStarBarrier");
	return ev;
}

static int64 *
pay_col(SiftDimPayloadShm *p, int c)
{
	return (int64 *) ((char *) p + p->cols[c].data_off);
}

static uint64 *
pay_valid(SiftDimPayloadShm *p, int c)
{
	return p->cols[c].valid_off ? (uint64 *) ((char *) p + p->cols[c].valid_off) : NULL;
}

/* The row of the parent that a key finds, and whether it finds one: the filter of the parent said it may, and its table says it does */
static bool
find_in_parent(SiftStarDimBundle *b, int parent, int64 key, bool maybe, uint32 *row)
{
	SiftDimShm *tab = sift_star_table(b, parent);
	SiftDimIter it;

	if (!maybe)
		return false;
	if (tab == NULL)
		return true;
	sift_dim_shm_find(tab, key, &it);
	return sift_dim_shm_next(&it, row);
}

/* The columns of the parent's row into the row of the dimension */
static void
flatten_row(SiftStarDimBundle *b, int d, int parent, uint32 row, uint32 prow)
{
	SiftDimPayloadShm *p = (SiftDimPayloadShm *) ((char *) b + b->dims[d].payload_off);
	const int	base = b->dims[d].n_own_cols;
	const int	pcols = sift_star_payload(b, parent)->n_cols;

	for (int c = 0; c < pcols; c++)
	{
		int64		v;
		const bool	valid = sift_star_pay(b, parent, c, prow, &v);
		uint64	   *bits = pay_valid(p, base + c);

		pay_col(p, base + c)[row] = v;
		if (bits && valid)
			bits[row >> 6] |= UINT64CONST(1) << (row & 63);
	}
}

typedef struct PassBufs
{
	int64	   *key;
	int64	   *pkey;
	int64	   *pay[SIFT_STAR_MAX_PAY_COLS];
	SiftVec		pvec;
} PassBufs;

static PassBufs *
pass_bufs(const SiftStarDimSlot *s)
{
	PassBufs   *pb = palloc0(sizeof(PassBufs));

	pb->key = palloc(sizeof(int64) * BLOCK_ROWS);
	pb->pkey = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	for (int c = 0; c < s->n_own_cols; c++)
		pb->pay[c] = palloc(sizeof(int64) * BLOCK_ROWS);
	pb->pvec = (SiftVec) {.type = SVEC_I64,.dense = true,.all_valid = true,.data = pb->pkey};
	return pb;
}

static void
pass_free(const SiftStarDimSlot *s, PassBufs *pb)
{
	for (int c = 0; c < s->n_own_cols; c++)
		pfree(pb->pay[c]);
	pfree(pb->pkey);
	pfree(pb->key);
	pfree(pb);
}

/* The rows of a block: kept in the arrays of the dimension, and the flag of each */
static void
pass_block(SiftStarDimBundle *b, int d, const SiftStarSource *src, PassBufs *pb, uint32 first)
{
	SiftStarDimSlot *s = &b->dims[d];
	SiftDimPayloadShm *p = (SiftDimPayloadShm *) ((char *) b + s->payload_off);
	int64	   *keys = (int64 *) ((char *) b + s->keys_off);
	uint8	   *pass = (uint8 *) b + s->pass_off;
	const uint32 n = Min(BLOCK_ROWS, s->raw_rows - first);
	const int	parent = s->parent_dim_id;
	const bool	flat = parent >= 0 && b->dims[parent].flattened_into_child;
	uint64		maybe[SIFT_VEC_WORDS] = {0};

	src->rows(src->arg, first, n, pb->key, parent >= 0 ? pb->pkey : NULL, pb->pay);
	memcpy(keys + first, pb->key, sizeof(int64) * n);
	for (int c = 0; c < s->n_own_cols; c++)
		memcpy(pay_col(p, c) + first, pb->pay[c], sizeof(int64) * n);
	if (parent < 0)
	{
		memset(pass + first, 1, n);
		return;
	}
	for (uint32 i = 0; i < n; i++)
		maybe[i >> 6] |= UINT64CONST(1) << (i & 63);
	if (has_sip(b->dims[parent].join_kind))
	{
		pb->pvec.len = n;
		sift_sip_probe_vec(sift_star_sip(b, parent), &pb->pvec, maybe, NULL);
	}
	for (uint32 i = 0; i < n; i++)
	{
		const SiftDimJoinKind pk = b->dims[parent].join_kind;
		uint32		prow = 0;
		const bool	match = find_in_parent(b, parent, pb->pkey[i], (maybe[i >> 6] >> (i & 63)) & 1, &prow);

		pass[first + i] = pk == SDIM_LEFT ? 1 : pk == SDIM_ANTI ? !match : match;
		if (flat && match)
			flatten_row(b, d, parent, first + i, prow);
	}
}

static void
pass_phase(SiftStarDimBundle *b, int d, const SiftStarSource *src, uint32 slot)
{
	SiftMorselQueue *q = (SiftMorselQueue *) ((char *) b + b->dims[d].queue_off);
	PassBufs   *pb = pass_bufs(&b->dims[d]);
	SiftMorsel	m;

	while (sift_morsel_claim(q, slot, &m))
		for (uint32 g = m.first; g < m.first + m.n; g++)
			pass_block(b, d, src, pb, g * BLOCK_ROWS);
	pass_free(&b->dims[d], pb);
}

/* The rows that stayed one after another, in the arrays they are in: the keys and each column with its validity bits */
static uint32
compact_rows(SiftStarDimBundle *b, int d)
{
	SiftStarDimSlot *s = &b->dims[d];
	SiftDimPayloadShm *p = (SiftDimPayloadShm *) ((char *) b + s->payload_off);
	int64	   *keys = (int64 *) ((char *) b + s->keys_off);
	const uint8 *pass = (const uint8 *) b + s->pass_off;
	uint32		kept = 0;

	for (uint32 r = 0; r < s->raw_rows; r++)
	{
		if (!pass[r])
			continue;
		keys[kept] = keys[r];
		for (int c = 0; c < p->n_cols; c++)
		{
			uint64	   *bits = pay_valid(p, c);

			pay_col(p, c)[kept] = pay_col(p, c)[r];
			if (bits)
			{
				const uint64 v = (bits[r >> 6] >> (r & 63)) & 1;

				bits[kept >> 6] = (bits[kept >> 6] & ~(UINT64CONST(1) << (kept & 63))) | (v << (kept & 63));
			}
		}
		kept++;
	}
	return kept;
}

/* One participant, between the pass and the build: the rows that stayed, and what the dimension is built as */
static void
compact_phase(SiftStarDimBundle *b, int d, uint32 n_participants)
{
	SiftStarDimSlot *s = &b->dims[d];
	SiftDimPayloadShm *p = (SiftDimPayloadShm *) ((char *) b + s->payload_off);
	SiftSipFilter *sip = has_sip(s->join_kind) ? sift_star_sip(b, d) : NULL;
	const int64 *keys = (const int64 *) ((char *) b + s->keys_off);
	const uint32 kept = compact_rows(b, d);

	s->surviving_rows = kept;
	p->nrows = kept;
	if (kept == 0 && sip)
	{
		sift_sip_finalize(sip);
		pg_atomic_write_u32(&b->any_empty_inner, 1);
	}
	else if (s->join_kind == SDIM_SEMI && p->n_cols == 0 && !s->flattened_into_child && kept <= SIFT_SIP_EXACT_MAX)
	{
		for (uint32 i = 0; i < kept; i++)
			sift_sip_insert_i64(sip, keys[i]);
		sift_sip_finalize(sip);
		s->elided = true;
	}
	else
		sift_dim_shm_init((char *) b + s->dim_shm_off, kept, n_participants);
}

static void
build_keys(void *arg, uint32 first, uint32 n, int64 *out)
{
	memcpy(out, (const int64 *) arg + first, sizeof(int64) * n);
}

static void
build_phase(SiftStarDimBundle *b, int d)
{
	SiftStarDimSlot *s = &b->dims[d];

	if (s->elided || (has_sip(s->join_kind) && s->surviving_rows == 0))
		return;
	sift_dim_shm_build_with_sip(sift_star_table(b, d), build_keys, (char *) b + s->keys_off, has_sip(s->join_kind) ? sift_star_sip(b, d) : NULL);
}

void
sift_star_build(SiftStarDimBundle *b, const SiftStarSource *src)
{
	const uint32 total = 3 * b->n_dims;
	const uint32 slot = pg_atomic_fetch_add_u32(&b->next_slot, 1);

	Assert(b->magic == SIFT_STAR_MAGIC);
	for (uint32 ph = BarrierAttach(&b->barrier); ph < total; ph++)
	{
		const int	d = ph / 3;

		switch (ph % 3)
		{
			case 0:
				if (!sift_star_empty(b))
					pass_phase(b, d, &src[d], slot);
				if (BarrierArriveAndWait(&b->barrier, barrier_event()) && !sift_star_empty(b))
					compact_phase(b, d, b->n_participants);
				break;
			case 1:
				BarrierArriveAndWait(&b->barrier, barrier_event());
				break;
			default:
				if (!sift_star_empty(b))
					build_phase(b, d);
				BarrierArriveAndWait(&b->barrier, barrier_event());
				break;
		}
	}
	BarrierDetach(&b->barrier);
}
