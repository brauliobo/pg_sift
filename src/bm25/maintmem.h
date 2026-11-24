/*
 * bm25/maintmem.h - the memory a maintenance step of a BM25 index may take
 */
#ifndef SIFT_BM25_MAINTMEM_H
#define SIFT_BM25_MAINTMEM_H

#include "core/memtrack.h"

/* maintenance_work_mem, or autovacuum_work_mem in an autovacuum worker that has one, shared by nparticipants processes */
extern Size sift_bm25_maint_bytes(int nparticipants);

#endif							/* SIFT_BM25_MAINTMEM_H */
