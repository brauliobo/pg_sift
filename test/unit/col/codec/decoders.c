/*
 * col/codec/decoders.c - col/fsst.c and col/compress.c on bytes that came from disk: a string coded to its last byte is decoded without reading the byte after it, a symbol table
 * padded to 8 bytes that does not fit what is left is no table, and sizes that do not fit the int of liblz4 are refused instead of cut to 32 bits.
 */
#include "postgres.h"

#include "col/codec/codec.h"
#include "col/compress.h"
#include "col/fsst.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

static void
identity_table(SiftFsst *t)
{
	memset(t, 0, sizeof(*t));
	t->nsym = SIFT_FSST_MAX_SYMBOLS;
	for (uint32 c = 0; c < SIFT_FSST_MAX_SYMBOLS; c++)
	{
		t->len[c] = 1;
		t->sym[c][0] = (uint8) c;
	}
	t->len[SIFT_FSST_ESCAPE] = 1;
}

static void
test_decode_stops_at_the_last_code(void)
{
	SiftFsst   *t = malloc(sizeof(*t));
	uint8	   *in = malloc(1);
	uint8	   *out = malloc(8 + SIFT_FSST_SLACK);

	identity_table(t);
	in[0] = 'x';
	SIFT_CHECK(sift_fsst_decode(t, in, 1, out) == 1 && out[0] == 'x');
	free(in);
	free(out);
	free(t);
}

static void
test_decode_escape_without_its_byte_is_refused(void)
{
	SiftFsst   *t = malloc(sizeof(*t));
	uint8	   *in = malloc(1);
	uint8	   *out = malloc(8 + SIFT_FSST_SLACK);

	identity_table(t);
	in[0] = SIFT_FSST_ESCAPE;
	SIFT_CHECK(sift_fsst_decode(t, in, 1, out) == UINT32_MAX);
	free(in);
	free(out);
	free(t);
}

static void
test_table_padding_must_fit(void)
{
	static const uint8 blob[13] = {4, 0, 0, 0, 2, 1, 1, 1, 'a', 'b', 'c', 'd', 'e'};
	SiftFsst   *t = malloc(sizeof(*t));
	uint8	   *p = malloc(sizeof(blob));

	memcpy(p, blob, sizeof(blob));
	SIFT_CHECK(sift_fsst_read_table(t, p, sizeof(blob)) == 0);
	free(p);
	free(t);
}

static void
test_table_padding_that_fits_is_the_size(void)
{
	static const uint8 blob[16] = {4, 0, 0, 0, 2, 1, 1, 1, 'a', 'b', 'c', 'd', 'e'};
	SiftFsst   *t = malloc(sizeof(*t));
	uint8	   *p = malloc(sizeof(blob));

	memcpy(p, blob, sizeof(blob));
	SIFT_CHECK(sift_fsst_read_table(t, p, sizeof(blob)) == 16);
	free(p);
	free(t);
}

static void
test_lz4_sizes_beyond_int_are_refused(void)
{
	const Size	huge = ((Size) 1 << 32) + 16;
	uint8		src[64] = {0},
				dst[256];

	SIFT_CHECK(sift_colz_bound(SIFT_CC_ALG_LZ4, huge) == 0);
	SIFT_CHECK(sift_colz_compress(SIFT_CC_ALG_LZ4, 0, src, huge, dst, sizeof(dst)) == 0);
	SIFT_CHECK(sift_colz_compress(SIFT_CC_ALG_LZ4, 0, src, sizeof(src), dst, huge) > 0);
	SIFT_CHECK(!sift_colz_decompress(SIFT_CC_ALG_LZ4, src, huge, dst, sizeof(dst)));
	SIFT_CHECK(!sift_colz_decompress(SIFT_CC_ALG_LZ4, src, sizeof(src), dst, huge));
}

int
main(void)
{
	test_decode_stops_at_the_last_code();
	test_decode_escape_without_its_byte_is_refused();
	test_table_padding_must_fit();
	test_table_padding_that_fits_is_the_size();
	test_lz4_sizes_beyond_int_are_refused();
	fprintf(stderr, "test_decoders: ok\n");
	return 0;
}
