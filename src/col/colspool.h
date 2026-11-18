/*
 * col/colspool.h - rows spooled to a temporary file as columnar chunks
 * A spool is the rows of one key range of a build, in the order the table gave them. Every chunk_rows rows (or sooner, when the memory of the spool passes its share) they are encoded
 * as a unit of the column engine, by the cascade codecs and, when it pays on a sample, a general-purpose layer, and appended to the file; the strings of a spool go through one
 * dictionary of the spool, written when it is sealed. A sealed spool is read back a chunk at a time, decoded onto the end of a batch. The format is private to the process that wrote it.
 * A plan fixes what the spools of one build share: the chunk size, the memory each may hold, the codec policy; it is made from the rows a build holds, so that the memory of all
 * the spools together stays within the share the build gives them, and its price per row is what the refusal of a build that cannot fit its files reads.
 */
#ifndef SIFT_COL_COLSPOOL_H
#define SIFT_COL_COLSPOOL_H

#include "nodes/pg_list.h"
#include "col/enc.h"

typedef struct SiftChunkPlan SiftChunkPlan;
typedef struct SiftChunkSpool SiftChunkSpool;

/* The rows of one spool a plan is made from: enough for the largest chunk and the layers priced on it */
#define SIFT_CHUNKPLAN_ROWS	(4 * 16384)

#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_col_spool_layer;		/* 0: the plan picks, 1: the cascade alone, 2: lz4 layer, 3: zstd layer, 4: no compression at all */
extern PGDLLIMPORT int sift_test_col_spool_chunk_rows;	/* 0: from the memory share, else this many rows of a chunk */
extern PGDLLIMPORT int sift_test_col_spool_share_kb;	/* 0: the share the build gives, else the memory all the spools of a build may hold, in kB */
extern PGDLLIMPORT int sift_test_col_spool_unbounded;	/* 1: a chunk is as large as it can be and no memory limit flushes it (a mutant of the share) */
extern PGDLLIMPORT int sift_test_col_spool_used_rows;	/* the chunk rows the last plan chose (0: the rows went to the row file) */
extern PGDLLIMPORT int sift_test_col_spool_est_bytes;	/* the bytes the last plan priced the files of the build at */
#endif

/*
 * The plan of nspools spools that share `share` bytes of memory, from the n (at most SIFT_CHUNKPLAN_ROWS) rows ref[] of one of them, in arrival order (what the spool will take: its first rows are what the plan measures,
 * encodes and prices); NULL when the share does not hold a chunk of a few hundred rows for each spool: the rows are then better written as packed rows
 */
extern SiftChunkPlan *sift_chunkplan_make(const SiftColSchema *sc, const SiftColRowRef *ref, uint32 n, int nspools, Size share);
extern uint32 sift_chunkplan_rows(const SiftChunkPlan *p);
/* The bytes a row takes in the file, as the sample of rows said */
extern double sift_chunkplan_row_bytes(const SiftChunkPlan *p);
/* "cascade", "lz4" or "zstd": the layer the chunks have over the cascade codecs */
extern const char *sift_chunkplan_layer(const SiftChunkPlan *p);

/* A spool whose memory is a child of parent; the chunks go to a temporary file */
extern SiftChunkSpool *sift_chunkspool_create(MemoryContext parent, SiftChunkPlan *p);
/* ref[0..n): rows of batches of the schema, appended a leaf at a time; a chunk that fills is written */
extern void sift_chunkspool_add_rows(SiftChunkSpool *s, const SiftColRowRef *ref, uint32 n);
extern void sift_chunkspool_seal(SiftChunkSpool *s);
/* The bytes of the file: the chunks written, then (sealed) the dictionaries */
extern uint64 sift_chunkspool_bytes(const SiftChunkSpool *s);
/* Decodes the next chunk of a sealed spool onto the end of dst (keyed, not sorted); false at the end. The first call reads from the start */
extern bool sift_chunkspool_read_into(SiftChunkSpool *s, SiftColBatch *dst);
extern void sift_chunkspool_close(SiftChunkSpool *s);

#endif
