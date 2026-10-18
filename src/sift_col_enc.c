/*
 * sift_col_enc.c - L1 of the column engine (docs/COL.md sections 1, 2.1-2.4, 2.6): schema, dictionaries, batches, column chunks, zones,
 * chunk tests and unit records. Values and bytes only; the segment writer and reader (sift_col_seg.c) and the sources of rows sit above.
 *
 *   1. schema         2. dictionaries      3. batches
 *   4. zones          5. codec seam        6. chunks
 *   7. chunk tests    8. units
 */
#include "sift_host.h"

#include "sift_col_enc.h"
#include "sift_popcount.h"

#define COLS_MAGIC		0x534C4F43	/* bytes 'C' 'O' 'L' 'S' */
#define COLU_MAGIC		0x554C4F43	/* "COLU" */

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static inline uint32
words_of(uint32 n)
{
	return (n + 63) / 64;
}

/* Appends n zero bytes */
static void
put_zero(StringInfo out, Size n)
{
	enlargeStringInfo(out, (int) n);
	memset(out->data + out->len, 0, n);
	out->len += (int) n;
	out->data[out->len] = '\0';
}

static void
put_pad(StringInfo out, Size align)
{
	put_zero(out, (align - (Size) out->len % align) % align);
}

static inline void
put_bytes(StringInfo out, const void *p, Size n)
{
	appendBinaryStringInfo(out, (const char *) p, (int) n);
}

#define put_val(out, v)	put_bytes(out, &(v), sizeof(v))

/* ---------------------------------------------------------------------------
 * 1. Schema
 * ------------------------------------------------------------------------- */

typedef struct SiftColDefDisk
{
	int16		id;
	int16		parent;
	uint8		kind;
	uint8		store;
	uint16		flags;
	int16		attno;
	uint16		pathlen;
	uint32		typid;
	uint16		scale;
	uint16		reserved;
} SiftColDefDisk;

/* NULL when the leaf list is valid, else why not */
static const char *
schema_check(const SiftColDef *defs, int n)
{
	if (n < 1 || n > SIFT_COL_MAX_COLS)
		return "the number of leaves is out of range";
	for (int i = 0; i < n; i++)
	{
		const SiftColDef *d = &defs[i];

		if (d->id != i)
			return "leaf ids must be 0..n-1 in order";
		if (d->kind != SIFT_COLK_SCALAR && d->kind != SIFT_COLK_GROUP)
			return "unknown leaf kind";
		if (d->store < SIFT_COLS_INT || d->store > SIFT_COLS_VAR)
			return "unknown storage class";
		if (d->store == SIFT_COLS_VAR && (d->kind != SIFT_COLK_SCALAR || d->parent != -1))
			return "a variable-length leaf is a top level scalar";
		if (d->path != NULL && strlen(d->path) > PG_UINT16_MAX)
			return "leaf path too long";
		if (d->scale > SIFT_COL_MAX_SCALE || (d->scale > 0 && d->store != SIFT_COLS_INT))
			return "a scale belongs to an integer leaf and is at most 18";
		if (d->kind == SIFT_COLK_GROUP && (d->parent != -1 || d->store != SIFT_COLS_INT))
			return "a group is a top level leaf of storage class INT";
		if (d->kind == SIFT_COLK_SCALAR && d->parent != -1 &&
			(d->parent < 0 || d->parent >= i || defs[d->parent].kind != SIFT_COLK_GROUP))
			return "the parent of a child must be an earlier group";
	}
	return NULL;
}

static void
schema_put_def(StringInfo out, const SiftColDef *d, bool with_flags)
{
	const char *path = d->path ? d->path : "";
	SiftColDefDisk disk = {d->id, d->parent, d->kind, d->store, with_flags ? d->flags : 0, d->attno, (uint16) strlen(path), d->typid, d->scale};

	put_val(out, disk);
	put_bytes(out, path, disk.pathlen);
}

static uint32
schema_hash(const SiftColDef *defs, int n)
{
	StringInfoData buf;
	uint32		h;

	initStringInfo(&buf);
	for (int i = 0; i < n; i++)
		schema_put_def(&buf, &defs[i], false);
	h = hash_bytes((const unsigned char *) buf.data, buf.len);
	pfree(buf.data);
	return h;
}

static SiftColSchema *
schema_build(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	SiftColSchema *s = MemoryContextAllocZero(mcxt, sizeof(SiftColSchema));

	s->ncols = n;
	s->cols = MemoryContextAlloc(mcxt, n * sizeof(SiftColDef));
	for (int i = 0; i < n; i++)
	{
		const char *path = defs[i].path ? defs[i].path : "";
		size_t		len = strlen(path);
		char	   *copy = MemoryContextAlloc(mcxt, len + 1);

		memcpy(copy, path, len + 1);
		s->cols[i] = defs[i];
		s->cols[i].path = copy;
		s->ngroups += defs[i].kind == SIFT_COLK_GROUP;
	}
	s->hash = schema_hash(defs, n);
	return s;
}

SiftColSchema *
sift_cschema_create(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	const char *why = schema_check(defs, n);

	if (why)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid sift_col leaf list: %s", why)));
	return schema_build(mcxt, defs, n);
}

void
sift_cschema_serialize(const SiftColSchema *s, StringInfo out)
{
	uint32		head[3] = {COLS_MAGIC, (uint32) s->ncols, s->hash};

	put_val(out, head);
	for (int i = 0; i < s->ncols; i++)
		schema_put_def(out, &s->cols[i], true);
}

SiftColSchema *
sift_cschema_parse(MemoryContext mcxt, const void *data, Size len)
{
	const uint8 *p = data;
	uint32		head[3];
	SiftColDef *defs;
	SiftColSchema *s = NULL;
	Size		pos = sizeof(head);

	if (len < sizeof(head))
		return NULL;
	memcpy(head, p, sizeof(head));
	if (head[0] != COLS_MAGIC || head[1] < 1 || head[1] > SIFT_COL_MAX_COLS)
		return NULL;
	defs = palloc0(head[1] * sizeof(SiftColDef));
	for (uint32 i = 0; i < head[1]; i++)
	{
		SiftColDefDisk disk;
		char	   *path;

		if (len - pos < sizeof(disk))
			goto out;
		memcpy(&disk, p + pos, sizeof(disk));
		pos += sizeof(disk);
		if (len - pos < disk.pathlen)
			goto out;
		path = palloc(disk.pathlen + 1);
		memcpy(path, p + pos, disk.pathlen);
		path[disk.pathlen] = '\0';
		pos += disk.pathlen;
		defs[i] = (SiftColDef) {disk.id, disk.parent, disk.kind, disk.store, disk.flags, disk.attno, disk.typid, path, (uint8) disk.scale};
	}
	if (schema_check(defs, (int) head[1]) == NULL && schema_hash(defs, (int) head[1]) == head[2])
		s = schema_build(mcxt, defs, (int) head[1]);
out:
	for (uint32 i = 0; i < head[1]; i++)
		if (defs[i].path)
			pfree((char *) defs[i].path);
	pfree(defs);
	return s;
}

/* ---------------------------------------------------------------------------
 * 2. Dictionaries
 * ------------------------------------------------------------------------- */

#define DICT_MINCAP		16

SiftCDict *
sift_cdict_create(MemoryContext mcxt)
{
	SiftCDict  *d = MemoryContextAllocZero(mcxt, sizeof(SiftCDict));

	d->mcxt = mcxt;
	d->cap = DICT_MINCAP;
	d->off = MemoryContextAllocZero(mcxt, (d->cap + 1) * sizeof(uint32));
	d->bytescap = 256;
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	return d;
}

static void
cdict_reset(SiftCDict *d)
{
	d->n = 0;
	d->nbytes = 0;
	d->off[0] = 0;
	if (d->slots)
		memset(d->slots, 0, d->nslots * sizeof(uint32));
}

static inline uint32
cdict_len(const SiftCDict *d, uint32 code)
{
	return d->off[code + 1] - d->off[code];
}

static uint32 *
cdict_probe(const SiftCDict *d, const char *s, uint32 len)
{
	uint32		mask = d->nslots - 1;

	for (uint32 i = hash_bytes((const unsigned char *) s, (int) len) & mask;; i = (i + 1) & mask)
	{
		uint32		c = d->slots[i];

		if (c == 0 || (cdict_len(d, c - 1) == len && memcmp(d->bytes + d->off[c - 1], s, len) == 0))
			return &d->slots[i];
	}
}

/* The hash table exists and has room for one more code at load <= 1/2 (rebuilt by doubling) */
static void
cdict_ready(SiftCDict *d)
{
	if (d->slots != NULL && (d->n + 1) * 2 <= d->nslots)
		return;
	if (d->slots)
		pfree(d->slots);
	d->nslots = pg_nextpower2_32(Max(DICT_MINCAP, d->n + 1) * 4);
	d->slots = MemoryContextAllocZero(d->mcxt, d->nslots * sizeof(uint32));
	for (uint32 c = 0; c < d->n; c++)
		*cdict_probe(d, d->bytes + d->off[c], cdict_len(d, c)) = c + 1;
}

uint32
sift_cdict_intern(SiftCDict *d, const char *s, uint32 len)
{
	uint32	   *slot;

	if (len > SIFT_COL_MAX_STR)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value of %u bytes is too large for a sift_col column (maximum %d)", len, SIFT_COL_MAX_STR),
				 errhint("Index left(column, %d) or a hash of the column instead.", SIFT_COL_MAX_STR)));
	cdict_ready(d);
	slot = cdict_probe(d, s, len);
	if (*slot)
		return *slot - 1;
	if (d->n == d->cap)
	{
		d->cap *= 2;
		d->off = repalloc(d->off, (d->cap + 1) * sizeof(uint32));
	}
	if (d->nbytes + len > d->bytescap)
	{
		d->bytescap = Max(d->bytescap * 2, d->nbytes + len);
		d->bytes = repalloc(d->bytes, d->bytescap);
	}
	memcpy(d->bytes + d->nbytes, s, len);
	d->nbytes += len;
	d->off[d->n + 1] = (uint32) d->nbytes;
	*slot = d->n + 1;
	return d->n++;
}

int64
sift_cdict_find(SiftCDict *d, const char *s, uint32 len)
{
	if (d->n == 0 || len > SIFT_COL_MAX_STR)
		return -1;
	cdict_ready(d);
	return (int64) *cdict_probe(d, s, len) - 1;
}

const char *
sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len)
{
	*len = cdict_len(d, code);
	return d->bytes + d->off[code];
}

void
sift_cdict_serialize(const SiftCDict *d, StringInfo out)
{
	put_val(out, d->n);
	put_bytes(out, d->off, (d->n + 1) * sizeof(uint32));
	put_bytes(out, d->bytes, d->nbytes);
}

SiftCDict *
sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used)
{
	const uint8 *p = data;
	uint32		n;
	SiftCDict  *d;

	if (len < sizeof(uint32))
		return NULL;
	memcpy(&n, p, sizeof(n));
	if ((len - sizeof(uint32)) / sizeof(uint32) < (Size) n + 1)
		return NULL;
	d = sift_cdict_create(mcxt);
	d->n = n;
	d->cap = Max(n, DICT_MINCAP);
	d->off = MemoryContextAlloc(mcxt, ((Size) d->cap + 1) * sizeof(uint32));
	memcpy(d->off, p + sizeof(uint32), ((Size) n + 1) * sizeof(uint32));
	d->nbytes = d->off[n];
	if (d->off[0] != 0 || len - sizeof(uint32) - ((Size) n + 1) * sizeof(uint32) < d->nbytes)
		return NULL;
	for (uint32 c = 0; c < n; c++)
		if (d->off[c + 1] < d->off[c] || cdict_len(d, c) > SIFT_COL_MAX_STR)
			return NULL;
	d->bytescap = Max(d->nbytes, 256);
	d->bytes = MemoryContextAlloc(mcxt, d->bytescap);
	memcpy(d->bytes, p + sizeof(uint32) * (n + 2), d->nbytes);
	*used = sizeof(uint32) * (n + 2) + d->nbytes;
	return d;
}

void
sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view)
{
	view->n = d->n;
	view->off = d->off;
	view->bytes = d->bytes;
	view->id = id;
}

/* ---------------------------------------------------------------------------
 * 3. Batches
 * ------------------------------------------------------------------------- */

static inline bool
leaf_is_str(const SiftColDef *d)
{
	return d->store == SIFT_COLS_STR && d->kind == SIFT_COLK_SCALAR;
}

SiftColBatch *
sift_cbatch_create(MemoryContext mcxt, const SiftColSchema *schema)
{
	SiftColBatch *b = MemoryContextAllocZero(mcxt, sizeof(SiftColBatch));

	b->mcxt = mcxt;
	b->schema = schema;
	b->rowcap = 256;
	b->keys = MemoryContextAlloc(mcxt, b->rowcap * sizeof(SiftKey));
	b->col = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCVals));
	b->dict = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCDict *));
	for (int i = 0; i < schema->ncols; i++)
	{
		if (leaf_is_str(&schema->cols[i]))
			b->dict[i] = sift_cdict_create(mcxt);
		if (schema->cols[i].kind == SIFT_COLK_GROUP)
			b->col[i].eoff = MemoryContextAllocZero(mcxt, (b->rowcap + 1) * sizeof(uint32));
	}
	return b;
}

void
sift_cbatch_reset(SiftColBatch *b, uint32 sb)
{
	b->sb = sb;
	b->nrows = 0;
	for (int i = 0; i < b->schema->ncols; i++)
	{
		SiftCVals  *v = &b->col[i];

		if (v->nulls)
			memset(v->nulls, 0, words_of(v->n) * sizeof(uint64));
		if (v->ovf)
			memset(v->ovf, 0, words_of(v->n) * sizeof(uint64));
		v->n = 0;
		if (v->eoff)
			v->eoff[0] = 0;
		if (v->voff)
			v->voff[0] = 0;
		if (b->dict[i])
			cdict_reset(b->dict[i]);
	}
}

static void
bitmap_set(MemoryContext mcxt, uint64 **bits, uint32 cap, uint32 i)
{
	if (*bits == NULL)
		*bits = MemoryContextAllocZero(mcxt, words_of(cap) * sizeof(uint64));
	(*bits)[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/* Appends one slot of a leaf: the key (0 for a NULL), whether it is NULL, whether it overflowed (then also NULL) */
static void
vals_put(MemoryContext mcxt, SiftCVals *v, int64 key, bool isnull, bool ovf)
{
	if (v->n == v->cap)
	{
		uint32		cap = Max(64, v->cap * 2);

		v->key = v->key ? repalloc(v->key, cap * sizeof(int64)) : MemoryContextAlloc(mcxt, cap * sizeof(int64));
		if (v->nulls)
		{
			v->nulls = repalloc(v->nulls, words_of(cap) * sizeof(uint64));
			memset(v->nulls + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		if (v->ovf)
		{
			v->ovf = repalloc(v->ovf, words_of(cap) * sizeof(uint64));
			memset(v->ovf + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		v->cap = cap;
	}
	v->key[v->n] = isnull ? 0 : key;
	if (isnull)
		bitmap_set(mcxt, &v->nulls, v->cap, v->n);
	if (ovf)
		bitmap_set(mcxt, &v->ovf, v->cap, v->n);
	v->n++;
}

/* Appends one slot of a VAR leaf: the bytes of the value (none for a NULL) */
static void
vals_put_var(MemoryContext mcxt, SiftCVals *v, const char *s, uint32 len, bool isnull, bool ovf)
{
	uint32		start = v->n == 0 ? 0 : v->voff[v->n];

	if (isnull)
		len = 0;
	if ((uint64) start + len > (uint64) PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col: the variable-length values of a unit exceed 2 GB")));
	if (v->voff == NULL)
	{
		v->voff = MemoryContextAlloc(mcxt, sizeof(uint32));
		v->voff[0] = 0;
	}
	if (v->n == v->cap)
		v->voff = repalloc(v->voff, ((Size) Max(64, v->cap * 2) + 1) * sizeof(uint32));
	vals_put(mcxt, v, len, isnull, ovf);
	if (start + len > v->vcap)
	{
		v->vcap = Max(Max(1024, v->vcap * 2), (Size) start + len);
		v->vb = v->vb ? repalloc(v->vb, v->vcap) : MemoryContextAlloc(mcxt, v->vcap);
	}
	if (len > 0)
		memcpy(v->vb + start, s, len);
	v->voff[v->n] = start + len;
}

/* Validates the key against the batch and makes room for the row; the row is counted by batch_row_end */
static void
batch_row_begin(SiftColBatch *b, SiftKey key)
{
	if (sift_block_sb(sift_key_block(key)) != b->sb)
		elog(ERROR, "sift_col: row %u/%u is outside superblock %u", sift_key_block(key), (uint32) (key & 0xFFFF), b->sb);
	if ((key & 0xFFFF) < 1 || (key & 0xFFFF) > MaxHeapTuplesPerPage)
		elog(ERROR, "sift_col: row offset %u is not a heap offset", (uint32) (key & 0xFFFF));
	if (b->nrows > 0 && key <= b->keys[b->nrows - 1])
		elog(ERROR, "sift_col: rows must be added in ascending TID order");
	if (b->nrows == b->rowcap)
	{
		b->rowcap *= 2;
		b->keys = repalloc(b->keys, b->rowcap * sizeof(SiftKey));
		for (int i = 0; i < b->schema->ncols; i++)
			if (b->col[i].eoff)
				b->col[i].eoff = repalloc(b->col[i].eoff, ((Size) b->rowcap + 1) * sizeof(uint32));
	}
	b->keys[b->nrows] = key;
}

static void
batch_group_end(SiftColBatch *b, int group, uint32 count)
{
	b->col[group].eoff[b->nrows + 1] = b->col[group].eoff[b->nrows] + count;
}

static void
put_value(SiftColBatch *b, int id, const SiftColValue *v)
{
	bool		isnull = v->isnull || v->overflow;
	int64		key = 0;

	if (b->schema->cols[id].store == SIFT_COLS_VAR)
	{
		vals_put_var(b->mcxt, &b->col[id], v->v.s, v->len, isnull, v->overflow);
		return;
	}
	if (!isnull)
		switch (b->schema->cols[id].store)
		{
			case SIFT_COLS_INT:
				key = v->v.i;
				break;
			case SIFT_COLS_STR:
				key = sift_cdict_intern(b->dict[id], v->v.s, v->len);
				break;
			default:
				key = sift_col_f8_key(v->v.f);
		}
	vals_put(b->mcxt, &b->col[id], key, isnull, v->overflow);
}

void
sift_cbatch_add(SiftColBatch *b, const SiftColRow *row)
{
	const SiftColSchema *s = b->schema;

	batch_row_begin(b, sift_key_from_tid((ItemPointer) &row->tid));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColValue *v = &row->val[id];
		uint32		count = 0;

		if (s->cols[id].parent >= 0)
			continue;
		put_value(b, id, v);
		if (s->cols[id].kind != SIFT_COLK_GROUP)
			continue;
		if (!v->isnull && !v->overflow)
		{
			if (v->v.i < 0 || v->v.i > PG_INT32_MAX)
				elog(ERROR, "sift_col: array of %lld elements", (long long) v->v.i);
			count = (uint32) v->v.i;
		}
		batch_group_end(b, id, count);
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
				for (uint32 j = 0; j < count; j++)
					put_value(b, c, &row->elem[c][j]);
	}
	b->nrows++;
}

void
sift_cbatch_value(const SiftColBatch *b, int col, uint32 slot, SiftColValue *v)
{
	const SiftCVals *vals = &b->col[col];
	int64		key = vals->key[slot];

	memset(v, 0, sizeof(*v));
	v->isnull = sift_col_bit_test(vals->nulls, slot);
	v->overflow = sift_col_bit_test(vals->ovf, slot);
	if (v->isnull)
		return;
	switch (b->schema->cols[col].store)
	{
		case SIFT_COLS_INT:
			v->v.i = key;
			break;
		case SIFT_COLS_STR:
			v->v.s = b->dict[col]->bytes + b->dict[col]->off[key];
			v->len = cdict_len(b->dict[col], (uint32) key);
			break;
		case SIFT_COLS_VAR:
			v->v.s = vals->vb + vals->voff[slot];
			v->len = (uint32) key;
			break;
		default:
			v->v.f = sift_col_f8_from_key(key);
	}
}

/* The elements of a row's arrays live in one block; elem[ncols] points at its values, the header holds the capacity */
typedef struct ElemBlock
{
	Size		cap;
	SiftColValue v[FLEXIBLE_ARRAY_MEMBER];
} ElemBlock;

#define ELEM_BLOCK(p)	((ElemBlock *) ((char *) (p) - offsetof(ElemBlock, v)))

void
sift_cbatch_row(const SiftColBatch *b, uint32 r, SiftColRow *row)
{
	const SiftColSchema *s = b->schema;
	Size		total = 0;
	SiftColValue *next;

	if (row->val == NULL)
	{
		row->val = MemoryContextAllocZero(b->mcxt, s->ncols * sizeof(SiftColValue));
		row->elem = MemoryContextAllocZero(b->mcxt, (s->ncols + 1) * sizeof(SiftColValue *));
	}
	row->ncols = s->ncols;
	sift_key_to_tid(b->keys[r], &row->tid);
	for (int id = 0; id < s->ncols; id++)
		if (s->cols[id].kind == SIFT_COLK_GROUP)
			for (int c = id + 1; c < s->ncols; c++)
				if (s->cols[c].parent == id)
					total += b->col[id].eoff[r + 1] - b->col[id].eoff[r];

	if (total > 0 && (row->elem[s->ncols] == NULL || ELEM_BLOCK(row->elem[s->ncols])->cap < total))
	{
		ElemBlock  *blk;

		if (row->elem[s->ncols])
			pfree(ELEM_BLOCK(row->elem[s->ncols]));
		blk = MemoryContextAlloc(b->mcxt, offsetof(ElemBlock, v) + Max(total, 16) * sizeof(SiftColValue));
		blk->cap = Max(total, 16);
		row->elem[s->ncols] = blk->v;
	}
	next = row->elem[s->ncols];
	memset(row->elem, 0, s->ncols * sizeof(SiftColValue *));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
		{
			memset(&row->val[id], 0, sizeof(SiftColValue));
			row->val[id].isnull = true;
			continue;
		}
		sift_cbatch_value(b, id, r, &row->val[id]);
		if (d->kind != SIFT_COLK_GROUP)
			continue;
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
			{
				uint32		first = b->col[id].eoff[r],
							count = b->col[id].eoff[r + 1] - first;

				row->elem[c] = next;
				for (uint32 j = 0; j < count; j++)
					sift_cbatch_value(b, c, first + j, next++);
			}
	}
}

void
sift_cbatch_row_free(SiftColRow *row)
{
	if (row->val == NULL)
		return;
	if (row->elem[row->ncols])
		pfree(ELEM_BLOCK(row->elem[row->ncols]));
	pfree(row->elem);
	pfree(row->val);
	memset(row, 0, sizeof(*row));
}

static void
batch_copy_row(SiftColBatch *dst, const SiftColBatch *src, uint32 r, SiftColRow *scratch)
{
	sift_cbatch_row(src, r, scratch);
	sift_cbatch_add(dst, scratch);
}

void
sift_cbatch_add_from(SiftColBatch *dst, const SiftColBatch *src, uint32 r)
{
	SiftColRow	row = {0};

	batch_copy_row(dst, src, r, &row);
	sift_cbatch_row_free(&row);
}

void
sift_cbatch_merge(SiftColBatch *dst, SiftColBatch *const *src, int nsrc)
{
	uint32	   *pos = palloc0(nsrc * sizeof(uint32));
	SiftColRow	row = {0};

	for (;;)
	{
		int			first = -1;
		SiftKey		min = 0;

		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && (first < 0 || src[i]->keys[pos[i]] < min))
			{
				first = i;
				min = src[i]->keys[pos[i]];
			}
		if (first < 0)
			break;
		batch_copy_row(dst, src[first], pos[first], &row);
		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && src[i]->keys[pos[i]] == min)
				pos[i]++;
	}
	sift_cbatch_row_free(&row);
	pfree(pos);
}

/* ---------------------------------------------------------------------------
 * 4. Zones
 * ------------------------------------------------------------------------- */

/* The one definition of a zone (docs/COL.md 2.1): the encoder and the verification of a stored chunk both come through here */
static void
zone_of_keys(const int64 *key, const uint64 *nulls, const uint64 *ovf, uint32 n, SiftColStore store, SiftCZone *z)
{
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN,
				carry = 0;		/* the sum is carry * 2^64 + sum (unsigned): exact, whatever the order of the additions */
	uint64		sum = 0;
	uint32		nnull = 0;
	bool		nosum;

	memset(z, 0, sizeof(*z));
	z->nvals = n;
	for (uint32 i = 0; i < n; i++)
	{
		int64		k = key[i];

		if (sift_col_bit_test(nulls, i))
		{
			nnull++;
			continue;
		}
		mn = Min(mn, k);
		mx = Max(mx, k);
		if (store == SIFT_COLS_INT || store == SIFT_COLS_VAR)		/* VAR: the keys are the lengths, their zone is no use but is exact */
		{
			uint64		before = sum;

			sum += (uint64) k;
			carry += (sum < before) - (k < 0);
		}
		else if (store == SIFT_COLS_STR)
			z->set[(k & 255) >> 6] |= UINT64CONST(1) << (k & 63);
		else
		{
			double		d = sift_col_f8_from_key(k);

			z->sum_f += d;
			z->flags |= d != d ? SIFT_COLZ_NAN : 0;
		}
	}
	z->nnull = nnull;
	if (nnull < n)
	{
		z->min = mn;
		z->max = mx;
		z->flags |= mn == mx ? SIFT_COLZ_CONST : 0;
	}
	nosum = !((carry == 0 && (int64) sum >= 0) || (carry == -1 && (int64) sum < 0));	/* the sum is outside int64 */
	z->sum_i = nosum ? 0 : (int64) sum;
	z->flags |= nosum ? SIFT_COLZ_NOSUM : 0;
	for (uint32 w = 0; ovf != NULL && w < words_of(n); w++)
		z->flags |= ovf[w] ? SIFT_COLZ_OVERFLOW : 0;
}

/* ---------------------------------------------------------------------------
 * 5. Codec seam: the plain FOR and PACK encodings of docs/COL.md 2.2. col-compress replaces this section (what a chunk of a given span
 * looks like, its size, its validity, and how it is written and read); the rest of the file only goes through these functions.
 * ------------------------------------------------------------------------- */

#define CODEC_BLOCK		4096	/* values unpacked at a time (a multiple of 64) */

static inline uint32
codec_slot(uint8 bits)
{
	return pg_rightmost_one_pos32(bits) - 3;	/* 8, 16, 32, 64 -> SIFT_COLW_8 .. SIFT_COLW_64 */
}

static bool
codec_valid(uint8 enc, uint8 bits)
{
	switch (enc)
	{
		case SIFT_COLENC_CONST:
			return bits == 0;
		case SIFT_COLENC_FOR:
			return bits == 8 || bits == 16 || bits == 32 || bits == 64;
		case SIFT_COLENC_PACK:
			return bits >= 1 && bits <= 31 && bits != 8 && bits != 16;
		case SIFT_COLENC_CC:
		case SIFT_COLENC_STRS:
			return bits == 0;
		default:
			return false;
	}
}

/* Payload bytes of n values */
static uint64
codec_paylen(uint8 enc, uint8 bits, uint32 n)
{
	if (enc == SIFT_COLENC_CONST)
		return 0;
	return enc == SIFT_COLENC_FOR ? (uint64) n * (bits / 8) : (uint64) words_of(n) * 8 * bits;
}

/* The smallest encoding that holds values 0..span */
static void
codec_choose(uint64 span, uint32 n, uint8 *enc, uint8 *bits)
{
	uint8		w = span == 0 ? 0 : pg_leftmost_one_pos64(span) + 1,
				fw = w <= 8 ? 8 : w <= 16 ? 16 : w <= 32 ? 32 : 64;

	*enc = SIFT_COLENC_FOR;
	*bits = fw;
	if (w == 0)
	{
		*enc = SIFT_COLENC_CONST;
		*bits = 0;
	}
	else if (codec_valid(SIFT_COLENC_PACK, w) && codec_paylen(SIFT_COLENC_PACK, w, n) < codec_paylen(SIFT_COLENC_FOR, fw, n))
	{
		*enc = SIFT_COLENC_PACK;
		*bits = w;
	}
}

/* Appends the payload of the n unsigned values u[] (each below 2^bits) */
static void
codec_put(StringInfo out, uint8 enc, uint8 bits, const uint64 *u, uint32 n)
{
	Size		len = codec_paylen(enc, bits, n);
	uint8	   *dst;

	enlargeStringInfo(out, (int) len);
	dst = (uint8 *) out->data + out->len;
	if (enc == SIFT_COLENC_FOR)
		for (uint32 i = 0; i < n; i++)
		{
			switch (bits)
			{
				case 8:
					dst[i] = (uint8) u[i];
					break;
				case 16:
					{
						uint16		x = (uint16) u[i];

						memcpy(dst + 2 * i, &x, sizeof(x));
					}
					break;
				case 32:
					{
						uint32		x = (uint32) u[i];

						memcpy(dst + 4 * i, &x, sizeof(x));
					}
					break;
				default:
					memcpy(dst + 8 * i, &u[i], sizeof(uint64));
			}
		}
	else
		for (uint32 s = 0; s < words_of(n) * 64; s += 64)
		{
			uint64		acc = 0;
			int			nb = 0;

			for (uint32 i = s; i < s + 64; i++)
			{
				acc |= (i < n ? u[i] : 0) << nb;
				nb += bits;
				if (nb >= 32)
				{
					uint32		x = (uint32) acc;

					memcpy(dst, &x, sizeof(x));
					dst += 4;
					acc >>= 32;
					nb -= 32;
				}
			}
		}
	out->len += (int) len;
	out->data[out->len] = '\0';
}

/* The codec's reader of a CC chunk (the blob is validated; verify also checks its check word) */
static SiftCc *
cc_open(const SiftCChunk *c, bool verify)
{
	SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, c->payload, c->paylen, verify);

	if (sift_cc_nrows(cc) != c->n)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the blob has %u values, the chunk %u",
																 sift_cc_nrows(cc), c->n)));
	return cc;
}

void
sift_cchunk_strs(const SiftCChunk *c, MemoryContext mcxt, uint32 **off, char **bytes)
{
	if (c->enc != SIFT_COLENC_STRS)
		elog(ERROR, "sift_col: chunk encoding %d holds no strings", c->enc);
	if (sift_cc_decode_strs(mcxt, c->payload, c->paylen, off, bytes) != c->n)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: the string list does not hold %u values", c->n)));
}

/* The lengths of the strings of a STRS chunk, the keys of its zone and its row tests */
static void
strs_lengths(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *off;
	char	   *bytes;

	sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	for (uint32 k = 0; k < m; k++)
		dst[k] = off[(idx ? idx[k] : k) + 1] - off[idx ? idx[k] : k];
	pfree(off);
	pfree(bytes);
}

/* dst[0..n) = base + payload (the NULL slots hold base) */
static void
codec_decode(const SiftCChunk *c, int64 *dst)
{
	uint32		tmp[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
			strs_lengths(c, NULL, c->n, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode(cc, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 i = 0; i < c->n; i++)
				dst[i] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64[codec_slot(c->bits)](c->payload, c->n, c->base, dst);
			break;
		default:
			for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
			{
				uint32		m = Min(CODEC_BLOCK, c->n - s);

				sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, m, c->base, dst + s);
			}
	}
}

static void
codec_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	uint32	   *tmp;

	switch (c->enc)
	{
		case SIFT_COLENC_STRS:
			strs_lengths(c, idx, m, dst);
			break;
		case SIFT_COLENC_CC:
			{
				SiftCc	   *cc = cc_open(c, false);

				sift_cc_decode_sel(cc, idx, m, dst);
				sift_cc_close(cc);
			}
			break;
		case SIFT_COLENC_CONST:
			for (uint32 k = 0; k < m; k++)
				dst[k] = c->base;
			break;
		case SIFT_COLENC_FOR:
			sift_colk->for_i64_sel[codec_slot(c->bits)](c->payload, idx, m, c->base, dst);
			break;
		default:
			tmp = palloc(c->n * sizeof(uint32) + SIFT_COL_PAD);
			sift_colk->unpack(c->payload, c->bits, c->n, tmp);
			sift_colk->for_i64_sel[SIFT_COLW_32](tmp, idx, m, c->base, dst);
			pfree(tmp);
	}
}

/* The widest payload value an encoding can hold */
static uint64
codec_maxval(const SiftCChunk *c)
{
	return c->enc == SIFT_COLENC_CONST ? 0 : c->bits >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << c->bits) - 1;
}

/* ---------------------------------------------------------------------------
 * 6. Chunks
 * ------------------------------------------------------------------------- */

#define CHUNK_HEAD		16
#define CHUNK_MAX_N		(1U << 28)

/* Appends the bitmap of n bits (NULL: all clear), tail bits cleared */
static void
put_bitmap(StringInfo out, const uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
	{
		uint64		x = bits ? bits[w] : 0;

		if (w == n / 64)
			x &= (UINT64CONST(1) << (n % 64)) - 1;
		put_val(out, x);
	}
}

/* Appends the chunk of n keys (NULL slots hold 0) and returns its zone; pol: the codec's policy of the leaf, NULL for the plain form */
static void
chunk_encode(StringInfo out, SiftColStore store, const SiftCVals *vals, const int64 *key, const SiftCcPolicy *pol, SiftCZone *z)
{
	const uint64 *nulls = vals->nulls,
			   *ovf = vals->ovf;
	uint32		n = vals->n;
	uint8		enc,
				bits,
				flags;
	uint64		span,
			   *u;
	int64		base;
	uint8		head[8];

	zone_of_keys(key, nulls, ovf, n, store, z);
	flags = (z->nnull > 0 ? SIFT_COLC_NULLS : 0) | ((z->flags & SIFT_COLZ_OVERFLOW) ? SIFT_COLC_OVF : 0);
	span = (uint64) z->max - (uint64) z->min;
	codec_choose(span, n, &enc, &bits);
	if (pol != NULL && enc != SIFT_COLENC_CONST)
		enc = SIFT_COLENC_CC, bits = 0;
	if (store == SIFT_COLS_VAR)
		enc = SIFT_COLENC_STRS, bits = 0;
	head[0] = enc;
	head[1] = bits;
	head[2] = flags;
	head[3] = 0;
	memcpy(head + 4, &n, sizeof(n));
	put_bytes(out, head, sizeof(head));
	base = enc == SIFT_COLENC_CC || enc == SIFT_COLENC_STRS ? 0 : z->min;
	put_val(out, base);
	if (flags & SIFT_COLC_NULLS)
		put_bitmap(out, nulls, n);
	if (flags & SIFT_COLC_OVF)
		put_bitmap(out, ovf, n);
	if (enc == SIFT_COLENC_STRS)
	{
		SiftCcPolicy dp;

		if (pol == NULL)
			sift_cc_policy_default(&dp);
		sift_cc_encode_strs(out, vals->voff, vals->vb, n, pol ? pol : &dp);
	}
	else if (enc == SIFT_COLENC_CC)
		sift_cc_encode(out, store == SIFT_COLS_F4 ? SIFT_CC_F4 : store == SIFT_COLS_F8 ? SIFT_CC_F8 : SIFT_CC_INT, key, n, nulls, pol, NULL);
	else if (enc != SIFT_COLENC_CONST)
	{
		u = palloc(Max(n, 1) * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
			u[i] = sift_col_bit_test(nulls, i) ? 0 : (uint64) key[i] - (uint64) z->min;
		codec_put(out, enc, bits, u, n);
		pfree(u);
	}
	put_pad(out, 8);
}

bool
sift_cchunk_parse(const uint8 *p, uint32 len, SiftCChunk *c)
{
	uint64		pos = CHUNK_HEAD,
				words;
	bool		whole;

	if (len < CHUNK_HEAD || ((uintptr_t) p & 7) != 0)
		return false;
	memset(c, 0, sizeof(*c));
	c->enc = p[0];
	c->bits = p[1];
	c->flags = p[2];
	memcpy(&c->n, p + 4, sizeof(c->n));
	memcpy(&c->base, p + 8, sizeof(c->base));
	if ((c->flags & ~(SIFT_COLC_NULLS | SIFT_COLC_OVF)) != 0 || ((c->flags & SIFT_COLC_OVF) && !(c->flags & SIFT_COLC_NULLS)) ||
		!codec_valid(c->enc, c->bits) || c->n > CHUNK_MAX_N)
		return false;
	words = words_of(c->n);
	if (c->flags & SIFT_COLC_NULLS)
	{
		c->nulls = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	if (c->flags & SIFT_COLC_OVF)
	{
		c->ovf = (const uint64 *) (p + pos);
		pos += words * 8;
	}
	whole = c->enc == SIFT_COLENC_CC || c->enc == SIFT_COLENC_STRS;
	c->paylen = whole ? (pos <= len ? (uint32) (len - pos) : 0) : (uint32) codec_paylen(c->enc, c->bits, c->n);
	if (pos + c->paylen > len || (whole && (c->paylen == 0 || c->paylen % 8 != 0 || c->n == 0)))
		return false;
	c->payload = p + pos;
	if (c->n % 64 != 0)
	{
		uint64		tail = ~UINT64CONST(0) << (c->n % 64);

		if ((c->nulls && (c->nulls[words - 1] & tail)) || (c->ovf && (c->ovf[words - 1] & tail)))
			return false;
	}
	for (uint64 w = 0; c->ovf != NULL && w < words; w++)
		if (c->ovf[w] & ~c->nulls[w])
			return false;
	return true;
}

static void
zero_nulls(const uint64 *nulls, uint32 n, int64 *dst)
{
	for (uint32 w = 0; nulls != NULL && w < words_of(n); w++)
		for (uint64 m = nulls[w]; m; m &= m - 1)
			dst[w * 64 + pg_rightmost_one_pos64(m)] = 0;
}

void
sift_cchunk_decode(const SiftCChunk *c, int64 *dst)
{
	codec_decode(c, dst);
	zero_nulls(c->nulls, c->n, dst);
}

void
sift_cchunk_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst)
{
	codec_decode_sel(c, idx, m, dst);
	for (uint32 k = 0; c->nulls != NULL && k < m; k++)
		if (sift_col_bit_test(c->nulls, idx[k]))
			dst[k] = 0;
}

void
sift_cchunk_zone(const SiftCChunk *c, SiftColStore store, SiftCZone *z)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	if (c->enc == SIFT_COLENC_CC)
		sift_cc_close(cc_open(c, true));
	sift_cchunk_decode(c, key);	/* a STRS chunk is verified by its decoding */
	zone_of_keys(key, c->nulls, c->ovf, c->n, store, z);
	pfree(key);
}

/* ---------------------------------------------------------------------------
 * 7. Chunk tests (docs/COL.md 2.2)
 *
 * A predicate becomes a test in the key domain. Floats need no rounding of the constant: F4 keys are the F8 keys of the promoted value, so
 * comparing a float4 column against a float8 constant in the key domain is exactly PostgreSQL's float48 comparison.
 * ------------------------------------------------------------------------- */

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static bool
set_has(const SiftCTest *t, int64 k)
{
	return bsearch(&k, t->set, t->nset, sizeof(int64), cmp_i64) != NULL;
}

static void
ctest_range(SiftCTest *t, int64 lo, int64 hi)
{
	t->kind = lo > hi ? SIFT_CT_NONE : SIFT_CT_RANGE;
	t->lo = lo;
	t->hi = hi;
}

/* Sorted distinct keys -> NONE/NONNULL (none), a range (one) or a set */
static void
ctest_keys(SiftCTest *t, int64 *keys, uint32 n, bool negate)
{
	uint32		m = 0;

	qsort(keys, n, sizeof(int64), cmp_i64);
	for (uint32 i = 0; i < n; i++)
		if (m == 0 || keys[m - 1] != keys[i])
			keys[m++] = keys[i];
	if (m == 0)
		t->kind = negate ? SIFT_CT_NONNULL : SIFT_CT_NONE;
	else if (m == 1)
	{
		ctest_range(t, keys[0], keys[0]);
		t->kind = negate ? SIFT_CT_NRANGE : SIFT_CT_RANGE;
	}
	else
	{
		t->kind = negate ? SIFT_CT_NSET : SIFT_CT_SET;
		t->set = keys;
		t->nset = m;
		t->lo = keys[0];
		t->hi = keys[m - 1];
	}
}

static int64
const_key(const SiftColDef *def, const SiftColValue *v)
{
	return def->store == SIFT_COLS_INT ? v->v.i : sift_col_f8_key(v->v.f);
}

static void
ctest_make_num(const SiftColDef *def, const SiftColPred *p, SiftCTest *t)
{
	int64		c = p->nvals > 0 ? const_key(def, &p->vals[0]) : 0;

	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
			ctest_range(t, c, c);
			t->kind = p->op == SIFT_COLOP_EQ ? SIFT_CT_RANGE : SIFT_CT_NRANGE;
			break;
		case SIFT_COLOP_LT:
			if (c == PG_INT64_MIN)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, PG_INT64_MIN, c - 1);
			break;
		case SIFT_COLOP_LE:
			ctest_range(t, PG_INT64_MIN, c);
			break;
		case SIFT_COLOP_GT:
			if (c == PG_INT64_MAX)
				t->kind = SIFT_CT_NONE;
			else
				ctest_range(t, c + 1, PG_INT64_MAX);
			break;
		case SIFT_COLOP_GE:
			ctest_range(t, c, PG_INT64_MAX);
			break;
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			{
				int64	   *keys = palloc(Max(p->nvals, 1) * sizeof(int64));

				for (int i = 0; i < p->nvals; i++)
					keys[i] = const_key(def, &p->vals[i]);
				ctest_keys(t, keys, p->nvals, p->op == SIFT_COLOP_NOTIN);
			}
			break;
		default:
			elog(ERROR, "sift_col: operator %d is not defined on a numeric column", p->op);
	}
}

/* Bytewise order of dictionary values (the C collation) */
static int
str_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			r = memcmp(a, b, Min(alen, blen));

	return r != 0 ? r : (alen > blen) - (alen < blen);
}

static bool
str_matches(SiftColOp op, const char *s, uint32 len, const SiftColValue *c)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			return str_cmp(s, len, c->v.s, c->len) < 0;
		case SIFT_COLOP_LE:
			return str_cmp(s, len, c->v.s, c->len) <= 0;
		case SIFT_COLOP_GT:
			return str_cmp(s, len, c->v.s, c->len) > 0;
		case SIFT_COLOP_GE:
			return str_cmp(s, len, c->v.s, c->len) >= 0;
		default:					/* PREFIX */
			return len >= c->len && memcmp(s, c->v.s, c->len) == 0;
	}
}

/* Turns the bit set of matching codes into a LUT test: none -> NONE, all -> NONNULL, else the LUT with its bounds and fold */
static void
ctest_lut_finish(SiftCTest *t, uint64 *lut, uint32 ncodes)
{
	uint32		count = 0;

	t->kind = SIFT_CT_LUT;
	t->lo = PG_INT64_MAX;
	t->hi = PG_INT64_MIN;
	for (uint32 c = 0; c < ncodes; c++)
		if (sift_col_bit_test(lut, c))
		{
			count++;
			t->lo = Min(t->lo, c);
			t->hi = Max(t->hi, c);
			t->fold[(c & 255) >> 6] |= UINT64CONST(1) << (c & 63);
		}
	t->fold_valid = true;
	t->lut = lut;
	t->lutbits = SIFT_COL_NWORDS(ncodes) * 64;
	if (count == 0)
		t->kind = SIFT_CT_NONE;
	else if (count == ncodes)
		t->kind = SIFT_CT_NONNULL;
}

static void
ctest_make_str(SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	uint64	   *lut;
	int64	   *codes = NULL;
	uint32		nfound = 0;

	if (dict == NULL)
		elog(ERROR, "sift_col: a test on a text column needs its dictionary");
	lut = palloc0(SIFT_COL_NWORDS(dict->n) * sizeof(uint64));
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_NE:
		case SIFT_COLOP_IN:
		case SIFT_COLOP_NOTIN:
			codes = palloc(Max(p->nvals, 1) * sizeof(int64));
			for (int i = 0; i < (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_NE ? 1 : p->nvals); i++)
			{
				int64		code = sift_cdict_find(dict, p->vals[i].v.s, p->vals[i].len);

				if (code >= 0)
					codes[nfound++] = code;
			}
			ctest_keys(t, codes, nfound, p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN);
			if (t->kind == SIFT_CT_RANGE)
			{
				t->fold[(t->lo & 255) >> 6] = UINT64CONST(1) << (t->lo & 63);
				t->fold_valid = true;
			}
			if (t->kind == SIFT_CT_SET || t->kind == SIFT_CT_NSET)
			{
				/* several codes: the same answer as a LUT over the dictionary */
				SiftCTest	set = *t;

				memset(t, 0, sizeof(*t));
				for (uint32 c = 0; c < dict->n; c++)
					if (set_has(&set, c) != (set.kind == SIFT_CT_NSET))
						lut[c >> 6] |= UINT64CONST(1) << (c & 63);
				ctest_lut_finish(t, lut, dict->n);
			}
			return;
		default:
			for (uint32 c = 0; c < dict->n; c++)
				if (str_matches(p->op, SIFT_COL_DICT_STR(dict, c), cdict_len(dict, c), &p->vals[0]))
					lut[c >> 6] |= UINT64CONST(1) << (c & 63);
			ctest_lut_finish(t, lut, dict->n);
	}
}

void
sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	memset(t, 0, sizeof(*t));
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
		t->kind = p->op == SIFT_COLOP_ISNULL ? SIFT_CT_ISNULL : SIFT_CT_NOTNULL;
	else if (def->store == SIFT_COLS_VAR)
		elog(ERROR, "sift_col: a variable-length leaf is tested for NULL only");
	else if (p->nvals < 1 && p->op != SIFT_COLOP_IN && p->op != SIFT_COLOP_NOTIN)
		elog(ERROR, "sift_col: predicate without a constant");
	else if (def->store == SIFT_COLS_STR)
		ctest_make_str(dict, p, t);
	else
		ctest_make_num(def, p, t);
}

bool
sift_ctest_zone(const SiftCTest *t, const SiftCZone *z)
{
	if (t->kind == SIFT_CT_NONE)
		return false;
	if (z->flags & SIFT_COLZ_OVERFLOW)
		return true;
	if (t->kind == SIFT_CT_ISNULL)
		return z->nnull > 0;
	if (z->nnull == z->nvals)
		return false;
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
		case SIFT_CT_LUT:
		case SIFT_CT_SET:
			if (z->max < t->lo || z->min > t->hi)
				return false;
			if (t->kind == SIFT_CT_SET)
			{
				const int64 *first = t->set;
				uint32		n = t->nset;

				while (n > 0)		/* the first member >= z->min */
				{
					uint32		h = n / 2;

					if (first[h] < z->min)
					{
						first += h + 1;
						n -= h + 1;
					}
					else
						n = h;
				}
				return first < t->set + t->nset && *first <= z->max;
			}
			if (t->fold_valid)
				return ((t->fold[0] & z->set[0]) | (t->fold[1] & z->set[1]) | (t->fold[2] & z->set[2]) | (t->fold[3] & z->set[3])) != 0;
			return true;
		case SIFT_CT_NRANGE:
			return z->min < t->lo || z->max > t->hi;
		case SIFT_CT_NSET:
			return !(z->min == z->max && set_has(t, z->min));
		default:					/* NONNULL, NOTNULL */
			return true;
	}
}

/* The test applied to one key: the specification of the vector paths below (and their fallback) */
static bool
key_matches(const SiftCTest *t, int64 k)
{
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
			return k >= t->lo && k <= t->hi;
		case SIFT_CT_NRANGE:
			return k < t->lo || k > t->hi;
		case SIFT_CT_LUT:
			return k >= 0 && k < t->lutbits && sift_col_bit_test(t->lut, (uint32) k);
		case SIFT_CT_SET:
			return set_has(t, k);
		default:					/* NSET */
			return !set_has(t, k);
	}
}

static void
bits_fill(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
		bits[w] = w == n / 64 ? (UINT64CONST(1) << (n % 64)) - 1 : ~UINT64CONST(0);
}

/* tmp (padded words of n bits) = rows whose key is in [lo, hi] (null slots hold base and are removed by the caller) */
static void
chunk_range(const SiftCChunk *c, int64 lo, int64 hi, uint64 *tmp)
{
	uint64		ulo,
				uhi,
				maxv = codec_maxval(c);
	uint32		nw = SIFT_COL_NWORDS(c->n);

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		SiftCc	   *cc = cc_open(c, false);

		sift_cc_filter(cc, lo, hi, tmp);
		sift_cc_close(cc);
		return;
	}
	if (lo > hi || hi < c->base)
		return;
	ulo = lo <= c->base ? 0 : (uint64) lo - (uint64) c->base;
	uhi = Min((uint64) hi - (uint64) c->base, maxv);
	if (ulo > uhi)
		return;
	if (c->enc == SIFT_COLENC_CONST)
		bits_fill(tmp, c->n);
	else if (c->enc == SIFT_COLENC_FOR)
		sift_colk->range[codec_slot(c->bits)](c->payload, c->n, ulo, uhi - ulo, tmp);
	else
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			sift_colk->range[SIFT_COLW_32](blk, m, ulo, uhi - ulo, tmp + s / 64);
		}
	}
}

/* tmp = rows matching, evaluated key by key (sets of many members, and what has no vector path) */
static void
chunk_scalar(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	int64	   *key = palloc((c->n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	sift_cchunk_decode(c, key);
	for (uint32 i = 0; i < c->n; i++)
		if (key_matches(t, key[i]))
			tmp[i >> 6] |= UINT64CONST(1) << (i & 63);
	pfree(key);
}

/* dst (nbits) = src (srcbits) shifted down by `by` bits, zero past the end of src */
static void
lut_shift(const uint64 *src, uint64 srcbits, uint64 by, uint64 *dst, uint32 nbits)
{
	uint64		srcwords = (srcbits + 63) / 64;

	for (uint32 w = 0; w < words_of(nbits); w++)
	{
		uint64		i = (by >> 6) + w,
					lo = i < srcwords ? src[i] : 0,
					hi = i + 1 < srcwords ? src[i + 1] : 0;

		dst[w] = (by & 63) ? (lo >> (by & 63)) | (hi << (64 - (by & 63))) : lo;
	}
}

/* chunk_lut of a CC chunk: the codec hands the codes as offsets from its minimum */
static void
cc_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	SiftCc	   *cc = cc_open(c, false);
	int64		min = sift_cc_min(cc);
	uint32	   *off;
	uint64	   *shifted;
	uint32		sbits;

	if (min >= 0 && (uint64) min < t->lutbits)
	{
		off = palloc(SIFT_CC_CAP32(c->n) * sizeof(uint32));
		sbits = t->lutbits - (uint32) min;
		shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
		lut_shift(t->lut, t->lutbits, min, shifted, sbits);
		if (sift_cc_decode_off32(cc, off))
			sift_colk->lut[2](off, c->n, shifted, tmp);
		else
			chunk_scalar(t, c, tmp);
		pfree(shifted);
		pfree(off);
	}
	sift_cc_close(cc);
}

/* tmp = rows whose code is in the LUT; the chunk holds code - base, so the LUT is shifted by base */
static void
chunk_lut(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				sbits;
	uint64	   *shifted;

	memset(tmp, 0, nw * sizeof(uint64));
	if (c->enc == SIFT_COLENC_CC)
	{
		cc_lut(t, c, tmp);
		return;
	}
	if (c->base < 0 || (uint64) c->base >= t->lutbits)
		return;
	if (c->enc == SIFT_COLENC_CONST)
	{
		if (sift_col_bit_test(t->lut, (uint32) c->base))
			bits_fill(tmp, c->n);
		return;
	}
	if (c->enc == SIFT_COLENC_FOR && c->bits == 64)		/* codes are 32 bits: only a foreign writer makes this */
	{
		chunk_scalar(t, c, tmp);
		return;
	}
	sbits = c->enc == SIFT_COLENC_FOR && c->bits == 8 ? 256 : c->enc == SIFT_COLENC_FOR && c->bits == 16 ? 65536 : t->lutbits - (uint32) c->base;
	shifted = palloc0(SIFT_COL_NWORDS(sbits) * sizeof(uint64));
	lut_shift(t->lut, t->lutbits, c->base, shifted, sbits);
	if (c->enc == SIFT_COLENC_FOR && c->bits <= 16)
		sift_colk->lut[codec_slot(c->bits)](c->payload, c->n, shifted, tmp);
	else if (c->enc == SIFT_COLENC_FOR && c->bits == 32)
		sift_colk->lut[2](c->payload, c->n, shifted, tmp);
	else if (c->enc == SIFT_COLENC_PACK)
	{
		uint32		blk[CODEC_BLOCK + SIFT_COL_PAD / sizeof(uint32)];

		for (uint32 s = 0; s < c->n; s += CODEC_BLOCK)
		{
			uint32		m = Min(CODEC_BLOCK, c->n - s);

			sift_colk->unpack(c->payload + (s / 64) * 8 * c->bits, c->bits, m, blk);
			sift_colk->lut[2](blk, m, shifted, tmp + s / 64);
		}
	}
	pfree(shifted);
}

/* Sets of up to this many members are OR-ed range tests, larger ones are searched */
#define SET_RANGES_MAX	8

static void
chunk_set(const SiftCTest *t, const SiftCChunk *c, uint64 *tmp)
{
	uint64	   *one = palloc(SIFT_COL_NWORDS(c->n) * sizeof(uint64));

	memset(tmp, 0, SIFT_COL_NWORDS(c->n) * sizeof(uint64));
	for (uint32 i = 0; i < t->nset; i++)
	{
		chunk_range(c, t->set[i], t->set[i], one);
		sift_colk->bits_or(tmp, one, SIFT_COL_NWORDS(c->n));
	}
	pfree(one);
}

void
sift_ctest_run(const SiftCTest *t, const SiftCChunk *c, uint64 *bits)
{
	uint32		nw = SIFT_COL_NWORDS(c->n),
				cw = words_of(c->n);
	uint64	   *tmp;
	bool		negate = t->kind == SIFT_CT_NRANGE || t->kind == SIFT_CT_NSET;

	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(bits, 0, nw * sizeof(uint64));
			return;
		case SIFT_CT_ISNULL:
			if (c->nulls)
				sift_colk->bits_and(bits, c->nulls, cw);
			memset(bits + (c->nulls ? cw : 0), 0, (nw - (c->nulls ? cw : 0)) * sizeof(uint64));
			return;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
			if (c->nulls)
				sift_colk->bits_andnot(bits, c->nulls, cw);
			return;
		default:
			break;
	}
	tmp = palloc(nw * sizeof(uint64));
	if (t->kind == SIFT_CT_RANGE || t->kind == SIFT_CT_NRANGE)
		chunk_range(c, t->lo, t->hi, tmp);
	else if (t->kind == SIFT_CT_LUT)
		chunk_lut(t, c, tmp);
	else if (t->nset > SET_RANGES_MAX)
	{
		chunk_scalar(t, c, tmp);	/* key_matches includes the negation of NSET */
		negate = false;
	}
	else
		chunk_set(t, c, tmp);
	if (negate)
		sift_colk->bits_andnot(bits, tmp, nw);
	else
		sift_colk->bits_and(bits, tmp, nw);
	if (c->nulls)
		sift_colk->bits_andnot(bits, c->nulls, cw);
	pfree(tmp);
}

/* ---------------------------------------------------------------------------
 * 8. Units (docs/COL.md 2.1)
 * ------------------------------------------------------------------------- */

#define UNIT_HEAD		(16 + SIFT_PAGEMASK_WORDS * 8)

/* Bytes of the offset bitmap of page p (0 for a dense page) */
static inline uint32
page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	return v->maxoff[p] == v->rowstart[p + 1] - v->rowstart[p] ? 0 : (v->maxoff[p] + 7) / 8;
}

static uint32
page_sparse_off(const SiftCUnitView *v, uint32 p)
{
	uint32		off = 0;

	for (uint32 q = 0; q < p; q++)
		off += page_sparse_len(v, q);
	return off;
}

/* Writes the offsets (1-based) of the rows of page p, which has rowstart[p + 1] - rowstart[p] of them; sparse: its bitmap */
static void
page_offsets(const SiftCUnitView *v, uint32 p, const uint8 *bitmap, uint16 *offs)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				n = 0;

	for (uint32 o = 1; o <= v->maxoff[p]; o++)
		if (bitmap == NULL || (bitmap[(o - 1) / 8] >> ((o - 1) % 8)) & 1)
			offs[n++] = (uint16) o;
	Assert(n == cnt);
	(void) cnt;
}

/* The row directory of the sorted keys of superblock sb, appended to out */
static void
unit_put_directory(StringInfo out, const SiftKey *keys, uint32 nrows, uint32 sb)
{
	uint32		head[4] = {COLU_MAGIC, nrows, 0, 0};
	uint64		pagemask[SIFT_PAGEMASK_WORDS] = {0};
	uint32	   *rowstart = palloc((SIFT_SB_PAGES + 1) * sizeof(uint32));
	uint16	   *maxoff = palloc(SIFT_SB_PAGES * sizeof(uint16));
	StringInfoData sparse;
	uint32		np = 0;

	initStringInfo(&sparse);
	for (uint32 r = 0, j; r < nrows; r = j)
	{
		uint32		page = sift_key_block(keys[r]) - sb * SIFT_SB_PAGES;
		uint32		mo;

		for (j = r; j < nrows && sift_key_block(keys[j]) == sift_key_block(keys[r]); j++)
			;
		mo = (uint32) (keys[j - 1] & 0xFFFF);
		pagemask[page / 64] |= UINT64CONST(1) << (page % 64);
		rowstart[np] = r;
		maxoff[np++] = (uint16) mo;
		if (mo != j - r)
		{
			Size		at = sparse.len;

			put_zero(&sparse, (mo + 7) / 8);
			for (uint32 k = r; k < j; k++)
				sparse.data[at + ((keys[k] & 0xFFFF) - 1) / 8] |= 1 << (((keys[k] & 0xFFFF) - 1) % 8);
		}
	}
	rowstart[np] = nrows;
	head[2] = np;
	put_val(out, head);
	put_bytes(out, pagemask, sizeof(pagemask));
	put_bytes(out, rowstart, (np + 1) * sizeof(uint32));
	put_bytes(out, maxoff, np * sizeof(uint16));
	put_bytes(out, sparse.data, sparse.len);
	put_pad(out, 4);
	pfree(rowstart);
	pfree(maxoff);
	pfree(sparse.data);
}

void
sift_cunit_encode(const SiftColBatch *b, uint32 *const *dictmap, const SiftCcPolicy *pol, StringInfo out, Size *start, SiftCZone *zones)
{
	const SiftColSchema *s = b->schema;
	Size		tab;

	put_pad(out, 8);
	*start = out->len;
	unit_put_directory(out, b->keys, b->nrows, b->sb);
	tab = out->len;
	put_zero(out, 2 * s->ncols * sizeof(uint32));
	for (int col = 0; col < s->ncols; col++)
	{
		const SiftCVals *v = &b->col[col];
		const int64 *key = v->key;
		int64	   *mapped = NULL;
		SiftCZone	zone;
		uint32		off,
					len;

		if (dictmap != NULL && dictmap[col] != NULL)
		{
			mapped = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				mapped[i] = sift_col_bit_test(v->nulls, i) ? 0 : dictmap[col][v->key[i]];
			key = mapped;
		}
		put_pad(out, 8);
		off = (uint32) (out->len - *start);
		chunk_encode(out, s->cols[col].store, v, key, pol ? &pol[col] : NULL, &zone);
		len = (uint32) (out->len - *start) - off;
		memcpy(out->data + tab + col * sizeof(uint32), &off, sizeof(off));
		memcpy(out->data + tab + (s->ncols + col) * sizeof(uint32), &len, sizeof(len));
		if (zones)
			zones[col] = zone;
		if (mapped)
			pfree(mapped);
	}
	/* the kernels read SIFT_COL_PAD bytes past the last chunk: a record parsed in place has them */
	enlargeStringInfo(out, SIFT_COL_PAD);
	memset(out->data + out->len, 0, SIFT_COL_PAD);
}

/* The bits of the sparse bitmap of a page: exactly cnt of them, the last at maxoff, none past it */
static bool
sparse_valid(const uint8 *bm, uint32 mo, uint32 cnt)
{
	uint32		pop = 0;

	for (uint32 i = 0; i < (mo + 7) / 8; i++)
		pop += sift_pc64(bm[i]);
	return pop == cnt && ((bm[(mo - 1) / 8] >> ((mo - 1) % 8)) & 1) && (mo % 8 == 0 || (bm[(mo - 1) / 8] >> (mo % 8)) == 0);
}

/*
 * avail: the bytes of the record present (the head of a unit read lazily, docs/COL.md 7), len: its full length. chunks: also validate every
 * chunk (needs all of them present).
 */
static bool
unit_parse(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v, bool chunks)
{
	uint32		head[4];
	uint64		pos = UNIT_HEAD,
				dirend,
				pop = 0;
	uint32		np;

	if (avail < UNIT_HEAD || avail > len || ((uintptr_t) rec & 7) != 0)
		return false;
	memset(v, 0, sizeof(*v));
	memcpy(head, rec, sizeof(head));
	memcpy(v->pagemask, rec + sizeof(head), sizeof(v->pagemask));
	np = head[2];
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		pop += sift_pc64(v->pagemask[w]);
	if (head[0] != COLU_MAGIC || head[1] > SIFT_COL_UNIT_MAX_ROWS || np > SIFT_SB_PAGES || pop != np || (np == 0) != (head[1] == 0))
		return false;
	v->rec = rec;
	v->len = len;
	v->schema = schema;
	v->nrows = head[1];
	v->npages = np;
	if (pos + (uint64) (np + 1) * 4 + (uint64) np * 2 > avail)
		return false;
	v->rowstart = (const uint32 *) (rec + pos);
	pos += (uint64) (np + 1) * 4;
	v->maxoff = (const uint16 *) (rec + pos);
	pos += (uint64) np * 2;
	v->sparse = rec + pos;
	if (v->rowstart[0] != 0 || v->rowstart[np] != v->nrows)
		return false;
	for (uint32 p = 0; p < np; p++)
	{
		uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
					mo = v->maxoff[p],
					sl;

		if (v->rowstart[p + 1] <= v->rowstart[p] || mo < cnt || mo > MaxHeapTuplesPerPage)
			return false;
		sl = page_sparse_len(v, p);
		if (pos + sl > avail || (sl > 0 && !sparse_valid(rec + pos, mo, cnt)))
			return false;
		pos += sl;
	}
	pos = (pos + 3) & ~UINT64CONST(3);
	if (pos + 8 * (uint64) schema->ncols > avail)
		return false;
	v->chunk_off = (const uint32 *) (rec + pos);
	v->chunk_len = v->chunk_off + schema->ncols;
	dirend = (pos + 8 * (uint64) schema->ncols + 7) & ~UINT64CONST(7);
	for (int col = 0; col < schema->ncols; col++)
	{
		SiftCChunk	c;

		if (v->chunk_len[col] == 0)
			continue;
		if (v->chunk_off[col] % 8 != 0 || v->chunk_off[col] < dirend || (uint64) v->chunk_off[col] + v->chunk_len[col] > len ||
			(chunks && (!sift_cchunk_parse(rec + v->chunk_off[col], v->chunk_len[col], &c) || (schema->cols[col].parent < 0 && c.n != v->nrows))))
			return false;
	}
	return true;
}

bool
sift_cunit_parse(const uint8 *rec, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, len, len, schema, v, true);
}

bool
sift_cunit_parse_head(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, avail, len, schema, v, false);
}

bool
sift_cunit_chunk(const SiftCUnitView *v, int col, SiftCChunk *c)
{
	if (v->chunk_len[col] == 0)
		return false;
	if (!sift_cchunk_parse(v->rec + v->chunk_off[col], v->chunk_len[col], c))
		elog(ERROR, "sift_col: chunk of a parsed unit is inconsistent");
	return true;
}

/* The p-th set bit of the page mask: the page of the superblock */
static uint32
unit_page(const SiftCUnitView *v, uint32 p)
{
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
	{
		uint32		c = sift_pc64(v->pagemask[w]);
		uint64		m = v->pagemask[w];

		if (p >= c)
		{
			p -= c;
			continue;
		}
		for (; p > 0; p--)
			m &= m - 1;
		return w * 64 + pg_rightmost_one_pos64(m);
	}
	elog(ERROR, "sift_col: page index %u is outside the unit", p);
	return 0;
}

BlockNumber
sift_cunit_page_block(const SiftCUnitView *v, uint32 sb, uint32 p)
{
	return sb * SIFT_SB_PAGES + unit_page(v, p);
}

void
sift_cunit_page_offmask(const SiftCUnitView *v, uint32 p, uint64 *offmask)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				sl = page_sparse_len(v, p);

	memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
	if (sl > 0)
		memcpy(offmask, v->sparse + page_sparse_off(v, p), sl);
	else
		for (uint32 o = 0; o < cnt; o++)
			offmask[o / 64] |= UINT64CONST(1) << (o % 64);
}

uint32
sift_cunit_row_page(const SiftCUnitView *v, uint32 row)
{
	uint32		lo = 0,
				hi = v->npages;		/* rowstart[lo] <= row < rowstart[hi] */

	while (hi - lo > 1)
	{
		uint32		mid = (lo + hi) / 2;

		if (v->rowstart[mid] <= row)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

void
sift_cunit_keys(const SiftCUnitView *v, uint32 sb, SiftKey *keys)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];

	for (uint32 p = 0; p < v->npages; p++)
	{
		uint32		sl = page_sparse_len(v, p);
		SiftKey		blk = sift_cunit_page_block(v, sb, p);

		page_offsets(v, p, sl > 0 ? bm : NULL, offs);
		for (uint32 r = v->rowstart[p]; r < v->rowstart[p + 1]; r++)
			keys[r] = (blk << 16) | offs[r - v->rowstart[p]];
		bm += sl;
	}
}

/* ---- decoding a unit into a batch ---- */

typedef struct UnitLeaf
{
	bool		present;
	SiftCChunk	c;
	int64	   *key;			/* decoded, NULL slots 0 */
	uint32	   *remap;			/* STR: code of the unit's dictionary -> code in the destination, PG_UINT32_MAX = not yet */
	uint32	   *eoff;			/* GROUP: element offset of each row, nrows + 1 */
	uint32	   *voff;			/* VAR: start of each string, n + 1 */
	char	   *vb;
} UnitLeaf;

/* Appends slot `slot` of leaf col of the unit to dst's leaf */
static void
unit_emit(SiftColBatch *dst, SiftCDict *const *dicts, UnitLeaf *lf, int col, uint32 slot)
{
	bool		isnull = !lf->present || sift_col_bit_test(lf->c.nulls, slot);
	bool		ovf = lf->present && sift_col_bit_test(lf->c.ovf, slot);
	int64		key = isnull ? 0 : lf->key[slot];

	if (lf->voff != NULL)
	{
		vals_put_var(dst->mcxt, &dst->col[col], lf->vb + lf->voff[slot], (uint32) key, isnull, ovf);
		return;
	}
	if (!isnull && lf->remap != NULL)
	{
		uint32	   *to;

		if (key < 0 || key >= dicts[col]->n)
			elog(ERROR, "sift_col: dictionary code %lld is outside the dictionary of %u values", (long long) key, dicts[col]->n);
		to = &lf->remap[key];
		if (*to == PG_UINT32_MAX)
			*to = sift_cdict_intern(dst->dict[col], dicts[col]->bytes + dicts[col]->off[key], cdict_len(dicts[col], (uint32) key));
		key = *to;
	}
	vals_put(dst->mcxt, &dst->col[col], key, isnull, ovf);
}

void
sift_cunit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, SiftColBatch *dst)
{
	const SiftColSchema *s = v->schema;
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	UnitLeaf   *lf = palloc0(s->ncols * sizeof(UnitLeaf));

	if (dst->schema->ncols != s->ncols || dst->sb != sb)
		elog(ERROR, "sift_col: a unit is decoded into a batch of its own schema and superblock");
	sift_cunit_keys(v, sb, keys);
	for (int col = 0; col < s->ncols; col++)
	{
		UnitLeaf   *l = &lf[col];

		l->present = sift_cunit_chunk(v, col, &l->c);
		if (!l->present)
			continue;
		l->key = palloc((l->c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
		sift_cchunk_decode(&l->c, l->key);
		if (s->cols[col].store == SIFT_COLS_VAR)
			sift_cchunk_strs(&l->c, CurrentMemoryContext, &l->voff, &l->vb);
		if (leaf_is_str(&s->cols[col]))
		{
			l->remap = palloc(Max(dicts[col]->n, 1) * sizeof(uint32));
			memset(l->remap, 0xFF, Max(dicts[col]->n, 1) * sizeof(uint32));
		}
		if (s->cols[col].kind == SIFT_COLK_GROUP)
		{
			l->eoff = palloc((v->nrows + 1) * sizeof(uint32));
			l->eoff[0] = 0;
			for (uint32 r = 0; r < v->nrows; r++)
				l->eoff[r + 1] = l->eoff[r] + (uint32) l->key[r];
		}
	}
	for (int col = 0; col < s->ncols; col++)
		if (s->cols[col].parent >= 0 && lf[col].present && lf[col].c.n != (lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0))
			elog(ERROR, "sift_col: group leaf %d has %u values for %u elements", col, lf[col].c.n,
				 lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0);

	for (uint32 r = 0; r < v->nrows; r++)
	{
		if (live != NULL && !sift_col_bit_test(live, r))
			continue;
		batch_row_begin(dst, keys[r]);
		for (int col = 0; col < s->ncols; col++)
		{
			if (s->cols[col].parent >= 0)
				continue;
			unit_emit(dst, dicts, &lf[col], col, r);
			if (s->cols[col].kind != SIFT_COLK_GROUP)
				continue;
			batch_group_end(dst, col, lf[col].present ? lf[col].eoff[r + 1] - lf[col].eoff[r] : 0);
			for (int ch = col + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == col && lf[col].present)
					for (uint32 j = lf[col].eoff[r]; j < lf[col].eoff[r + 1]; j++)
						unit_emit(dst, dicts, &lf[ch], ch, j);
		}
		dst->nrows++;
	}
	for (int col = 0; col < s->ncols; col++)
	{
		if (lf[col].key)
			pfree(lf[col].key);
		if (lf[col].remap)
			pfree(lf[col].remap);
		if (lf[col].eoff)
			pfree(lf[col].eoff);
		if (lf[col].voff)
		{
			pfree(lf[col].voff);
			pfree(lf[col].vb);
		}
	}
	pfree(lf);
	pfree(keys);
}
