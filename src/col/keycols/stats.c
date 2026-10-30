/*
 * col/keycols/stats.c - the statistics of a column for the build-time defaults: sizes of the index, a sample, ANALYZE when unknown
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "catalog/index.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "col/keycols/keycols_int.h"


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
		*ndistinct = -*ndistinct * heap->rd_rel->reltuples;
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

/* about how many bytes a row of the index takes for a column, from its statistics: the bits of its distinct values, and the strings of a dictionary, at most its width */
double
keycols_est_col_bytes(Relation heap, const FormData_pg_attribute *att, double d, int32 width)
{
	return Min(width, log2(Max(d, 2)) / 8 + (att->attbyval ? 0 : width * d / Max(heap->rd_rel->reltuples, 1)));
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

/* The table has a row estimate of at least GORD_AUTO_ROWS and no statistics of the first column of the index: the choice would decide nothing, so the indexed columns of the table are analyzed, as ANALYZE (columns) does (the NOTICE of the arrays it chooses says so). A REINDEX cannot: ANALYZE opens the index it rebuilds */
void
keycols_analyze_if_unknown(Relation heap, Relation index)
{
	BlockNumber pages;
	double		tuples,
				allvis;
	AttrNumber	hatt = index->rd_index->indkey.values[0];
	StringInfoData cmd;
	const char *sep = "";

	if (hatt == 0 || ReindexIsProcessingIndex(RelationGetRelid(index)) || ReindexIsProcessingHeap(RelationGetRelid(heap)) || SearchSysCacheExists3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false)))
		return;
	table_relation_estimate_size(heap, NULL, &pages, &tuples, &allvis);
	if (tuples < GORD_AUTO_ROWS)
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
