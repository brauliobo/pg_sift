/*
 * col/rowfile.h - rows in a temporary file: the rows of a build by key ranges (build/spill.c). A row is packed as col/rowpack.c writes it; the rows
 * go back to back into blocks, and a block is compressed when the build has a codec and it makes the block smaller.
 */
#ifndef SIFT_COL_ROWFILE_H
#define SIFT_COL_ROWFILE_H

#include "lib/stringinfo.h"
#include "col/enc.h"

typedef struct SiftRowFile SiftRowFile;

/*
 * A file of rows of the schema, in the current resource owner's temporary files; scratch is the buffer the blocks are compressed into, shared by the
 * files of one caller. With file false no file is made and the rows are only counted: sift_rowfile_bytes says what a file of them would take.
 */
extern SiftRowFile *sift_rowfile_create(const SiftColSchema *sc, StringInfo scratch, bool file);
extern void sift_rowfile_put(SiftRowFile *f, const SiftColRow *row);
/* The bytes the file holds, or would hold, so far (the block being filled counts as it is) */
extern uint64 sift_rowfile_bytes(SiftRowFile *f);
/* Ends the writes: the last block goes out and the memory of the blocks is freed */
extern void sift_rowfile_seal(SiftRowFile *f);
/*
 * Reads the next row of a sealed file into row (arrays of ncols, allocated by the caller): its strings stay in the block, valid until the next call, its arrays
 * of elements are in the current context; false at the end. The first call reads from the start.
 */
extern bool sift_rowfile_get(SiftRowFile *f, SiftColRow *row);
extern void sift_rowfile_close(SiftRowFile *f);

#endif							/* SIFT_COL_ROWFILE_H */
