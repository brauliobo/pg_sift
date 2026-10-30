/*
 * col/dictshm_int.h - the layout of the shared table of parsed dictionaries
 * the table, its entries and records, and the one function the test hooks call
 * the directory and the ring of records are described in col/dictshm.c
 */
#ifndef SIFT_COL_DICTSHM_INT_H
#define SIFT_COL_DICTSHM_INT_H

#include "storage/spin.h"

#include "col/dictshm.h"

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

typedef enum
{
	E_FREE,
	E_LIVE,
	E_STALE						/* replaced by a newer record of its key, still read by the backends that pin it */
} EntState;

typedef struct DshmEnt
{
	uint8		state;
	uint32		asks;			/* times it was read, by all backends: a dictionary asked often is worth its hash table and sorted order (dict.c) */
	uint64		orows;			/* rows of top-N scans compared with its strings, by all backends and statements (SiftCDict.orows): the sorted order is worth making at DICT_ORDER_ROWS times its entries */
	int32		pins;			/* backends reading the record in place: its bytes are not overwritten while there are any */
	DictKey		key;
	Size		off,
				len;
	uint64		tick;			/* oldest of a bucket goes when it is full */
}			DshmEnt;

typedef struct DshmFlight
{
	Size		off,
				len;			/* a record being copied in: len 0 when the place is free */
}			DshmFlight;

typedef struct Dshm
{
	slock_t		mutex;
	uint64		tick;
	Size		size;			/* of the ring: what the backend that made this table took from sift.col_dict_shared_cache_kb, whatever the setting of the others */
	bool		ring_ready;		/* the ring of records is made: a parallel worker, which cannot make it, joins it */
	Size		head;
	uint32		hold;			/* writes still skipped */
	uint32		writes,
				thrown,
				found;			/* of the window */
	DshmFlight	flight[DSHM_FLIGHT];
	DshmEnt		dir[DSHM_BUCKETS * DSHM_WAYS];
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
extern bool ring_take(Dshm *s, Size len, Size *off, int *flight);
extern DshmEnt *bucket(Dshm *s, const DictKey *k);

#endif
