/*
 * col/f2/f2_rel.c - what an index of format major 2 derives from its leaves: the kinds and hints of the columns of a part, the leaves its key_order names, and the cells and the key of a row.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/jsonb.h"

#include "col/f2/f2.h"
#include "col/pg.h"
#include "col/scope.h"

static void
unsupported(Relation index, const char *what)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col index \"%s\" cannot be built in format major 2 with %s", RelationGetRelationName(index), what),
					errhint("Leave format_major at 1, or take %s out of the index.", what)));
}

static SiftCellKind
cell_kind(const SiftColDef *d)
{
	return d->store == SIFT_COLS_INT ? SIFT_CELL_INT : (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8) ? SIFT_CELL_F8 : SIFT_CELL_STR;
}

static uint8
chunk_hint(const SiftColDef *d)
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
				break;
			return i;
		}
	}
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("key_order column \"%s\" of index \"%s\" is not an integer, date, timestamp, boolean, text, uuid or numeric(p <= 18, s) column", name, RelationGetRelationName(index))));
	pg_unreachable();
}

/* The key a leaf holds the values of at the top of its jsonb document: a path of the column's name, a dot and a plain key, with the [] of a list of objects */
static bool
top_level_key(const SiftColDef *d, const char **key, int *len)
{
	size_t		nl;
	int			k;

	if (d->parent >= 0 || d->lane > 0 || d->path == NULL || d->path[0] == '\0' || d->name == NULL || (d->flags & SIFT_COLD_DERIVED))
		return false;
	nl = strlen(d->name);
	if (strncmp(d->path, d->name, nl) != 0 || d->path[nl] != '.')
		return false;
	*key = d->path + nl + 1;
	for (k = 0; ((*key)[k] >= 'a' && (*key)[k] <= 'z') || ((*key)[k] >= 'A' && (*key)[k] <= 'Z') || ((*key)[k] >= '0' && (*key)[k] <= '9') || (*key)[k] == '_'; k++)
		;
	*len = k;
	return k > 0 && ((*key)[k] == '\0' || strcmp(*key + k, "[]") == 0);
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

static void
check_supported(Relation index, const SiftColRel *r)
{
	if (r->nconn > 0)
		unsupported(index, "connections");
}

SiftF2Rel *
sift_f2_rel(Relation index)
{
	/* check-memory: allow check_supported and key_leaf read the entry, no catalog */
	SiftColRel *r = sift_col_rel(index);
	const SiftColOptions *o = sift_col_options(index);
	MemoryContext old;
	SiftF2Rel  *f;
	int			n,
				klen;

	if (r->f2 != NULL)
		return r->f2;
	check_supported(index, r);
	old = MemoryContextSwitchTo(GetMemoryChunkContext(r));
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

		if (r->json[d->attno] && top_level_key(d, &f->key[i], &klen))
		{
			f->key_leaf[i] = true;
			f->key_len[i] = klen;
			f->key_attno[i] = d->attno;
			f->has_keys = true;
		}
		f->kinds[i] = cell_kind(d);
		f->hints[i] = chunk_hint(d);
		f->scales[i] = d->scale;
	}
	for (int k = 0; k < o->ko_n; k++)
		f->key_cols[f->nkey++] = key_leaf(index, r->schema, (const char *) o + o->ko_name[k]);
	if (r->shred != NULL)
		sift_col_shred_keep_payloads(r->shred);
	f->spec = (SiftPartSpec) {.ncols = n,.kinds = f->kinds,.nsort = f->nkey,.sort_cols = f->key_cols,.hints = f->hints,.scales = f->scales,.key_leaf = f->has_keys ? f->key_leaf : NULL,
	.parent = f->has_groups ? f->parent : NULL};
	memcpy(f->spec.sort_flags, o->ko_flags, sizeof(f->spec.sort_flags));
	sift_f2_kll_columns(index, r, sift_col_options_sketches(index), &f->kll);
	f->spec.kll = f->kll.n > 0 ? &f->kll : NULL;
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

static SiftCell
cell_of(const SiftF2Rel *f, int c, const SiftColValue *v)
{
	SiftCell	cell = {.isnull = v->isnull,.tag = cell_tag(v)};

	if (v->isnull)
	{
		cell.xp = v->xp;
		cell.xplen = v->xplen;
		return cell;
	}
	if (f->kinds[c] == SIFT_CELL_INT)
		cell.i = v->v.i;
	else if (f->kinds[c] == SIFT_CELL_F8)
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
		cells[f->key_cols[k]] = cell_of(f, f->key_cols[k], &v);
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
			cells[c] = cell_of(f, c, &row.val[c]);
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
			elem[c][j] = cell_of(f, c, &row.elem[c][j]);
	}
	sift_row_key(&f->spec, cells, sift_f2_tid(tid), key);
}
