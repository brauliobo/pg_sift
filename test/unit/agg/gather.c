/*
 * agg/gather.c - agg/gather.h: the rows of a vector picked by an index list keep every byte of the element of the type of the vector (a 16 byte numeric, a 32 byte numeric product, a uuid,
 * an interval, a string view as well as the 8 byte types), the validity of the rows and the dictionary of a vector of codes; a NONE index and a NULL row are NULL, whose bytes are zero.
 */
#include "postgres.h"

#include "agg/gather.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define NROWS	300
#define NPICK	700

typedef struct Case
{
	SiftVecType type;
	size_t		width;
} Case;

static const Case CASES[] = {
	{SVEC_BOOL, sizeof(uint8)}, {SVEC_I32, sizeof(int32)}, {SVEC_I64, sizeof(int64)}, {SVEC_F64, sizeof(double)}, {SVEC_NUM64, sizeof(int64)},
	{SVEC_NUM128, sizeof(SiftI128)}, {SVEC_UUID, sizeof(SiftUuid16)}, {SVEC_STRVIEW, sizeof(SiftStrView)}, {SVEC_DICT32, sizeof(uint32)},
	{SVEC_INTERVAL, sizeof(SiftInterval16)}, {SVEC_DATUM, sizeof(Datum)}, {SVEC_NUM256, sizeof(SiftI256)},
};

static bool
row_null(uint32 r)
{
	return r % 7 == 3;
}

static void
fill_source(SiftVec *src, uint64 *valid, uint8 *data, size_t width)
{
	memset(valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 r = 0; r < NROWS; r++)
	{
		for (size_t b = 0; b < width; b++)
			data[r * width + b] = (uint8) (1 + (r * 31 + b * 17) % 251);
		if (!row_null(r))
			valid[r >> 6] |= UINT64CONST(1) << (r & 63);
	}
	*src = (SiftVec) {.data = data,.valid = valid,.len = NROWS,.all_valid = false,.dense = true};
}

static void
check_case(const Case *c, bool all_valid)
{
	uint64		valid[SIFT_VEC_WORDS];
	uint64		out_valid[SIFT_VEC_WORDS];
	uint32		idx[NPICK];
	static const int dictum = 0;
	uint8	   *data = calloc(NROWS, 32);
	uint8	   *out_data = calloc(NPICK, 32);
	SiftVec		src,
				out;

	SIFT_CHECK(sift_gather_width(c->type) == c->width);
	fill_source(&src, valid, data, c->width);
	src.type = c->type;
	src.dict = &dictum;
	if (all_valid)
	{
		src.all_valid = true;
		src.valid = NULL;
	}
	for (uint32 i = 0; i < NPICK; i++)
		idx[i] = i % 11 == 5 ? SIFT_GATHER_NONE : (uint32) ((i * 7919) % NROWS);
	sift_vec_gather(&out, out_valid, out_data, &src, idx, NPICK);
	SIFT_CHECK(out.type == c->type && out.len == NPICK && out.dense && !out.all_valid && out.dict == &dictum);
	for (uint32 i = 0; i < NPICK; i++)
	{
		const bool	picked = idx[i] != SIFT_GATHER_NONE && (all_valid || !row_null(idx[i]));
		const bool	got = (out_valid[i >> 6] >> (i & 63)) & 1;

		SIFT_CHECK(got == picked);
		if (picked)
			SIFT_CHECK(memcmp(out_data + i * c->width, data + (size_t) idx[i] * c->width, c->width) == 0);
		else
			for (size_t b = 0; b < c->width; b++)
				SIFT_CHECK(out_data[i * c->width + b] == 0);
	}
	free(data);
	free(out_data);
}

int
main(void)
{
	int			ncases = 0;

	pgshim_keep();
	for (size_t k = 0; k < lengthof(CASES); k++)
		for (int all_valid = 0; all_valid < 2; all_valid++, ncases++)
			check_case(&CASES[k], all_valid);
	SIFT_CHECK(lengthof(CASES) == SVEC_NUM256 + 1);
	printf("par_gather: ok (%d cases)\n", ncases);
	return 0;
}
