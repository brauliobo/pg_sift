/*
 * col/v1/rowfile.h - rows in a temporary file: the rows of a build by key ranges (build/spill.c). A row is packed as col/v1/rowpack.c writes it; the rows
 * go back to back into blocks, and a block is compressed when the build has a codec and it makes the block smaller.
 */
#ifndef SIFT_COL_ROWFILE_H
#define SIFT_COL_ROWFILE_H

#include "lib/stringinfo.h"
#include "col/v1/enc.h"

typedef struct SiftRowFile SiftRowFile;

/* Where the blocks go: nowhere (the rows are only counted), a temporary file, or memory (the blocks stay in the memory context the file was created in until they are read back) */
typedef enum SiftRowSink
{
	SIFT_ROWS_COUNT,
	SIFT_ROWS_FILE,
	SIFT_ROWS_MEMORY
} SiftRowSink;

/*
 * A file of rows of the schema, in the current resource owner's temporary files; scratch is the buffer the blocks are compressed into, shared by the
 * files of one caller. A block goes out when it holds block raw bytes, so the file holds up to twice that in memory. With file false no file is made and the
 * rows are only counted: sift_rowfile_bytes says what a file of them would take. With versions each row is preceded by the xmin and cmin of its version (the rows of a
 * build have none: false, and 0 to put).
 */
extern SiftRowFile *sift_rowfile_create(const SiftColSchema *sc, StringInfo scratch, SiftRowSink sink, Size block, bool versions);
#define SIFT_ROWFILE_BLOCK_MAX	((Size) 32 << 10)
#define SIFT_ROWFILE_BLOCK_MIN	((Size) 1 << 10)	/* the least a block holds: under it the codec gains nothing */
extern void sift_rowfile_put(SiftRowFile *f, const SiftColRow *row, uint32 xmin, uint32 cmin);
/* The bytes the file holds, or would hold, so far (the block being filled counts as it is) */
extern uint64 sift_rowfile_bytes(SiftRowFile *f);
/* Ends the writes: the last block goes out and the memory of the blocks is freed */
extern void sift_rowfile_seal(SiftRowFile *f);
/*
 * Reads the next row of a sealed file into row (arrays of ncols, allocated by the caller): its strings stay in the block, valid until the next call, its arrays
 * of elements are in the current context; false at the end. The first call reads from the start.
 */
extern bool sift_rowfile_get(SiftRowFile *f, SiftColRow *row, uint32 *xmin, uint32 *cmin);
extern void sift_rowfile_close(SiftRowFile *f);

#endif							/* SIFT_COL_ROWFILE_H */
