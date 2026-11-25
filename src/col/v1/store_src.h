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
#include "table/v1/rel.h"

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
	SiftV6Input in;
	const SiftSrcLayout *lay;
	const SiftSrcLeaf *col_leaf[SIFT_V6_MAX_COLS];
	int			col_read[SIFT_V6_MAX_COLS];		/* of each column and boolean of the input, the entry of read_vec that holds its leaf */
	SiftV1Member *col_mem[SIFT_V6_MAX_COLS];	/* of each column of the input that is the float8 of a member of a map of a document, its reader: the heap row gives the rows the leaves cannot hold */
	AttrNumber	mem_att[SIFT_V6_MAX_COLS];
	uint8		jkind[SIFT_V6_MAX_COLS];	/* of a column that is the leaf of a key of a document: SIFT_V1_JSON_VALUE, whose rows the leaf cannot hold are read from the heap row if the batch keeps them, or SIFT_V1_JSON_TYPE, its JSON type */
	const SiftSrcLeaf *jleaf[SIFT_V6_MAX_COLS];
	AttrNumber	jatt[SIFT_V6_MAX_COLS];	/* the column of the heap row of the document of a JSON column */
	uint64	   *jexc[SIFT_V6_MAX_COLS];	/* the rows of the batch whose leaf could not hold the value */
	bool		has_jexc[SIFT_V6_MAX_COLS];
	uint8	   *jtag[SIFT_V6_MAX_COLS];	/* the JSON type of each row of the unit: the index of json_types */
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
	uint64	   *bits;			/* the rows of the unit the snapshot sees and nothing has been taken of */
	int			pos;
	SiftV6Vexpr *quals;
	const SiftTidBridge *bridge;
	const SiftV1UnitSink *cube;
	int			ntypes;
	SiftV1TypeTest types[SIFT_V1_MAX_TYPE_TESTS];
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

/* The heap row of a tag the source gave: false when the snapshot does not see it */
extern bool sift_v1_src_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull);

/* The columns of a batch that the leaves cannot hold, read from the heap row of each row the batch keeps (col/v1_engine/store_ops_heap.c): jsonb documents, values of keys, members of maps */
extern void sift_v1_src_fill_heap(SiftV1Src *s, const SiftV6Batch *b);

/* The JSON type of each row of the unit the column i reads, and of the row r in slot n of the batch */
extern void sift_v1_src_read_types(SiftV1Src *s, int i);
extern void sift_v1_src_put_type(SiftV1Src *s, int i, uint32 r, int n);

#endif							/* SIFT_COL_V1_STORE_SRC_H */
