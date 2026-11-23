/*
 * col/part_format.h - an immutable part of format major 2: one SIFT_PT_PARTHDR page (the header, the table of sections and the extents of the stream), the SIFT_PT_EXTLIST
 * pages when the extents do not fit it, and a byte stream in SIFT_PT_DATA pages.
 *
 * The table of sections is a list of (tag, flags, crc, offset, length) sorted by tag, so a reader finds what it needs and skips the rest: a section it does not know
 * is ignored unless it is marked required, and then the part is refused. The sections replace the fixed slots, the version ladders and the trailers of the formats before.
 */
#ifndef SIFT_PART_FORMAT_H
#define SIFT_PART_FORMAT_H

#include "core/host.h"

#include "core/f2/io_stream.h"
#include "core/f2/manifest.h"

#define SIFT_PARTHDR_MAGIC		0x50484432U		/* "PHD2" */
#define SIFT_PARTHDR_VERSION	1

typedef struct SiftSecDirEntry
{
	uint16		tag;			/* SiftSectionTag */
	uint16		flags;
	uint32		crc32c;			/* of the payload; 0 with SIFT_SEC_FLAG_NO_CRC */
	uint64		off;			/* in the stream (in the payload of the header page with SIFT_SEC_FLAG_INLINE) */
	uint64		len;
} SiftSecDirEntry;

StaticAssertDecl(sizeof(SiftSecDirEntry) == 24, "SiftSecDirEntry must be 24 bytes");

#define SIFT_SEC_FLAG_REQUIRED	(UINT16_C(1) << 0)	/* a reader that does not know the tag refuses the part */
#define SIFT_SEC_FLAG_INLINE	(UINT16_C(1) << 1)
#define SIFT_SEC_FLAG_COMPRESSED (UINT16_C(1) << 2)
#define SIFT_SEC_FLAG_NO_CRC	(UINT16_C(1) << 3)	/* a large stream section whose granules carry their own sums */

typedef enum SiftSectionTag
{
	SEC_SCHEMA = 0x0001,
	SEC_GRANULE_DIR = 0x0002,
	SEC_GRANULES = 0x0003,
	SEC_DICTS = 0x0004,

	SEC_VERS = 0x0100,
	SEC_TID_INDEX = 0x0101,
	SEC_DELVEC = 0x0102,
	SEC_GRANULE_TSTRIDE = 0x0110,	/* 256-row SiftTimeStride[] per granule */
	SEC_VARIANT_EXC = 0x0111,	/* the (granule, column) pairs that keep an exception stream */

	SEC_ZONES = 0x0200,
	SEC_PREAGG = 0x0201,
	SEC_PREAGG_SUPER = 0x0202,
	SEC_PREAGG_ROLLUP = 0x0203,
	SEC_PREAGG_EXCEPT = 0x0204,
	SEC_SETS = 0x0205,
	SEC_HLL = 0x0206,
	SEC_KTAB = 0x0207,
	SEC_RUNS = 0x0208,
	SEC_POINT = 0x0209,
	/* 0x020A is retired and never written */
	SEC_SKIP_INDEX = 0x020B,	/* per-granule skipping indexes */
	SEC_KLL = 0x020C,			/* quantile sketches */
	SEC_KEY_MARKS = 0x020D,		/* the first and the last key of each granule, as the order preserving image of the key (col/f2/key_marks.h, "KMRK"); optional */
	SEC_COL_STATS = 0x020E,		/* the bytes, the distinct estimate and the dominant encoding of each column, which seed the order of the filters ("CSTA"); optional */

	SEC_LEAF_STATS = 0x0300,
	SEC_INLINE_META = 0x0301,
	SEC_CONN_META = 0x0303,
	SEC_VSTORE_REFS = 0x0304,
	SEC_SCHEMA_DEFAULTS = 0x0305,	/* sift_table: the fast defaults and the dropped attributes the part was written under (col/f2/f2_defaults.h); optional */

	SEC_SCOPE_HDR = 0x0401,
	SEC_SCOPE_DICT = 0x0402,
	SEC_SCOPE_L0_SLICE = 0x0403,
	SEC_SCOPE_L1 = 0x0404,
	SEC_SCOPE_TOPK = 0x0405,
	SEC_SCOPE_MONTHMAP = 0x0406,
	SEC_SCOPE_L2 = 0x0407,
	SEC_SCOPE_KEYDIR = 0x0408,
	SEC_SCOPE_ENT_ROUTE = 0x0409	/* entity to primary parent routing */
} SiftSectionTag;

#define SIFT_PART_FLAG_UNFROZEN	(UINT32_C(1) << 0)	/* some inserter is not frozen: SEC_VERS holds the transactions of the granules that are not of one transaction and command */
#define SIFT_PART_FLAG_SCOPE	(UINT32_C(1) << 1)
#define SIFT_PART_FLAG_ROOTS	(UINT32_C(1) << 2)
#define SIFT_PART_FLAG_UNKEYED	(UINT32_C(1) << 3)
#define SIFT_PART_FLAG_L0_MICRO	(UINT32_C(1) << 4)	/* level 0 only */
#define SIFT_PART_FLAG_BUILT	SIFT_PART_FLAG_L0_MICRO	/* level 1 and up: written whole by CREATE INDEX, a compaction does not merge it with its neighbours by count */
#define SIFT_PART_FLAG_TIME_HOT	(UINT32_C(1) << 5)	/* output of the hot side of a compaction: every row of it is newer than the hot watermark */
#define SIFT_PART_FLAG_TIME_COLD (UINT32_C(1) << 6)	/* output of the cold side: the rows older than the watermark (late arrivals, history) */
#define SIFT_PART_FLAG_HAS_TIME0 (UINT32_C(1) << 7)	/* SiftPartDesc.time_min[0] and time_max[0] are valid */
#define SIFT_PART_FLAG_HAS_TIME1 (UINT32_C(1) << 8)	/* SiftPartDesc.time_min[1] and time_max[1] are valid */
/* Of the descriptor in the manifest only (the header of a part is immutable): sift_part_try_seal sets them without rewriting a page of the part */
#define SIFT_PART_FLAG_ALL_VISIBLE (UINT32_C(1) << 9)	/* every inserter of the part is frozen or known committed: that it is older than a snapshot says it is there for it */
#define SIFT_PART_FLAG_DV_SETTLED (UINT32_C(1) << 10)	/* every deleter of the deletion vector is committed (SIFT_DV_FLAG_ALL_SETTLED): its rows are dead for the snapshots past dv_max_xid */
#define SIFT_PART_FLAG_PACKED_SLOT (UINT32_C(1) << 11)	/* of the descriptor only: the part is a slot of a SIFT_PT_PACKED_PART page, the slot index in bits 12..15 */
#define SIFT_PART_PACK_SLOT_SHIFT 12
#define SIFT_PART_PACK_SLOT_MASK (UINT32_C(0xF) << SIFT_PART_PACK_SLOT_SHIFT)
#define SIFT_PART_PACK_SLOT(flags) (((flags) & SIFT_PART_PACK_SLOT_MASK) >> SIFT_PART_PACK_SLOT_SHIFT)
#define SIFT_PART_PACK_FLAGS(slot) (SIFT_PART_FLAG_PACKED_SLOT | (((uint32) (slot) & 0xF) << SIFT_PART_PACK_SLOT_SHIFT))
/* Of the header only: every section is inline and the part has no data page, its bytes are those of the header page (or of a slot of a packed page) */
#define SIFT_PART_FLAG_INLINE_STREAM (UINT32_C(1) << 16)

typedef struct SiftPartHeaderData
{
	uint32		magic;
	uint16		version;
	uint16		nsec;
	uint32		crc32c;
	uint32		flags;

	uint64		part_gen;
	uint32		part_id;
	uint16		level;
	uint16		ncols;

	uint64		nrows;
	uint64		stream_bytes;
	uint32		ngranules;
	uint32		granule_rows;

	FullTransactionId min_xid;
	FullTransactionId max_xid;

	uint32		total_extents;
	uint32		ninline_extents;
	BlockNumber extlist_blk;
	uint32		inline_meta_off;
	uint32		inline_meta_len;
	uint32		secdir_off;
	uint32		extents_off;
	uint32		pad;
} SiftPartHeaderData;

StaticAssertDecl(sizeof(SiftPartHeaderData) == 104, "SiftPartHeaderData must be 104 bytes");

/* Bytes [off, off + n) of a section whose payload is not in memory, written to out; called in ascending order, each byte once */
typedef void (*SiftSecFill) (void *arg, uint64 off, uint64 n, char *out);

/*
 * What a writer gives for a section: the payload, and whether it goes in the header page. A section that is large and made of what is already stored (the granules of parts that are joined) is
 * given as fill, which the writer calls for one page at a time while it writes the stream: nothing of it is held in memory, and its checksum is made on the way. It does not go in the header page.
 */
typedef struct SiftSecInput
{
	uint16		tag;
	uint16		flags;			/* REQUIRED, COMPRESSED, NO_CRC */
	const void *data;
	uint64		len;
	bool		inline_in_header;
	SiftSecFill fill;
	void	   *fill_arg;
} SiftSecInput;

typedef struct SiftPartIn
{
	uint32		part_id;
	uint64		part_gen;
	uint16		level;
	uint16		ncols;
	uint32		flags;
	uint64		nrows;
	uint32		ngranules;
	uint32		granule_rows;
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	const SiftSecInput *sections;
	int			nsections;
	int			inline_extent_cap;	/* the extents kept in the header page; < 0: as many as fit */
	uint32		max_extent_blocks;	/* the most blocks one extent of the stream takes; 0: the largest class */
	bool		bulk_tail;		/* the pages of the stream past the end of the file are written with smgr_bulk_write (core/extlock.h): a part that is written once, whole, by one backend */
} SiftPartIn;

/*
 * The next parts this backend writes take the blocks of their streams from these extents first (the allocator gives the rest): what moves a part to blocks below its own, which it asked
 * sift_fsm_alloc_below for. sift_part_release_preferred gives back what the writes did not use.
 */
extern void sift_part_prefer_extents(Relation rel, const SiftExtent *ext, int n);
extern void sift_part_release_preferred(Relation rel);

/* One block of the preferred extents for a page of the manifest; false when none is preferred (the allocator is then asked) */
extern bool sift_part_take_preferred_page(Relation rel, BlockNumber *blk);

/* The extents preferred for the relation file are forgotten, not released: the file they are blocks of is gone (core/f2/fsm_cache.c) */
extern void sift_part_discard_preferred(const RelFileLocator *locator);

/* Writes the stream, the extent list and the header page of a part; fills in the descriptor of everything but the sort key range and returns the header block in it */
extern SiftPartDesc sift_part_write(Relation rel, const SiftPartIn *in);

/*
 * A part of at most SIFT_PACKED_SLOT_MAX_BYTES with every section inline is a slot of a SIFT_PT_PACKED_PART page: the bytes of its header page payload, kept next to those of up to 15 others
 * (sift_compact_pack_tails). The page is written once, sealed, and never changed; it is released when no descriptor of the manifest names it.
 */
#define SIFT_PACKED_MAGIC			UINT32_C(0x5346504B)
#define SIFT_PACKED_VERSION			1
#define SIFT_PACKED_MAX_SLOTS		16
#define SIFT_PACKED_SLOT_MAX_BYTES	2000
#define SIFT_PACKED_MIN_BATCH		2

typedef struct SiftPackedSlot
{
	uint64		part_gen;
	uint32		part_id;
	uint16		off;			/* from SiftPageGetData(page), MAXALIGNed */
	uint16		len;
} SiftPackedSlot;

typedef struct SiftPackedPageData
{
	uint32		magic;
	uint16		version;
	uint16		nslots;
	uint32		crc32c;			/* at SIFT_PAGE_CRC_OFF */
	uint16		used_end;
	uint16		reserved;
	SiftPackedSlot slots[SIFT_PACKED_MAX_SLOTS];
} SiftPackedPageData;

StaticAssertDecl(sizeof(SiftPackedSlot) == 16, "SiftPackedSlot must be 16 bytes");
StaticAssertDecl(offsetof(SiftPackedPageData, crc32c) == SIFT_PAGE_CRC_OFF, "SiftPackedPageData.crc32c must sit at SIFT_PAGE_CRC_OFF");
StaticAssertDecl(sizeof(SiftPackedPageData) == 272, "SiftPackedPageData must be 272 bytes");

/* An opened part: the header, the table of sections, the extents (inline and indirect) and where each page of the stream starts */
typedef struct SiftPart
{
	SiftPartHeaderData hdr;
	SiftSecDirEntry *secdir;
	SiftExtent *extents;
	uint64	   *pagestart;		/* the first page of the stream in each extent */
	BlockNumber header_blk;
	BlockNumber *extlist_pages;
	int			nextlist_pages;
	char	   *inline_payload;	/* the payload area of the header page (the bytes of the slot of a packed part), for the sections kept in it */
	uint32		inline_len;		/* the bytes of inline_payload in use */
	bool		is_packed;
	uint8		pack_slot;
	struct SiftTierRemote *remote;	/* a part in object storage (SIFT_PART_EXT_REMOTE): the stream is read from the object; there are no extents */
	struct SiftPartDicts *dicts;	/* SEC_DICTS decoded (col/granule.c), once dicts_read */
	bool		dicts_read;
	void	   *key_marks;		/* SEC_KEY_MARKS opened (col/f2/f2_marks.c), once key_marks_read; NULL: the part has none */
	bool		key_marks_read;
	SiftIoMode	io_mode;		/* how the reads that span pages are streamed: SIFT_IOS_FULL_SEQ unless the opener (a compaction) says otherwise */
} SiftPart;

/* Reads and checks the header page of a part and its extent list; ERROR for a section the build does not know that is marked required */
extern SiftPart *sift_part_open(Relation rel, BlockNumber header_blk, uint64 part_gen, uint32 part_id);

/* The part sift_part_write would write, kept in memory and written nowhere (a standby makes the part of a micro-slab so, for the statement that reads it); d gets what sift_part_write fills, with no header block */
extern SiftPart *sift_part_in_memory(Relation rel, const SiftPartIn *in, SiftPartDesc *d);

/* The same for a part named by its descriptor: a packed part finds its slot by the index the flags hold, a part in object storage (SIFT_PART_EXT_REMOTE) is opened from its object */
extern SiftPart *sift_part_open_desc(Relation rel, const SiftPartDesc *d, uint32 part_id);

/* The bytes of the header page payload of a part with every section inline, which a slot of a packed page keeps as they are (NULL for a part with a data page) */
extern char *sift_part_inline_image(Relation rel, const SiftPartDesc *d, uint32 part_id, uint32 *len);

/* A part in object storage (col/f2/f2_tier_host.c): the image of its header page and the reader of its stream; ERROR when the object cannot be had; the bytes [off, off + len) of the stream */
extern void sift_tier_host_open(Relation rel, const SiftPartDesc *d, char **image, uint32 *image_len, struct SiftTierRemote **remote);
extern void sift_tier_host_read(Relation rel, struct SiftTierRemote *remote, uint64 off, uint64 len, void *dst);

typedef struct SiftPartRef
{
	uint32		part_id;
	uint64		part_gen;
} SiftPartRef;

/*
 * Whether no descriptor of the manifest names the packed page once the parts leaving it are gone (what releases it with the commit that drops its last part). A slot whose partition is no longer
 * in the manifest counts as named: the page is kept, never released under a part that may still use it.
 */
extern bool sift_packed_unreferenced(Relation rel, BlockNumber blk, const SiftPartRef *leaving, int nleaving);

/* A batch of parts whose bytes are written as one packed page: the slot of item i is i */
typedef struct SiftPackItem
{
	uint64		part_gen;
	uint32		part_id;
	const char *image;
	uint16		len;
} SiftPackItem;

/* The block of the sealed page that holds the items (at most SIFT_PACKED_MAX_SLOTS of at most SIFT_PACKED_SLOT_MAX_BYTES, all together within a page) */
extern BlockNumber sift_pack_write(Relation rel, const SiftPackItem *items, int n);

/* How many of the items, from the first, a packed page holds */
extern int	sift_pack_fit(const SiftPackItem *items, int n);

extern bool sift_section_known(uint16 tag);

/* The block of page p of the stream of an opened part */
extern BlockNumber sift_part_page_block(const SiftPart *part, uint64 page);
extern const SiftSecDirEntry *sift_part_section(const SiftPart *part, uint16 tag);

/* The payload of a section (a copy; its checksum is verified unless NO_CRC); NULL when the part has none */
extern void *sift_part_read_section(Relation rel, const SiftPart *part, uint16 tag, uint64 *len);

/* len bytes of the stream from byte off: through a stream of the blocks when they span pages (core/io_stream.h) */
extern void sift_part_read_bytes(Relation rel, const SiftPart *part, uint64 off, uint64 len, void *dst);

/* Bytes of the stream and where they go in the buffer the caller gives */
typedef struct SiftByteRange
{
	uint64		off;
	uint32		len;
	uint32		dst_off;
} SiftByteRange;

/* The ranges read together: every block they touch is read once, by one stream, and each range copied from it into dst_base + dst_off */
extern void sift_part_read_ranges(Relation rel, const SiftPart *part, const SiftByteRange *ranges, int n, char *dst_base, SiftIoMode mode);

/* The blocks the part owns, for the release of a dropped part: header page, extent-list pages and the extents of the stream (none for a packed part or a part in object storage) */
extern SiftExtent *sift_part_blocks(const SiftPart *part, int *n);

#endif							/* SIFT_PART_FORMAT_H */
