/*
 * agg/from/source.c - the source of a scan: the table or index it reads
 * Its columns, the expressions on them and the leaves the scan keeps.
 */
#include "postgres.h"

#include "access/table.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"

#include "agg/part.h"
#include "agg/from/int.h"
#include "core/cmp.h"

static List *recordset_funcs(AggCtx *c, int grp);
static Bitmapset *function_columns(AggCtx *c, int grp, RangeTblFunction *f);

/* Opens the source of the rel and its layout, the agg_index_pick-th of its column indexes (agg_index_usable: how many there are); false if none */
bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;
	int			n = 0;

	*ixout = NULL;
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return part_source(c) || agg_refuse(PARTITION_SOURCE);
	if (sift_colsrc_is_table(get_rel_relam(rte->relid)))
	{
		rel = table_open(rte->relid, NoLock);
		c->lay = sift_colsrc_layout(rel);
		stats_scaled(rel, &c->stats);
		sift_colsrc_preagg(rel, &c->pa);
		table_close(rel, NoLock);
		c->indexoid = InvalidOid;
		return true;
	}
	foreach_node(IndexOptInfo, ii, c->rel->indexlist)
		if (sift_colsrc_is_index(ii->relam) && !ii->hypothetical && (ii->indpred == NIL || ii->predOK) && n++ == agg_index_pick)
			*ixout = ii;
	agg_index_usable = Max(agg_index_usable, n);
	if (*ixout == NULL)
		return false;
	rel = index_open((*ixout)->indexoid, AccessShareLock);
	c->lay = sift_colsrc_layout(rel);
	index_facts(rel, c->lay->nleaves, &c->stats, &c->pa);
	index_close(rel, AccessShareLock);
	c->indexoid = (*ixout)->indexoid;
	return true;
}

static List *
vars_of_rel(AggCtx *c, List *exprs)
{
	List	   *out = NIL;

	foreach_ptr(Node, e, exprs)
	{
		Node	   *x = e ? copyObject(e) : NULL;

		if (x)
			ChangeVarNodes(x, 1, c->relid, 0);
		out = lappend(out, x);
	}
	return out;
}

void
build_attexprs(AggCtx *c)
{
	c->attexpr = vars_of_rel(c, c->lay->attexprs);
	c->attalias = vars_of_rel(c, c->lay->attalias);
}

void
finish_leaves(AggCtx *c, SiftAggSpec *s)
{
	int			n = 0,
				m = -1;

	/* the columns of jsonb_to_recordset that nothing reads but a value they cannot store makes it raise: the source reads those rows again */
	for (int g = -1; (g = bms_next_member(recordset_groups(c), g)) >= 0;)
		if (recordset_recheck_ok(c, g))
		{
			Bitmapset  *cols = recordset_columns(c, g);

			for (m = -1; (m = bms_next_member(cols, m)) >= 0;)
				if (c->stats.ovf[m])
				{
					c->used = bms_add_member(c->used, m);
					c->rec_ok = bms_add_member(c->rec_ok, m);
				}
		}
	m = -1;
	s->leaves = palloc(Max(bms_num_members(c->used), 1) * sizeof(int));
	while ((m = bms_next_member(c->used, m)) >= 0)
		s->leaves[n++] = m;
	s->nleaves = n;
	sift_sort_i32(s->leaves, n);
	s->recheck = palloc0(Max(n, 1));
	s->gate = palloc0(Max(n, 1) * sizeof(uint64));
	for (int i = 0; i < n; i++)
		s->recheck[i] = s->leaves[i] < c->lay->nleaves ? spec_recheck(c, s, s->leaves[i]) : SRK_NONE;
	for (int j = 0; j < s->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && OidIsValid(s->vleaf[j].cast = member_recheck(c, s, j)))
			s->vleaf[j].recheck = SRK_CAST;
}

/* can SQL read a leaf of this type from the text of a scalar through its input function (the leaf types of the shredder)? */
static bool
text_io_leaf(const SiftSrcLeaf *l)
{
	if (l->wide || l->kind != SCL_SCALAR)
		return false;
	switch (l->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
			return true;
		default:
			return false;
	}
}

/*
 * Does jsonb_to_recordset read the unnested list alone: nothing else reads it or one of its columns, and each column the query reads is read as
 * a column of the record (and has a type SQL reads from the text of a scalar)? Then a row whose document holds a value the leaves cannot store is
 * read again by the source as the function reads it.
 */
bool
recordset_recheck_ok(AggCtx *c, int grp)
{
	int			m = -1;

	if (!bms_is_member(grp, recordset_groups(c)) || c->search || bms_is_member(grp, c->rec_bad) ||
		!OidIsValid(c->indexoid) || !sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return false;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].obj == grp)	/* the length of the list: a row that raises has no length */
			return false;
	/* the error of a row is the list's, whichever call reads it: calls that declare other columns raise for other values */
	foreach_ptr(RangeTblFunction, f, recordset_funcs(c, grp))
		if (!bms_equal(function_columns(c, grp, f), function_columns(c, grp, linitial(recordset_funcs(c, grp)))))
			return false;
	while ((m = bms_next_member(c->used, m)) >= 0)
		if (leaf_of(c, m)->parent == grp && (!bms_is_member(m, c->rec_ok) || bms_is_member(m, c->rec_bad) || !text_io_leaf(leaf_of(c, m))))
			return false;
	return true;
}

/* the lists a jsonb_to_recordset unnests: the one of the FROM, and those the EXISTS tests of the query read */
Bitmapset *
recordset_groups(AggCtx *c)
{
	Bitmapset  *groups = NULL;

	if (c->func_kind == FK_RECORDSET && c->group >= 0)
		groups = bms_add_member(groups, c->group);
	foreach_ptr(RecFunc, rf, c->rec_funcs)
		groups = bms_add_member(groups, rf->grp);
	return groups;
}

/* the calls of jsonb_to_recordset that unnest list grp: the FROM's, and those of the EXISTS tests */
static List *
recordset_funcs(AggCtx *c, int grp)
{
	List	   *funcs = NIL;

	if (c->func_kind == FK_RECORDSET && grp == c->group)
		funcs = list_make1(c->func);
	foreach_ptr(RecFunc, rf, c->rec_funcs)
		if (rf->grp == grp)
			funcs = lappend(funcs, rf->f);
	return funcs;
}

/* the columns one call declares that have a leaf in the index */
static Bitmapset *
function_columns(AggCtx *c, int grp, RangeTblFunction *f)
{
	Bitmapset  *cols = NULL;

	for (int i = 0; i < list_length(f->funccolnames); i++)
	{
		int			leaf = child_leaf(c, grp, strVal(list_nth(f->funccolnames, i)), list_nth_oid(f->funccoltypes, i));

		if (leaf >= 0 && text_io_leaf(leaf_of(c, leaf)))
			cols = bms_add_member(cols, leaf);
	}
	return cols;
}

/*
 * The columns the function declares that have a leaf in the index: it reads all of them for every element, whether the query does or not, and
 * raises for a value its type does not take.
 */
Bitmapset *
recordset_columns(AggCtx *c, int grp)
{
	Bitmapset  *cols = NULL;

	foreach_ptr(RangeTblFunction, f, recordset_funcs(c, grp))
		cols = bms_join(cols, function_columns(c, grp, f));
	return cols;
}
