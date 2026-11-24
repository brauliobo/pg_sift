/*
 * agg/v6/par/distinct.c - the radix COUNT(DISTINCT) of agg/v6/par/distinct.h: phase 1, the seal and the answer (phase 2 is distinct_p2.c). Nothing here calls the server.
 */
#include "postgres.h"

#include "agg/v6/par/distinct_int.h"
#include "agg/v6/v6_distinct_u64.h"
#include "core/popcount.h"

static inline uint32
rec_bytes(const SiftParDistinct *d)
{
	return d->grouped ? sizeof(DistRec) : sizeof(uint64);
}

static bool
worker_init(SiftParDistinct *d, DistWorker *w, SiftArena *state)
{
	sift_radix_init(&w->raw, state);
	sift_radix_init(&w->agg, state);
	if (d->direct ? !sift_keytab_init_direct(&w->keys, state, d->ks, d->sp.direct_lo, d->sp.direct_n) : !sift_keytab_init(&w->keys, state, d->ks, false))
		return false;
	if (d->sp.vstr && !sift_keytab_init(&w->vals, state, d->vs, false))
		return false;
	if (!d->grouped)
	{
		const SiftTabKey nokey = {.h = sift_key_hash(&d->ks, NULL, NULL, 0)};

		return sift_keytab_intern(&w->keys, &nokey) == 0;
	}
	return true;
}

SiftParDistinct *
sift_par_distinct_create(SiftParArenas *ar, const SiftParDistinctSpec *spec, const SiftParDistinctHooks *hooks, SiftErr *err)
{
	SiftArena  *boot = &ar->w[0].state;
	SiftParDistinct *d = sift_arena_alloc0(boot, sizeof(SiftParDistinct), SIFT_ARENA_ALIGN);

	if (d != NULL)
		d->w = sift_arena_alloc0(boot, sizeof(DistWorker) * ar->n, SIFT_ARENA_ALIGN);
	if (d == NULL || d->w == NULL)
		return dist_fail(err, "parallel distinct: no room for the state"), NULL;
	d->sp = *spec;
	d->hooks = *hooks;
	d->ar = ar;
	d->nw = ar->n;
	d->grouped = spec->kw > 0 || spec->kstr;
	d->direct = spec->direct_n > 0;
	d->ks = (SiftKeyShape) {.kw = spec->kw,.str = spec->kstr,.npay = dist_pay_last(d) + 1};
	d->vs = (SiftKeyShape) {.str = true};
	for (int t = 0; t < d->nw; t++)
		if (!worker_init(d, &d->w[t], &ar->w[t].state))
			return dist_fail(err, "parallel distinct: an arena is too small for the tables"), NULL;
	return d;
}

/* ---- phase 1 ---- */

static SiftTabKey
row_key(const SiftParDistinct *d, const SiftParDistinctBatch *b, uint32 i, uint64 *words)
{
	SiftTabKey		k = {.w = words};

	for (int j = 0; j < d->sp.kw; j++)
		words[j] = b->key[j][i];
	if (d->sp.kstr)
	{
		k.p = b->kstr[i].p;
		k.len = b->kstr[i].len;
	}
	return k;
}

#define KEY_AHEAD	16

static void
hash_keys(const SiftParDistinct *d, const SiftParDistinctBatch *b, uint64 *hs)
{
	uint64		words[SIFT_PAR_GROUP_MAX_KEYS];

	for (uint32 i = 0; i < b->n; i++)
	{
		const SiftTabKey k = row_key(d, b, i, words);

		hs[i] = sift_key_hash(&d->ks, words, k.p, k.len);
	}
}

/* The number of the key of row i in the table of the thread; the hashes of the rows ahead are known (hs), and their places in the table are brought in */
static uint32
key_of(const SiftParDistinct *d, DistWorker *w, const SiftParDistinctBatch *b, const uint64 *hs, uint32 i)
{
	uint64		words[SIFT_PAR_GROUP_MAX_KEYS];
	SiftTabKey		k;

	if (!d->grouped)
		return 0;
	k = row_key(d, b, i, words);
	if (d->direct)
		return sift_keytab_intern(&w->keys, &k);
	k.h = hs[i];
	if (i + KEY_AHEAD < b->n)
		sift_keytab_prefetch_slot(&w->keys, hs[i + KEY_AHEAD]);
	if (i + KEY_AHEAD / 2 < b->n)
		sift_keytab_prefetch_ent(&w->keys, hs[i + KEY_AHEAD / 2]);
	return sift_keytab_intern(&w->keys, &k);
}

static bool
add_row(const SiftParDistinct *d, DistWorker *w, uint32 kid, const SiftParDistinctBatch *b, uint32 i)
{
	uint64	   *pay = sift_keytab_pay(&w->keys, sift_keytab_at(&w->keys, kid));

	pay[0]++;
	for (int m = 0; m < d->sp.nsum; m++)
		pay[1 + m] += b->sum[m][i];
	for (int m = 0; m < d->sp.nmm; m++)
	{
		const SiftStrRef s = b->mm[m][i];

		if (s.len != SIFT_STR_NULL && !dist_mm_offer(pay + dist_pay_mm(d, m), (d->sp.mm_max >> m) & 1, s.p, s.len, w->keys.a))
			return false;
	}
	return true;
}

/* The value of row i into the partition of its key (with no key: of itself); a NULL is no value; false when the arena has no room */
static bool
put_value(const SiftParDistinct *d, DistWorker *w, uint32 kid, const SiftParDistinctBatch *b, uint32 i)
{
	DistRec		rec = {.kid = kid};
	uint64		h;

	if (d->sp.vstr)
	{
		const SiftStrRef s = b->vstr[i];
		SiftTabKey		k = {.p = s.p,.len = s.len};

		if (s.len == SIFT_STR_NULL)
			return true;
		k.h = h = sift_key_hash(&d->vs, NULL, s.p, s.len);
		if ((rec.v = sift_keytab_intern(&w->vals, &k)) == SIFT_KEYTAB_NONE)
			return false;
	}
	else
	{
		if (b->vnull != NULL && b->vnull[i])
			return true;
		rec.v = b->val[i];
		h = sift_du64_mix(rec.v);
	}
	if (!d->grouped)
		return sift_radix_put(&w->raw, sift_radix_part(h), &rec.v, sizeof(rec.v));
	return sift_radix_put(&w->raw, sift_radix_part(sift_keytab_at(&w->keys, kid)[0]), &rec, sizeof(rec));
}

static bool
rows_in(const SiftParDistinct *d, DistWorker *w, const SiftParDistinctBatch *b, uint64 *hs)
{
	for (uint32 i = 0; i < b->n; i++)
	{
		const uint32 kid = key_of(d, w, b, hs, i);

		if (kid == SIFT_KEYTAB_NONE)
			return false;
		if (!add_row(d, w, kid, b, i) || !put_value(d, w, kid, b, i))
			return false;
	}
	return true;
}

static bool
distinct_phase1(void *vd, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SiftParDistinct *d = vd;
	SiftParDistinctBatch b = {0};
	uint64	   *hs;

	*done = 0;
	if (!d->hooks.fill(d->hooks.arg, t, lo, hi, scratch, &b, err))
		return false;
	hs = d->grouped && !d->direct ? sift_arena_alloc(scratch, (size_t) b.n * sizeof(uint64), 8) : NULL;
	if (!sift_radix_fits(&d->w[t].raw, (uint64) b.n * rec_bytes(d)) || (d->grouped && !d->direct && hs == NULL))
		return dist_fail(err, "parallel distinct: the arena of a thread has no room for a batch");
	if (hs != NULL)
		hash_keys(d, &b, hs);
	return rows_in(d, &d->w[t], &b, hs) || dist_fail(err, "parallel distinct: the arena of a thread has no room for the keys");
}

/* ---- the seal: the rows and sums of the keys of a thread, to the partitions of the keys ---- */

static bool
spill_keys(const SiftParDistinct *d, DistWorker *w)
{
	const uint32 words = 1 + dist_pay_last(d);

	if (!sift_radix_fits(&w->agg, (uint64) w->keys.count * words * sizeof(uint64)))
		return false;
	for (uint32 id = 0; id < w->keys.count; id++)
	{
		uint64	   *e = sift_keytab_at(&w->keys, id);
		uint64	   *rec = sift_radix_reserve(&w->agg, sift_radix_part(e[0]), words * sizeof(uint64));

		if (rec == NULL)
			return false;
		rec[0] = id;
		memcpy(rec + 1, sift_keytab_pay(&w->keys, e), (words - 1) * sizeof(uint64));
	}
	return true;
}

static bool
distinct_seal(void *vd, uint32 *nparts, SiftErr *err)
{
	SiftParDistinct *d = vd;

	*nparts = SIFT_RADIX_PARTS;
	for (int t = 0; d->grouped && !d->direct && t < d->nw; t++)
		if (!spill_keys(d, &d->w[t]))
			return dist_fail(err, "parallel distinct: the arena of a thread has no room for its keys");
	return true;
}

static bool
distinct_phase2(void *vd, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	return sift_dist_phase2(vd, t, part, work) || dist_fail(err, "parallel distinct: the arena of a partition is full");
}

static void
distinct_overflow(void *vd, const SiftParRange *left, uint32 nleft)
{
	SiftParDistinct *d = vd;

	d->hooks.overflow(d->hooks.arg, left, nleft);
}

const SiftParOps sift_par_distinct_ops = {.phase1 = distinct_phase1,.seal = distinct_seal,.phase2 = distinct_phase2,.overflow_drain = distinct_overflow};

/* ---- the answer ---- */

typedef void (*EachFn) (void *arg, const SiftParDistinctRow *row);

/* The payload of an entry of a key into the row of the answer, the texts in the states mm (3 words each) */
static void
row_add(const SiftParDistinct *d, SiftParDistinctRow *row, uint64 *mm, const uint64 *pay)
{
	row->rows += pay[0];
	for (int m = 0; m < d->sp.nsum; m++)
		row->sum[m] += pay[1 + m];
	row->distinct += pay[dist_pay_last(d)];
	for (int m = 0; m < d->sp.nmm; m++)
	{
		const uint64 *s = pay + dist_pay_mm(d, m);

		if (s[1] != 0)
			dist_mm_offer(mm + 3 * m, (d->sp.mm_max >> m) & 1, (const uint8 *) (uintptr_t) s[0], (uint32) (s[1] - 1), NULL);
	}
}

static void
row_emit(const SiftParDistinct *d, SiftParDistinctRow *row, const uint64 *mm, EachFn fn, void *arg)
{
	for (int m = 0; m < d->sp.nmm; m++)
		if (mm[3 * m + 1] != 0)
			row->mm[m] = (SiftStrRef) {(const uint8 *) (uintptr_t) mm[3 * m], (uint32) (mm[3 * m + 1] - 1)};
	fn(arg, row);
}

static void
each_set(const SiftParDistinct *d, EachFn fn, void *arg)
{
	SiftParDistinctRow row = {0};
	uint64		mm[3 * SIFT_PAR_DISTINCT_MM_MAX] = {0};

	for (int t = 0; t < d->nw; t++)
	{
		row_add(d, &row, mm, sift_keytab_pay(&d->w[t].keys, sift_keytab_at(&d->w[t].keys, 0)));
		row.distinct += d->w[t].cnt;
	}
	row_emit(d, &row, mm, fn, arg);
}

/* A direct table: the key of each entry is the same in every thread, which counted the values of the keys of its partitions in its own entry */
static void
each_direct(const SiftParDistinct *d, EachFn fn, void *arg)
{
	for (uint32 id = 0; id < d->sp.direct_n; id++)
	{
		SiftParDistinctRow row = {.key = {d->sp.direct_lo + id}};
		uint64		mm[3 * SIFT_PAR_DISTINCT_MM_MAX] = {0};

		for (int t = 0; t < d->nw; t++)
			row_add(d, &row, mm, sift_keytab_pay(&d->w[t].keys, sift_keytab_at(&d->w[t].keys, id)));
		if (row.rows > 0)
			row_emit(d, &row, mm, fn, arg);
	}
}

static void
each_key(const SiftParDistinct *d, const uint64 *e, EachFn fn, void *arg)
{
	const SiftTabKey k = sift_keytab_key(e);
	SiftParDistinctRow row = {.str = k.p,.len = k.len};
	uint64		mm[3 * SIFT_PAR_DISTINCT_MM_MAX] = {0};

	memcpy(row.key, k.w, d->sp.kw * sizeof(uint64));
	row_add(d, &row, mm, e + SIFT_KEY_HEAD + d->sp.kw);
	row_emit(d, &row, mm, fn, arg);
}

void
sift_par_distinct_each(const SiftParDistinct *d, EachFn fn, void *arg)
{
	if (!d->grouped)
		return each_set(d, fn, arg);
	if (d->direct)
		return each_direct(d, fn, arg);
	for (int t = 0; t < d->nw; t++)
		for (const DistOut *o = d->w[t].out; o != NULL; o = o->next)
			for (uint32 i = 0; i < o->n; i++)
				each_key(d, o->ent + (size_t) i * d->w[t].keys.ew, fn, arg);
}

/* ---- dictionary codes ---- */

uint64
sift_par_distinct_bits_or(const uint64 *const *bits, int nbits, uint32 nwords, int t, int nthreads, uint64 *out)
{
	const uint64 lo = (uint64) nwords * t / nthreads,
				hi = (uint64) nwords * (t + 1) / nthreads;
	uint64		set = 0;

	for (uint64 i = lo; i < hi; i++)
	{
		uint64		x = 0;

		for (int b = 0; b < nbits; b++)
			x |= bits[b][i];
		if (out != NULL)
			out[i] = x;
		set += sift_pc64(x);
	}
	return set;
}
