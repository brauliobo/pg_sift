/*
 * col/gpu.h - what the Vulkan path of the aggregate scan (docs/GPU.md) needs of the column engine of format major 2: the identity a part's chunk is cached by on the device, whether the
 * rows of a part may be read without a test of each row, and a stored chunk handed to the device as it is. The device decodes the chunk itself (LZ4, constant, frame of reference, delta, run
 * lengths, dictionary, decimal and split-bits doubles, the nulls of the chunk), so nothing is decompressed or unpacked here.
 */
#ifndef SIFT_COL_GPU_H
#define SIFT_COL_GPU_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

#include "agg/vk.h"
#include "col/f2/part_vis.h"
#include "col/granule/granule.h"

/* A part is immutable, a rewrite is a new part_gen and TRUNCATE a new relation file: a cached chunk is never stale */
extern SiftVkKey sift_gpu_chunk_key(const RelFileLocator *loc, uint32 part_id, uint64 part_gen, uint32 phys, uint32 granule);

/* The leaves whose chunks the device decodes: integers (a numeric of at most 18 digits, a boolean) and floats */
extern bool sift_gpu_leaf_ok(SiftCellKind kind, uint8 hint);

/*
 * Whether the immutable part can be read for the snapshot on the device: false with the reason for a level 0 part. *cls is how its rows are seen by the snapshot (col/f2/part_vis.h):
 * SIFT_VIS_PER_ROW leaves the decision to its granules (sift_gpu_granule_eligible). The deleters of a part (its deletion vector, the pending deletes of its partition) are no reason to
 * refuse it: the device is handed the rows the snapshot sees deleted as a mask (agg/gpu_dead.h). The column deltas of its partition are decided by the plan (agg/gpu_f2_plan.c), by the
 * columns the query reads.
 */
extern bool sift_gpu_part_eligible(const SiftPartDesc *d, FullTransactionId snap_xmin, SiftVisClass *cls, const char **why);

/* The granule of a part whose inserters are not all settled needs no test of each row when they are settled for the snapshot: its deleters are in the mask of dead rows whatever they are */
extern bool sift_gpu_granule_eligible(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId snap_xmin);

/*
 * The chunk of column phys of the granule as stored, for the device: bytes point into the record of the granule, which must live as long as the chunk is used. z is the zone of the column in the
 * granule (the field of an integer is its distance from the least value of the zone). False, and why, for a chunk the device does not take: an encoding it has no decoder for, a wrapper that
 * is not LZ4, exceptions (values the column could not hold), a column that is not an integer or a float.
 */
extern bool sift_gpu_chunk_fill(const SiftGranuleView *v, int phys, SiftCellKind kind, const SiftZone *z, SiftVkChunk *out, const char **why);

/*
 * The blob of the codec (from its magic, blen bytes) as the device reads it: when a node of it is wrapped in zstd, which the device does not unwrap, the blob with the node replaced by what it
 * holds is appended to out (empty out: the blob is read as it is). False, and why, for a node the device does not take.
 */
extern bool sift_gpu_blob_unwrap(const uint8 *blob, uint32 blen, StringInfo out, const char **why);

#endif							/* SIFT_COL_GPU_H */
