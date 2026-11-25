/*
 * core/spool.h - a byte spool that is memory until it outgrows its share of maintenance_work_mem, then a temporary file.
 * The small sections a segment writer produces out of order (dictionary, live rows, field lengths, INCLUDE records) are written
 * and read once, front to back: they cost no temporary file unless the segment is large.
 */
#ifndef SIFT_CORE_SPOOL_H
#define SIFT_CORE_SPOOL_H

#include "core/host.h"

typedef struct SiftSpool SiftSpool;

/* An empty spool in the current memory context */
extern SiftSpool *sift_spool_begin(void);
extern void sift_spool_write(SiftSpool *sp, const void *data, Size len);
extern uint64 sift_spool_len(const SiftSpool *sp);
/* The next read starts at the first byte, and the spool is not written to after */
extern void sift_spool_rewind(SiftSpool *sp);
/* The next len bytes; fewer than len left is an error */
extern void sift_spool_read(SiftSpool *sp, void *dst, Size len);
extern void sift_spool_close(SiftSpool *sp);

#endif
