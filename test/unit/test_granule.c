/*
 * test_granule.c - the decoders of col/granule and col/f2/view.c on real granules (granule_fixture.h), run by 4 pool-like threads that each own a 2 MB arena: every column kind through the
 * _a entrypoints, dense, point and as ColViews, bit for bit against the decode of the backend path; damaged chunks of every kind give the error of the backend path as data; an arena too
 * small for a chunk says so and is never overrun. The palloc/ereport trap of trapshim.h is armed on every worker.
 */
#include "postgres.h"

#include "col/compress.h"
#include "col/f2/view.h"
#include "col/granule/a.h"
#include "granule_fixture.h"
#include "pgshim.h"
#include "unit.h"

/* A call of an _a entrypoint that must succeed: its error is the message of the failure */
#define CHECK_A(call) \
	do { \
		if (!(call)) { \
			fprintf(stderr, "%s:%d: %s failed: code %d \"%s\" (asked %u, left %u)\n", __FILE__, __LINE__, #call, err.code, err.msg, err.arg0, err.arg1); \
			abort(); \
		} \
	} while (0)

#define WORKERS		4
#define ARENA_BYTES	((size_t) 2 << 20)
#define CAP_VALUES	8192			/* the most values of one chunk: the elements of a group are at most SIFT_GRANULE_ROWS */
#define GRANULES	15
#define MUTANTS		9
#define MUTANT_GRANULES 5

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ROUNDS		1
#else
#define ROUNDS		3
#endif

static const int required_encs[] = {
	SIFT_CENC_INT_FOR, SIFT_CENC_INT_DELTA, SIFT_CENC_INT_CONST, SIFT_CENC_DICT_CODE, SIFT_CENC_STR_INLINE, SIFT_CENC_FLOAT_RAW, SIFT_CENC_LIST_LEN, SIFT_CENC_ALL_NULL,
	SIFT_CENC_BOOL_BITS, SIFT_CENC_NUM64, SIFT_CENC_NUM128, SIFT_CENC_UUID16, SIFT_CENC_INTERVAL16, SIFT_CENC_RLE, SIFT_CENC_ALP, SIFT_CENC_STR_VIEW, SIFT_CENC_FLOAT_BSS,
	SIFT_CENC_STR_FSST, SIFT_CENC_STR_BLOCK, SIFT_CENC_FIXED16, SIFT_CENC_KEY_PRESENT, SIFT_CENC_ALP_RD
};

static void
test_fixture_coverage(const GranuleFixture *fx)
{
	const uint32 flags = SIFT_CHUNK_ALL_NULL | SIFT_CHUNK_HAS_KINDS | SIFT_CHUNK_COMPRESSED | SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_ELEMS |
		((SIFT_ENC_NULL_SPARSE | SIFT_ENC_PART_DICT | SIFT_ENC_NONNULL_SPARSE | SIFT_ENC_KEY_HAS_MASK) >> 8 << 16);
	SiftChunk	c;

	for (int i = 0; i < (int) lengthof(required_encs); i++)
		if (!(fx->enc_seen & (UINT64CONST(1) << required_encs[i])))
			fprintf(stderr, "fixture: no chunk of encoding %d\n", required_encs[i]), abort();
	CHECK((fx->flags_seen & flags) == flags);
	CHECK(fx->wrapped_seen & (UINT64CONST(1) << SIFT_CENC_STR_INLINE));
	sift_chunk_open(&fx->g[0].view, granule_fixture_col(fx, "int_jnull") + 1, &c);
	CHECK(c.owned & SIFT_CHUNK_OWN_EXC && c.exc->n_entries > SIFT_EXC_SPARSE_MAX);
	sift_chunk_close(&c);
	for (int g = 0; g < fx->ngranules; g++)
	{
		const FxRef *url = &fx->g[g].ref[granule_fixture_col(fx, "url")];
		uint64		bytes = 0;

		for (int r = 0; r < url->n; r++)
			bytes += url->s[r].len;
		CHECK(url->n == FX_ROWS && bytes > 60 * FX_ROWS && bytes < 400 * FX_ROWS);
	}
}

/* The output arrays of one decode: in the arena of a worker, in the context of the setup thread for the backend path */
typedef struct Buf
{
	int64	   *i;
	double	   *f;
	bool	   *nl;
	uint8	   *tag;
	SiftStrCell *s;
	uint32	   *codes;
	uint16	   *rows;
} Buf;

static Buf
buf_make(void *(*alloc) (void *cx, size_t n), void *cx)
{
	return (Buf) {alloc(cx, CAP_VALUES * 8), alloc(cx, CAP_VALUES * 8), alloc(cx, CAP_VALUES), alloc(cx, CAP_VALUES), alloc(cx, CAP_VALUES * sizeof(SiftStrCell)),
		alloc(cx, CAP_VALUES * 4), alloc(cx, CAP_VALUES * 2)};
}

/* The buffers of a worker are its own, not the arena's: what the arena holds is what the decoders took */
static void *
thread_alloc(void *cx, size_t n)
{
	void	   *p = malloc(n);

	CHECK(p != NULL);
	return p;
}

static void
buf_free(Buf *b)
{
	free(b->i), free(b->f), free(b->nl), free(b->tag), free(b->s), free(b->codes), free(b->rows);
}

static void *
pg_alloc(void *cx, size_t n)
{
	return palloc(n);
}

static bool
same_cell(const SiftStrCell *a, const SiftStrCell *b)
{
	return a->isnull == b->isnull && (a->isnull || (a->len == b->len && memcmp(a->p, b->p, a->len) == 0));
}

static bool
dict_cell(const SiftGranuleDict *d, uint32 code, const SiftStrCell *want)
{
	return code < d->n && d->off[code + 1] - d->off[code] == want->len && memcmp(d->bytes + d->off[code], want->p, want->len) == 0;
}

/* Value at of a decode is the reference's value of row r */
static void
check_row(const FxColSpec *spec, const FxRef *ref, int r, int at, const Buf *b)
{
	if (spec->kind == SIFT_CELL_INT)
		CHECK(b->nl[at] == ref->isnull[r] && b->i[at] == ref->i[r]);
	else if (spec->kind == SIFT_CELL_F8)
		CHECK(b->nl[at] == ref->isnull[r] && memcmp(&b->f[at], &ref->f[r], sizeof(double)) == 0);
	else
		CHECK(same_cell(&b->s[at], &ref->s[r]));
}

typedef struct Hw
{
	uint32		dense, point, view, sparse;
} Hw;

typedef struct Verify
{
	const GranuleFixture *fx;
	SiftArena  *arena[WORKERS];
	Hw			hw[WORKERS][FX_MAX_COLS];
	uint32		whole[WORKERS];			/* the arena of the views of every column of one granule at once */
} Verify;

static uint32
grown(const SiftArena *a, SiftArenaMark m)
{
	return a->high_water - m;
}

static void
verify_dense(Verify *vf, int w, const FxGranule *gr, int c, Buf *b, Hw *hw)
{
	const FxColSpec *spec = &vf->fx->cols[c];
	const FxRef *ref = &gr->ref[c];
	SiftArena  *a = vf->arena[w];
	SiftArenaMark m = sift_arena_mark(a);
	SiftErr		err;
	SiftChunk	ch;
	SiftGranuleDict dict;
	uint16		count;
	int			n;
	bool		coded;

	a->high_water = a->used;
	CHECK(sift_granule_chunk_len_a(a, &err, &gr->view, c, &n) && n == ref->n);
	CHECK(sift_granule_tags_a(a, &err, &gr->view, c, b->tag) && memcmp(b->tag, ref->tag, n) == 0);
	if (spec->kind == SIFT_CELL_INT)
		CHECK_A(sift_granule_ints_a(a, &err, &gr->view, c, b->i, b->nl));
	else if (spec->kind == SIFT_CELL_F8)
		CHECK_A(sift_granule_f8_a(a, &err, &gr->view, c, b->f, b->nl));
	else
		CHECK_A(sift_granule_strs_a(a, &err, &gr->view, c, b->s));
	for (int r = 0; r < n; r++)
		check_row(spec, ref, r, r, b);
	if (spec->kind == SIFT_CELL_STR)
	{
		CHECK(sift_granule_str_codes_a(a, &err, &gr->view, c, b->codes, b->nl, &dict, &coded) && coded == ref->has_codes);
		for (int r = 0; coded && r < n; r++)
			CHECK(b->nl[r] == ref->isnull[r] && (b->nl[r] || dict_cell(&dict, b->codes[r], &ref->s[r])));
	}
	if (spec->group_of == 0)
	{
		SiftArenaMark mm = sift_arena_mark(a);

		CHECK_A(sift_granule_nonnull_rows_a(a, &err, &gr->view, c, b->rows, FX_ROWS, &count));
		for (int r = 0, k = 0; r < FX_ROWS; r++)
			if (!ref->isnull[r])
				CHECK(k < count && b->rows[k++] == r);
		CHECK(a->used == mm);
	}
	CHECK_A(sift_chunk_open_a(a, &err, &gr->view, c + 1, &ch) && ch.nrows == n && sift_chunk_close_a(a, &err, &ch));
	hw->dense = Max(hw->dense, grown(a, m));
	sift_arena_rewind(a, m);
}

static void
verify_point(Verify *vf, int w, const FxGranule *gr, int c, Buf *b, Hw *hw)
{
	const FxColSpec *spec = &vf->fx->cols[c];
	const FxRef *ref = &gr->ref[c];
	SiftArena  *a = vf->arena[w];
	SiftArenaMark m = sift_arena_mark(a);
	SiftErr		err;
	SiftGranuleDict dict;
	bool		coded;

	a->high_water = a->used;
	if (spec->kind == SIFT_CELL_INT)
		CHECK_A(sift_granule_point_ints_a(a, &err, &gr->view, c, gr->sel, gr->nsel, b->i, b->nl));
	else if (spec->kind == SIFT_CELL_F8)
		CHECK_A(sift_granule_point_f8_a(a, &err, &gr->view, c, gr->sel, gr->nsel, b->f, b->nl));
	else
		CHECK_A(sift_granule_point_strs_a(a, &err, &gr->view, c, gr->sel, gr->nsel, b->s));
	for (int j = 0; j < gr->nsel; j++)
		check_row(spec, ref, gr->sel[j], j, b);
	if (spec->kind == SIFT_CELL_STR)
	{
		CHECK(sift_granule_point_str_codes_a(a, &err, &gr->view, c, gr->sel, gr->nsel, b->codes, b->nl, &dict, &coded) && coded == ref->has_codes);
		for (int j = 0; coded && j < gr->nsel; j++)
			CHECK(b->nl[j] == ref->isnull[gr->sel[j]] && (b->nl[j] || dict_cell(&dict, b->codes[j], &ref->s[gr->sel[j]])));
		for (int j = 0; j < gr->nsel; j += 1 + gr->nsel / 4)
		{
			const char *p;
			uint32		len;
			bool		valid;
			uint8		tag;

			CHECK(sift_granule_point_str_a(a, &err, &gr->view, c, gr->sel[j], &p, &len, &valid) && valid == !ref->isnull[gr->sel[j]]);
			CHECK(!valid || (len == ref->s[gr->sel[j]].len && memcmp(p, ref->s[gr->sel[j]].p, len) == 0));
			CHECK(sift_granule_point_tag_a(a, &err, &gr->view, c, gr->sel[j], &tag) && tag == ref->tag[gr->sel[j]]);
		}
	}
	hw->point = Max(hw->point, grown(a, m));
	sift_arena_rewind(a, m);
}

static void
check_view(const FxColSpec *spec, const FxRef *ref, const ColView *cv, const uint16 *rows, int nrows, bool codes)
{
	int			n = rows != NULL ? nrows : FX_ROWS;

	for (int j = 0; j < n; j++)
	{
		int			r = rows != NULL ? rows[j] : j;

		CHECK(cv->isnull[r] == ref->isnull[r] && cv->tag[r] == ref->tag[r]);
		if (spec->kind == SIFT_CELL_INT)
			CHECK(cv->i[r] == ref->i[r]);
		else if (spec->kind == SIFT_CELL_F8)
			CHECK(memcmp(&cv->f[r], &ref->f[r], sizeof(double)) == 0);
		else if (cv->dcode != NULL)
			CHECK(ref->isnull[r] || dict_cell(cv->dict, cv->dcode[r], &ref->s[r]));
		else
			CHECK(ref->isnull[r] || (cv->slen[r] == ref->s[r].len && memcmp(cv->s[r], ref->s[r].p, cv->slen[r]) == 0));
	}
	CHECK((cv->dcode != NULL) == (codes && ref->has_codes));
}

static void
verify_view(Verify *vf, int w, const FxGranule *gr, int c, Hw *hw)
{
	const FxColSpec *spec = &vf->fx->cols[c];
	SiftArena  *a = vf->arena[w];
	SiftErr		err;
	ColView		cv;

	for (int pass = 0; pass < 4; pass++)
	{
		bool		sparse = pass & 1,
					codes = pass & 2;
		SiftArenaMark m = sift_arena_mark(a);

		if (codes && spec->kind != SIFT_CELL_STR)
			continue;
		a->high_water = a->used;
		CHECK_A(sift_f2_decode_view_a(a, &err, NULL, &vf->fx->defs[c], &gr->view, c, FX_ROWS, codes, sparse ? gr->sel : NULL, gr->nsel, &cv));
		check_view(spec, &gr->ref[c], &cv, sparse ? gr->sel : NULL, gr->nsel, codes);
		if (sparse)
			hw->sparse = Max(hw->sparse, grown(a, m));
		else
			hw->view = Max(hw->view, grown(a, m));
		sift_arena_rewind(a, m);
	}
}

/* The columns of a ClickBench scan that wants the wide strings, as views of one granule at once: the arena holds them all */
static const char *const scan_cols[] = {"url", "title", "referer", "phrase", "lang", "browser", "counter", "event_time", "user_id", "watch_id", "price", "geo"};

static uint32
verify_granule_views(Verify *vf, int w, const FxGranule *gr)
{
	SiftArena  *a = vf->arena[w];
	SiftArenaMark m = sift_arena_mark(a);
	SiftErr		err;
	ColView		cv;

	a->high_water = a->used;
	for (int i = 0; i < (int) lengthof(scan_cols); i++)
	{
		int			c = granule_fixture_col(vf->fx, scan_cols[i]);

		CHECK_A(sift_f2_decode_view_a(a, &err, NULL, &vf->fx->defs[c], &gr->view, c, FX_ROWS, true, NULL, 0, &cv));
		check_view(&vf->fx->cols[c], &gr->ref[c], &cv, NULL, 0, true);
	}
	return grown(a, m);
}

static void
verify_worker(void *arg, int w)
{
	Verify	   *vf = arg;
	SiftArena  *a = vf->arena[w];
	Buf			b = buf_make(thread_alloc, NULL);
	SiftArenaMark base = sift_arena_mark(a);
	uint32		whole;

	for (int round = 0; round < ROUNDS; round++)
		for (int g = 0; g < vf->fx->ngranules; g++)
		{
			const FxGranule *gr = &vf->fx->g[g];

			if ((g + round) % WORKERS != w)
				continue;
			for (int c = 0; c < vf->fx->ncols; c++)
			{
				Hw		   *hw = &vf->hw[w][c];

				verify_dense(vf, w, gr, c, &b, hw);
				if (vf->fx->cols[c].group_of == 0)
				{
					verify_point(vf, w, gr, c, &b, hw);
					verify_view(vf, w, gr, c, hw);
				}
			}
			whole = verify_granule_views(vf, w, gr);
			vf->whole[w] = Max(vf->whole[w], whole);
			sift_arena_rewind(a, base);
			CHECK(a->used == base && !sift_arena_scoped());
		}
	buf_free(&b);
	sift_colz_thread_fini();
}

static void
print_high_water(const Verify *vf)
{
	uint32		whole = 0;

	printf("high_water of the 2 MB arena, bytes (dense | point | view | sparse view), largest of %d granules:\n", vf->fx->ngranules);
	for (int c = 0; c < vf->fx->ncols; c++)
	{
		Hw			hw = {0};

		for (int w = 0; w < WORKERS; w++)
		{
			hw.dense = Max(hw.dense, vf->hw[w][c].dense);
			hw.point = Max(hw.point, vf->hw[w][c].point);
			hw.view = Max(hw.view, vf->hw[w][c].view);
			hw.sparse = Max(hw.sparse, vf->hw[w][c].sparse);
		}
		printf("  %-16s %9u %9u %9u %9u\n", vf->fx->cols[c].name, hw.dense, hw.point, hw.view, hw.sparse);
	}
	for (int w = 0; w < WORKERS; w++)
		whole = Max(whole, vf->whole[w]);
	printf("  the %d columns of a scan as views of one granule at once: %u bytes of %zu\n", (int) lengthof(scan_cols), whole, ARENA_BYTES);
	CHECK(whole < ARENA_BYTES);
}

static void
test_workers(const GranuleFixture *fx)
{
	Verify	   *vf = palloc0(sizeof(Verify));
	uint64		traps = sift_test_trap_count();

	vf->fx = fx;
	for (int w = 0; w < WORKERS; w++)
		vf->arena[w] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	unit_run_workers(WORKERS, verify_worker, vf);
	CHECK(sift_test_trap_count() == traps);
	print_high_water(vf);
}

/* The damage of a chunk of every kind: what the backend path raises, a worker answers as data */

typedef enum Op
{
	OP_LEN, OP_TAGS, OP_NONNULL, OP_DENSE, OP_CODES, OP_POINT, OP_PCODES, OP_PSTR, OP_PTAG, OP_VIEW, OP_VSPARSE, OP_VCODES, OP_VSCODES, NOPS
} Op;

typedef struct OpArgs
{
	const SiftGranuleView *v;
	const FxColSpec *spec;
	const SiftColDef *def;
	int			col;
	Op			op;
	const uint16 *sel;
	uint16		nsel;
	Buf		   *b;
	int			n;
	ColView		cv;
	bool		coded;
	uint16		count;
	const char *p;
	uint32		len;
	SiftGranuleDict dict;
} OpArgs;

typedef struct Out
{
	bool		ok;
	int			sqlstate;
	char		msg[96];
	uint64		digest;
} Out;

static uint64
mix(uint64 h, const void *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ ((const uint8 *) p)[i]) * 0x100000001B3ULL;
	return h;
}

static uint64
mix_cells(uint64 h, const SiftStrCell *s, int n)
{
	for (int i = 0; i < n; i++)
	{
		h = mix(h, &s[i].isnull, 1);
		if (!s[i].isnull)
			h = mix(mix(h, &s[i].len, 4), s[i].p, s[i].len);
	}
	return h;
}

static uint64
mix_dict_string(uint64 h, const SiftGranuleDict *d, uint32 code)
{
	return code < d->n ? mix(h, d->bytes + d->off[code], d->off[code + 1] - d->off[code]) : h;
}

static uint64
mix_view(uint64 h, const OpArgs *x, const uint16 *rows, int nrows)
{
	const ColView *cv = &x->cv;
	int			n = rows != NULL ? nrows : FX_ROWS;

	for (int j = 0; j < n; j++)
	{
		int			r = rows != NULL ? rows[j] : j;

		h = mix(mix(h, &cv->isnull[r], 1), &cv->tag[r], 1);
		if (cv->kind == SIFT_CELL_INT)
			h = mix(h, &cv->i[r], 8);
		else if (cv->kind == SIFT_CELL_F8)
			h = mix(h, &cv->f[r], 8);
		else if (cv->dcode != NULL)
			h = cv->isnull[r] ? h : mix_dict_string(h, cv->dict, cv->dcode[r]);
		else if (!cv->isnull[r])
			h = mix(h, cv->s[r], cv->slen[r]);
	}
	return h;
}

/* What the op wrote, after it ran: the same for the backend path and the worker */
static uint64
digest(const OpArgs *x)
{
	const Buf  *b = x->b;
	uint64		h = 0xcbf29ce484222325ULL;
	int			n = x->op == OP_POINT || x->op == OP_PCODES ? x->nsel : x->n;

	switch (x->op)
	{
		case OP_LEN:
			return mix(h, &x->n, 4);
		case OP_TAGS:
			return mix(h, b->tag, n);
		case OP_NONNULL:
			return mix(mix(h, &x->count, 2), b->rows, x->count * 2);
		case OP_DENSE:
		case OP_POINT:
			if (x->spec->kind == SIFT_CELL_STR)
				return mix_cells(h, b->s, n);
			return mix(mix(h, x->spec->kind == SIFT_CELL_INT ? (void *) b->i : (void *) b->f, n * 8), b->nl, n);
		case OP_CODES:
		case OP_PCODES:
			h = mix(h, &x->coded, 1);
			for (int i = 0; x->coded && i < n; i++)
				h = mix_dict_string(mix(h, &b->nl[i], 1), &x->dict, b->nl[i] ? UINT32_MAX : b->codes[i]);
			return h;
		case OP_PSTR:
			return x->len == UINT32_MAX ? h : mix(mix(h, &x->len, 4), x->p, x->len);
		case OP_PTAG:
			return mix(h, b->tag, 1);
		case OP_VIEW:
		case OP_VCODES:
			return mix_view(h, x, NULL, 0);
		default:
			return mix_view(h, x, x->sel, x->nsel);
	}
}

static bool
op_applies(const OpArgs *x)
{
	switch (x->op)
	{
		case OP_LEN:
		case OP_TAGS:
		case OP_DENSE:
			return true;
		case OP_NONNULL:
		case OP_POINT:
		case OP_PTAG:
		case OP_VIEW:
		case OP_VSPARSE:
			return x->spec->group_of == 0;
		default:
			return x->spec->kind == SIFT_CELL_STR && x->spec->group_of == 0;
	}
}

static bool
op_codes_view(const OpArgs *x)
{
	return x->spec->kind == SIFT_CELL_STR && (x->op == OP_VCODES || x->op == OP_VSCODES);
}

static void
run_pg(void *arg)
{
	OpArgs	   *x = arg;
	const SiftGranuleView *v = x->v;
	Buf		   *b = x->b;
	int			c = x->col;
	bool		sparse = x->op == OP_VSPARSE || x->op == OP_VSCODES;

	switch (x->op)
	{
		case OP_LEN:
			x->n = sift_granule_chunk_len(v, c);
			break;
		case OP_TAGS:
			sift_granule_tags(v, c, b->tag);
			break;
		case OP_NONNULL:
			x->count = sift_granule_nonnull_rows(v, c, b->rows, FX_ROWS);
			break;
		case OP_DENSE:
			if (x->spec->kind == SIFT_CELL_INT)
				sift_granule_ints(v, c, b->i, b->nl);
			else if (x->spec->kind == SIFT_CELL_F8)
				sift_granule_f8(v, c, b->f, b->nl);
			else
				sift_granule_strs(v, c, b->s);
			break;
		case OP_CODES:
			x->coded = sift_granule_str_codes(v, c, b->codes, b->nl, &x->dict);
			break;
		case OP_POINT:
			if (x->spec->kind == SIFT_CELL_INT)
				sift_granule_point_ints(v, c, x->sel, x->nsel, b->i, b->nl);
			else if (x->spec->kind == SIFT_CELL_F8)
				sift_granule_point_f8(v, c, x->sel, x->nsel, b->f, b->nl);
			else
				sift_granule_point_strs(v, c, x->sel, x->nsel, b->s);
			break;
		case OP_PCODES:
			x->coded = sift_granule_point_str_codes(v, c, x->sel, x->nsel, b->codes, b->nl, &x->dict);
			break;
		case OP_PSTR:
			if (!sift_granule_point_str(v, c, x->sel[x->nsel / 2], &x->p, &x->len))
				x->len = UINT32_MAX;
			break;
		case OP_PTAG:
			b->tag[0] = sift_granule_point_tag(v, c, x->sel[x->nsel / 2]);
			break;
		default:
			x->cv = sift_f2_decode_view(x->def, v, c, FX_ROWS, op_codes_view(x), sparse ? x->sel : NULL, x->nsel);
	}
}

static bool
run_a(SiftArena *a, SiftErr *err, OpArgs *x)
{
	const SiftGranuleView *v = x->v;
	Buf		   *b = x->b;
	int			c = x->col;
	bool		sparse = x->op == OP_VSPARSE || x->op == OP_VSCODES;
	bool		valid;

	switch (x->op)
	{
		case OP_LEN:
			return sift_granule_chunk_len_a(a, err, v, c, &x->n);
		case OP_TAGS:
			return sift_granule_tags_a(a, err, v, c, b->tag);
		case OP_NONNULL:
			return sift_granule_nonnull_rows_a(a, err, v, c, b->rows, FX_ROWS, &x->count);
		case OP_DENSE:
			if (x->spec->kind == SIFT_CELL_INT)
				return sift_granule_ints_a(a, err, v, c, b->i, b->nl);
			if (x->spec->kind == SIFT_CELL_F8)
				return sift_granule_f8_a(a, err, v, c, b->f, b->nl);
			return sift_granule_strs_a(a, err, v, c, b->s);
		case OP_CODES:
			return sift_granule_str_codes_a(a, err, v, c, b->codes, b->nl, &x->dict, &x->coded);
		case OP_POINT:
			if (x->spec->kind == SIFT_CELL_INT)
				return sift_granule_point_ints_a(a, err, v, c, x->sel, x->nsel, b->i, b->nl);
			if (x->spec->kind == SIFT_CELL_F8)
				return sift_granule_point_f8_a(a, err, v, c, x->sel, x->nsel, b->f, b->nl);
			return sift_granule_point_strs_a(a, err, v, c, x->sel, x->nsel, b->s);
		case OP_PCODES:
			return sift_granule_point_str_codes_a(a, err, v, c, x->sel, x->nsel, b->codes, b->nl, &x->dict, &x->coded);
		case OP_PSTR:
			if (!sift_granule_point_str_a(a, err, v, c, x->sel[x->nsel / 2], &x->p, &x->len, &valid))
				return false;
			x->len = valid ? x->len : UINT32_MAX;
			return true;
		case OP_PTAG:
			return sift_granule_point_tag_a(a, err, v, c, x->sel[x->nsel / 2], &b->tag[0]);
		default:
			return sift_f2_decode_view_a(a, err, NULL, x->def, v, c, FX_ROWS, op_codes_view(x), sparse ? x->sel : NULL, x->nsel, &x->cv);
	}
}

static int
sqlstate_of(SiftErrCode code)
{
	switch (code)
	{
		case SIFT_ERR_CORRUPT:
			return ERRCODE_DATA_CORRUPTED;
		case SIFT_ERR_UNSUPPORTED:
			return ERRCODE_FEATURE_NOT_SUPPORTED;
		case SIFT_ERR_INVALID_PARAM:
			return ERRCODE_INVALID_PARAMETER_VALUE;
		default:
			return 0;
	}
}

typedef struct Mutant
{
	int			g;
	int			col;
	int			kind;
	uint32		seed;
} Mutant;

/* The record of the granule with the damage of kind m->kind in the chunk of m->col: the table entry of the chunk or its bytes. The same in every thread that applies it */
static void
apply_patch(uint8 *dst, const FxGranule *gr, const Mutant *m)
{
	SiftChunkRef *ref;
	uint8	   *body;
	uint32		len;
	uint32		seed = m->seed * 2654435761u + 12345;

	memcpy(dst, gr->rec, gr->len);
	ref = &((SiftGranuleHeader *) dst)->chunks[m->col + 1];
	body = dst + ref->off;
	len = ref->len;
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	if (len < 16)
		return;
	switch (m->kind)
	{
		case 0:
			ref->len = len / 2;
			break;
		case 1:
			ref->len = 8 + seed % 8;
			break;
		case 2:
			body[seed % Min(len, 64)] ^= 1 << (seed >> 20 & 7);
			break;
		case 3:
			body[seed % len] ^= 1 << (seed >> 20 & 7);
			break;
		case 4:
			memset(body + (seed % Min(len / 4, 32)) * 4, 0xFF, 4);
			break;
		case 5:
			{
				uint32		big = 0x40000000u + (seed & 0x3FFFFFFFu);

				memcpy(body + (seed % (len / 4)) * 4, &big, 4);
			}
			break;
		case 6:
			ref->enc = (ref->enc & ~SIFT_ENC_MASK) | (1 + seed % 26);
			break;
		case 7:
			ref->flags ^= 1 << (seed % 6);
			ref->enc ^= (seed >> 8 & 1) ? 0 : (UINT16_C(0x100) << (seed >> 10 & 3));
			break;
		default:
			memset(body, 0, len);
	}
}

static SiftGranuleView
mutant_view(const FxGranule *gr, const uint8 *rec)
{
	SiftGranuleView v = gr->view;

	v.rec = (const char *) rec;
	v.hdr = (const SiftGranuleHeader *) rec;
	return v;
}

static OpArgs
args_make(const GranuleFixture *fx, const SiftGranuleView *v, const FxGranule *gr, int col, Op op, int n, Buf *b)
{
	return (OpArgs) {.v = v,.spec = &fx->cols[col],.def = &fx->defs[col],.col = col,.op = op,.sel = gr->sel,.nsel = gr->nsel,.b = b,.n = n};
}

typedef struct Fuzz
{
	const GranuleFixture *fx;
	Mutant	   *mut;
	int			nmut;
	Out		  (*res)[NOPS];
	SiftArena  *arena[WORKERS];
	_Atomic int mismatches;
	_Atomic int errors;
	_Atomic int full;
	_Atomic int oks;
} Fuzz;

/* Ops after the length of the chunk run when it is one the buffers hold */
static bool
op_runs(const OpArgs *x, int ncap)
{
	return op_applies(x) && (x->op == OP_LEN || (ncap >= 0 && ncap <= CAP_VALUES));
}

static void
fuzz_reference(Fuzz *fz)
{
	Buf			b = buf_make(pg_alloc, NULL);

	pgshim_keep();
	for (int i = 0; i < fz->nmut; i++)
	{
		const Mutant *m = &fz->mut[i];
		const FxGranule *gr = &fz->fx->g[m->g];
		uint8	   *rec = palloc(gr->len);
		SiftGranuleView v;
		int			ncap = 0;

		apply_patch(rec, gr, m);
		v = mutant_view(gr, rec);
		for (int op = 0; op < NOPS; op++)
		{
			OpArgs		x = args_make(fz->fx, &v, gr, m->col, op, ncap, &b);
			Out		   *o = &fz->res[i][op];

			if (!op_runs(&x, ncap))
				continue;
			o->ok = pgshim_try(run_pg, &x);
			if (!o->ok)
			{
				o->sqlstate = pgshim_sqlstate;
				snprintf(o->msg, sizeof(o->msg), "%s", pgshim_errmsg);
			}
			else
				o->digest = digest(&x);
			if (op == OP_LEN)
				ncap = o->ok ? x.n : -1;
		}
		pgshim_reset();
	}
}

static void
fuzz_check(Fuzz *fz, int i, int op, const OpArgs *x, bool ok, const SiftErr *err)
{
	const Out  *o = &fz->res[i][op];
	const Mutant *m = &fz->mut[i];
	bool		same;

	if (ok)
	{
		atomic_fetch_add(&fz->oks, 1);
		same = o->ok && o->digest == digest(x);
	}
	else if (err->code == SIFT_ERR_ARENA_FULL)
	{
		atomic_fetch_add(&fz->full, 1);
		return;
	}
	else
	{
		atomic_fetch_add(&fz->errors, 1);
		same = !o->ok && o->sqlstate == sqlstate_of(err->code) && strncmp(o->msg, err->msg, sizeof(err->msg) - 1) == 0 &&
			strlen(err->msg) == Min(strlen(o->msg), sizeof(err->msg) - 1);
	}
	if (!same)
	{
		atomic_fetch_add(&fz->mismatches, 1);
		fprintf(stderr, "mismatch: mutant %d column %s kind %d op %d: backend path %s, worker %s\n", i, fz->fx->cols[m->col].name, m->kind, op, o->ok ? "ok" : o->msg,
				ok ? "ok" : err->msg);
	}
}

static void
fuzz_worker(void *arg, int w)
{
	Fuzz	   *fz = arg;
	SiftArena  *a = fz->arena[w];
	Buf			b = buf_make(thread_alloc, NULL);
	SiftArenaMark base = sift_arena_mark(a);

	for (int i = w; i < fz->nmut; i += WORKERS)
	{
		const Mutant *m = &fz->mut[i];
		const FxGranule *gr = &fz->fx->g[m->g];
		uint8	   *rec = malloc(gr->len);
		SiftGranuleView v;
		int			ncap = 0;

		apply_patch(rec, gr, m);
		v = mutant_view(gr, rec);
		for (int op = 0; op < NOPS; op++)
		{
			OpArgs		x = args_make(fz->fx, &v, gr, m->col, op, ncap, &b);
			SiftErr		err;
			bool		ok;

			if (!op_runs(&x, ncap))
				continue;
			sift_arena_rewind(a, base);
			ok = run_a(a, &err, &x);
			CHECK(!sift_arena_scoped() && a->used <= a->cap && (ok || err.code != SIFT_ERR_ARENA_FULL || err.arg0 > 0));
			fuzz_check(fz, i, op, &x, ok, &err);
			if (op == OP_LEN)
				ncap = ok ? x.n : -1;
		}
		free(rec);
	}
	buf_free(&b);
	sift_colz_thread_fini();
}

static void
test_damage(const GranuleFixture *fx)
{
	Fuzz	   *fz = palloc0(sizeof(Fuzz));
	int			ng = Min(MUTANT_GRANULES, fx->ngranules);
	uint64		traps = sift_test_trap_count();

	fz->fx = fx;
	fz->nmut = ng * fx->ncols * MUTANTS;
	fz->mut = palloc(fz->nmut * sizeof(Mutant));
	fz->res = palloc0(fz->nmut * sizeof(Out[NOPS]));
	for (int g = 0, i = 0; g < ng; g++)
		for (int c = 0; c < fx->ncols; c++)
			for (int k = 0; k < MUTANTS; k++)
				fz->mut[i++] = (Mutant) {.g = g,.col = c,.kind = k,.seed = (g * 131 + c) * 17 + k};
	for (int w = 0; w < WORKERS; w++)
		fz->arena[w] = sift_arena_create(CurrentMemoryContext, ARENA_BYTES);
	fuzz_reference(fz);
	unit_run_workers(WORKERS, fuzz_worker, fz);
	printf("damage: %d mutants, %d decodes: %d equal, %d the error of the backend path, %d arena full, %d mismatches\n", fz->nmut,
		   atomic_load(&fz->oks) + atomic_load(&fz->errors) + atomic_load(&fz->full), atomic_load(&fz->oks), atomic_load(&fz->errors), atomic_load(&fz->full),
		   atomic_load(&fz->mismatches));
	CHECK(atomic_load(&fz->mismatches) == 0 && atomic_load(&fz->errors) > 0 && atomic_load(&fz->oks) > 0);
	CHECK(sift_test_trap_count() == traps);
}

/* An arena too small for the chunk says so, with what was asked and what was left, and is never written past its cap */
typedef struct Small
{
	const GranuleFixture *fx;
	SiftArena  *small[2];
	SiftArena  *big;
	size_t		fits;
} Small;

static bool
strs_fit(SiftArena *a, const GranuleFixture *fx, SiftStrCell *cells, SiftErr *err)
{
	sift_arena_reset(a);
	return sift_granule_strs_a(a, err, &fx->g[2].view, granule_fixture_col(fx, "url"), cells);
}

static void
small_worker(void *arg, int w)
{
	Small	   *s = arg;
	static _Thread_local SiftStrCell cells[FX_ROWS];
	SiftErr		err;
	size_t		lo = 0,
				hi = ARENA_BYTES;

	for (int i = 0; i < 2; i++)
	{
		CHECK(!strs_fit(s->small[i], s->fx, cells, &err));
		CHECK(err.code == SIFT_ERR_ARENA_FULL && err.arg0 > 0 && s->small[i]->used <= s->small[i]->cap && !sift_arena_scoped());
	}
	CHECK(strs_fit(s->big, s->fx, cells, &err));
	while (hi - lo > 64)
	{
		s->big->cap = lo + (hi - lo) / 2;
		if (strs_fit(s->big, s->fx, cells, &err))
			hi = s->big->cap;
		else
		{
			CHECK(err.code == SIFT_ERR_ARENA_FULL && err.arg0 > 0 && s->big->used <= s->big->cap);
			lo = s->big->cap;
		}
	}
	s->fits = hi;
	s->big->cap = ARENA_BYTES;
	CHECK(strs_fit(s->big, s->fx, cells, &err) && same_cell(&cells[0], &s->fx->g[2].ref[granule_fixture_col(s->fx, "url")].s[0]));
	sift_colz_thread_fini();
}

static void
test_arena_full(const GranuleFixture *fx)
{
	Small		s = {.fx = fx,.small = {sift_arena_create(CurrentMemoryContext, 4096), sift_arena_create(CurrentMemoryContext, 65536)},
	.big = sift_arena_create(CurrentMemoryContext, ARENA_BYTES)};

	unit_run_workers(1, small_worker, &s);
	printf("arena full: the URL strings of a granule need %zu bytes, 4 KB and 64 KB arenas answer ARENA_FULL\n", s.fits);
	CHECK(s.fits > 64 * 1024 && s.fits < ARENA_BYTES);
}

int
main(void)
{
	GranuleFixture *fx;

	setvbuf(stdout, NULL, _IOLBF, 0);
	granule_fixture_init();
	fx = granule_fixture_build(GRANULES, 1);
	test_fixture_coverage(fx);
	test_workers(fx);
	test_arena_full(fx);
	test_damage(fx);
	granule_fixture_free(fx);
	sift_colz_thread_fini();
	puts("test_granule: ok");
	return 0;
}
