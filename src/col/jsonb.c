/*
 * col/jsonb.c - the jsonb attributes of a sift_col index: the "paths" reloption is parsed by the shredder
 * (col/shred.h), whose columns and groups become the index's leaves, and every tuple's jsonb values are shredded into the rows the engine
 * stores. One level of arrays and objects is held: a list (key[]) or map (key.*) group becomes a GROUP leaf with the group's columns as
 * children; a map also gets the member name as a text child.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/shred_pg.h"

typedef enum LeafKind
{
	LEAF_ROOT_COL,				/* a column of the root group */
	LEAF_GROUP,					/* a list or map */
	LEAF_CHILD_COL,				/* a column of a group */
	LEAF_MAP_KEY				/* the member name of a map */
} LeafKind;

typedef struct ShredLeaf
{
	SiftColDef	def;			/* parent relative to the attribute's first leaf */
	LeafKind	kind;
	int			col;			/* shredder column */
	int			group;			/* shredder group */
} ShredLeaf;

typedef struct ShredAttr
{
	char	   *name;
	int			attno;
	int			n;
	ShredLeaf  *leaf;
} ShredAttr;

struct SiftColShredAdapter
{
	SiftShredSpec *spec;
	SiftShred  *sh;
	int			natts;
	ShredAttr  *attr;			/* [natts + 1] */
	char	  **attname;		/* [natts + 1] */
};

/* The first key of a canonical path (a.b[].c: a), unquoted */
static char *
leading_key(const char *path)
{
	StringInfoData b;
	const char *p = path;

	initStringInfo(&b);
	if (*p == '"')
	{
		for (p++; *p; p++)
		{
			if (*p == '"' && p[1] == '"')
				p++;
			else if (*p == '"')
				break;
			appendStringInfoChar(&b, *p);
		}
	}
	else
		for (; *p && *p != '.' && *p != '['; p++)
			appendStringInfoChar(&b, *p);
	return b.data;
}

static int
attr_of(const SiftColShredAdapter *a, const char *path)
{
	char	   *key = leading_key(path);

	for (int i = 1; i <= a->natts; i++)
		if (a->attr[i].name && strcmp(a->attr[i].name, key) == 0)
			return i;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("the paths option names \"%s\", which is not a jsonb column of the index", key),
			 errhint("A path starts with the name of an indexed jsonb column.")));
	return 0;
}

static void
leaf_type(SiftShredType t, uint8 *store, uint8 *type)
{
	switch (t)
	{
		case SIFT_SHRED_BOOL:
			*store = SIFT_COLS_INT, *type = SIFT_COLT_BOOL;
			break;
		case SIFT_SHRED_INT2:
			*store = SIFT_COLS_INT, *type = SIFT_COLT_INT2;
			break;
		case SIFT_SHRED_INT4:
			*store = SIFT_COLS_INT, *type = SIFT_COLT_INT4;
			break;
		case SIFT_SHRED_INT8:
			*store = SIFT_COLS_INT, *type = SIFT_COLT_INT8;
			break;
		case SIFT_SHRED_FLOAT4:
			*store = SIFT_COLS_F4, *type = SIFT_COLT_FLOAT4;
			break;
		case SIFT_SHRED_FLOAT8:
			*store = SIFT_COLS_F8, *type = SIFT_COLT_FLOAT8;
			break;
		case SIFT_SHRED_TEXT:
		case SIFT_SHRED_ASTEXT:
			*store = SIFT_COLS_STR, *type = SIFT_COLT_TEXT;
			break;
	}
}

static ShredLeaf *
add_leaf(ShredAttr *at, LeafKind kind, int parent, uint8 kind_def, const char *path, int col, int group)
{
	ShredLeaf  *l = &at->leaf[at->n++];

	memset(l, 0, sizeof(*l));
	l->kind = kind;
	l->col = col;
	l->group = group;
	l->def = (SiftColDef) {.parent = parent,.kind = kind_def,.path = path};
	return l;
}

static void
add_column(const SiftColShredAdapter *a, ShredAttr *at, int c, int parent, LeafKind kind, int group)
{
	const SiftShredColDef *cd = &a->spec->cols[c];
	ShredLeaf  *l = add_leaf(at, kind, parent, SIFT_COLK_SCALAR, pstrdup(cd->name), c, group);

	leaf_type(cd->type, &l->def.store, &l->def.type);
	if (cd->type == SIFT_SHRED_ASTEXT)
		l->def.flags |= SIFT_COLD_ASTEXT;
}

static void
add_group(const SiftColShredAdapter *a, ShredAttr *at, int g)
{
	const SiftShredGroupDef *gd = &a->spec->groups[g];
	int			gid = at->n;
	ShredLeaf  *l;

	if (gd->parent != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col holds one level of arrays and objects: \"%s\" is nested in another", gd->name)));
	l = add_leaf(at, LEAF_GROUP, -1, SIFT_COLK_GROUP, pstrdup(gd->name), -1, g);
	l->def.store = SIFT_COLS_INT;
	l->def.type = SIFT_COLT_INT8;
	if (gd->kind == SIFT_SHRED_MAP)
	{
		l = add_leaf(at, LEAF_MAP_KEY, gid, SIFT_COLK_SCALAR, psprintf("%s#key", gd->name), -1, g);
		l->def.store = SIFT_COLS_STR;
		l->def.type = SIFT_COLT_TEXT;
	}
	for (int i = 0; i < gd->ncols; i++)
		add_column(a, at, gd->cols[i], gid, LEAF_CHILD_COL, g);
}

/* The adapter of a paths specification over the first natts attributes of desc (the jsonb ones are the columns it may name) */
SiftColShredAdapter *
sift_col_shred_create_for(MemoryContext mcxt, TupleDesc desc, int natts, const char *paths)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColShredAdapter *a = palloc0(sizeof(SiftColShredAdapter));
	const SiftShredSpec *spec;

	a->natts = natts;
	a->attr = palloc0((a->natts + 1) * sizeof(ShredAttr));
	for (int i = 1; i <= a->natts; i++)
		if (TupleDescAttr(desc, i - 1)->atttypid == JSONBOID)
		{
			a->attr[i].name = pstrdup(NameStr(TupleDescAttr(desc, i - 1)->attname));
			a->attr[i].attno = i;
		}
	a->spec = sift_shred_spec_parse(paths);
	spec = a->spec;
	if (spec->ngroups < 1 || spec->groups[0].kind != SIFT_SHRED_ROOT)
		elog(ERROR, "sift_col: the shredder spec has no root group");
	for (int i = 1; i <= a->natts; i++)
		a->attr[i].leaf = palloc0((spec->ncols + spec->ngroups * 2 + 1) * sizeof(ShredLeaf));
	for (int c = 0; c < spec->ncols; c++)
		if (spec->cols[c].group == 0)
			add_column(a, &a->attr[attr_of(a, spec->cols[c].name)], c, -1, LEAF_ROOT_COL, 0);
	for (int g = 1; g < spec->ngroups; g++)
		add_group(a, &a->attr[attr_of(a, spec->groups[g].name)], g);
	/* lives as long as the relcache entry, like the other backend caches (scripts/leakcheck.sh exempts "sift cache" contexts) */
	a->sh = sift_shred_create(spec, NULL, true);
	MemoryContextSwitchTo(old);
	return a;
}

SiftColShredAdapter *
sift_col_shred_create(MemoryContext mcxt, Relation index)
{
	const char *paths = sift_col_options_paths(index);

	if (paths[0] == '\0')
		return NULL;
	return sift_col_shred_create_for(mcxt, RelationGetDescr(index), IndexRelationGetNumberOfKeyAttributes(index), paths);
}

int
sift_col_shred_nleaves(const SiftColShredAdapter *a, int attno)
{
	return a->attr[attno].n;
}

void
sift_col_shred_leaf(const SiftColShredAdapter *a, int attno, int i, SiftColDef *def)
{
	*def = a->attr[attno].leaf[i].def;
}

static void
set_value(SiftColValue *v, const SiftShredDoc *doc, const SiftShredCol *col, SiftShredType type, uint32 i)
{
	memset(v, 0, sizeof(*v));
	v->tag = col->tag[i];
	switch (col->state[i])
	{
		case SIFT_SHRED_ABSENT:
		case SIFT_SHRED_NULL:
			v->isnull = true;
			return;
		case SIFT_SHRED_OVERFLOW:
			v->isnull = v->overflow = true;
			return;
		default:
			break;
	}
	switch (type)
	{
		case SIFT_SHRED_FLOAT4:
		case SIFT_SHRED_FLOAT8:
			v->v.f = col->val[i].f;
			break;
		case SIFT_SHRED_TEXT:
		case SIFT_SHRED_ASTEXT:
			v->v.s = doc->text + col->val[i].s.off;
			v->len = col->val[i].s.len;
			break;
		default:
			v->v.i = col->val[i].i;
	}
}

static void
set_key(SiftColValue *v, const SiftShred *sh, uint32 id)
{
	uint32		len;
	const char *s = id == SIFT_SHRED_NO_KEY ? NULL : sift_cdict_str(sh->dict, id, &len);

	memset(v, 0, sizeof(*v));
	if (s == NULL)
		v->isnull = v->overflow = true;
	else
	{
		v->v.s = s;
		v->len = len;
	}
}

/* The group's own value in the row (its element count) and the children's elements */
static void
fill_group(SiftColShredAdapter *a, const ShredAttr *at, int gl, SiftColRow *row, int first, MemoryContext mcxt)
{
	const ShredLeaf *gleaf = &at->leaf[gl];
	const SiftShredGroup *grp = &a->sh->doc.grp[gleaf->group];
	SiftColValue *gv = &row->val[first + gl];
	uint32		n = grp->n;

	memset(gv, 0, sizeof(*gv));
	if (grp->lstate[0] != SIFT_SHRED_VALUE)
	{
		gv->isnull = true;
		gv->overflow = grp->lstate[0] == SIFT_SHRED_OVERFLOW;
		return;
	}
	gv->v.i = n;
	for (int i = gl + 1; i < at->n && at->leaf[i].def.parent == gl; i++)
	{
		const ShredLeaf *l = &at->leaf[i];
		SiftColValue *e = MemoryContextAlloc(mcxt, Max(n, 1) * sizeof(SiftColValue));

		for (uint32 k = 0; k < n; k++)
		{
			if (l->kind == LEAF_MAP_KEY)
				set_key(&e[k], a->sh, grp->keyid[k]);
			else
				set_value(&e[k], &a->sh->doc, &a->sh->doc.col[l->col], a->spec->cols[l->col].type, k);
		}
		row->elem[first + i] = e;
	}
}

static void
shred_row_fill(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftShredRecheck *rc)
{
	SiftShred  *sh = a->sh;

	if (rc)
		sift_shred_recheck_begin(rc);
	sift_shred_begin(sh);
	sift_shred_enter(sh, false);
	for (int i = 1; i <= a->natts; i++)
	{
		SiftShredKey k;

		if (a->attr[i].n == 0 || isnull[i - 1])
			continue;
		k = sift_shred_key(sh, a->attr[i].name, strlen(a->attr[i].name));
		if (k != SIFT_SHRED_SKIP)
			sift_shred_container_rc(sh, &DatumGetJsonbP(values[i - 1])->root, rc);
		if (k == SIFT_SHRED_STOP)
			break;
	}
	sift_shred_leave(sh);
	sift_shred_finish(sh);
}

void
sift_col_shred_row_rc(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftColRow *row, const int *first,
					  MemoryContext mcxt, SiftShredRecheck *rc)
{
	SiftShred  *sh = a->sh;
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftShredLimits limits = sh->limits;

	/* a row read again is the one SQL reads whole: the limits that keep a write from growing a unit without bound are wider */
	if (rc)
	{
		sh->limits.max_elems = Max(limits.max_elems, SIFT_SHRED_RECHECK_ELEMS);
		sh->limits.max_bytes = Max(limits.max_bytes, SIFT_SHRED_RECHECK_BYTES);
	}
	PG_TRY();
	{
		shred_row_fill(a, values, isnull, rc);
	}
	PG_FINALLY();
	{
		sh->limits = limits;
	}
	PG_END_TRY();
	for (int i = 1; i <= a->natts; i++)
	{
		const ShredAttr *at = &a->attr[i];

		for (int l = 0; l < at->n; l++)
		{
			const ShredLeaf *lf = &at->leaf[l];

			if (lf->kind == LEAF_ROOT_COL)
				set_value(&row->val[first[i] + l], &sh->doc, &sh->doc.col[lf->col], a->spec->cols[lf->col].type, 0);
			else if (lf->kind == LEAF_GROUP)
				fill_group(a, at, l, row, first[i], mcxt);
		}
	}
	MemoryContextSwitchTo(old);
}

void
sift_col_shred_row(SiftColShredAdapter *a, const Datum *values, const bool *isnull, SiftColRow *row, const int *first, MemoryContext mcxt)
{
	sift_col_shred_row_rc(a, values, isnull, row, first, mcxt, NULL);
}

/* The recheck of the rows of the adapter's spec (col/shred_pg.h): how the query reads leaf i of attribute attno, a column or a list of the shredder */
SiftShredRecheck *
sift_col_shred_recheck_create(MemoryContext mcxt, const SiftColShredAdapter *a)
{
	return sift_shred_recheck_create(mcxt, a->spec);
}

void
sift_col_shred_recheck_set(SiftShredRecheck *rc, const SiftColShredAdapter *a, int attno, int i, uint8 mode)
{
	const ShredLeaf *l = &a->attr[attno].leaf[i];

	if (l->kind == LEAF_GROUP)
		rc->grp[l->group] = mode;
	else if (l->kind != LEAF_MAP_KEY)
		rc->col[l->col] = mode;
}

void
sift_col_shred_recheck_member(SiftShredRecheck *rc, const SiftColShredAdapter *a, int attno, int i, const char *key, Oid cast)
{
	sift_shred_recheck_member(rc, a->attr[attno].leaf[i].col, key, cast);
}
