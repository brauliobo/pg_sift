/*
 * core/f2/f2_vm_xlog.h - the visibility map of a sift_table and the resource manager that logs it.
 *
 * A sift_table has the map PostgreSQL's index-only scan reads (VISIBILITYMAP_FORKNUM), indexed by the virtual block number of a row (vblock, never the page of the main fork). The
 * map is set and cleared by this code only: visibilitymap_set() would log XLOG_HEAP2_VISIBLE, whose redo sets PD_ALL_VISIBLE in the main fork block of the same number, and that block is
 * a page of the manifest or of a part. The records here touch the map fork only. A set carries the newest transaction the bits make visible to everyone, so that the replay on a hot standby
 * ends the queries whose snapshot might not see it, as heap_xlog_visible does; a clear needs no such horizon.
 *
 * The resource manager can be registered only by a preloading server: without it nothing sets a bit and the planner is told that no row is all visible.
 */
#ifndef SIFT_F2_VM_XLOG_H
#define SIFT_F2_VM_XLOG_H

#include "postgres.h"

#include "access/visibilitymapdefs.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

#define SIFT_RMGR_ID				142
#define SIFT_RMGR_NAME				"pg_sift_table_vm"
#define SIFT_XLOG_VM_SET			0x10	/* bits set on one page of the map, with the horizon the standby must not hold a snapshot below */
#define SIFT_XLOG_VM_CLEAR			0x20	/* the bits of one vblock cleared, before the delete or the update that makes them untrue is logged */

#define SIFT_VM_MAPSIZE				(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define SIFT_VM_BLOCKS_PER_BYTE		(BITS_PER_BYTE / BITS_PER_HEAPBLOCK)
#define SIFT_VM_BLOCKS_PER_PAGE		(SIFT_VM_MAPSIZE * SIFT_VM_BLOCKS_PER_BYTE)
#define SIFT_VM_WORDS_PER_PAGE		(SIFT_VM_MAPSIZE / sizeof(uint64))
#define SIFT_VM_BLOCKS_PER_WORD		(SIFT_VM_BLOCKS_PER_BYTE * sizeof(uint64))
#define SIFT_VM_PAGE_OF(vblock)		((BlockNumber) ((vblock) / SIFT_VM_BLOCKS_PER_PAGE))

typedef struct SiftXlVmSet
{
	RelFileLocator locator;
	BlockNumber vm_blkno;			/* the page of the map fork */
	TransactionId snapshotConflictHorizon;	/* the newest transaction the bits hide from no snapshot; invalid: none to wait for */
	uint16		nwords;				/* the 64-bit words of the page that change */
	uint8		flags;				/* VISIBILITYMAP_ALL_VISIBLE, with VISIBILITYMAP_ALL_FROZEN where a bit of that kind is set */
	uint8		pad;
	/* then uint16 word_idx[nwords], then uint64 bit_or[nwords]: the bytes of a word in the order of the bytes of the page */
} SiftXlVmSet;

StaticAssertDecl(sizeof(SiftXlVmSet) == 24, "SiftXlVmSet must be 24 bytes");
StaticAssertDecl(offsetof(SiftXlVmSet, snapshotConflictHorizon) == 16, "the horizon of SiftXlVmSet sits at byte 16");

typedef struct SiftXlVmClear
{
	RelFileLocator locator;
	BlockNumber vblock;				/* the virtual block whose bits are cleared */
	uint8		flags;				/* VISIBILITYMAP_VALID_BITS */
	uint8		pad[3];
} SiftXlVmClear;

StaticAssertDecl(sizeof(SiftXlVmClear) == 20, "SiftXlVmClear must be 20 bytes");

/* From _PG_init: registers the resource manager where the library is preloaded */
extern void sift_vm_xlog_init(void);

/* Whether the server logs the map: it was preloaded, so the records can be replayed here and on a standby that preloads too */
extern bool sift_vm_rmgr_active(void);

/* The bits of the vblocks of one page of the map, as the page holds them: word i covers vblocks SIFT_VM_BLOCKS_PER_WORD * i and up */
extern uint64 sift_vm_page_word(Page page, int word);

/* ORs the words into the exclusively locked page of the map, logs them with the horizon (permanent relations) and unlocks; false where nothing changed (the buffer is unlocked all the same) */
extern bool sift_vm_apply_set(Relation rel, Buffer vmbuf, uint8 flags, TransactionId horizon, const uint16 *word_idx, const uint64 *bit_or, int nwords);

/* Clears the bits of the vblock in the page of the map, logging it (permanent relations) before the caller logs the change they stood for; false where no bit was set */
extern bool sift_vm_apply_clear(Relation rel, Buffer vmbuf, BlockNumber vblock);

#endif							/* SIFT_F2_VM_XLOG_H */
