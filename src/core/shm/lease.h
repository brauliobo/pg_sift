/*
 * core/shm/lease.h - what a writer of a relation takes from Block 0 in advance, so that the commit of a part asks Block 0 for nothing but its own manifest record.
 */
#ifndef SIFT_SHM_LEASE_H
#define SIFT_SHM_LEASE_H

#include "postgres.h"

#ifdef SIFT_TESTING
extern uint64 sift_lease_block0_x_locks;
#define SIFT_NOTE_BLOCK0_X()	(sift_lease_block0_x_locks++)
#else
#define SIFT_NOTE_BLOCK0_X()	((void) 0)
#endif

#endif							/* SIFT_SHM_LEASE_H */
