/*
 * col/rowfile.c - rows in a temporary file (rowfile.h)
 * A block is its raw length, its stored length (the same when it is stored as it is) and its bytes. A file lives in one process, so the codec is not
 * recorded: the one that compresses a block is the one that reads it back.
 */
#include "postgres.h"
#include "storage/buffile.h"
#include "col/codec.h"
#include "col/colspool.h"
#include "col/compress.h"
#include "col/rowfile.h"
#include "col/rowpack.h"
#include "core/varint.h"

#define ROWFILE_VARINT_LEN	10

struct SiftRowFile
{
	const SiftColSchema *sc;
	Size		block;			/* the raw bytes a block holds: the rows that make it pass this */
	MemoryContext cxt;			/* the memory of the file: the block is made again in it when the file is read */
	BufFile    *file;			/* NULL: the rows are only counted */
	StringInfo	scratch;
	StringInfoData blk;			/* the rows of the block being filled, or the block being read */
	uint64		bytes;			/* the blocks that went out */
	const char *at;				/* reading: the next row of blk, and the end of the rows it holds */
	const char *end;
	bool		reading;
	bool		versions;		/* each row is preceded by the xmin and cmin of its version */
};

static int
rowfile_alg(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_spool_layer == 4)
		return SIFT_CC_ALG_NONE;
#endif
	return sift_colz_available(SIFT_CC_ALG_LZ4) ? SIFT_CC_ALG_LZ4 : sift_colz_available(SIFT_CC_ALG_ZSTD) ? SIFT_CC_ALG_ZSTD : SIFT_CC_ALG_NONE;
}

SiftRowFile *
sift_rowfile_create(const SiftColSchema *sc, StringInfo scratch, bool file, Size block, bool versions)
{
	SiftRowFile *f = palloc0(sizeof(SiftRowFile));

	f->sc = sc;
	f->block = block;
	f->versions = versions;
	f->cxt = CurrentMemoryContext;
	f->scratch = scratch;
	f->file = file ? BufFileCreateTemp(false) : NULL;
	initStringInfo(&f->blk);
	return f;
}

static void
flush_block(SiftRowFile *f)
{
	uint32		hdr[2] = {f->blk.len, f->blk.len};
	const char *data = f->blk.data;
	int			alg = rowfile_alg();

	if (f->blk.len == 0)
		return;
	if (alg != SIFT_CC_ALG_NONE)
	{
		Size		cap = sift_colz_bound(alg, f->blk.len),
					clen;

		enlargeStringInfo(f->scratch, cap);
		clen = sift_colz_compress(alg, 0, f->blk.data, f->blk.len, f->scratch->data, cap);
		if (clen > 0 && clen < f->blk.len)
		{
			hdr[1] = (uint32) clen;
			data = f->scratch->data;
		}
	}
	if (f->file != NULL)
	{
		BufFileWrite(f->file, hdr, sizeof(hdr));
		BufFileWrite(f->file, data, hdr[1]);
	}
	f->bytes += sizeof(hdr) + hdr[1];
	resetStringInfo(&f->blk);
}

void
sift_rowfile_put(SiftRowFile *f, const SiftColRow *row, uint32 xmin, uint32 cmin)
{
	if (f->versions)
	{
		enlargeStringInfo(&f->blk, 2 * ROWFILE_VARINT_LEN);
		f->blk.len = (uint8 *) sift_put_varint((uint8 *) f->blk.data + f->blk.len, xmin) - (uint8 *) f->blk.data;
		f->blk.len = (uint8 *) sift_put_varint((uint8 *) f->blk.data + f->blk.len, cmin) - (uint8 *) f->blk.data;
	}
	sift_colrow_pack(&f->blk, f->sc, row);
	if (f->blk.len >= f->block)
		flush_block(f);
}

uint64
sift_rowfile_bytes(SiftRowFile *f)
{
	return f->bytes;
}

void
sift_rowfile_seal(SiftRowFile *f)
{
	flush_block(f);
	pfree(f->blk.data);
	f->blk.data = NULL;
}

static bool
next_block(SiftRowFile *f)
{
	uint32		hdr[2];
	int			alg = rowfile_alg();

	if (BufFileReadMaybeEOF(f->file, hdr, sizeof(hdr), true) == 0)
		return false;
	enlargeStringInfo(&f->blk, hdr[0]);
	if (hdr[1] == hdr[0])
		BufFileReadExact(f->file, f->blk.data, hdr[0]);
	else
	{
		enlargeStringInfo(f->scratch, hdr[1]);
		BufFileReadExact(f->file, f->scratch->data, hdr[1]);
		SIFT_INVARIANT(sift_colz_decompress(alg, f->scratch->data, hdr[1], f->blk.data, hdr[0]), "sift_col build: a block of a key range does not decompress");
	}
	f->at = f->blk.data;
	f->end = f->blk.data + hdr[0];
	return true;
}

bool
sift_rowfile_get(SiftRowFile *f, SiftColRow *row, uint32 *xmin, uint32 *cmin)
{
	if (!f->reading)
	{
		MemoryContext old = CurrentMemoryContext;

		if (f->bytes == 0)
			return false;
		SIFT_INVARIANT(BufFileSeek(f->file, 0, 0, SEEK_SET) == 0, "sift_col build: cannot rewind the file of a key range");
		MemoryContextSwitchTo(f->cxt);
		initStringInfo(&f->blk);
		MemoryContextSwitchTo(old);
		f->reading = true;
	}
	if (f->at == f->end && !next_block(f))
		return false;
	if (f->versions)
	{
		uint64		x;

		f->at = (const char *) sift_get_varint64_n((const uint8 *) f->at, (const uint8 *) f->end, &x);
		*xmin = (uint32) x;
		f->at = (const char *) sift_get_varint64_n((const uint8 *) f->at, (const uint8 *) f->end, &x);
		*cmin = (uint32) x;
	}
	f->at = sift_colrow_unpack(f->at, f->sc, row);
	return true;
}

void
sift_rowfile_close(SiftRowFile *f)
{
	if (f->file != NULL)
		BufFileClose(f->file);
	if (f->blk.data != NULL)
		pfree(f->blk.data);
	pfree(f);
}
