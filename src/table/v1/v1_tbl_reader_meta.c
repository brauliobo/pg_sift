/*
 * table/v1/v1_tbl_reader_meta.c - what the planner reads of a sift_table through the unit source: the leaves of its layout (the columns of the table and the declared
 * paths of its jsonb columns) and its statistics (col/v1/v1_reader.h).
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/col.h"
#include "col/v1/ktab.h"
#include "col/v1/pagg.h"
#include "col/v1/src_wide.h"
#include "col/v1/v1_reader.h"
#include "table/v1/table_v1.h"
#include "table/v1/v1_tbl_reader.h"

static SiftSrcVecType
leaf_vtype(const SiftColDef *def, TupleDesc desc)
{
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return def->type == SIFT_COLT_NUMERIC || TupleDescAttr(desc, def->attno - 1)->attlen == 8 ? SCV_I64 : SCV_I32;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return SCV_F64;
		case SIFT_COLS_STR:
			return SCV_CODE;
		default:
			return SCV_I64;
	}
}

/* numeric of any other shape than the narrow integer leaf, money and interval: a WHERE conjunct tests them on the exact values (SAP_WIDE) */
static bool
leaf_tested(const SiftColDef *def, Oid typid)
{
	return typid == CASHOID || typid == INTERVALOID || (typid == NUMERICOID && def->store == SIFT_COLS_VAR);
}

/* a leaf the executor reads through a dictionary of the datum images (agg/wide.h): the exact image of a column the table keeps by value as a size */
static bool
leaf_dict(const SiftColDef *def, Oid typid)
{
	return sift_srcwide_type(typid) && def->store != SIFT_COLS_INT;
}

/* The expression of a stored generated column, as the planner sees it in a query: a query that writes it reads the column (always the value the row holds) */
static Node *
generation_expr(Relation rel, Form_pg_attribute att)
{
	if (att->attgenerated != ATTRIBUTE_GENERATED_STORED)
		return NULL;
	return (Node *) expression_planner((Expr *) build_column_default(rel, att->attnum));
}

/* A leaf of a declared path of a jsonb column, not one of the table's own columns */
bool
sift_v1_tbl_leaf_is_path(const SiftColDef *def)
{
	return def->attno > 0 && def->path[0];
}

/*
 * The planner's view of the table: a scalar leaf per column, the leaves of the declared paths after them (when every segment has them, else the
 * normal plan reads the jsonb column); the exact datum images (SIFT_COLS_VAR) are not values the executor reads
 */
SiftSrcLayout *
sift_v1_tbl_layout(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));
	int			npath = 0;

	lay->heapoid = RelationGetRelid(rel);
	lay->is_table = true;
	lay->natts = desc->natts;
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		lay->attexprs = lappend(lay->attexprs, att->attisdropped ? NULL :
								(Node *) makeVar(1, a, att->atttypid, att->atttypmod, att->attcollation, 0));
		lay->attalias = lappend(lay->attalias, generation_expr(rel, att));
	}
	lay->nleaves = cur->ncols - 1;	/* the last leaf holds the exceptions */
	for (int i = 0; i < lay->nleaves; i++)
		npath += sift_v1_tbl_leaf_is_path(&cur->cols[i]);
	if (npath > 0 && !sift_tbl_paths_complete(rel, st))
		lay->nleaves -= npath;		/* the path leaves follow the columns */
	lay->leaf = palloc0(sizeof(SiftSrcLeaf) * lay->nleaves);
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftColDef *def = &cur->cols[i];
		Oid			typid = sift_col_def_typid(desc, def);
		bool		wide = leaf_dict(def, typid);

		if (sift_v1_tbl_leaf_is_path(def))
		{
			sift_src_def_leaf(cur->cols, i, desc, &lay->leaf[i]);
			continue;
		}
		lay->leaf[i] = (SiftSrcLeaf) {.id = i,.attno = def->attno,.parent = -1,.kind = SCL_SCALAR,.vtype = wide ? SCV_CODE : leaf_vtype(def, desc),
		.typid = typid,.wide = wide,.wtypid = leaf_tested(def, typid) ? typid : InvalidOid,.scale = def->scale,.lossy = !wide && def->store == SIFT_COLS_VAR};
	}
	return lay;
}

/*
 * The share of the superblocks whose stored units may answer from their pre-aggregates, those with no write buffer row and no tombstone (tbl_sb_mask),
 * estimated without reading either: a write buffer holds the superblocks of its vblock range, and each dead row the statistics count (a delete or an update
 * leaves one tombstone) is in one superblock, so d of them touch nsb * (1 - exp(-d / nsb)) of the nsb superblocks.
 */
static double
exact_share(Relation rel, SiftTblRel *st, double nsb)
{
	PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(RelationGetRelid(rel));
	double		dead = tab ? Max((double) tab->dead_tuples, 0.0) : 0.0,
				wbuf = 0.0;

	for (int i = 0; i < st->nwbuf; i++)
		wbuf += ceil((double) st->wbuf[i].vcap / SIFT_SB_PAGES);
	return Max(0.0, 1.0 - (wbuf + nsb * (1.0 - exp(-dead / nsb))) / nsb);
}

void
sift_v1_tbl_stats(Relation rel, SiftSrcStats *st)
{
	SiftTblRel *tr = sift_tbl_rel(rel);
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, tr);
	SiftColLeafStats ls[SIFT_COL_MAX_COLS];
	bool		wovf[SIFT_COL_MAX_COLS],
				wjnull[SIFT_COL_MAX_COLS];

	memset(st, 0, sizeof(*st));
	st->nrows = Max(rel->rd_rel->reltuples, 0);
	st->nunits = SIFT_TBL_NEXT_VBLOCK(&tr->dir->meta) / SIFT_SB_PAGES + 1;
	st->allvisible_frac = 1.0;
	st->exact_frac = exact_share(rel, tr, st->nunits);
	sift_tbl_path_stats(rel, tr, ls, wovf, wjnull);
	sift_src_leaf_stats(cur->cols, cur->ncols - 1, ls, wovf, wjnull, st);
	/* distinct values of a column: what ANALYZE of the table found (negative: a share of the rows), 0 until it has run */
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES && i < cur->ncols - 1; i++)
	{
		HeapTuple	tp;

		if (sift_v1_tbl_leaf_is_path(&cur->cols[i]))
			continue;
		tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(rel)), Int16GetDatum(cur->cols[i].attno), BoolGetDatum(false));
		if (!HeapTupleIsValid(tp))
			continue;
		st->ndistinct[i] = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
		if (st->ndistinct[i] < 0)
			st->ndistinct[i] *= -st->nrows;
		ReleaseSysCache(tp);
	}
}

/* ---- pre-aggregates: the sidecar every stored unit carries (docs/COL-PREAGG.md) ---- */

void
sift_v1_tbl_preagg(Relation rel, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;
	const SiftColSchema *schema = sift_tbl_layout_schema(rel, sift_tbl_rel(rel));

	/* the segments are written with the spec of their layout, whose columns are the leaves of the table's */
	sift_tbl_pagg_spec(rel, schema, &spec);
	sift_ktab_spec(schema, &spec);
	sift_src_preagg_spec(&spec, pa);
}
