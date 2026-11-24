/*
 * col/f2/f2_gbuf.h - the part pin and the decoded granule of the shared Format 2.0 cache, for sift_col and sift_table alike (merged into col/f2/f2_cache.h at integration).
 *
 * A pin holds a part opened once (header, granule directory, TID index, dictionaries, deletion vector) and no Buffer; a granule buffer is reference counted by the scan cursor and by
 * every slot that points into it, and is freed when the last reference goes.
 */
#ifndef SIFT_F2_GBUF_H
#define SIFT_F2_GBUF_H

#include "postgres.h"

#include "storage/relfilelocator.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_store.h"
#include "col/f2/part_format.h"
#include "col/granule/granule.h"

typedef struct SiftF2PartPin
{
	RelFileLocator locator;
	uint64		part_gen;
	uint32		part_id;
	uint32		refcnt;
	bool		stale;			/* its relation was invalidated while it was held: dropped when the last holder lets go */
	bool		uncached;		/* not in the cache (forgotten while held, or the pin of a micro-slab a standby made): the last release deletes it */
	uint64		lru_tick;
	MemoryContext cx;
	MemoryContext dv_cx;
	struct PinEntry *entry;		/* its place in the cache of pins (f2_gbuf.c): a release finds it without a search */
	struct SiftGranIntervalIdx *gidx;	/* the granules by their addresses, made when a lookup first meets a part of many granules (f2_locate.c); lives in cx */

	SiftPart   *part;
	SiftPartDesc desc;
	SiftGranuleDesc *gdir;
	SiftTidIndexEntry *tidx;
	const SiftPartDicts *dicts;
	struct SiftF2ColMap *col_map;	/* NULL when the part has the schema of the store */
	uint64		tid_min;
	uint64		tid_max;

	uint64		dv_gen;
	uint32		dv_len;			/* the bytes of the vector the pin decoded, delta tail included: a delta changes them and not dv_gen */
	SiftDv	   *dv;

	/* What a scan reads of the part before a granule (f2_part_pin_scan.c), read when first asked for */
	SiftVariantExcSummary *exc;
	int			nexc;
	bool		exc_read;
	SiftZone  **zones;			/* per leaf of the part, the zones of its granules; NULL: not read */
	struct SiftPointDir *point;	/* SEC_POINT: the header of the point directory, NULL when the part has none */
	bool		point_read;
	struct SiftTidDir *tdir;	/* the granule of every address of the part, made when a lookup finds the envelopes of no use (f2_tid_dir.c); lives in cx */
	bool		tdir_denied;	/* the budget had no room for it: the lookups use the envelopes */
	struct SiftRescanPart *rescan;	/* the granules an ordered scan decoded (scan/rescan_cache.c) */
} SiftF2PartPin;

typedef struct SiftF2GranuleBuf
{
	uint32		refcnt;
	MemoryContext cx;
	const SiftF2Store *store;
	SiftF2PartPin *pin;

	uint32		granule_id;
	uint16		nrows;
	uint16		ncols;

	char	   *rec;
	SiftGranuleView view;
	SiftGranuleDesc gdesc;
	uint64	   *tids;
	SiftRowVer *vers;
	TransactionId single_xmin;
	CommandId	single_cmin;
	bool		is_single_xact;
	bool		is_all_frozen;

	uint16		point_reads;	/* the cells read alone from a granule of the point cache (sift_f2_gbuf_cell) */
	bool	   *col_decoded;
	int64	  **col_i;
	double	  **col_f;
	SiftStrCell **col_s;
	uint8	  **col_tag;
	bool	  **col_isnull;

	/* What the host lays over a column as it is decoded: for a table the column deltas its scan's snapshot sees on this granule; NULL for none (one branch in the decode) */
	void		(*patch) (struct SiftF2GranuleBuf *gb, int phys);
	const void *patch_arg;

	/* A granule of the point cache (sift_f2_gbuf_load_lazy): only the chunks asked for are read */
	SiftLazyGranule *lazy;		/* NULL: rec holds every chunk */
	bool		tids_sorted;	/* the addresses do not descend, set where they are read */
	uint16	   *tid_ht;			/* when they do: the rows by address, plus one (open addressing), made at the first lookup */
	uint32		tid_ht_mask;
	bool		cached;
} SiftF2GranuleBuf;

/* What a TRUNCATE of a table made in the transaction leaves: the file stays and its generations start again. The pins and the address index of the file are dropped (the ones a lookup or a granule holds go when it is done) */
extern void sift_f2_gbuf_forget(Relation rel);

/* The part of the descriptor opened once (cached under sift.backend_cache_kb) and given back; the pin of a granule is held until it is released */
extern SiftF2PartPin *sift_f2_pin_open(Relation rel, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id);
extern void sift_f2_pin_release(SiftF2PartPin *pin);

/* The addresses of the rows of granule g of the pin's part, ascending as the granule keeps them, in out (at most SIFT_GRANULE_ROWS of them); reads the address chunk only, in the current memory context */
extern void sift_f2_pin_granule_tids(Relation rel, SiftF2PartPin *pin, uint32 g, uint64 *out);


extern SiftF2GranuleBuf *sift_f2_gbuf_load(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g);
extern SiftF2GranuleBuf *sift_f2_gbuf_ref(SiftF2GranuleBuf *gb);
extern void sift_f2_gbuf_unref(SiftF2GranuleBuf *gb);
extern const uint64 *sift_f2_gbuf_tids(SiftF2GranuleBuf *gb);

/* The first row of the granule that has the address, or -1: a binary search when the granule's addresses ascend, else a table of the rows by address */
extern int	sift_f2_gbuf_row_of(SiftF2GranuleBuf *gb, uint64 tid);

/* The first row with the address after row `after` (-1: from the first), or -1: the rows of an address VACUUM gave to a later row are walked one by one */
extern int	sift_f2_gbuf_row_next(SiftF2GranuleBuf *gb, uint64 tid, int after);

/* Column phys of the granule decoded (once): integers, floats or string cells, and for string columns that may hold wide values their tags */
extern void sift_f2_gbuf_decode(SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, bool wide);

/*
 * The granule of the point cache: the header and the chunk table only, then _tid; the other chunks are read by sift_f2_gbuf_column when a row asks for a column of them, so a lookup
 * of one row reads the blocks of the columns it returns, not those of the granule. parent holds the buffer (the cache's context, or one of the statement).
 */
extern SiftF2GranuleBuf *sift_f2_gbuf_load_lazy(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g, MemoryContext parent);
extern void sift_f2_gbuf_column(Relation rel, SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, bool wide);

/* A cell of a column of the granule: the value of its kind (i, f, or s), whether it is NULL, and for a column that may hold exceptions the tag of the row */
typedef struct SiftF2Cell
{
	bool		isnull;
	uint8		tag;
	int64		i;
	double		f;
	SiftStrCell s;
} SiftF2Cell;

/*
 * The cell of row of column phys, in the current memory context. A granule of the point cache (sift_f2_gbuf_load_lazy) that has not decoded the column gives it from the column's chunk alone, a
 * row at a time, until SIFT_F2_POINT_READS cells have been read this way, and decodes the column whole after that, as a granule that is not lazy does at once. True when it decoded the column whole.
 */
#define SIFT_F2_POINT_READS	64
extern bool sift_f2_gbuf_cell(Relation rel, SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, uint16 row, bool wide, SiftF2Cell *cell);

/* The bytes of the buffer and what it holds, for the budget of the cache */
extern Size sift_f2_gbuf_bytes(const SiftF2GranuleBuf *gb);

/* The row at an address, if it is in the manifest read now: its granule (a reference the caller gives back with sift_f2_gbuf_unref), the row in it and its partition */
typedef struct SiftF2Loc
{
	SiftPartitionEntry entry;
	SiftF2GranuleBuf *gb;
	uint16		row;
} SiftF2Loc;

/* The row as the relation has it: the rows the transaction staged are not looked for and its stage is not written */
extern bool sift_f2_locate_settled(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc);

/* The same in the parts the manifest lists, without writing the stage or the slabs: a row they hold is not found */
extern bool sift_f2_locate_published(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc);

/*
 * Calls cb for every granule that may hold an address of [tid_lo, tid_hi], with the partition of its part; cb returns true to stop. The parts are found by the envelope of the
 * addresses in their descriptors (SiftPartDesc.tid_min, tid_max): the ones that cannot hold an address are not opened, and a lookup of one address on parts that do not overlap
 * is O(log parts). Of an opened part the granules are told by the TID index of the part and its bloom.
 */
typedef bool (*SiftF2GranuleVisit) (SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, void *arg);
extern void sift_f2_visit_granules(Relation rel, SiftF2Store *store, uint64 tid_lo, uint64 tid_hi, SiftF2GranuleVisit cb, void *arg);

/* The same, the granule of each visit made by load (a reference the visit gives back): the point cache's, which reads a granule's chunks as they are asked for */
typedef SiftF2GranuleBuf *(*SiftF2GranuleLoader) (Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 g);
extern void sift_f2_visit_granules_via(Relation rel, SiftF2Store *store, uint64 tid_lo, uint64 tid_hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg);

/* The row at an address, each granule made by load (a reference in loc->gb); false when no part or slab holds it */
extern bool sift_f2_locate_via(Relation rel, SiftF2Store *store, uint64 tid, SiftF2GranuleLoader load, SiftF2Loc *loc);

/* Where the row at an address lies in the parts as published, as a key whose high bits tell its part and whose low bits its granule (UINT64_MAX: in no part, a row of a slab): sorting addresses by it puts those of one granule together */
extern uint64 sift_f2_locate_order(Relation rel, SiftF2Store *store, uint64 tid);

/* The manifest as of now with the slabs written out as parts: valid until mcxt is reset */
struct SiftPlanSnapshot;
extern const struct SiftPlanSnapshot *sift_f2_view(Relation rel, SiftF2Store *store, MemoryContext mcxt);

#endif							/* SIFT_F2_GBUF_H */
