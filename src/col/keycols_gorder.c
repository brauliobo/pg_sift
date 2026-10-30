/*
 * col/keycols_gorder.c - order_array_columns by default: the columns that take a global order
 */
#include "postgres.h"

#include <math.h>

#include "utils/varlena.h"
#include "col/am.h"
#include "col/keycols_int.h"
#include "col/gorder.h"

typedef struct GordCand
{
	int			leaf;
	bool		time;			/* a date or a timestamp: a range or an ORDER BY .. LIMIT is what it is read by */
	bool		text;			/* a text or varchar column: ORDER BY .. LIMIT is what it is read by */
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
	if (x->corr != y->corr)
		return x->corr < y->corr ? -1 : 1;
	return x->leaf - y->leaf;
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
	double		gdist = 0;
	GordCand   *cand;
	bool	   *fit;
	int			ncand = 0,
				npick = 0,
				pick[GORD_AUTO_MAX];
	double		room,
				index_bytes;
	StringInfoData v;

	(void) sift_cpagg_auto_list(opt, &autolist);
	if (opt != NULL && !autolist)
		return;
	(void) sift_cpagg_auto_list(gopt, &gauto);
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE || (gopt[0] != '\0' && !gauto && strcmp(gopt, "off") != 0))
		return;
	keycols_analyze_if_unknown(heap, index);
	if (heap->rd_rel->reltuples < GORD_AUTO_ROWS)
		return;
	index_bytes = keycols_est_index_bytes(heap, index) + GORD_ROW_OVERHEAD;
	room = index_bytes * GORD_AUTO_GROWTH_PCT / 100;
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

		if (d->parent >= 0 || hatt == 0 || !(text || sift_cgord_leaf_ok(rel->schema, d)))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		if (cut && sift_col_key_cut(rel))
			continue;
		if (!keycols_col_stats(heap, hatt, &ndistinct, &cr, &width) || ndistinct < GORD_AUTO_DISTINCT || cr >= GORD_AUTO_CORR || (text && width > GORD_TEXT_WIDTH))
			continue;
		cand[ncand] = (GordCand) {c, sift_cgord_leaf_time(d), text, cr, 0, 0, 0, 0};
		if (text)
		{
			/* the keys and addresses (4.4 bytes) and the strings, which front coding shortens by a tenth; the build adds the sort of the strings, by strcoll under a locale */
			cand[ncand].bytes = 4.4 + Min(width, 255) * Min(1.0, ndistinct / Max(heap->rd_rel->reltuples, 1)) * 0.9;
			cand[ncand].write = (varstr_cmp("a", 1, "B", 1, TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attcollation) < 0 ? GORD_TEXT_WRITE_LOC : GORD_TEXT_WRITE_C) * 4.0 / index->rd_index->indnatts;
		}
		ncand++;
	}
	qsort(cand, ncand, sizeof(GordCand), gord_cand_cmp);
	/* each array is a structure of its own: it must fit the room, and a column that does not leaves it to the next; the sorted run of a column not in order_columns comes with it */
	for (int i = 0; i < ncand; i++)
	{
		const SiftColDef *d = &rel->schema->cols[cand[i].leaf];
		bool		narrow = d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_DATE;
		double		bytes = cand[i].text ? cand[i].bytes : narrow ? 4.4 : 5.6;

		/* a date, timestamp or text column: the default formula decides, B = log2(S) / log2(1.05) against C = G / 3 + W / 5, with no ceiling; the others fit the room */
		if (cand[i].text)
		{
			double		b = log2(GORD_TEXT_SPEEDUP) / log2(1.05),
						cost = bytes / index_bytes * 100.0 / 3.0 + cand[i].write / 5.0;

			fit[i] = b > cost;
			cand[i].benefit = b;
			cand[i].cost = cost;
		}
		else
			fit[i] = cand[i].time ? log2(GORD_TIME_SPEEDUP) / log2(1.05) > bytes / index_bytes * 100.0 / 3.0 + GORD_TIME_WRITE_PCT / 5.0 : bytes <= room;
		timepick |= fit[i] && cand[i].time;
	}
	/* the arrays of a time column are grouped by the entity column that has the most values: WHERE g = x ORDER BY ts LIMIT n then reads n entries of x in each segment, where the plain order has the units read (3.3 to 12.2 M instructions against 0.27 M for a b-tree, 500,000 to 3,000,000 rows). The entity column is not an array of its own then */
	for (int c = 0; timepick && sift_col_format_minor >= SIFT_COL_FORMAT_PRESENCE && strcmp(gopt, "off") != 0 && GORD_GROUP_BYTES <= room && c < rel->schema->ncols; c++)
	{
		const SiftColDef *d = &rel->schema->cols[c];
		AttrNumber	hatt = keycols_heap_attno(index, d);
		double		ndistinct,
					cr;
		int32		width;
		bool		cut = false;

		if (d->parent >= 0 || hatt == 0 || !sift_cgord_leaf_ok(rel->schema, d) || sift_cgord_leaf_time(d))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		if (cut || !keycols_col_stats(heap, hatt, &ndistinct, &cr, &width) || cr >= GORD_AUTO_CORR || ndistinct < GORD_GROUP_DISTINCT || heap->rd_rel->reltuples / ndistinct < GORD_GROUP_DEPTH)
			continue;
		/* a foreign key or a column named *_id is the entity the rows belong to; among those, or among the rest, the one of most values */
		if (keycols_is_fk_column(heap, hatt) || keycols_ends_in_id(keycols_leaf_name(index, d)))
			ndistinct += heap->rd_rel->reltuples;
		if (ndistinct > gdist)
		{
			gdist = ndistinct;
			grp = c;
		}
	}
	for (int i = 0; i < ncand && npick < GORD_AUTO_MAX; i++)
		if (fit[i] && cand[i].leaf != grp)
		{
			pick[npick++] = cand[i].leaf;
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
	initStringInfo(&v);
	appendStringInfoString(&v, "auto: ");
	for (int i = 0; i < npick; i++)
		appendStringInfo(&v, "%s%s", i ? ", " : "", keycols_leaf_name(index, &rel->schema->cols[pick[i]]));
	sift_col_set_option(index, "order_array_columns", v.data);
}
