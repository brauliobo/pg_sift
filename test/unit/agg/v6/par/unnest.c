/*
 * agg/par/unnest.c - agg/par/unnest.c on 1 to 4 threads (the trap shim flags the helpers: no palloc, no ereport): the pairs of (row, element) of arrays held as slots of 1024 rows, against the pairs a plain
 * loop makes. Integer and text elements (short ones in their views, long ones in the slot), NULL arrays, empty arrays and NULL elements, inner and left joins, rows the leader left out of the mask, integer
 * and text keys of the row beside the element; the two columns of a jsonb_to_recordset element (one of them left out of the slot when the statement does not read it) and the elements the quals of the
 * element keep; and what the threads must leave to the leader: a NULL in a summed column, more pairs than the sink holds. The slot the leader folds itself gives the same groups.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"

#include "agg/par/unnest.h"
#include "agg/vexpr/str.h"
#include "core/hash.h"
#include "core/par/pool.h"
#include "host.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define NSLOT		16
#define PARENTS		(NSLOT * SIFT_VEC_LEN)
#define MAX_LEN		6
#define SCRATCH		(1 << 16)
#define LONG_FMT	"an-element-longer-than-twelve-%d"

static int	nchecks;

void
sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	SIFT_CHECK(false);
}

#define PASS(cond) do { nchecks++; SIFT_CHECK(cond); } while (0)

/* ---- the rows ---- */

typedef struct Data
{
	bool		text_elem;
	bool		null_qty;
	bool		masked;			/* the quals of the element keep the elements whose integer is not a multiple of 3 */
	int64		grp[PARENTS];
	bool		grp_null[PARENTS];
	int64		qty[PARENTS];
	bool		qty_null[PARENTS];
	char		label[PARENTS][32];
	bool		label_null[PARENTS];
	bool		doc_null[PARENTS];
	bool		out[PARENTS];		/* left out of the mask */
	uint32		len[PARENTS];
	uint32		first[PARENTS + 1];
	int64		ival[PARENTS * MAX_LEN];
	char		sval[PARENTS * MAX_LEN][40];
	bool		enull[PARENTS * MAX_LEN];
} Data;

static Data *
make_data(bool text_elem, bool null_qty, bool masked)
{
	Data	   *d = calloc(1, sizeof(Data));

	d->text_elem = text_elem;
	d->null_qty = null_qty;
	d->masked = masked;
	for (uint32 p = 0; p < PARENTS; p++)
	{
		const uint64 h = sift_fmix64(p * 2654435761u + 7);

		d->grp[p] = (int64) (h % 9) - 3;
		d->grp_null[p] = (h >> 40) % 29 == 0;
		d->qty[p] = (int64) ((h >> 8) % 100);
		d->qty_null[p] = null_qty && (h >> 46) % 701 == 0;
		d->label_null[p] = (h >> 16) % 13 == 0;
		snprintf(d->label[p], sizeof(d->label[p]), (h >> 20) % 3 == 0 ? "label-of-more-than-twelve-%d" : "l%d", (int) (h % 17));
		d->doc_null[p] = (h >> 24) % 11 == 0;
		d->len[p] = d->doc_null[p] ? 0 : (uint32) ((h >> 28) % (MAX_LEN + 1));
		d->out[p] = (h >> 32) % 97 == 0;
		d->first[p + 1] = d->first[p] + d->len[p];
		for (uint32 e = d->first[p]; e < d->first[p + 1]; e++)
		{
			const uint64 g = sift_fmix64(e * 40503u + 11);

			d->enull[e] = g % 13 == 0;
			d->ival[e] = (int64) (g >> 8) % 50 - 10;
			snprintf(d->sval[e], sizeof(d->sval[e]), (g >> 20) % 3 == 0 ? LONG_FMT : "e%d", (int) ((g >> 8) % 40));
		}
	}
	return d;
}

static void *
vec_mem(size_t bytes)
{
	return aligned_alloc(64, ((bytes + SIFT_VEC_PAD + 63) / 64) * 64);
}

/* ---- the statements: keys and aggregates over (grp, qty, label, element) ---- */

typedef enum Shape
{
	KEY_GRP_ELEM,				/* grp and the element; count(*), sum(qty) */
	KEY_ELEM,					/* the element; count(*), sum(qty) */
	KEY_LABEL,					/* the text of the row; count(*), sum(qty) */
	KEY_LABEL_ELEM,				/* the text of the row and the element; count(*), sum(qty) */
	KEY_RECORD					/* the text and the integer column of a recordset element; count(*), sum(qty) */
} Shape;

typedef struct Stmt
{
	SiftV6Spec	spec;
	SiftV6Ext	ext;
	SiftV6Pipe *pipe;
	SiftParWordShape sh;
	Shape		shape;
	bool		left;
	bool		text_elem;
	int			nel;			/* the columns of the element: 2 for a recordset (the integer, then the text) */
	uint32		eused;
	SiftVecType type[5];
} Stmt;

static bool
passes(const Data *d, uint32 e)
{
	return !d->masked || d->ival[e] % 3 != 0;
}

/* The slot of the rows [s * 1024, (s + 1) * 1024), the strings of its views in memory of its own; the columns of the element the statement reads (eused) are in it */
static SiftUnSlot *
make_slot(const Data *d, uint32 s, const Stmt *st)
{
	SiftUnSlot *sl = vec_mem(sizeof(SiftUnSlot));
	const SiftVecType *type = st->type;
	const uint32 at = s * SIFT_VEC_LEN,
				base = d->first[at],
				nelems = d->first[at + SIFT_VEC_LEN] - base;
	char	   *strs = calloc(1, (size_t) (PARENTS + 2 * nelems) * 40 + 64);
	char	   *sat = strs;

	memset(sl, 0, sizeof(*sl));
	sl->str = strs;
	sl->n = SIFT_VEC_LEN;
	sl->nelems = nelems;
	sl->ncols = 3;
	sl->nel = st->nel;
	for (int i = 0; i < 3; i++)
	{
		sl->data[i] = vec_mem(SIFT_VEC_LEN * 16);
		sl->valid[i] = calloc(SIFT_VEC_WORDS + 1, sizeof(uint64));
	}
	for (int c = 0; c < st->nel; c++)
	{
		if (!((st->eused >> c) & 1))
		{
			sl->elem[c] = (SiftVec) {.type = type[3 + c]};
			continue;
		}
		sl->edata[c] = vec_mem(nelems * 16 + 16);
		sl->evalid[c] = calloc(nelems / 64 + 2, sizeof(uint64));
	}
	sl->ebits = calloc(nelems / 64 + 2, sizeof(uint64));
	for (uint32 i = 0; i < SIFT_VEC_LEN; i++)
	{
		const uint32 p = at + i;
		const bool	label_ok = !d->label_null[p];

		((int64 *) sl->data[0])[i] = d->grp[p];
		((int64 *) sl->data[1])[i] = d->qty[p];
		if (label_ok)
		{
			const size_t len = strlen(d->label[p]);

			memcpy(sat, d->label[p], len);
			((SiftStrView *) sl->data[2])[i] = sift_vstr_make(sat, len);
			sat += len;
		}
		else
			((SiftStrView *) sl->data[2])[i] = (SiftStrView) {0};
		sl->valid[0][i >> 6] |= (uint64) !d->grp_null[p] << (i & 63);
		sl->valid[1][i >> 6] |= (uint64) !d->qty_null[p] << (i & 63);
		sl->valid[2][i >> 6] |= (uint64) label_ok << (i & 63);
		sl->mask[i >> 6] |= (uint64) !d->out[p] << (i & 63);
		sl->doc_valid[i >> 6] |= (uint64) !d->doc_null[p] << (i & 63);
		sl->lengths[i] = d->len[p];
		sl->offsets[i] = d->first[p] - base;
	}
	sl->offsets[SIFT_VEC_LEN] = nelems;
	for (uint32 k = 0; k <= SIFT_VEC_LEN / 64; k++)
		sl->block_off[k] = d->first[at + k * 64] - base;
	for (uint32 e = 0; e < nelems; e++)
	{
		const uint32 g = base + e;

		for (int c = 0; c < st->nel; c++)
		{
			if (!((st->eused >> c) & 1))
				continue;
			sl->evalid[c][e >> 6] |= (uint64) !d->enull[g] << (e & 63);
			if (type[3 + c] == SVEC_STRVIEW)
			{
				const size_t len = strlen(d->sval[g]);

				memcpy(sat, d->sval[g], len);
				((SiftStrView *) sl->edata[c])[e] = sift_vstr_make(sat, len);
				sat += len;
			}
			else
				((int64 *) sl->edata[c])[e] = d->ival[g];
		}
		sl->ebits[e >> 6] |= (uint64) passes(d, g) << (e & 63);
	}
	sl->emask = d->masked ? sl->ebits : NULL;
	for (int i = 0; i < 3; i++)
		sl->col[i] = (SiftVec) {.data = sl->data[i],.valid = sl->valid[i],.len = SIFT_VEC_LEN,.dense = true,.type = type[i]};
	for (int c = 0; c < st->nel; c++)
		if ((st->eused >> c) & 1)
			sl->elem[c] = (SiftVec) {.data = sl->edata[c],.valid = sl->evalid[c],.dense = true,.type = type[3 + c]};
	return sl;
}

static void
free_slot(SiftUnSlot *sl)
{
	for (int i = 0; i < 3; i++)
	{
		free(sl->data[i]);
		free(sl->valid[i]);
	}
	for (int c = 0; c < sl->nel; c++)
	{
		free(sl->edata[c]);
		free(sl->evalid[c]);
	}
	free(sl->ebits);
	free(sl->str);
	free(sl);
}

static Stmt *
make_stmt(Shape shape, bool text_elem, bool left, bool record)
{
	Stmt	   *st = calloc(1, sizeof(Stmt));
	SiftV6Spec *sp = &st->spec;
	const Oid	etype = text_elem ? TEXTOID : INT4OID;
	const int	ecol = record && text_elem ? 1 : 0;
	const SiftV6Col grp = {.attno = 1,.typid = INT4OID},
				qty = {.attno = 2,.typid = INT4OID},
				label = {.attno = 3,.typid = TEXTOID},
				elem = {.src = SV6SRC_ELEM,.slot = ecol,.typid = etype},
				ival = {.src = SV6SRC_ELEM,.slot = 0,.typid = INT4OID},
				sval = {.src = SV6SRC_ELEM,.slot = 1,.typid = TEXTOID};

	st->shape = shape;
	st->left = left;
	st->text_elem = text_elem;
	st->nel = record ? 2 : 1;
	st->eused = shape == KEY_RECORD ? 3 : 1u << ecol;
	st->type[0] = st->type[1] = SVEC_I64;
	st->type[2] = SVEC_STRVIEW;
	st->type[3] = record ? SVEC_I64 : text_elem ? SVEC_STRVIEW : SVEC_I64;
	st->type[4] = SVEC_STRVIEW;
	sp->ext = &st->ext;
	st->ext.unnest = true;
	st->ext.elem = (SiftV6XElem) {.attno = 4,.type = etype,.left = left};
	sp->key = calloc(2, sizeof(SiftV6Col));
	sp->agg = calloc(2, sizeof(SiftV6Agg));
	sp->nkeys = shape == KEY_GRP_ELEM || shape == KEY_LABEL_ELEM || shape == KEY_RECORD ? 2 : 1;
	sp->key[0] = shape == KEY_GRP_ELEM ? grp : shape == KEY_ELEM ? elem : shape == KEY_RECORD ? sval : label;
	sp->key[1] = shape == KEY_RECORD ? ival : elem;
	sp->naggs = 2;
	sp->agg[0] = (SiftV6Agg) {.kind = SV6_COUNT_STAR};
	sp->agg[1] = (SiftV6Agg) {.kind = SV6_SUM,.arg = qty};
	SIFT_CHECK(sift_par_unnest_shape(sp, &st->sh));
	st->pipe = sift_v6_pipe_new(CurrentMemoryContext, sp, NULL, NULL, 3 + st->nel, st->type);
	st->pipe->key_slot[0] = shape == KEY_GRP_ELEM ? 0 : shape == KEY_ELEM ? 3 + ecol : shape == KEY_RECORD ? 4 : 2;
	st->pipe->key_slot[1] = shape == KEY_RECORD ? 3 : 3 + ecol;
	for (int v = 0; v < sift_v6_spec_nvec(sp); v++)
		st->pipe->arg_slot[v] = sift_v6_vec_col(sp, v) == NULL ? -1 : sift_v6_vec_col(sp, v)->src == SV6SRC_ELEM ? 3 + ecol : 1;
	return st;
}

static void
free_stmt(Stmt *st)
{
	free(st->spec.key);
	free(st->spec.agg);
	free(st);
}

/* ---- the groups ---- */

typedef struct Rec
{
	uint64		key[2];
	uint8		str[2][40];
	uint32		len[2];
	uint64		count;
	uint64		sum;
	uint64		nn;
} Rec;

typedef struct Vec
{
	Rec		   *r;
	uint32		n;
	uint32		cap;
} Vec;

static void
vec_add(Vec *v, const Rec *r)
{
	if (v->n == v->cap)
		v->r = realloc(v->r, sizeof(Rec) * (v->cap = v->cap ? v->cap * 2 : 1024));
	v->r[v->n++] = *r;
}

static int
cmp_rec(const void *a, const void *b)
{
	const Rec  *x = a,
			   *y = b;

	for (int j = 0; j < 2; j++)
		if (x->key[j] != y->key[j])
			return x->key[j] < y->key[j] ? -1 : 1;
	for (int j = 0; j < 2; j++)
	{
		if (x->len[j] != y->len[j])
			return x->len[j] < y->len[j] ? -1 : 1;
		if (x->len[j] != SIFT_STR_NULL && memcmp(x->str[j], y->str[j], x->len[j]) != 0)
			return memcmp(x->str[j], y->str[j], x->len[j]);
	}
	return 0;
}

static Vec
merged(Vec *in, int n)
{
	Vec			all = {0},
				m = {0};

	for (int t = 0; t < n; t++)
		for (uint32 i = 0; i < in[t].n; i++)
			vec_add(&all, &in[t].r[i]);
	qsort(all.r, all.n, sizeof(Rec), cmp_rec);
	for (uint32 i = 0; i < all.n; i++)
	{
		if (m.n > 0 && cmp_rec(&m.r[m.n - 1], &all.r[i]) == 0)
		{
			m.r[m.n - 1].count += all.r[i].count;
			m.r[m.n - 1].sum += all.r[i].sum;
			m.r[m.n - 1].nn += all.r[i].nn;
		}
		else
			vec_add(&m, &all.r[i]);
	}
	free(all.r);
	return m;
}

#define NULLW	UINT64_MAX
#define SIGN	(UINT64CONST(1) << 63)

static void
set_text(Rec *r, int j, const char *s, bool null)
{
	r->len[j] = null ? SIFT_STR_NULL : (uint32) strlen(s);
	if (!null)
		memcpy(r->str[j], s, r->len[j]);
}

/* The groups of the pairs, from the rows: every row of the mask, each of its elements the quals keep, a NULL element for the row of a left join with none */
static void
add_pair(Vec *v, const Data *d, const Stmt *st, uint32 p, uint32 e, bool none)
{
	const bool	enull = none || d->enull[e];
	Rec			r = {.count = 1,.sum = d->qty_null[p] ? 0 : (uint64) d->qty[p],.nn = !d->qty_null[p]};

	if (st->shape == KEY_GRP_ELEM)
	{
		r.key[0] = d->grp_null[p] ? NULLW : (uint64) d->grp[p] ^ SIGN;
		r.key[1] = enull ? NULLW : (uint64) d->ival[e] ^ SIGN;
	}
	else if (st->shape == KEY_RECORD)
	{
		set_text(&r, 0, enull ? "" : d->sval[e], enull);
		r.key[1] = enull ? NULLW : (uint64) d->ival[e] ^ SIGN;
	}
	else if (st->shape == KEY_ELEM && st->text_elem)
		set_text(&r, 0, enull ? "" : d->sval[e], enull);
	else if (st->shape == KEY_ELEM)
		r.key[0] = enull ? NULLW : (uint64) d->ival[e] ^ SIGN;
	else if (st->shape == KEY_LABEL_ELEM)
	{
		set_text(&r, 0, d->label[p], d->label_null[p]);
		if (st->text_elem)
			set_text(&r, 1, enull ? "" : d->sval[e], enull);
		else
			r.key[1] = enull ? NULLW : (uint64) d->ival[e] ^ SIGN;
	}
	else
		set_text(&r, 0, d->label[p], d->label_null[p]);
	vec_add(v, &r);
}

static Vec
reference(const Data *d, const Stmt *st)
{
	Vec			v = {0},
				out;

	for (uint32 p = 0; p < PARENTS; p++)
	{
		uint32		kept = 0;

		if (d->out[p])
			continue;
		for (uint32 e = d->first[p]; e < d->first[p + 1]; e++)
			if (passes(d, e))
			{
				add_pair(&v, d, st, p, e, false);
				kept++;
			}
		if (kept == 0 && st->left)
			add_pair(&v, d, st, p, d->first[p], true);
	}
	out = merged(&v, 1);
	free(v.r);
	return out;
}

static bool
same(const Vec *a, const Vec *b)
{
	if (a->n != b->n)
		return false;
	for (uint32 i = 0; i < a->n; i++)
		if (cmp_rec(&a->r[i], &b->r[i]) != 0 || a->r[i].count != b->r[i].count || a->r[i].sum != b->r[i].sum || a->r[i].nn != b->r[i].nn)
			return false;
	return true;
}

/* ---- the run ---- */

typedef struct Ctx
{
	SiftParUnnest *un;
	SiftUnSlot *slot[NSLOT];
	Vec			out[SIFT_PAR_MAX_THREADS];
	const SiftParWordShape *sh;
} Ctx;

static bool
fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	Ctx		   *c = arg;

	return sift_par_unnest_batch(c->un, worker, c->slot[lo], err, out);
}

static void
emit(void *arg, int worker, const SiftParGroupRow *row)
{
	Ctx		   *c = arg;
	Rec			r = {.count = row->count,.sum = row->sum[c->sh->v_slot[1]],.nn = row->sum[c->sh->n_slot[1]]};

	for (int j = 0; j < c->sh->gs.nk; j++)
		if (sift_par_group_is_str(&c->sh->gs, j))
		{
			r.len[j] = row->str[j].len;
			if (r.len[j] != SIFT_STR_NULL)
				memcpy(r.str[j], row->str[j].p, r.len[j]);
		}
		else
			r.key[j] = row->key[j];
	vec_add(&c->out[worker], &r);
}

static void
overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	SIFT_CHECK(false);
}

static bool
feed(void *arg, uint32 item)
{
	return item < NSLOT;
}

static void
release(void *arg, uint32 item)
{
}

static void
fallback(void *arg, uint32 item)
{
	SIFT_CHECK(false);
}

static Vec
run(Ctx *c, const Stmt *st, uint32 workers)
{
	SiftParGroupHooks hooks = {.fill = fill,.emit = emit,.overflow = overflow,.arg = c};
	SiftParArenas *ar;
	SiftParGroup *g;
	SiftErr		err = {0};
	SiftParOps	ops = sift_par_group_ops;
	Vec			v;

	c->sh = &st->sh;
	c->un = sift_par_unnest_new(CurrentMemoryContext, &st->sh, st->pipe, st->left, workers, 3, st->nel, st->eused);
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, 8 << 20, 8 << 20);
	g = sift_par_group_create(ar, &st->sh.gs, false, UINT64_MAX, &hooks, &err);
	SIFT_CHECK(g != NULL);
	ops.feed_slot = feed;
	ops.release_slot = release;
	ops.fallback_item = fallback;
	SIFT_CHECK(sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = workers}, &ops, g, ar, NULL));
	PASS(!sift_par_pool_busy());
	v = merged(c->out, SIFT_PAR_MAX_THREADS);
	for (int t = 0; t < SIFT_PAR_MAX_THREADS; t++)
	{
		free(c->out[t].r);
		c->out[t] = (Vec) {0};
	}
	return v;
}

/* The slots through the pipe of the leader (what a slot a thread left is folded with): the words of every slot, as a group table of the leader would add them */
static Vec
replay(Ctx *c, const Stmt *st)
{
	SiftParWords *words = sift_par_words_new(CurrentMemoryContext, &st->sh, 1u << 20);
	SiftV6Pipe *own = sift_v6_pipe_clone(CurrentMemoryContext, st->pipe, sift_par_words_sink, words);
	SiftUnPairs *pairs = sift_un_pairs_new(CurrentMemoryContext, own, 3, st->nel, st->eused);
	Vec			all = {0},
				v;

	for (uint32 s = 0; s < NSLOT; s++)
	{
		SiftParGroupBatch b = {0};

		sift_par_words_reset(words);
		sift_un_slot_pairs(pairs, c->slot[s], st->left, NULL);
		sift_par_words_out(words, &b);
		for (uint32 i = 0; i < b.n; i++)
		{
			Rec			r = {.count = 1,.sum = b.arg[st->sh.v_slot[1]][i],.nn = b.arg[st->sh.n_slot[1]][i]};

			for (int j = 0; j < st->sh.gs.nk; j++)
				if (sift_par_group_is_str(&st->sh.gs, j))
				{
					r.len[j] = b.str[j][i].len;
					if (r.len[j] != SIFT_STR_NULL)
						memcpy(r.str[j], b.str[j][i].p, r.len[j]);
				}
				else
					r.key[j] = b.key[j][i];
			vec_add(&all, &r);
		}
	}
	v = merged(&all, 1);
	free(all.r);
	return v;
}

static void
make_slots(Ctx *c, const Data *d, const Stmt *st)
{
	for (uint32 s = 0; s < NSLOT; s++)
		c->slot[s] = make_slot(d, s, st);
}

static void
free_slots(Ctx *c)
{
	for (uint32 s = 0; s < NSLOT; s++)
		free_slot(c->slot[s]);
}

static void
check_parity(Shape shape, bool text_elem, bool left, bool null_qty, bool record, bool masked, const char *what)
{
	Data	   *d = make_data(text_elem, null_qty, masked);
	Stmt	   *st = make_stmt(shape, text_elem, left, record);
	Ctx			c = {0};
	Vec			want = reference(d, st),
				again;

	make_slots(&c, d, st);
	PASS(want.n > 0);
	for (uint32 w = 1; w <= 4; w++)
	{
		Vec			got = run(&c, st, w);

		if (!same(&got, &want))
			fprintf(stderr, "%s: %u threads: %u groups, want %u\n", what, w, got.n, want.n);
		PASS(same(&got, &want));
		free(got.r);
	}
	again = replay(&c, st);
	PASS(same(&again, &want));
	free(again.r);
	free(want.r);
	free_slots(&c);
	free(d);
	free_stmt(st);
	pgshim_reset();
}

/* What a thread leaves to the leader: a NULL in a summed column, more pairs than the sink holds */
static void
check_scan(bool null_qty, uint32 per_row, int expect_code)
{
	Data	   *d = make_data(false, null_qty, false);
	Stmt	   *st = make_stmt(KEY_ELEM, false, false, false);
	Ctx			c = {0};
	SiftParUnnest *un;
	SiftParGroupBatch out = {0};
	SiftErr		err = {0};
	bool		ok;

	make_slots(&c, d, st);
	if (per_row > 0)
	{
		SiftUnSlot *sl = c.slot[0];

		sl->nelems = per_row * SIFT_VEC_LEN;
		free(sl->edata[0]);
		sl->edata[0] = vec_mem(sl->nelems * 8 + 16);
		sl->evalid[0] = realloc(sl->evalid[0], (sl->nelems / 64 + 2) * sizeof(uint64));
		memset(sl->evalid[0], 0xFF, (sl->nelems / 64 + 2) * sizeof(uint64));
		memset(sl->edata[0], 0, sl->nelems * 8);
		sl->elem[0].data = sl->edata[0];
		sl->elem[0].valid = sl->evalid[0];
		memset(sl->mask, 0xFF, sizeof(sl->mask));
		memset(sl->doc_valid, 0xFF, sizeof(sl->doc_valid));
		for (uint32 i = 0; i <= SIFT_VEC_LEN; i++)
			sl->offsets[i] = i * per_row;
		for (uint32 i = 0; i < SIFT_VEC_LEN; i++)
			sl->lengths[i] = per_row;
		for (uint32 k = 0; k <= SIFT_VEC_LEN / 64; k++)
			sl->block_off[k] = k * 64 * per_row;
	}
	un = sift_par_unnest_new(CurrentMemoryContext, &st->sh, st->pipe, false, 1, 3, st->nel, st->eused);
	ok = sift_par_unnest_batch(un, 0, c.slot[0], &err, &out);
	PASS(ok == (expect_code == 0));
	PASS(ok || err.code == expect_code);
	free_slots(&c);
	free(d);
	free_stmt(st);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	check_parity(KEY_GRP_ELEM, false, false, false, false, false, "inner, int element, grp key");
	check_parity(KEY_GRP_ELEM, false, true, false, false, false, "left, int element, grp key");
	check_parity(KEY_ELEM, true, false, false, false, false, "inner, text element");
	check_parity(KEY_ELEM, true, true, false, false, false, "left, text element");
	check_parity(KEY_ELEM, false, true, false, false, false, "left, int element");
	check_parity(KEY_LABEL, true, false, false, false, false, "inner, text key of the row");
	check_parity(KEY_LABEL, false, true, false, false, false, "left, text key of the row");
	check_parity(KEY_LABEL_ELEM, false, false, false, false, false, "inner, text key of the row and int element");
	check_parity(KEY_LABEL_ELEM, true, true, false, false, false, "left, text key of the row and text element");
	check_parity(KEY_GRP_ELEM, false, true, true, false, false, "left, NULL quantities, grp key");
	check_parity(KEY_ELEM, true, false, true, false, false, "inner, NULL quantities, text element");
	check_parity(KEY_LABEL_ELEM, true, true, true, false, false, "left, NULL quantities, text key and text element");
	check_parity(KEY_RECORD, false, false, false, true, false, "inner, recordset, both columns");
	check_parity(KEY_RECORD, true, true, false, true, false, "left, recordset, both columns");
	check_parity(KEY_ELEM, false, false, false, true, false, "inner, recordset, the integer column only");
	check_parity(KEY_ELEM, true, true, false, true, false, "left, recordset, the text column only");
	check_parity(KEY_GRP_ELEM, false, false, false, true, true, "inner, recordset, the quals keep some elements");
	check_parity(KEY_RECORD, true, true, true, true, true, "left, NULL quantities, recordset, the quals keep some elements");
	check_parity(KEY_LABEL_ELEM, true, true, false, false, true, "left, array, the quals keep some elements");
	check_scan(false, 0, 0);
	check_scan(true, 0, 0);
	check_scan(false, 100, SIFT_ERR_ARENA_FULL);
	printf("par_unnest: ok (%d checks)\n", nchecks);
	return 0;
}
