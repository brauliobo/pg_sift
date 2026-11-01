/*
 * col/keycols/gorder.c - order_array_columns by default: the columns that take a global order
 */
#include "postgres.h"

#include <math.h>

#include "utils/varlena.h"
#include "col/am.h"
#include "col/keycols/keycols_int.h"
#include "col/gorder.h"
#include "col/seg_int.h"

typedef struct GordCand
{
	int			leaf;
	bool		time;			/* a date or a timestamp: a range or an ORDER BY .. LIMIT is what it is read by */
	bool		text;			/* a text or varchar column: ORDER BY .. LIMIT is what it is read by */
	bool		ties;			/* fewer than GORD_AUTO_DISTINCT values: weighed like a text column, by the OFFSET that ends in a group of equal keys */
	bool		nofit;			/* a date or timestamp in heap order (correlation over GORD_AUTO_CORR) or one the units are cut by: no array of its own, but one for the group */
	bool		grouped;		/* an integer, date or timestamp: the second order sorted by the entity column can be written for it (a numeric or text array has none) */
	double		corr;
	double		bytes;			/* a text column: the bytes a row of its array, the share of the build it adds, and its collation */
	double		write;
	double		benefit;		/* B and C of the choice of a text column, said by the NOTICE */
	double		cost;
} GordCand;

static int
gord_cand_cmp(const void *a, const void *b)
{
	const GordCand *x = a,
			   *y = b;

	if (x->time != y->time)
		return x->time ? -1 : 1;
	if (x->text != y->text)
		return x->text ? -1 : 1;
	if (x->text)
		return x->leaf - y->leaf;	/* the order of the index: the statistics of two columns of strings differ run to run */
	if ((int) (x->corr * 10) != (int) (y->corr * 10))
		return (int) (x->corr * 10) < (int) (y->corr * 10) ? -1 : 1;	/* tenths: the correlations of columns of random values differ run to run */
	return x->leaf - y->leaf;
}

static int
pick_cmp(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

void
sift_col_auto_gorder(Relation heap, Relation index)
{
	const char *opt = sift_col_options_order_array_columns(index);
	const char *gopt = sift_col_options_order_array_group(index);
	const SiftColRel *rel;
	bool		autolist,
				gauto,
				timepick = false;
	int			grp = -1;
	double		gdist = 0,
				gdepth = 0;
	GordCand   *cand;
	bool	   *fit;
	int			ncand = 0,
				npick = 0,
				ntext = 0,
				nends = 0,
				pick[GORD_AUTO_MAX + SIFT_CGORD_ENDS_LEAVES];
	double		index_bytes;
	StringInfoData v;

	(void) sift_cpagg_auto_list(opt, &autolist);
	if (opt != NULL && !autolist)
		return;
	(void) sift_cpagg_auto_list(gopt, &gauto);
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || (gopt[0] != '\0' && !gauto && strcmp(gopt, "off") != 0))
		return;
	keycols_analyze_if_unknown(heap, index);
	if (keycols_heap_rows(heap) < GORD_AUTO_ROWS)
		return;
	index_bytes = keycols_est_index_bytes(heap, index) + GORD_ROW_OVERHEAD;
	rel = sift_col_rel_private(index, CurrentMemoryContext);
	cand = palloc(rel->schema->ncols * sizeof(GordCand));
	fit = palloc(rel->schema->ncols * sizeof(bool));
	for (int c = 0; c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = keycols_heap_attno(index, d);
		double		ndistinct,
					cr;
		int32		width;
		bool		cut = false;

		bool		text = sift_cgord_leaf_text(d);
		bool		ties,
					nofit;

		if (d->parent >= 0 || hatt == 0 || !(text || sift_cgord_leaf_ok(rel->schema, d) || sift_cgord_leaf_numeric(rel->schema, d)))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		cut = cut && sift_col_key_cut(rel);
		if (!keycols_col_stats(heap, hatt, &ndistinct, &cr, &width) || (text && width > GORD_TEXT_WIDTH))
			continue;
		nofit = cut || cr >= GORD_AUTO_CORR;
		ties = !nofit && ndistinct < GORD_AUTO_DISTINCT;
		if (((ties || nofit) && (text || !sift_cgord_leaf_time(d) || ndistinct < 2)) || (nofit && !sift_cgord_leaf_ok(rel->schema, d)))
			continue;
		cand[ncand] = (GordCand) {c, sift_cgord_leaf_time(d), text, ties, nofit, sift_cgord_leaf_ok(rel->schema, d), cr, 0, 0, 0, 0};
		if (ties)
		{
			double		rows = keycols_heap_rows(heap),
						nullfrac = keycols_col_nullfrac(heap, hatt),
						group = rows * Max(nullfrac, (1.0 - nullfrac) / ndistinct);

			cand[ncand].bytes = GORD_TIES_BYTES;
			cand[ncand].write = GORD_TIES_WRITE * 7.0 / index->rd_index->indnatts;
			cand[ncand].benefit = Max(GORD_TIES_SPEEDUP * rows / (GORD_TIES_OFFSET + group), 1.0);
		}
		if (text)
		{
			/*
			 * the keys and addresses (4.4 bytes) and, as the writer chooses (seg_write/points.c), either the dictionary code of each distinct string (the bits of a code) and the dictionary
			 * written in blocks to be read by code (a tenth of the width of each string more than the compact form), or the strings, which front coding shortens by a tenth, when the
			 * dictionary does not fit the cache of parsed ones; the build adds the sort of the strings, by strcoll under a locale
			 */
			double		distinct = Min(1.0, ndistinct / Max(keycols_heap_rows(heap), 1));

			if ((double) sift_cdict_parsed_bytes((Size) (ndistinct * Min(width, 255)), (uint32) Min(ndistinct, (double) UINT32_MAX)) > (double) sift_cdict_cache_budget() * CDICT_CODES_PCT / 100.0)
				cand[ncand].bytes = 4.4 + Min(width, 255) * distinct * 0.9;
			else
				cand[ncand].bytes = 4.4 + distinct * (sift_cdict_perm_bits((uint32) Min(ndistinct, (double) UINT32_MAX)) / 8.0 + Min(width, 255) * 0.1);
			cand[ncand].write = (varstr_cmp("a", 1, "B", 1, TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attcollation) < 0 ? GORD_TEXT_WRITE_LOC : GORD_TEXT_WRITE_C) * 4.0 / index->rd_index->indnatts;
		}
		ncand++;
	}
	qsort(cand, ncand, sizeof(GordCand), gord_cand_cmp);
	/*
	 * A text column is an array of its own: the default formula decides, B = log2(S) / log2(1.05) against C = G / 3 + W / 5, with no ceiling. A date, timestamp
	 * or integer column keeps the ends of its order only (column format minor 40): 8,192 entries and the first NULL rows a segment, 0.07 bytes a row of 1,000,000, where the
	 * units hold the order the zones cannot prune: nothing to weigh. Whether a segment keeps the whole order is sift_cgord_pay's (cold blocks of the units against the bytes of the
	 * order). Every size and segment count measured keeps the whole order, so the
	 * 25% growth limit of a single structure is not applied to it, even where it is about 60% of a narrow index: without it a range across keys, an OFFSET or a point lookup reads more
	 * than twice the cold blocks or instructions.
	 */
	for (int i = 0; i < ncand; i++)
	{
		if (cand[i].nofit)
			fit[i] = false;
		else if (cand[i].text)
		{
			double		b = log2(GORD_TEXT_SPEEDUP) / log2(1.05),
						cost = cand[i].bytes / index_bytes * 100.0 / 3.0 + cand[i].write / 5.0;

			fit[i] = b > cost;
			cand[i].benefit = b;
			cand[i].cost = cost;
		}
		else if (cand[i].ties)
		{
			double		b = log2(cand[i].benefit) / log2(1.05),
						cost = cand[i].bytes / index_bytes * 100.0 / 3.0 + cand[i].write / 5.0;

			fit[i] = b > cost;
			cand[i].benefit = b;
			cand[i].cost = cost;
		}
		else
			fit[i] = true;
		timepick |= cand[i].grouped && (fit[i] || cand[i].time);
	}
	/*
	 * the arrays of a date, timestamp or integer column are grouped by the entity column that has the most values: WHERE g = x ORDER BY ts LIMIT n then reads n entries of x in each segment, where the plain
	 * order has the units read (3.3 to 12.2 M instructions against 0.27 M for a b-tree, 500,000 to 3,000,000 rows). The entity column is not an array of its own then. A date or timestamp that
	 * no formula above gives an array (in heap order, or of few values and too little OFFSET to pay) gets one for the group
	 */
	for (int c = 0; timepick && sift_col_format_minor >= SIFT_COL_FORMAT_PRESENCE && strcmp(gopt, "off") != 0 && c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = keycols_heap_attno(index, d);
		double		ndistinct,
					cr;
		int32		width;
		bool		cut = false;
		double		depth;

		if (d->parent >= 0 || hatt == 0 || !sift_cgord_leaf_ok(rel->schema, d) || sift_cgord_leaf_time(d))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		if (cut || !keycols_col_stats(heap, hatt, &ndistinct, &cr, &width) || cr >= GORD_AUTO_CORR || ndistinct < GORD_GROUP_DISTINCT || keycols_heap_rows(heap) / ndistinct < GORD_GROUP_DEPTH)
			continue;
		depth = keycols_heap_rows(heap) / ndistinct;
		/* a foreign key or a column named *_id is the entity the rows belong to; among those, or among the rest, the one of most values */
		if (keycols_is_fk_column(heap, hatt) || keycols_ends_in_id(keycols_leaf_name(index, d)))
			ndistinct += keycols_heap_rows(heap);
		if (ndistinct > gdist)
		{
			gdist = ndistinct;
			gdepth = depth;
			grp = c;
		}
	}
	/*
	 * the second order against the build and the size it adds, as for a text array: B = log2(S) / log2(1.05), C = G / 3 + W / 5. G is what it writes: GORD_GROUP_BYTES an entry of each grouped array and the
	 * directory of the groups (SIFT_CGORD_GRPENT bytes for each of them, so a few rows an entity cost it), less the array the entity does not get any more
	 */
	if (grp >= 0)
	{
		double		bytes = 0;
		int			ngrouped = 0;

		for (int i = 0; i < ncand; i++)
			if (cand[i].leaf == grp && fit[i])
				bytes -= GORD_ENDS_BYTES;
			else if (cand[i].grouped && (fit[i] || cand[i].time) && ngrouped < SIFT_CGORD_ENDS_LEAVES)
			{
				ngrouped++;
				bytes += GORD_GROUP_BYTES + SIFT_CGORD_GRPENT / Max(gdepth, 1.0);
			}
		if (ngrouped == 0 || log2(GORD_GROUP_SPEEDUP) / log2(1.05) <= bytes / index_bytes * 100.0 / 3.0 + GORD_GROUP_WRITE / 5.0)
			grp = -1;
	}
	for (int i = 0; i < ncand; i++)
		if ((fit[i] || (grp >= 0 && cand[i].time && cand[i].grouped)) && cand[i].leaf != grp && (cand[i].text ? ntext < GORD_AUTO_MAX : nends < SIFT_CGORD_ENDS_LEAVES))
		{
			pick[npick++] = cand[i].leaf;
			ntext += cand[i].text;
			nends += !cand[i].text;
			if (cand[i].ties && fit[i])
			{
				const char *name = keycols_leaf_name(index, &rel->schema->cols[cand[i].leaf]);

				ereport(NOTICE, (errmsg("sift_col index \"%s\": order_array_columns = 'auto' chose the column \"%s\" of few values: benefit %.0f against cost %.0f", RelationGetRelationName(index), name, cand[i].benefit, cand[i].cost),
								 errdetail("ORDER BY %s, key LIMIT n OFFSET m reads the rows of the group of equal values the offset ends in, where the units read every row (300,000 rows of 365 values, OFFSET 10,000: 113 M to 11 M instructions). The array takes about %.0f bytes a row (%.0f%% of the index) and CREATE INDEX about %.0f%% longer.",
										   name, cand[i].bytes, cand[i].bytes / index_bytes * 100.0, cand[i].write),
								 errhint("ALTER INDEX %s SET (order_array_columns = 'off'), then REINDEX, drops it; order_array_columns = '%s' keeps it by name.", quote_identifier(RelationGetRelationName(index)), name)));
			}
			if (cand[i].text)
			{
				const char *name = keycols_leaf_name(index, &rel->schema->cols[cand[i].leaf]);

				ereport(NOTICE, (errmsg("sift_col index \"%s\": order_array_columns = 'auto' chose the text column \"%s\": benefit %.0f against cost %.0f", RelationGetRelationName(index), name, cand[i].benefit, cand[i].cost),
								 errdetail("ORDER BY %s LIMIT n reads n entries of each segment, where the units compare the strings of every row (100,000 rows: 82.8 M to 0.27 M instructions). The array takes about %.0f bytes a row (%.0f%% of the index, G) and CREATE INDEX does about %.0f%% more instructions (W)%s: the benefit is log2(%.0f) / log2(1.05), the cost G / 3 + W / 5.",
										   name, cand[i].bytes, cand[i].bytes / index_bytes * 100.0, cand[i].write, cand[i].write > GORD_TEXT_WRITE_C * 4.0 / index->rd_index->indnatts ? ", the sort of the strings under the collation" : "", GORD_TEXT_SPEEDUP),
								 errhint("ALTER INDEX %s SET (order_array_columns = 'off'), then REINDEX, drops it; order_array_columns = '%s' keeps it by name.", quote_identifier(RelationGetRelationName(index)), name)));
			}
		}
	/* a text column the formula rejected, the cheapest of them: said once, with the setting that builds it anyway (a narrow index under a locale pays the sort of the strings in a large share of its build) */
	{
		int			rej = -1;

		for (int i = 0; i < ncand; i++)
			if (cand[i].text && !fit[i] && (rej < 0 || cand[i].cost < cand[rej].cost))
				rej = i;
		if (rej >= 0)
		{
			const char *name = keycols_leaf_name(index, &rel->schema->cols[cand[rej].leaf]);

			ereport(NOTICE, (errmsg("sift_col index \"%s\": order_array_columns = 'auto' did not choose the text column \"%s\": benefit %.0f against cost %.0f", RelationGetRelationName(index), name, cand[rej].benefit, cand[rej].cost),
							 errdetail("ORDER BY %s LIMIT n would read n entries of each segment, where the units compare the strings of every row. The array takes about %.0f bytes a row (%.0f%% of the index, G) and CREATE INDEX would do about %.0f%% more instructions (W)%s: the cost is G / 3 + W / 5 against the benefit log2(%.0f) / log2(1.05).",
									   name, cand[rej].bytes, cand[rej].bytes / index_bytes * 100.0, cand[rej].write, cand[rej].write > GORD_TEXT_WRITE_C * 4.0 / index->rd_index->indnatts ? ", the sort of the strings under the collation, in a share of the build that falls as the index has more columns" : "", GORD_TEXT_SPEEDUP),
							 errhint("CREATE INDEX .. WITH (order_array_columns = '%s'), or ALTER INDEX %s SET (order_array_columns = '%s') and REINDEX, if ORDER BY %s LIMIT n matters more than the build.", name, quote_identifier(RelationGetRelationName(index)), name, name)));
		}
	}
	if (grp >= 0)
	{
		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", keycols_leaf_name(index, &rel->schema->cols[grp]));
		sift_col_set_option(index, "order_array_group", v.data);
	}
	else if (gauto)
		sift_col_set_option(index, "order_array_group", "auto");
	if (npick == 0)
	{
		if (opt != NULL)
			sift_col_set_option(index, "order_array_columns", "auto");
		return;
	}
	/* in the order of the index: the correlations that chose among more leaves than the cap differ run to run */
	qsort(pick, npick, sizeof(int), pick_cmp);
	initStringInfo(&v);
	appendStringInfoString(&v, "auto: ");
	for (int i = 0; i < npick; i++)
		appendStringInfo(&v, "%s%s", i ? ", " : "", keycols_leaf_name(index, &rel->schema->cols[pick[i]]));
	sift_col_set_option(index, "order_array_columns", v.data);
}
