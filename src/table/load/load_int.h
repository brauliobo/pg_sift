/*
 * table/load/load_int.h - what the files of the bulk load share: the load and its run, and the functions one file calls from another
 */
#ifndef SIFT_TABLE_LOAD_INT_H
#define SIFT_TABLE_LOAD_INT_H

#include "access/xact.h"
#include "core/parbuild.h"
#include "table/seg_int.h"

/*
 * A load takes whole superblocks of TIDs (a group owns its superblocks), LOAD_SBS at a time: its rows are encoded one superblock per unit and
 * the units of a group become one segment. The rows wait in the backend as records in a buffer (a write buffer's bytes per unit) until a run is
 * full, then the segments of its groups are written and published in one go, under the lock that serializes the creations of segments (what
 * the orphan sweep of a VACUUM waits for: the blocks written are in no directory until then), so a load adds a directory entry every few
 * hundred thousand rows and the merges of the writers keep the directory as they do for the groups of the promotions. A run is one group,
 * or one for each participant of a parallel context (the backend and the workers max_parallel_maintenance_workers allows) that write the
 * groups at the same time: the transposition of the rows and the encoding are most of what a load costs besides the parse of its input.
 * The rows past the last full run are written when the command ends (finish_bulk_insert), or at the latest before the transaction commits,
 * so a row never outlives its transaction in memory.
 * A load that has no workers to give a run to (stream) buffers no rows: a unit is encoded into the writer of the open segment when it is full, the
 * segment goes on over the runs while their superblocks follow each other and ends at a memory budget, and the segments are published when the
 * command ends. It leaves the few large segments that the merge of the many small ones of the runs would make, and that merge costs about as
 * many instructions as writing the segments did.
 */
#define LOAD_SBS		8
#define LOAD_RING_UNITS	4
/*
 * The ring writes its groups without the lock of the active segment, so the writers of the table can take the room they need in the directory before
 * the segments are published: the ring runs while the directory has this many entries free besides the segments it holds, else the load goes on in the
 * way that keeps the lock for the groups of a run
 */
#define LOAD_RING_ROOM	32
#define ring_room(st, held)		((st)->dir.nsegs + (held) + LOAD_RING_ROOM <= sift_tbl_max_segments - 2)
#define LOAD_SB_ROWS	((uint64) SIFT_TBL_VBLOCKS_SB * SIFT_TBL_MAX_OFF)
/* The backend and three workers: the buffer of a run is LOAD_MAX_GROUPS * LOAD_SBS write buffers */
#define LOAD_MAX_GROUPS	4
#define LOAD_MAX_UNITS	(LOAD_SBS * LOAD_MAX_GROUPS)
/*
 * A run is shared between the participants of a parallel context from this many bytes of rows (the start of the workers, about 15 ms, then
 * costs a fifth of the encoding they take over), and every one gets two units at least
 */
#define LOAD_PARALLEL_BYTES	((Size) 16 << 20)
#define LOAD_MIN_UNITS	2
/* sift.test_load_unit_bytes (a testing build) takes the place of the write buffer limit as the bytes of a unit and sets the parallel start at its multiples, so that a small load reaches the ring */
#ifdef SIFT_TESTING
#define load_budget()	(sift_test_load_segment_kb > 0 ? (Size) sift_test_load_segment_kb * 1024 : Max((Size) maintenance_work_mem * 1024 / 2, (Size) 64 << 20))
#define load_unit_bytes(ld)	(sift_test_load_unit_bytes > 0 ? (Size) sift_test_load_unit_bytes * 1024 : (Size) (ld)->unit_kb * 1024)
#define load_parallel_bytes()	(sift_test_load_unit_bytes > 0 ? (Size) sift_test_load_unit_bytes * 1024 * LOAD_SBS : LOAD_PARALLEL_BYTES)
#else
#define load_budget()	Max((Size) maintenance_work_mem * 1024 / 2, (Size) 64 << 20)
#define load_unit_bytes(ld)	((Size) (ld)->unit_kb * 1024)
#define load_parallel_bytes()	LOAD_PARALLEL_BYTES
#endif

/* A row in the buffer of a run: this header, then the tuple in heap format (the fields of the transaction unset), each record at a MAXALIGN */
typedef struct LoadRec
{
	uint32		len;			/* of the header and the tuple */
	CommandId	cid;
} LoadRec;

/* A run as the writers of its groups read it: group g is the units from g * per, unit u is urows[u] records from uoff[u] of the buffer of the rows, of the superblock of vblock first + u * SIFT_TBL_VBLOCKS_SB */
typedef struct LoadRun
{
	SiftParCommon common;		/* in shared memory: the table */
	bool		vers;			/* the rows carry their xmin (xid) and cmin; false: frozen */
	TransactionId xid;
	uint64		first;
	int			units;
	int			per;			/* units of a group: LOAD_SBS, fewer when the units are shared out between the participants */
	int			limit_kb;		/* the write buffer limit of the table, which the participants use as the leader read it */
	uint32		urows[LOAD_MAX_UNITS];
	Size		uoff[LOAD_MAX_UNITS + 1];
} LoadRun;

typedef struct Load
{
	Oid			relid;
	SubTransactionId subid;		/* the (sub)transaction whose xid the rows carry */
	bool		refused;		/* the table keeps the write buffers (sift_tbl_bulk_refusal) */
	int			unit_kb;		/* the write buffer limit of the table: a unit stops at its bytes */
	bool		open;			/* a run of superblocks is reserved */
	MemoryContext cxt;
	TupleDesc	desc;
	Datum	   *flat;			/* the values of a row with its external values read in */
	StringInfoData rows;		/* the records of the run */
	LoadRun		run;
	int			maxunits;		/* units of a run: LOAD_SBS for each group written at once */
	uint32		n;				/* rows of the current unit */
	Size		bytes;			/* their size in row format: a unit stops at the bytes of a write buffer */
	bool		ring_ok;		/* the rows come from COPY: the workers may outlive a run */
	bool		noring;
	struct LoadRingLeader *rl;	/* the ring of the workers that write the groups while the command reads on */
	bool		stream;			/* no workers: a unit is encoded into the open segment as it fills, no buffer of rows, a segment ends at the memory budget (load.c) */
	SealWriter	sw;
	MemoryContext rowcx;
	Size		budget;			/* memory the writer of a segment may hold */
	uint64		seg_next;		/* the vblock after the last unit of the open segment */
	SiftSegEntry *segs;			/* the segments written and not yet in the directory */
	int			nsegs;
	int			maxsegs;
} Load;

/* The segments of the groups of a run, in the order of their superblocks */
typedef struct Published
{
	const SiftSegEntry *segs;
	int			n;
} Published;

bool		load_row_work_unsafe_in_parallel_mode(Relation rel);

void		load_end_unit(Load *ld);

void		load_publish_groups(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg);
int			load_lock_with_room(Relation rel, SiftTblRel *st, int n);
void		load_publish_all(Relation rel, SiftTblRel *st, Published *pub);
void		load_write_group(const LoadRun *r, const char *rows, Relation rel, int g, SiftSegEntry *seg);
bool		load_write_parallel(const LoadRun *r, const char *rows, Relation rel, int ngroups, SiftSegEntry *segs);
int			load_share_units(LoadRun *r, Size bytes, int participants);

void		load_ring_take_slot(Load *ld, Relation rel);
void		load_ring_hand_over(Load *ld, Relation rel, SiftTblRel *st, bool last);
bool		load_ring_start(Load *ld, Relation rel, SiftTblRel *st);
void		load_ring_stop(Load *ld, Relation rel, SiftTblRel *st);

#endif
