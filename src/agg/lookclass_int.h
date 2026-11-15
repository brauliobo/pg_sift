/*
 * agg/lookclass_int.h - what the classes of a table's keys (lookclass.c) share with the gang of workers that makes them together (lookclassp.c)
 */
#ifndef SIFT_AGG_LOOKCLASS_INT_H
#define SIFT_AGG_LOOKCLASS_INT_H

#include "agg/lookcol_int.h"

/*
 * The strings of the text columns of the classes: the dictionary of a unit gives a code of its own, so a text value is told apart by an id among the strings the
 * load has met (the bytes: two values the collation calls equal but that differ in bytes are two classes, which is exact), and a class holds the id.
 */
typedef struct StrIds
{
	char	   *buf;			/* the strings, each a uint32 length and its bytes */
	Size		len;
	Size		cap;
	uint32	   *off;			/* [id - 1] the offset of the string in buf */
	uint32		n;
	uint32		ocap;
	uint32	   *slot;			/* the id in a hash slot, 0 free */
	uint32		nslot;
	MemoryContext cxt;
} StrIds;

typedef struct CompMemo CompMemo;

typedef struct DenseLoad
{
	StrIds		strs;			/* the strings of the text columns */
	int			key;			/* the key's column, in need[] */
	int			ncol;			/* the columns that tell the classes apart: the columns of the join but the key */
	int			col[SIFT_AGG_MAX_JCOLS];	/* ... each one in need[], or -1 for an expression computed over the slot (comp[]) */
	CompCol	   *comp;
	int			ncomp;
	CompMemo   *memo;			/* the outcomes of the tests by the values of the columns they read */
	TupleTableSlot *slot;		/* the row, for the expressions */
	ExprContext *ectx;
	int64		cv[SIFT_AGG_MAX_JCOLS];	/* the value of each computed column of the row, and whether it is NULL */
	bool		cvnull[SIFT_AGG_MAX_JCOLS];
	uint16	   *lut;			/* the class (1..) of each key, 0 none */
	uint32		cap;
	uint32		n;				/* the keys the table spans: the largest + 1 */
	uint32		limit;			/* the most keys the table may span */
	uint64		rows;
	uint32		ncls;
	int64	   *tv;				/* [class * ncol + c] the value of column c of the class */
	uint32	   *tnull;			/* per class: bit c set when column c is NULL */
	int32	   *ckey;			/* per class: the first key that has it */
	uint16	   *ctab;			/* the class in a hash slot, 0 free */
	bool		bad;			/* a duplicate key, a key out of range or too many classes */
	MemoryContext cxt;
	/* a participant of a gang: lut is the one array of the gang, cap keys, whose slots are claimed with a compare-and-swap */
	bool		shared;
	bool		over;			/* ... a key lay beyond the array */
	uint16		idbase;			/* ... the ids of its classes follow this one: no two participants give the same id */
	uint64	   *cpos;			/* ... per class: where its first row lies in the index (superblock, unit of the superblock, row), by which the leader orders the classes of all */
	uint64		upos;
	uint32		lastsb;
	uint32		ord;
} DenseLoad;



#define DENSE_SLOTS		(4 * DENSE_MAX_CLASSES)

extern uint32 strids_id(StrIds *s, const char *str, uint32 len);
extern void dense_tables(DenseLoad *d, bool positions);
extern void dense_unit(void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows);
extern uint32 dense_intern(DenseLoad *d, const int64 *tv, uint32 nulls, int32 key, uint64 pos);
extern bool dense_gang(AggExec *ex, Relation rel, Relation ix, ColNeed *need, int nneed, DenseLoad *d);

#endif
