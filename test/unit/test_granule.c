/*
 * test_granule.c - the decoders of col/granule and col/f2/f2_view.c on real granules (granule_fixture.h): the fixture covers the encodings, and the backend-path point decode agrees with
 * the full decode it is the reference of.
 */
#include "postgres.h"

#include "col/granule/granule_int.h"
#include "granule_fixture.h"
#include "unit.h"

#define GRANULES	15

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

	for (int i = 0; i < (int) lengthof(required_encs); i++)
		if (!(fx->enc_seen & (UINT64CONST(1) << required_encs[i])))
			fprintf(stderr, "fixture: no chunk of encoding %d\n", required_encs[i]), abort();
	CHECK((fx->flags_seen & flags) == flags);
	CHECK(fx->wrapped_seen & (UINT64CONST(1) << SIFT_CENC_STR_INLINE));
}

static void
test_fixture_columns(const GranuleFixture *fx)
{
	SiftChunk	c;

	for (int g = 0; g < fx->ngranules; g++)
	{
		const FxGranule *gr = &fx->g[g];
		const FxRef *url = &gr->ref[granule_fixture_col(fx, "url")];
		uint64		bytes = 0;

		for (int r = 0; r < url->n; r++)
			bytes += url->s[r].len;
		CHECK(gr->len == gr->desc.data_len && gr->view.hdr->nrows == FX_ROWS && gr->view.dicts == &fx->dicts);
		CHECK(url->n == FX_ROWS && bytes > 60 * FX_ROWS && bytes < 400 * FX_ROWS);
	}
	sift_chunk_open(&fx->g[0].view, granule_fixture_col(fx, "int_jnull") + 1, &c);
	CHECK(c.owned & SIFT_CHUNK_OWN_EXC && c.exc->n_entries > SIFT_EXC_SPARSE_MAX);
	sift_chunk_close(&c);
	CHECK(sift_part_dict_find(&fx->dicts, granule_fixture_col(fx, "url"), SIFT_DICT_KIND_ZSTD) != NULL);
	CHECK(sift_part_dict_find(&fx->dicts, granule_fixture_col(fx, "title"), SIFT_DICT_KIND_FSST) != NULL);
	CHECK(sift_part_dict_find(&fx->dicts, granule_fixture_col(fx, "lang"), SIFT_DICT_KIND_STR) != NULL);
}

/* The point decode of the backend path against the full decode: the fixture's reference means the same thing for both */
static void
test_reference_points(const GranuleFixture *fx)
{
	for (int g = 0; g < fx->ngranules; g++)
	{
		const FxGranule *gr = &fx->g[g];

		for (int c = 0; c < fx->ncols; c++)
		{
			const FxRef *ref = &gr->ref[c];
			int64		iv[FX_SEL_MAX];
			double		fv[FX_SEL_MAX];
			SiftStrCell sv[FX_SEL_MAX];
			bool		nl[FX_SEL_MAX];

			if (fx->cols[c].group_of > 0)
				continue;
			if (fx->cols[c].kind == SIFT_CELL_INT)
				sift_granule_point_ints(&gr->view, c, gr->sel, gr->nsel, iv, nl);
			else if (fx->cols[c].kind == SIFT_CELL_F8)
				sift_granule_point_f8(&gr->view, c, gr->sel, gr->nsel, fv, nl);
			else
				sift_granule_point_strs(&gr->view, c, gr->sel, gr->nsel, sv);
			for (int j = 0; j < gr->nsel; j++)
			{
				int			r = gr->sel[j];

				if (fx->cols[c].kind == SIFT_CELL_STR)
				{
					CHECK(sv[j].isnull == ref->isnull[r]);
					CHECK(sv[j].isnull || (sv[j].len == ref->s[r].len && memcmp(sv[j].p, ref->s[r].p, sv[j].len) == 0));
					continue;
				}
				CHECK(nl[j] == ref->isnull[r]);
				if (fx->cols[c].kind == SIFT_CELL_INT)
					CHECK(iv[j] == ref->i[r]);
				else
					CHECK(memcmp(&fv[j], &ref->f[r], sizeof(double)) == 0);
			}
		}
	}
}

int
main(void)
{
	GranuleFixture *fx;

	granule_fixture_init();
	fx = granule_fixture_build(GRANULES, 1);
	test_fixture_coverage(fx);
	test_fixture_columns(fx);
	test_reference_points(fx);
	granule_fixture_free(fx);
	puts("test_granule: ok");
	return 0;
}
