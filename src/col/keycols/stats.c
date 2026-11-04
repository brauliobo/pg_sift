/*
 * col/keycols/stats.c - the statistics of a column for the build-time defaults: sizes of the index, a sample, ANALYZE when unknown
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "catalog/index.h"
#include "access/htup_details.h"
#include "executor/spi.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "col/keycols/keycols_int.h"


/* the live rows per page of a table that no VACUUM or ANALYZE has counted: those of up to 32 pages spread over it. The planner's guess from the widths of the columns is 16 times too many for a table of 1,000-byte strings,
 * and what an earlier CREATE INDEX leaves in pg_class differs between PostgreSQL 17 (the count of that build) and 18 (nothing), so the same table was analyzed by one and not by the other */
static double
keycols_sampled_density(Relation heap, BlockNumber nblocks)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	BlockNumber step = Max(nblocks / 32, 1);
	double		rows = 0;
	int			pages = 0;

	for (BlockNumber blkno = 0; blkno < nblocks && pages < 32; blkno += step, pages++)
	{
		Buffer		buf = ReadBufferExtended(heap, MAIN_FORKNUM, blkno, RBM_NORMAL, strategy);
		Page		page;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!PageIsNew(page))
			for (OffsetNumber off = FirstOffsetNumber; off <= PageGetMaxOffsetNumber(page); off++)
			{
				ItemId		lp = PageGetItemId(page, off);

				/* a deleted row has lost the flag that an insert sets */
				if (ItemIdIsNormal(lp) && (((HeapTupleHeader) PageGetItem(page, lp))->t_infomask & HEAP_XMAX_INVALID))
					rows++;
			}
		UnlockReleaseBuffer(buf);
	}
	FreeAccessStrategy(strategy);
	return pages > 0 ? rows / pages : 0;
}

/* the rows of the heap: the planner's estimate (the rows pg_class holds per page, times the pages now), or the density of a sample of the pages when the table was never vacuumed or analyzed (reltuples -1: a table loaded and indexed in one go) */
double
keycols_heap_rows(Relation heap)
{
	BlockNumber pages;
	double		tuples,
				allvis;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

	if (heap->rd_rel->reltuples < 0 || heap->rd_rel->relpages == 0)
		return nblocks * keycols_sampled_density(heap, nblocks);
	table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvis);
	return tuples;
}

/* the statistics of heap column hatt: distinct values, the correlation with the heap order (1 when unknown) and the average width; false without them */
bool
keycols_col_stats(Relation heap, AttrNumber hatt, double *ndistinct, double *corr, int32 *width)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	AttStatsSlot sslot;

	if (tp == NULL)
		return false;
	*ndistinct = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
	if (*ndistinct < 0)
		*ndistinct = -*ndistinct * keycols_heap_rows(heap);
	*width = ((Form_pg_statistic) GETSTRUCT(tp))->stawidth;
	*corr = 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers > 0)
			*corr = fabs(sslot.numbers[0]);
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return true;
}

/* The share of the rows of the table that are NULL in a column (0 without statistics) */
double
keycols_col_nullfrac(Relation heap, AttrNumber hatt)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	double		f;

	if (tp == NULL)
		return 0;
	f = ((Form_pg_statistic) GETSTRUCT(tp))->stanullfrac;
	ReleaseSysCache(tp);
	return f;
}

/* about how many bytes a row of the index takes for a column, from its statistics: the bits of its distinct values, and the strings of a dictionary, at most its width */
double
keycols_est_col_bytes(Relation heap, const FormData_pg_attribute *att, double d, int32 width)
{
	return Min(width, log2(Max(d, 2)) / 8 + (att->attbyval ? 0 : width * d / Max(keycols_heap_rows(heap), 1)));
}

/* ... and of the whole index */
double
keycols_est_index_bytes(Relation heap, Relation index)
{
	double		bytes = 0;

	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	hatt = index->rd_index->indkey.values[i];
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), i);
		double		d,
					corr;
		int32		width;

		if (hatt == 0 || !keycols_col_stats(heap, hatt, &d, &corr, &width))
			bytes += att->attlen > 0 ? att->attlen : 32;
		else
			bytes += keycols_est_col_bytes(heap, att, d, width);
	}
	return bytes;
}

/* The table has a row estimate of at least GORD_AUTO_ROWS and no statistics of the first column of the index: the choice would decide nothing, so the indexed columns of the table are analyzed, as ANALYZE (columns) does (the NOTICE of the arrays it chooses says so). A REINDEX cannot: ANALYZE opens the index it rebuilds, nor a VACUUM (its rewrite of the segments chooses again here): PostgreSQL refuses an ANALYZE inside it */
void
keycols_analyze_if_unknown(Relation heap, Relation index)
{
	AttrNumber	hatt = index->rd_index->indkey.values[0];
	StringInfoData cmd;
	const char *sep = "";

	if (hatt == 0 || (MyProc->statusFlags & PROC_IN_VACUUM) || ReindexIsProcessingIndex(RelationGetRelid(index)) || ReindexIsProcessingHeap(RelationGetRelid(heap)) || SearchSysCacheExists3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false)))
		return;
	if (keycols_heap_rows(heap) < GORD_AUTO_ROWS)
		return;
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "ANALYZE %s.%s (", quote_identifier(get_namespace_name(RelationGetNamespace(heap))), quote_identifier(RelationGetRelationName(heap)));
	for (int i = 0; i < index->rd_index->indnatts; i++)
	{
		AttrNumber	att = index->rd_index->indkey.values[i];

		if (att == 0)
			continue;
		appendStringInfo(&cmd, "%s%s", sep, quote_identifier(get_attname(RelationGetRelid(heap), att, false)));
		sep = ", ";
	}
	appendStringInfoChar(&cmd, ')');
	if (SPI_connect() != SPI_OK_CONNECT)
		return;
	(void) SPI_execute(cmd.data, false, 0);
	SPI_finish();
	CommandCounterIncrement();
}
