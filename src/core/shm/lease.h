/*
 * core/shm/lease.h - what a writer of a relation takes from Block 0 in advance, so that the commit of a part asks Block 0 for nothing but its own manifest record.
 *
 * A slot of a table in dynamic shared memory belongs to one relation file. It leases the generations of parts in chunks of SIFT_LEASE_GEN_CHUNK and the virtual blocks of a sift_table in chunks of
 * SIFT_LEASE_VBLOCK_CHUNK: the backends take their numbers from a chunk with one compare-and-swap, and the one that finds the chunk used up asks Block 0 for the next, once, under the lock of the slot.
 * The numbers a slot hands out grow with time, as the ones Block 0 handed out one by one did, so the parts of a partition stay in the order of their generations. A slot that is given to
 * another relation, or a server that restarts, loses what is left of its chunks: a number is never used twice and never promised twice.
 *
 * The slot also holds up to SIFT_LEASE_PAGES pages of the metadata slabs that a writer took in one record of the allocator root (Block 1, no Block 0) and has not used yet. They are allocated
 * and named by nothing: whoever looks for the pages that nothing names (the orphan sweep, the check of the blocks) first gives them back (sift_lease_page_drain). The record names the window they
 * were taken from (core/f2/fsm/fsm.h, pool_lo and pool_mask), so that the pages a restart or a crash takes from the pool are not taken for a leak by the check of the blocks and are freed by the sweep.
 *
 * Where the table cannot be had (no room in the shared memory) every call asks the owner of the numbers, one request at a time.
 */
#ifndef SIFT_SHM_LEASE_H
#define SIFT_SHM_LEASE_H

#include "postgres.h"

#include "storage/block.h"
#include "storage/relfilelocator.h"

#define SIFT_LEASE_SLOTS			2048
#define SIFT_LEASE_PROBES			8
#define SIFT_LEASE_GEN_CHUNK		1024
#define SIFT_LEASE_VBLOCK_CHUNK		4096
#define SIFT_LEASE_PAGES			32

typedef enum SiftLeaseKind
{
	SIFT_LEASE_GEN,
	SIFT_LEASE_VBLOCK
} SiftLeaseKind;

/* Reserves `want` numbers in the owner (Block 0, logged) and returns the first; *got of them (at most want) are reserved, and an ERROR where there are none */
typedef uint64 (*SiftLeaseGrow) (void *arg, uint64 want, uint64 *got);

/*
 * A run of numbers of the kind for the relation file: the first of them. It holds n, or fewer (*got, at least min) when the chunk has no more and the next one is not worth waiting for; what is
 * left of a chunk that holds less than min is given up. grow reserves a chunk in the owner when the slot has none that holds min, or where there is no slot.
 */
extern uint64 sift_lease_take(SiftLeaseKind kind, const RelFileLocator *loc, uint64 n, uint64 min, SiftLeaseGrow grow, void *arg, uint64 *got);

/* The next virtual block the slot of the file hands out (every row of the file is below it); UINT64_MAX where the slot has no chunk: the next_vblock of the owner is then the bound */
extern uint64 sift_lease_vblock_cursor(const RelFileLocator *loc);

/* A page of the pool of the file, false when it has none */
extern bool sift_lease_page_pop(const RelFileLocator *loc, BlockNumber *blk);
/*
 * True when the pool is empty and this backend is to fill it (false: it holds pages, someone else is filling it, or there is no slot); a call that returns true is followed by sift_lease_page_fill. *lost is
 * true when the pool was never filled since its slot was claimed: what an earlier pool of the file held (a restart, a crash or another file's use of the slot took it from the shared memory) is not known to it.
 */
extern bool sift_lease_page_room(const RelFileLocator *loc, bool *lost);
/* Puts the pages taken for the pool in it and ends the filling (n may be 0: it is given up); the pages that did not fit are not kept, the number that were is returned */
extern int	sift_lease_page_fill(const BlockNumber *blks, int n);
/* Takes every page of the pool of the file out (blks holds SIFT_LEASE_PAGES); how many */
extern int	sift_lease_page_drain(const RelFileLocator *loc, BlockNumber *blks);

/* Where a writer locks Block 0 exclusively (counted by the testing library: sift._test_lease_block0_x) */
typedef enum SiftBlock0Site
{
	SIFT_B0_COUNTER,
	SIFT_B0_FSM,
	SIFT_B0_VBLOCK,
	SIFT_B0_PUBLISH,
	SIFT_B0_SITES
} SiftBlock0Site;

#ifdef SIFT_TESTING
extern uint64 sift_lease_block0_x_locks[SIFT_B0_SITES];
/* Gives up what is left of the chunk of vblocks of the file, so that the next run is asked of Block 0 (the caller holds the relation in AccessExclusiveLock: no writer takes from the chunk meanwhile) */
extern void sift_lease_vblock_forget(const RelFileLocator *loc);
#define SIFT_NOTE_BLOCK0_X(site)	(sift_lease_block0_x_locks[site]++)
#else
#define SIFT_NOTE_BLOCK0_X(site)	((void) 0)
#endif

#endif							/* SIFT_SHM_LEASE_H */
