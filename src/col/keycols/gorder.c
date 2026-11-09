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
	bool		point;			/* a nearly unique integer in heap order (a serial id): its array is runs of consecutive addresses, 0.4 bytes a row, and answers the equality that the point directory (3.3) would */
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

/*
 * layout: a build (CREATE INDEX, REINDEX) decides key_order too, with the arrays; a merge that chooses again (sift_col_auto_redecide) leaves it as the build chose, its
 * segments hold the rows in the order they were written in, which a merge does not sort by a key the build did not use.
 */
void
sift_col_auto_gorder(Relation heap, Relation index, bool layout)
{
	const char *opt = sift_col_options_order_array_columns(index);
	const char *gopt = sift_col_options_order_array_group(index);
	const SiftColRel *rel;
	bool		autolist,
				gauto,
				keyorder,
				timepick = false;
	int			grp = -1,
				egrp = -1,
				oldgrp = -1,
				tkey = -1;
	double		gdist = 0,
				gdepth = 0;
	GordCand   *cand;
	bool	   *fit;
	int			ncand = 0,
				npick = 0,
				ntext = 0,
				nends = 0,
				pick[GORD_AUTO_MAX + SIFT_CGORD_ENDS_LEAVES];
	bool		gonly[GORD_AUTO_MAX + SIFT_CGORD_ENDS_LEAVES];
	bool	   *gsel;			/* by candidate: the array has a second order */
	double		index_bytes;
	StringInfoData v;

	/* the key_order an earlier build chose is decided again below, with the rest of the layout */
	if (layout && sift_col_options_key_order_auto(index))
	{
		sift_col_set_option(index, "key_order", NULL);
		opt = sift_col_options_order_array_columns(index);
		gopt = sift_col_options_order_array_group(index);
	}
	/* the options below are written while these are read: copies, the relcache rebuilds the options a write touches */
	opt = opt ? pstrdup(opt) : NULL;
	gopt = pstrdup(gopt);
	/* a key_cut of the user's shapes the units, as the user chose: only a build's own choice or 'off' takes the layout along */
	keyorder = layout && sift_col_options_key_order(index)[0] == '\0' && !sift_col_options_order_columns_set(index) && strcmp(gopt, "off") != 0 &&
		(sift_col_options_key_cut_auto(index) || strcmp(sift_col_options_key_cut(index), "off") == 0);
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
	gsel = palloc0(rel->schema->ncols * sizeof(bool));
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
					nofit,
					point;

		if (d->parent >= 0 || hatt == 0 || !(text || sift_cgord_leaf_ok(rel->schema, d) || sift_cgord_leaf_numeric(rel->schema, d)))
			continue;
		for (int k = 0; k < rel->ncut; k++)
			cut |= rel->cut[k] == c;
		cut = cut && sift_col_key_cut(rel);
		if (!keycols_col_stats(heap, hatt, &ndistinct, &cr, &width) || (text && width > GORD_TEXT_WIDTH))
			continue;
		point = !cut && cr >= GORD_AUTO_CORR && keycols_point_key(heap, index, rel->schema, d, &cr);
		nofit = (cut || cr >= GORD_AUTO_CORR) && !point;
		ties = !nofit && ndistinct < GORD_AUTO_DISTINCT;
		if (((ties || nofit) && (text || !sift_cgord_leaf_time(d) || ndistinct < 2)) || (nofit && !sift_cgord_leaf_ok(rel->schema, d)))
			continue;
		cand[ncand] = (GordCand) {c, sift_cgord_leaf_time(d), text, ties, nofit, sift_cgord_leaf_ok(rel->schema, d) && !point, point, cr, 0, 0, 0, 0};
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
		/* one array at a time, the dates and timestamps first: an array has a second order while the whole still pays (a narrow index can afford one where it cannot afford all) */
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < ncand; i++)
			{
				double		more = bytes + GORD_GROUP_BYTES + SIFT_CGORD_GRPENT / Max(gdepth, 1.0);

				if (cand[i].leaf == grp || !cand[i].grouped || !(fit[i] || cand[i].time) || cand[i].time != (pass == 0) || ngrouped >= SIFT_CGORD_ENDS_LEAVES)
					continue;
				if (log2(GORD_GROUP_SPEEDUP) / log2(1.05) > more / index_bytes * 100.0 / 3.0 + GORD_GROUP_WRITE / 5.0)
				{
					bytes = more;
					ngrouped++;
					gsel[i] = true;
				}
			}
		if (ngrouped == 0)
			grp = -1;
	}
	/*
	 * key_order: the rows of a unit kept in the order of a time column and the order arrays of that column alone, grouped by the entity where there is one ('group, key': the
	 * latest rows of each group read their entries, 2 to 6 times fewer instructions than the arrays grouped by the column of most values that the plain choice below takes, whose
	 * other arrays go: ORDER BY another column LIMIT n over many rows costs more, see docs/OPTIONS.md); with no entity, a key_cut by a time column takes it for a key alone (a time
	 * column is what ORDER BY .. LIMIT and OFFSET are read by). The key of the cut, or else the first time column the arrays below would take.
	 */
	if (keyorder)
	{
		int			ctime = -1;
		double		best = 0;
		bool		ecut = false,
					eclus = false,
					timecut;
		double		cnd = 0;

		oldgrp = grp;
		egrp = -1;
		for (int c = 0; c < rel->schema->ncols; c++)
		{
			const SiftColDef *d = &rel->schema->cols[c];
			AttrNumber	hatt = keycols_heap_attno(index, d);
			double		nd,
						cr,
						score;
			int32		width;
			bool		cut = false;

			if (d->parent >= 0 || hatt == 0 || !sift_cgord_leaf_ok(rel->schema, d))
				continue;
			for (int k = 0; sift_col_key_cut(rel) && k < rel->ncut; k++)
				cut |= rel->cut[k] == c;
			if (cut && sift_cgord_leaf_time(d))
			{
				double		ccr;
				int32		cwidth;

				ctime = c;
				if (!keycols_col_stats(heap, hatt, &cnd, &ccr, &cwidth))
					cnd = 0;
			}
			if (sift_cgord_leaf_time(d) || !(keycols_is_fk_column(heap, hatt) || keycols_ends_in_id(keycols_leaf_name(index, d))) || !keycols_col_stats(heap, hatt, &nd, &cr, &width) ||
				nd < GORD_KEYORDER_ENTITIES || keycols_heap_rows(heap) / nd < GORD_GROUP_DEPTH)
				continue;
			/* the entity is the key the cut chose, else the foreign key or *_id column of most values, in the order of the heap or not */
			score = nd + (cut ? 1e12 : 0.0);
			if (score > best)
			{
				best = score;
				egrp = c;
				ecut = cut;
				eclus = cr >= GORD_AUTO_CORR;
				gdepth = keycols_heap_rows(heap) / nd;
			}
		}
		timecut = ctime >= 0;
		for (int i = 0; ctime < 0 && egrp >= 0 && i < ncand; i++)
			if (cand[i].time && cand[i].grouped)
				ctime = cand[i].leaf;
		tkey = ctime;
		/*
		 * The layout replaces a second order the plain choice builds (by this entity or another column), or serves the entity the units are cut by or the heap follows; where the plain choice builds
		 * none it would add one: 100 to 500 entities in a heap that does not follow them, +24 to +46% of the index for fewer instructions than it costs on average (B - C -3 to -13), so the plain choice stays;
		 * under a cut by a date it would replace a group by another column (20 and 40 entities, random order: B - C of +0.8, -0.4, 0.0, +6.8 and +1.7, -0.8, +1.6, -2.2 in four builds, index +2 to +22%): nothing gained beyond the noise
		 */
		if (egrp >= 0 && !ecut && !eclus && (oldgrp < 0 || (timecut && oldgrp != egrp)))
			tkey = -1;
		/*
		 * A key alone (no entity) needs keys that tell the rows apart: the dates of a table (730 values over 300,000 rows) are runs of equal keys, and a top-N that filters another column reads
		 * the order of the key and fetches the rows to test the filter (441 heap fetches, 0.15 ms against 12 blocks and 0.05 ms in the units the cut keeps); a timestamp of distinct values has none of that
		 */
		if (egrp < 0 && cnd < GORD_AUTO_DISTINCT)
			tkey = -1;
		/*
		 * With an entity the weighted formula decides, as for the second order above: B of the latest rows of each entity against C of the key's array grouped by it, less the plain
		 * arrays of the columns, the key's too, which the layout does not build (a key alone is not weighed: a cut by a time column has paid for itself)
		 */
		if (tkey >= 0 && egrp >= 0)
		{
			double		bytes = GORD_GROUP_BYTES + SIFT_CGORD_GRPENT / Max(gdepth, 1.0);

			for (int i = 0; i < ncand; i++)
				if (fit[i] && !cand[i].text && !cand[i].point)
					bytes -= GORD_ENDS_BYTES;
			if (log2(GORD_GROUP_SPEEDUP) / log2(1.05) <= bytes / index_bytes * 100.0 / 3.0 + GORD_GROUP_WRITE / 5.0)
				tkey = -1;
		}
	}
	if (tkey >= 0)
	{
		grp = egrp;
		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s%s%s", grp >= 0 ? keycols_leaf_name(index, &rel->schema->cols[grp]) : "", grp >= 0 ? ", " : "",
						 keycols_leaf_name(index, &rel->schema->cols[tkey]));
		if (opt != NULL)
			sift_col_set_option(index, "order_array_columns", NULL);
		if (gopt[0] != '\0')
			sift_col_set_option(index, "order_array_group", NULL);
		sift_col_set_option(index, "key_order", v.data);
		return;
	}
	for (int i = 0; i < ncand; i++)
		if ((fit[i] || (grp >= 0 && cand[i].time && cand[i].grouped && gsel[i])) && cand[i].leaf != grp && (cand[i].text ? ntext < GORD_AUTO_MAX : nends < SIFT_CGORD_ENDS_LEAVES))
		{
			pick[npick++] = cand[i].leaf;
			gonly[npick - 1] = !fit[i];
			ntext += cand[i].text;
			nends += !cand[i].text;
		}
	if (grp >= 0)
	{
		initStringInfo(&v);
		appendStringInfo(&v, "auto: %s", keycols_leaf_name(index, &rel->schema->cols[grp]));
		/* the arrays that only the second order serves (a time column the units are cut by or that follows the heap): no plain order beside it */
		for (int i = 0; i < npick; i++)
			if (gonly[i])
				appendStringInfo(&v, ", %s", keycols_leaf_name(index, &rel->schema->cols[pick[i]]));
		/* ... and, after the ';', the arrays of their own that have one beside it (the others keep the plain order alone) */
		appendStringInfoChar(&v, ';');
		for (int i = 0, n = 0; i < ncand; i++)
			if (gsel[i] && fit[i] && cand[i].leaf != grp)
				appendStringInfo(&v, "%s %s", n++ ? "," : "", keycols_leaf_name(index, &rel->schema->cols[cand[i].leaf]));
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
