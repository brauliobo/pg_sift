/*
 * col/v1/store_src.h - the source of the unified scan of Format 1 (col/v1/store_ops.c): the state of a scan, and what the files of col/v1_engine/ that read the heap row of the rows a batch keeps share.
 */
#ifndef SIFT_COL_V1_STORE_SRC_H
#define SIFT_COL_V1_STORE_SRC_H

#include "postgres.h"

#include "access/tableam.h"
#include "executor/tuptable.h"

#include "agg/batch_source.h"
#include "col/v1/reader.h"
#include "col/v1/store_ops.h"
#include "core/tidbridge.h"
#include "core/v1_col/tomb/tomb.h"
#include "table/v1/rel.h"

/* A text qual of the fold over a dictionary leaf: pass holds, for the strings of dictionary dict, whether each passes it */
typedef struct SiftV1TextTest
{
	int			leaf;
	const struct SiftTextQual *q;
	const void *dict;
	bool	   *pass;
} SiftV1TextTest;

#define SIFT_V1_MAX_TEXT_TESTS	8

typedef struct SiftV1Src
{
	SiftV6Source base;
	MemoryContext cx;
	MemoryContext unit_cx;		/* what one unit needs: the bits of its rows, reset when the next one starts */
	MemoryContext batch_cx;		/* the bytes of the long strings and the datums of one batch, reset when the next one starts */
	SiftV1Rel	rels;
	Snapshot	snap;
	const SiftV1ReaderOps *rd;
	void	   *src;
	uint64		units_read;		/* the units whose leaves the source decoded */
	uint64		rows_read;		/* ... and their rows */
	SiftV6Input in;
	const SiftSrcLayout *lay;
	const SiftSrcLeaf *col_leaf[SIFT_V6_MAX_COLS];
	int			col_read[SIFT_V6_MAX_COLS];		/* of each column and boolean of the input, the entry of read_vec that holds its leaf */
	SiftV1Member *col_mem[SIFT_V6_MAX_COLS];	/* of each column of the input that is the float8 of a member of a map of a document, its reader: the heap row gives the rows the leaves cannot hold */
	AttrNumber	mem_att[SIFT_V6_MAX_COLS];
	uint8		jkind[SIFT_V6_MAX_COLS];	/* of a column that is the leaf of a key of a document: SIFT_V1_JSON_VALUE, whose rows the leaf cannot hold are read from the heap row if the batch keeps them, SIFT_V1_JSON_TYPE, its JSON type, or SIFT_V1_JSON_PRESENT, doc ? 'k' */
	const SiftSrcLeaf *jleaf[SIFT_V6_MAX_COLS];
	AttrNumber	jatt[SIFT_V6_MAX_COLS];	/* the column of the heap row of the document of a JSON column */
	uint64	   *jexc[SIFT_V6_MAX_COLS];	/* the rows of the batch whose leaf could not hold the value */
	bool		has_jexc[SIFT_V6_MAX_COLS];
	uint64		raises[SIFT_VEC_WORDS];	/* the rows of the batch whose value of a key the cast raises for (SiftV6Input.raise_late) */
	bool		has_raises;
	uint8	   *jtag[SIFT_V6_MAX_COLS];	/* the JSON type of each row of the unit (the index of json_types), or of a presence column 1 where the document has the key */
	bool		jrootless[SIFT_V6_MAX_COLS];	/* SiftV1TypeTest.rootless of a presence column */
	int			ndocs;			/* the columns of the input that are a jsonb document, read from the heap row of each row the batch keeps */
	int			doc_col[SIFT_V6_MAX_COLS];
	AttrNumber	doc_att[SIFT_V6_MAX_COLS];
	const SiftSrcLeaf *mask_leaf[SIFT_V6_MAX_MASKS];
	int			mask_read[SIFT_V6_MAX_MASKS];
	int			nread;
	int			read_leaf[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS + SIFT_V1_MAX_ELEM_COLS + 1];	/* the distinct leaves of a unit that are read: one read of a leaf is valid until the next of the same leaf */
	SiftSrcVec	read_vec[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS + SIFT_V1_MAX_ELEM_COLS + 1];
	SiftV1Elems *elems;			/* the shredded array of the scan: the slots of read_vec of its lengths leaf and of its element columns, and its elements for the rows of the batch */
	int			elem_group;
	int			nelem_child;
	int			elem_child[SIFT_V1_MAX_ELEM_COLS];
	const SiftV1ElemBatch *elem_batch;
	SiftSrcUnit unit;
	bool		in_unit;
	bool		vet_unit;		/* the unit has rows on a heap page that is not all-visible: its rows end the batch, which checks the ones its quals keep (sift_v1_src_vet) */
	int			vet_from;		/* the first row of the batch that is of a unit to vet, the last unit of the batch; -1 when the batch has none */
	uint64	   *bits;			/* the rows of the unit the tombstones leave and nothing has been taken of */
	int			pos;
	SiftV6Vexpr *quals;
	const SiftTidBridge *bridge;
	const SiftTombLog *hot;		/* a sift_table with HOT chains: the bridge holds a row by the TID of its chain's root */
	const SiftV1UnitSink *cube;
	bool		tid_range;		/* SiftV1Scan.tid_range */
	uint64		tid_lo;
	uint64		tid_hi;
	int			ntypes;
	SiftV1TypeTest types[SIFT_V1_MAX_TYPE_TESTS];
	int			ntexts;
	SiftV1TextTest texts[SIFT_V1_MAX_TEXT_TESTS];
	int			score_col;		/* the column of the input that holds the scores of the bridge, -1 for none */
	uint64		kept[SIFT_VEC_WORDS];
	float4		score[SIFT_VEC_LEN];
	SiftVexprCtx *vctx;
	TupleTableSlot *fslot;
	IndexFetchTableData *index_fetch;	/* an index entry names the root of a chain of versions of the heap row; a sift_table row is its own */
	SiftVec		vec[SIFT_V6_MAX_COLS];
	void	   *data[SIFT_V6_MAX_COLS];
	uint64	   *valid[SIFT_V6_MAX_COLS];
	uint64	   *nan[SIFT_V6_MAX_COLS];
	bool		has_nan[SIFT_V6_MAX_COLS];
	int16		typlen[SIFT_V6_MAX_COLS];
	bool		typbyval[SIFT_V6_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_V6_MAX_MASKS];
	uint64	   *mbits[SIFT_V6_MAX_MASKS];
	uint64	   *mvalid[SIFT_V6_MAX_MASKS];
	uint64		mask[SIFT_VEC_WORDS];
	uint64		tag[SIFT_VEC_LEN];
	uint32		ords[SIFT_VEC_LEN];
	ItemPointerData tids[SIFT_VEC_LEN];
} SiftV1Src;

#define SIFT_V1_JSON_VALUE	1
#define SIFT_V1_JSON_TYPE	2
#define SIFT_V1_JSON_PRESENT	3

/* The heap row of a tag the source gave: false when the snapshot does not see it */
extern bool sift_v1_src_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull);

/* The heap row of tag into fslot: the version the snapshot sees of the chain an index entry names, or the row of a sift_table; false when it sees none */
extern bool sift_v1_src_fetch_row(SiftV1Src *s, uint64 tag);

/* The text bridge of the scan, which names a row of a sift_table by the root of its HOT chain (col/v1_engine/store_ops_bridge.c) */
extern void sift_v1_src_bridge_begin(SiftV1Src *s, const SiftTidBridge *bridge);
/* Narrows the mask of b to the rows the bridge holds, the scores of its hits in the score column; false when none is left */
extern bool sift_v1_src_bridge_narrow(SiftV1Src *s, SiftV6Batch *b);

/* Narrows the mask of b to the rows whose heap row's ctid is in the range of the scan; false when none is left */
extern bool sift_v1_src_tid_narrow(SiftV1Src *s, SiftV6Batch *b);

/* Narrows the mask of b to the rows of the unit (from vet_from) the snapshot sees, read from the heap for the pages that are not all-visible (col/v1_engine/store_ops_vet.c); false when none is left */
extern bool sift_v1_src_vet(SiftV1Src *s, SiftV6Batch *b);

/* The columns of a batch that the leaves cannot hold, read from the heap row of each row the batch keeps (col/v1_engine/store_ops_heap.c): jsonb documents, values of keys, members of maps */
extern void sift_v1_src_fill_heap(SiftV1Src *s, SiftV6Batch *b);
/* SiftV6Source.raise: PostgreSQL's error for a row of the raises of the last batch */
extern void sift_v1_src_raise(SiftV6Source *base, int row);

/* The type tests of scan and the text quals of its fold that a dictionary leaf decides (col/v1_engine/store_ops_text.c) */
extern void sift_v1_src_bind_tests(SiftV1Src *s, const SiftV1Scan *scan);

/* Leaves out of s->bits the rows of the unit whose string passes no text qual, testing each string of a dictionary once; false when none is left */
extern bool sift_v1_src_text_pass(SiftV1Src *s);

/* The rows of bits of the unit whose key of leaf l has one of the JSON types (SIFT_SRC_JT_*); false when none is left */
extern bool sift_v1_src_filter_types(SiftV1Src *s, const SiftSrcLeaf *l, uint32 types, uint64 *bits);

/* The rows of bits of the unit whose document holds the key of the test t (doc ? 'k', col/v1_engine/store_ops_jsonb.c); false when none is left */
extern bool sift_v1_src_filter_key(SiftV1Src *s, const SiftV1TypeTest *t, uint64 *bits);

/* The JSON type of each row of the unit the column i reads, and of the row r in slot n of the batch */
extern void sift_v1_src_read_types(SiftV1Src *s, int i);
extern void sift_v1_src_put_type(SiftV1Src *s, int i, uint32 r, int n);

/* doc ? 'k' of each row of the unit the presence column i reads (col/v1_engine/store_ops_jsonb.c), and of the row r in slot n of the batch */
extern void sift_v1_src_read_presence(SiftV1Src *s, int i);
extern void sift_v1_src_put_presence(SiftV1Src *s, int i, uint32 r, int n);

/* Begins the reader over the range (NULL: every superblock), its units pruned by the bounds and the OR sets of the quals (col/v1/store_prune.c) */
extern void sift_v1_src_prune_begin(SiftV1Src *s, const SiftV1Scan *scan);

#endif							/* SIFT_COL_V1_STORE_SRC_H */
