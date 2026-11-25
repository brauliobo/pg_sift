/*
 * core/f2/proj.h - secondary projections of a relation of format major 2 (SIFT_PT_PROJDIR = 44): the pages of the projection directory and what they hold.
 *
 * A projection is a second, derived copy of the rows of the relation: SIFT_PROJ_SORT keeps them in another sort order (and may keep a subset of the columns), SIFT_PROJ_AGG keeps one
 * summary row per group of the rows of one base part (a cube of counts, sums, minima and maxima). Both are Format 2.0 parts, written by the same writer as the parts of the relation.
 *
 * The metapage names the head of a chain of SIFT_PT_PROJDIR pages (proj_dir_blk) that holds the SiftManifestProjRef of every projection in ascending proj_id. A ref names the projection's
 * own two-level directory of parts (partdir_root_blk: a SIFT_PT_PARTDIR leaf of one partition whose SIFT_PT_PARTLIST chain holds the SiftPartDesc of the projection's parts) and the chain
 * of SIFT_PT_PROJDIR pages with its SiftProjLink entries (link_blk): the base part each projection part was derived from and the state of that part's deletion vector it reflects. All of
 * them are copy on write and swapped with the commit of the manifest, so a reader that has block 0 has a directory that matches the parts of the relation it names.
 *
 * Freshness is never stored as a flag that a writer has to flip: a link says "this projection part was derived from base part src_gen at deletion vector generation src_dv_gen", and a
 * reader compares that to the descriptor of the base part it holds. A base part that was rewritten, dropped or given another deletion vector no longer matches its link, so the reader
 * takes the base part and not the projection part; a maintenance step replaces the link. A stale answer is not representable.
 */
#ifndef SIFT_PROJ_H
#define SIFT_PROJ_H

#include "core/host.h"

#include "core/f2/manifest/manifest.h"
#include "core/page.h"

#define SIFT_PROJDIR_MAGIC		0x50524A44U		/* "PRJD" */
#define SIFT_PROJ_NAME_LEN		48
#define SIFT_PROJ_ID_MAX		65534
#define SIFT_PROJ_PART_ID		1		/* the part_id of the one partition of a projection: the pages of its parts carry it */
#define SIFT_PROJ_MAX			SIFT_PROJ_ID_MAX

typedef enum SiftProjKind
{
	SIFT_PROJ_SORT = 1,			/* a secondary physical sort order: ORDER BY c1, c2, ... [INCLUDE (...)] */
	SIFT_PROJ_AGG = 2			/* a materialized pre-aggregate: GROUP BY k1, ... MEASURES count(*), sum(m1), ... */
} SiftProjKind;

typedef enum SiftProjState
{
	SIFT_PROJ_BUILDING = 0,		/* the first parts are being written: no reader uses it */
	SIFT_PROJ_READY = 1,
	SIFT_PROJ_STALE = 2			/* the definition no longer fits the relation (a dropped column): kept for DROP, never read */
} SiftProjState;

/* One projection as the directory keeps it */
typedef struct SiftManifestProjRef
{
	uint16		proj_id;		/* 1..SIFT_PROJ_ID_MAX */
	uint8		kind;			/* SiftProjKind */
	uint8		state;			/* SiftProjState */
	uint32		flags;
	char		name[SIFT_PROJ_NAME_LEN];	/* NUL-terminated, unique within the relation */
	BlockNumber partdir_root_blk;	/* the SIFT_PT_PARTDIR leaf of the projection's parts */
	uint32		partdir_level;	/* 0: the root is a leaf */
	uint32		npartitions;	/* 1: a projection is one partition */
	uint32		nparts;			/* the parts of that partition */
	uint64		manifest_gen;	/* generation of the pages of the projection's directory */
	uint64		synced_version;	/* the manifest version of the commit that last wrote the projection */
	uint64		total_rows;
	uint64		total_bytes;
	uint32		spec_hash;		/* CRC32C of the canonical spec in sift.projection_defs */
	BlockNumber link_blk;		/* the head of the chain of links, or InvalidBlockNumber */
	uint32		nlinks;
	uint32		pad;
	uint64		link_gen;		/* generation of the pages of the chain of links */
} SiftManifestProjRef;

StaticAssertDecl(sizeof(SiftManifestProjRef) == 128, "SiftManifestProjRef must be 128 bytes");

/* The flags of a ref: what the spec of the projection asks of the executors (all in SiftManifestProjRef.flags, which was 0) */
#define SIFT_PROJREF_FLAG_PARTIAL	(UINT32_C(1) << 0)	/* the spec has a WHERE: the projection holds the rows that satisfy it */
#define SIFT_PROJREF_FLAG_DISTINCT	(UINT32_C(1) << 1)	/* an AGG spec that keeps a distinct-value column as its last group key (count(DISTINCT)) */
#define SIFT_PROJREF_FLAG_FILTERED	(UINT32_C(1) << 2)	/* an AGG spec with a measure that has a FILTER */

/* A projection part and the base part it reflects (see the header of this file); src_gen is the key of the chain */
#define SIFT_PROJLINK_INVERTIBLE	(UINT32_C(1) << 0)	/* an AGG part that holds no minimum or maximum: a dead row of the base part may be subtracted from its cells */
#define SIFT_PROJLINK_HAS_WHERE_EXC	(UINT32_C(1) << 1)	/* a row of the base part held an exception tag on a column of a WHERE or FILTER: the cells cannot be trusted to have judged it */

typedef struct SiftProjLink
{
	uint64		src_gen;		/* part_gen of the base part */
	uint64		proj_gen;		/* part_gen of the projection part */
	uint64		src_dv_gen;		/* dv_gen of the base part when the projection part was written; 0: none */
	uint32		src_ndead;		/* its dead rows then */
	uint32		flags;			/* SIFT_PROJLINK_* */
} SiftProjLink;

StaticAssertDecl(sizeof(SiftProjLink) == 32, "SiftProjLink must be 32 bytes");

#define SIFT_PROJDIR_FLAG_LINKS		(UINT16_C(1) << 0)	/* the page holds SiftProjLink of the projection in the special space's part_id, not refs */

/* One page of the directory: refs in ascending proj_id, or the links of one projection in ascending src_gen; the next page of the chain after them */
typedef struct SiftProjDirPageData
{
	uint32		magic;
	uint16		nentries;
	uint16		flags;			/* SIFT_PROJDIR_FLAG_* */
	uint32		crc32c;
	BlockNumber next_blk;		/* the next SIFT_PT_PROJDIR page of the chain, or InvalidBlockNumber */
	uint64		manifest_version;
	uint64		proj_dir_gen;	/* the generation of the chain: every page of it carries it */
	char		entries[FLEXIBLE_ARRAY_MEMBER];
} SiftProjDirPageData;

StaticAssertDecl(offsetof(SiftProjDirPageData, entries) == 32, "SiftProjDirPageData header is 32 bytes");

#define SIFT_PROJDIR_REF_CAP	((SIFT_PAGE_PAYLOAD - offsetof(SiftProjDirPageData, entries)) / sizeof(SiftManifestProjRef))
#define SIFT_PROJDIR_LINK_CAP	((SIFT_PAGE_PAYLOAD - offsetof(SiftProjDirPageData, entries)) / sizeof(SiftProjLink))

StaticAssertDecl(SIFT_PROJDIR_REF_CAP == 63, "a projection directory page holds 63 refs");

/* The parts of a projection as its directory keeps them, with the links of the parts to the base parts; both ascending by part_gen / src_gen */
typedef struct SiftProjParts
{
	SiftPartDesc *parts;
	int			nparts;
	SiftProjLink *links;
	int			nlinks;
} SiftProjParts;

/*
 * The change of one projection as a delta of its parts and links, applied by the commit of the manifest to the lists it reads under block 0's version (so that the commits of two partitions
 * that both reflect into the projection never overwrite each other): parts leave by part_gen, parts are added, the links of base parts that left or were rewritten go by src_gen and the
 * new links come. Every part has exactly one link and every link one part when the delta is applied. create gives the ref of a new projection; otherwise the ref is the one in the directory.
 */
typedef struct SiftProjDelta
{
	uint16		proj_id;
	bool		create;
	SiftManifestProjRef ref;	/* create: kind, state, flags, name, spec_hash; unused otherwise */
	bool		set_state;
	uint8		state;
	bool		move_if_present;	/* a move of a part to other blocks: when the projection, the part or its link is gone by the commit, the directory is left as it is */
	bool	   *move_applied;	/* move_if_present: set by the commit, whether the move was applied */
	const uint64 *drop_proj_gens;
	int			ndrop_parts;
	const SiftPartDesc *add_parts;
	int			nadd_parts;
	const uint64 *drop_src_gens;
	int			ndrop_links;
	const SiftProjLink *add_links;
	int			nadd_links;
} SiftProjDelta;

/* What an edit of the directory does with a commit: deltas of projections by proj_id, and projections dropped with every page they own (the caller releases the extents of their parts) */
typedef struct SiftManifestProjUpdate
{
	const SiftProjDelta *deltas;
	int			ndeltas;
	const uint16 *drop_ids;
	int			ndrop;
} SiftManifestProjUpdate;

/* The blocks a projection part owns (its header page, extent-list pages and stream extents), for the release of a part that leaves; implemented where parts are opened (col/f2/proj/build.c) */
extern SiftExtent *sift_proj_part_extents(Relation rel, const SiftPartDesc *d, int *n);

/* The delta a commit that drops the base parts applies to every projection: the links of those parts go, and the projection parts they name */
extern void sift_proj_delta_for_dropped_parts(const SiftProjParts *p, const uint64 *dropped_src_gens, int ndropped, SiftProjDelta *out);

/* A move (move_if_present) still applies to cur: every part it drops is there, and each link it adds replaces the link that names the part it drops */
extern bool sift_proj_delta_applies(const SiftProjParts *cur, const SiftProjDelta *d);

/* cur with the delta applied, in out (palloc'd arrays); ERROR when a part or link it drops is not there, or an added one is twice, or the result is not one link for each part */
extern void sift_proj_apply_delta(const SiftProjParts *cur, const SiftProjDelta *d, SiftProjParts *out);

struct SiftColMetaPageData;

/* The refs the metapage's chain holds, ascending by proj_id; NULL and *nprojs = 0 without reading a page when the relation has none */
extern SiftManifestProjRef *sift_manifest_read_projs(Relation rel, const struct SiftColMetaPageData *meta, int *nprojs);

/* The ref of the named projection (NULL: none) among the refs read */
extern const SiftManifestProjRef *sift_proj_find(const SiftManifestProjRef *refs, int nrefs, const char *name);

/* The parts and links of a projection ref, read with the checks of the pages (magic, CRC32C, generation, order, counts) */
extern SiftProjParts sift_manifest_read_proj_parts(Relation rel, const SiftManifestProjRef *ref);

/* The blocks of the chain of refs alone, and of the directory of one projection (its part directory, part list and links), each palloc'd, *n of them */
extern BlockNumber *sift_proj_ref_dir_blocks(Relation rel, const struct SiftColMetaPageData *meta, int *n);
extern BlockNumber *sift_proj_one_dir_blocks(Relation rel, const SiftManifestProjRef *ref, int *n);

/* The blocks of the directory chain and of every projection's directory, for the checker and for a drop (palloc'd, *n of them) */
extern BlockNumber *sift_proj_dir_blocks(Relation rel, const struct SiftColMetaPageData *meta, int *n);

/*
 * Writes the directory of one projection afresh (a leaf of one partition, its part list, its links) with the generation gen and the version of the commit that publishes it, and fills
 * the fields of ref that name it. The blocks come from the metadata slab; they are added to created so that a commit that loses the race returns them.
 */
struct BlkVec;
extern void sift_proj_write_parts(Relation rel, SiftManifestProjRef *ref, uint64 gen, uint64 version, const SiftProjParts *p, struct BlkVec *created);

#endif							/* SIFT_PROJ_H */
