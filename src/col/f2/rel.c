/*
 * col/f2/rel.c - what an index of format major 2 derives from its leaves: the kinds and hints of the columns of a part, the leaves its key_order names, and the cells and the key of a row.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/jsonb.h"

#include "col/f2/f2.h"
#include "col/f2/l0/stage.h"
#include "col/f2/preagg/v2.h"
#include "col/pg.h"
#include "col/scope/scope.h"
#include "col/shred/shred.h"

SiftCellKind
sift_f2_def_kind(const SiftColDef *d)
{
	return d->store == SIFT_COLS_INT ? SIFT_CELL_INT : (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8) ? SIFT_CELL_F8 : SIFT_CELL_STR;
}

uint8
sift_f2_def_hint(const SiftColDef *d)
{
	if (d->store == SIFT_COLS_INT && d->type == SIFT_COLT_NUMERIC && d->lanes < 2)
		return SIFT_CHINT_NUM64;
	if (d->store == SIFT_COLS_INT && d->type == SIFT_COLT_BOOL)
		return SIFT_CHINT_BOOL;
	if (d->store == SIFT_COLS_STR && d->type == SIFT_COLT_UUID)
		return SIFT_CHINT_UUID;
	if (d->store == SIFT_COLS_STR && (d->flags & SIFT_COLD_ASTEXT))
		return SIFT_CHINT_AUTO | SIFT_CHINT_ASTEXT;
	return SIFT_CHINT_AUTO;
}

/* Whether the leaf holds a value the rows can be sorted by: an integer, a date, a timestamp, a boolean, an exact number held as a scaled integer, a text or a uuid (a text of the bytes of the leaf, which is the order a part keeps whatever the collation; a char(n) and a name are not) */
static bool
key_type(Relation index, const SiftColDef *d)
{
	Oid			type;

	if (d->lanes > 1 || d->kind != SIFT_COLK_SCALAR)
		return false;
	if (d->store == SIFT_COLS_INT)
		return true;
	if (d->store != SIFT_COLS_STR || d->lane > 0 || d->parent >= 0 || (d->flags & (SIFT_COLD_ASTEXT | SIFT_COLD_DERIVED | SIFT_COLD_LOSSY)))
		return false;
	type = TupleDescAttr(RelationGetDescr(index), d->attno - 1)->atttypid;
	return type == TEXTOID || type == VARCHAROID || type == UUIDOID;
}

/* The leaf a column named by key_order is */
static int
key_leaf(Relation index, const SiftColSchema *s, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
	{
		const SiftColDef *d = &s->cols[i];

		if (d->path[0] == '\0' && d->name != NULL && strcmp(d->name, name) == 0)
		{
			if (!key_type(index, d))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("key_order column \"%s\" of index \"%s\" is not an integer, date, timestamp, boolean, text, uuid or numeric(p <= 18, s) column", name, RelationGetRelationName(index))));
			return i;
		}
	}
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_order option \"%s\": column \"%s\" is not a column of the index", name, name)));
	pg_unreachable();
}

/* The key a leaf holds the values of at the top of its jsonb document: a path of the column's name, a dot and a plain key, with the [] of a list of objects */
bool
sift_f2_def_top_key(const SiftColDef *d, const char **key, int *len)
{
	size_t		nl;
	int			k;

	if (d->parent >= 0 || d->lane > 0 || d->path == NULL || d->path[0] == '\0' || d->name == NULL || (d->flags & SIFT_COLD_DERIVED))
		return false;
	nl = strlen(d->name);
	if (strncmp(d->path, d->name, nl) != 0 || d->path[nl] != '.')
		return false;
	*key = d->path + nl + 1;
	for (k = 0; sift_shred_bare_char((unsigned char) (*key)[k]); k++)
		;
	*len = k;
	return k > 0 && ((*key)[k] == '\0' || strcmp(*key + k, "[]") == 0);
}

/* The leaf of the keys of a map: the text child of its group, named `<map>#key` */
static bool
is_map_key(const SiftColDef *d)
{
	const size_t n = d->path != NULL ? strlen(d->path) : 0;

	return d->parent >= 0 && n > 4 && strcmp(d->path + n - 4, "#key") == 0;
}

static bool
is_time_leaf(const SiftColDef *d)
{
	return d->path[0] == '\0' && d->store == SIFT_COLS_INT && d->lanes < 2 && d->lane == 0 && d->kind == SIFT_COLK_SCALAR && d->parent < 0 && !(d->flags & SIFT_COLD_LOSSY) &&
		(d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ);
}

/* The time columns of the manifest: the date and timestamp columns of the table, those of key_order first, then in the order of the table; at most SIFT_MANIFEST_TIME_COLS of them */
static void
time_leaves(SiftF2Rel *f, const SiftColSchema *schema)
{
	int			n = 0;

	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < (pass == 0 ? f->nkey : schema->ncols) && n < SIFT_MANIFEST_TIME_COLS; i++)
		{
			int			c = pass == 0 ? f->key_cols[i] : i;
			bool		taken = false;

			for (int k = 0; k < n; k++)
				taken |= f->spec.time_cols[k] == c + 1;
			if (taken || !is_time_leaf(&schema->cols[c]))
				continue;
			f->spec.time_cols[n] = c + 1;
			f->time_unit[n] = schema->cols[c].type == SIFT_COLT_DATE ? USECS_PER_DAY : 1;
			n++;
		}
}

/* What the option preagg of the index declares: the cubes it names (NULL: none, they are chosen from the data); *on is whether the parts get cubes at all */
static const SiftPreaggDecl *
sift_f2_preagg_decl(Relation index, const SiftColSchema *schema, bool *on)
{
	SiftPreaggDecl *decl = palloc0(sizeof(SiftPreaggDecl));
	bool		off;
	const char *bad = sift_preagg_decl_parse(sift_col_options_preagg(index), schema, false, decl, &off);

	if (bad != NULL)
		sift_col_option_error("preagg", sift_col_options_preagg(index), bad);
	*on = !off;
	return decl->ndims > 0 || decl->nrdims > 0 ? decl : NULL;
}

/* The options the declarations of the index name, read before its state: what they look up can take a lock that reloads the entry, which frees the cached state and the options (not the leaves) */
static void
f2_declared(Relation index, const SiftPreaggDecl **decl, bool *preagg, SiftF2KllCols *kll)
{
	*decl = sift_f2_preagg_decl(index, sift_col_rel(index)->schema, preagg);
	sift_f2_kll_columns(index, sift_col_rel(index), sift_col_options_sketches(index), kll);
}

SiftF2Rel *
sift_f2_rel(Relation index)
{
	MemoryContext old;
	const SiftPreaggDecl *decl;
	const SiftColOptions *o;
	SiftColRel *r;
	SiftF2KllCols kll;
	SiftF2Rel  *f;
	int			n,
				klen;
	bool		preagg;
	bool	   *map_key = NULL;

	if (sift_col_rel(index)->f2 != NULL)
		return sift_col_rel(index)->f2;
	old = MemoryContextSwitchTo(index->rd_indexcxt);
	f2_declared(index, &decl, &preagg, &kll);
#ifdef SIFT_TESTING
	sift_col_test_inval(index);	/* the lookups of the declarations can take a lock that reloads the entry */
#endif
	/* check-memory: allow key_leaf reads the entry, no catalog */
	r = sift_col_rel(index);
	o = sift_col_options(index);
	n = r->nscope > 0 ? r->scope_first : r->schema->ncols;
	f = palloc0(sizeof(SiftF2Rel));
	f->ncols = n;
	f->kinds = palloc(n * sizeof(SiftCellKind));
	f->hints = palloc(n);
	f->scales = palloc(n);
	f->parent = palloc(n * sizeof(int16));
	f->key_leaf = palloc0(n * sizeof(bool));
	f->key = palloc0(n * sizeof(char *));
	f->key_len = palloc0(n * sizeof(uint16));
	f->key_attno = palloc0(n * sizeof(int16));
	for (int i = 0; i < n; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];

		f->parent[i] = d->parent;
		f->has_groups |= d->kind == SIFT_COLK_GROUP;

		if (r->json[d->attno] && sift_f2_def_top_key(d, &f->key[i], &klen))
		{
			f->key_leaf[i] = true;
			f->key_len[i] = klen;
			f->key_attno[i] = d->attno;
			f->has_keys = true;
		}
		if (is_map_key(d))
		{
			map_key = map_key != NULL ? map_key : palloc0(n * sizeof(bool));
			map_key[i] = true;
		}
		f->kinds[i] = sift_f2_def_kind(d);
		f->hints[i] = sift_f2_def_hint(d);
		f->scales[i] = d->scale;
	}
	for (int k = 0; k < o->ko_n; k++)
		f->key_cols[f->nkey++] = key_leaf(index, r->schema, (const char *) o + o->ko_name[k]);
	if (r->shred != NULL)
		sift_col_shred_keep_payloads(r->shred);
	f->spec = (SiftPartSpec) {.ncols = n,.kinds = f->kinds,.nsort = f->nkey,.sort_cols = f->key_cols,.hints = f->hints,.scales = f->scales,.key_leaf = f->has_keys ? f->key_leaf : NULL,
	.parent = f->has_groups ? f->parent : NULL,.preagg = preagg,.preagg_decl = decl,.map_key = map_key};
	memcpy(f->spec.sort_flags, o->ko_flags, sizeof(f->spec.sort_flags));
	f->kll = kll.n > 0 ? memcpy(palloc(sizeof(kll)), &kll, sizeof(kll)) : NULL;
	f->spec.kll = f->kll;
	time_leaves(f, r->schema);
	if (r->nscope > 0 && sift_scope_time_leaf(r->schema->cols, n) >= 0)
		f->spec.time_col = sift_scope_time_leaf(r->schema->cols, n) + 1;
	MemoryContextSwitchTo(old);
	r->f2 = f;
	return f;
}

int
sift_f2_key_leaf(const SiftF2Rel *f, int attno, const char *key, int len)
{
	for (int c = 0; c < f->ncols; c++)
		if (f->key_leaf[c] && f->key_attno[c] == attno && f->key_len[c] == len && memcmp(f->key[c], key, len) == 0)
			return c;
	return -1;
}

static uint8
cell_tag(const SiftColValue *v)
{
	if (!v->isnull)
		return v->tag;
	if (v->overflow)
		return v->tag == SIFT_COLTAG_UNKNOWN ? SIFT_COLTAG_UNSURE : v->tag;
	return v->tag == SIFT_COLTAG_JNULL ? SIFT_COLTAG_JNULL : 0;
}

SiftCell
sift_f2_value_cell(SiftCellKind kind, const SiftColValue *v)
{
	SiftCell	cell = {.isnull = v->isnull,.tag = cell_tag(v)};

	if (v->isnull)
	{
		cell.xp = v->xp;
		cell.xplen = v->xplen;
		return cell;
	}
	if (kind == SIFT_CELL_INT)
		cell.i = v->v.i;
	else if (kind == SIFT_CELL_F8)
		cell.f = v->v.f;
	else
	{
		cell.s = v->v.s;
		cell.slen = v->len;
	}
	return cell;
}

void
sift_f2_key_of(const SiftColRel *r, const SiftF2Rel *f, SiftCell *cells, const ItemPointerData *tid, const Datum *values, const bool *isnull, uint8 *key)
{
	for (int k = 0; k < f->nkey; k++)
	{
		const SiftColDef *d = &r->schema->cols[f->key_cols[k]];
		SiftColValue v = {.isnull = isnull[d->attno - 1]};

		if (!v.isnull)
			sift_col_datum_values(d, values[d->attno - 1], r->name[d->attno], &v);
		cells[f->key_cols[k]] = sift_f2_value_cell(f->kinds[f->key_cols[k]], &v);
	}
	sift_row_key(&f->spec, cells, sift_f2_tid(tid), key);
}

/* A column whose value the leaf cannot hold keeps the datum as its payload; the leaves of a path got theirs from the shredder */
static void
keep_datums(Relation index, const SiftColRel *r, MemoryContext rowcxt, const Datum *values, const bool *isnull, SiftColRow *row)
{
	MemoryContext old = MemoryContextSwitchTo(rowcxt);

	for (int a = 1; a <= r->natts; a++)
	{
		SiftColValue *v = &row->val[r->first[a]];

		if (r->json[a] || isnull[a - 1] || !v->overflow || TupleDescAttr(index->rd_att, a - 1)->attlen != -1)
			continue;
		v->xp = (const char *) PG_DETOAST_DATUM_PACKED(values[a - 1]);
		v->xplen = VARSIZE_ANY(v->xp);
	}
	MemoryContextSwitchTo(old);
}

/*
 * jsonb_exists is true of an array that holds the key as a string and of a string that is the key, which the shredder, that finds the keys of objects, leaves null with the tag
 * SIFT_COLTAG_NONE (no value where an object was declared): such a row of the leaf of the key has the tag SIFT_COLTAG_ROOTKEY instead, and the leaf no longer says the key is missing
 */
static void
tag_root_keys(const SiftF2Rel *f, int natts, MemoryContext rowcxt, const Datum *values, const bool *isnull, SiftCell *cells)
{
	MemoryContext old = MemoryContextSwitchTo(rowcxt);
	Jsonb	  **docs = NULL;

	for (int c = 0; c < f->ncols; c++)
	{
		int			a = f->key_attno[c] - 1;
		JsonbValue	key;

		if (!f->key_leaf[c] || !cells[c].isnull || (cells[c].tag != 0 && cells[c].tag != SIFT_COLTAG_NONE) || isnull[a])
			continue;
		if (docs == NULL)
			docs = palloc0(natts * sizeof(Jsonb *));
		if (docs[a] == NULL)
			docs[a] = DatumGetJsonbP(values[a]);
		if (JB_ROOT_IS_OBJECT(docs[a]))
			continue;
		key = (JsonbValue) {.type = jbvString,.val.string = {.len = f->key_len[c],.val = unconstify(char *, f->key[c])}};
		if (findJsonbValueFromContainer(&docs[a]->root, JB_FOBJECT | JB_FARRAY, &key) != NULL)
			cells[c].tag = SIFT_COLTAG_ROOTKEY;
	}
	MemoryContextSwitchTo(old);
}

void
sift_f2_row(Relation index, SiftF2Rel *f, MemoryContext rowcxt, const ItemPointerData *tid, const Datum *values, const bool *isnull, SiftCell *cells, SiftCell **elem, uint8 *key)
{
	/* check-memory: allow the row is made from the entry before anything is read */
	SiftColRel *r = sift_col_rel(index);
	SiftColRow	row;

	sift_col_make_row(r, (ItemPointer) tid, values, isnull, &row, rowcxt);
	keep_datums(index, r, rowcxt, values, isnull, &row);
	for (int c = 0; c < f->ncols; c++)
		if (f->parent[c] < 0)
			cells[c] = sift_f2_value_cell(f->kinds[c], &row.val[c]);
	if (f->has_keys)
		tag_root_keys(f, r->natts, rowcxt, values, isnull, cells);
	for (int c = 0; c < f->ncols; c++)
	{
		int			g = f->parent[c];
		int64		n;

		if (g < 0)
			continue;
		n = cells[g].isnull ? 0 : cells[g].i;
		elem[c] = n > 0 ? MemoryContextAlloc(rowcxt, n * sizeof(SiftCell)) : NULL;
		for (int64 j = 0; j < n; j++)
			elem[c][j] = sift_f2_value_cell(f->kinds[c], &row.elem[c][j]);
	}
	sift_row_key(&f->spec, cells, sift_f2_tid(tid), key);
}

bool
sift_f2_holds_rows(Relation rel)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	bool		holds = sift_stage_holds_in(rel, 0, PG_UINT64_MAX);

	for (int i = 0; i < mr.n && !holds; i++)
		holds = mr.entries[i].nparts > 0 || mr.entries[i].l0_slab_blk != InvalidBlockNumber || mr.entries[i].pend_del_blk != InvalidBlockNumber;
	return holds;
}
