/*
 * col/dictshm/int.h - the layout of the shared table of parsed dictionaries
 * the table, its entries and records, and the functions the test hooks call
 * the directory and the ring of records are described in col/dictshm/dictshm.c
 */
#ifndef SIFT_COL_DICTSHM_INT_H
#define SIFT_COL_DICTSHM_INT_H

#include "port/atomics.h"
#include "storage/lwlock.h"

#include "col/dictshm/dictshm.h"

#define DSHM_BUCKETS	64
#define DSHM_WAYS		8
#define DSHM_FLIGHT		8
/* places tried for a record before the write is given up: the ring is held by readers */
#define DSHM_TRIES		16
#define DSHM_MAGIC		0x44534833
/* the names carry the layout: the control table (the directory, small) and the ring of records, which is made at the size the control table records */
#define DSHM_NAME		"sift_col_dict_5"
#define DSHM_RING_NAME	"sift_col_dict_ring_5"
/* the smallest record worth sharing: under it the segment is as cheap to read */
#define DSHM_MIN_BYTES	(128 * 1024)
#define DSHM_WINDOW		16
#define DSHM_HOLD		64
#define DSHM_ALIGN		64
/* the reads and misses are counted by the process number of the reader: a counter is on the line of no other reader */
#define DSHM_COUNTS		32
/* a record read this many writes ago is read again as new: the oldest of a full bucket goes, so a read writes the tick of its entry once in a while */
#define DSHM_TICK_LAG	64
#define DSHM_TRANCHE	"SiftDictShm"

typedef enum
{
	E_FREE,
	E_LIVE,
	E_STALE						/* replaced by a newer record of its key, still read by the backends that pin it */
} EntState;

typedef struct DshmEnt
{
	/* MEM[MEM-SHM-14]: DSM shared dict dir entry (88 B, was 96; 512 entries: -4 KB; DictKey has no padding) */
	pg_atomic_uint64 orows;		/* rows of top-N scans compared with its strings, by all backends and statements (SiftCDict.orows): the sorted order is worth making at DICT_ORDER_ROWS times its entries */
	DictKey		key;
	Size		off,
				len;
	pg_atomic_uint64 tick;		/* oldest of a bucket goes when it is full */
	pg_atomic_uint32 asks;		/* times it was read, by all backends: a dictionary asked often is worth its hash table and sorted order (dict.c) */
	pg_atomic_uint32 pins;		/* backends reading the record in place: its bytes are not overwritten while there are any */
	uint32		hdr_flags;		/* the flags of the header of its record, as the writer that looks for a record with the arrays it has to make reads them */
	uint8		state;
}			DshmEnt;
StaticAssertDecl(sizeof(DshmEnt) == 88, "DshmEnt is 88 bytes");

typedef struct DshmFlight
{
	Size		off,
				len;			/* a record being copied in: len 0 when the place is free */
	DictKey		key;			/* whose record it is, with the arrays of flags: a backend that misses the same one meanwhile does not copy it again */
	uint32		flags;
}			DshmFlight;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) DshmCount
{
	pg_atomic_uint64 hits,		/* reads that found a record, and that did not: since the table was made (sift.shared_memory) */
				misses;
} DshmCount;

/*
 * The ring_lock is the writers': the ring, its index of places, the flights and the window. The bucket_locks guard the entries of a bucket: a reader that pins a record holds its
 * bucket shared, a writer that enters or evicts one holds it exclusive (and the ring_lock before it, a bucket at a time); a pin is given back without a lock.
 */
typedef struct Dshm
{
	LWLockPadded ring_lock;
	LWLockPadded bucket_lock[DSHM_BUCKETS];
	int			tranche;
	Size		size;			/* of the ring: what the backend that made this table took from sift.col_dict_shared_cache_kb, whatever the setting of the others */
	bool		ring_ready;		/* the ring of records is made: a parallel worker, which cannot make it, joins it */
	Size		head;
	pg_atomic_uint64 tick;		/* the writes so far */
	uint32		hold;			/* writes still skipped */
	uint32		writes,
				thrown;			/* of the window */
	uint64		window_hits;	/* the reads that found a record, when the window began */
	DshmFlight	flight[DSHM_FLIGHT];
	DshmEnt		dir[DSHM_BUCKETS * DSHM_WAYS];
	int			nord;
	int16		ord[DSHM_BUCKETS * DSHM_WAYS];	/* the entries that hold a place of the ring, by the place: no two overlap; one that went free stays until its place is taken again */
	DshmCount	count[DSHM_COUNTS] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
}			Dshm;

/*
 * A record, the arrays 8-aligned after the header: off[n + 1], the strings, the hash slots when the flag says so (nslots, as cdict_ready makes them), the
 * codes in byte order (n). Readers use them where they lie.
 */
typedef struct DshmHdr
{
	uint32		magic;
	uint32		n;
	uint32		nbytes;
	uint32		flags;
	uint32		nslots;
	DictKey		key;			/* whose record it is: a directory entry may name a place that another record took */
}			DshmHdr;

#define F_BLOCKED	1
#define F_SORTED	2			/* sorted is the stored order of the blocks form */
#define F_SLOTS		4
#define F_ORDER		8			/* sorted is there, stored or built */

typedef struct Layout
{
	Size		bytes_at,
				slots_at,
				order_at,
				total;
}			Layout;

extern Dshm  *dshm_get(void);
extern bool ring_get(Dshm *s);
extern Layout layout(uint32 n, uint32 nbytes, uint32 flags, uint32 nslots);
extern void ord_drop(Dshm *s, int slot);
extern bool ring_take(Dshm *s, Size len, Size *off, int *flight, int *evicted);
extern DshmEnt *bucket(Dshm *s, const DictKey *k);
extern uint64 dshm_hits(Dshm *s);
extern void ent_set(DshmEnt *e, const DictKey *k, Size off, Size len, uint32 hdr_flags, uint32 asks, uint64 orows, uint64 tick);

#endif
