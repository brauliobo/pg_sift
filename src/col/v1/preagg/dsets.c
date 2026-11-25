/*
 * col/v1/preagg/dsets.c - the id sets by calendar month of a key group (docs/COL-SCOPE.md "Id sets by month")
 *
 * A key group of a scope segment holds the rows of one scope key. count(DISTINCT leaf) by month of a date or timestamp leaf is the number of different values of the leaf in the rows
 * of each month, and a month of a unit holds a few rows of each value: the sets are of the group, one bitmap of the values of each 32-bit integer dim of the cube for each month the
 * rows of the group have. The blob (host order, every field a multiple of 8 bytes):
 *   u32 nleaf, u32 nmonths, u32 cal leaf, u32 0
 *   i64 month[nmonths]          the first day of each month (days since 2000-01-01), ascending
 *   per leaf (ascending): u32 leaf, u32 nwords, i64 wbase, then u64 words[nmonths][nwords]: bit b of a month's words is the value (wbase * 64 + b)
 * The rows without a date are in no month (a group answers only for a histogram without that group). A value is set by the rows of a month that are not NULL in the leaf.
 */
/* check-memory: allow-allocs the rows of a key group, at most SIFT_SUPER_KEY_ROWS */
#include "core/host.h"

#include "col/v1/preagg/preagg.h"

typedef struct DsetsHead
{
	uint32		nleaf;
	uint32		nmonths;
	uint32		cal;
	uint32		pad;
} DsetsHead;

typedef struct DsetsLeaf
{
	uint32		leaf;
	uint32		nwords;
	int64		wbase;
} DsetsLeaf;

/* the values a bitmap spans at most: 4 M values, 512 kB a month */
#define DSETS_SPAN		((int64) 1 << 22)

static bool
dsets_type_ok(const SiftColDef *d)
{
	return d->store == SIFT_COLS_INT && d->parent < 0 && (d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4);
}

int
sift_cdsets_ordinal(const SiftColSchema *s, int leaf)
{
	int			n = 0;

	for (int c = 0; c < leaf; c++)
		n += dsets_type_ok(&s->cols[c]);
	return dsets_type_ok(&s->cols[leaf]) && n < SIFT_CDSETS_LEAVES ? n : -1;
}

bool
sift_cdsets_leaf_ok(const SiftColSchema *s, int leaf)
{
	return sift_cdsets_ordinal(s, leaf) >= 0;
}

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static inline bool
bit_at(const uint64 *bits, uint32 i)
{
	return bits != NULL && (bits[i / 64] >> (i % 64) & 1);
}

static bool
any_bit(const uint64 *bits, uint32 n)
{
	for (uint32 i = 0; bits != NULL && i < n / 64; i++)
		if (bits[i])
			return true;
	return bits != NULL && n % 64 && (bits[n / 64] & ((UINT64CONST(1) << (n % 64)) - 1));
}

/* The record of one leaf (rows without a date skipped), appended to out: false when the leaf has none (a negative or far apart value, an overflow, a byte of sets for each row exceeded) */
static bool
dsets_leaf(const SiftCPaggCol *c, int leaf, const int64 *cell, const SiftCPaggCol *date, uint32 nrows, const int64 *month, uint32 nmonths, StringInfo out)
{
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	DsetsLeaf	rec;
	uint64	   *words;
	uint32		m = 0;

	if (!c->ready || c->key == NULL || any_bit(c->ovf, nrows))
		return false;
	for (uint32 i = 0; i < nrows; i++)
		if (!bit_at(c->nulls, i) && !bit_at(date->nulls, i))
		{
			lo = Min(lo, c->key[i]);
			hi = Max(hi, c->key[i]);
		}
	if (lo < 0 || hi < lo || hi - lo >= DSETS_SPAN)
		return false;
	rec = (DsetsLeaf) {leaf, (uint32) ((hi >> 6) - (lo >> 6) + 1), lo >> 6};
	if ((uint64) nmonths * rec.nwords * sizeof(uint64) > nrows)
		return false;
	words = palloc0((Size) nmonths * rec.nwords * sizeof(uint64));
	for (uint32 i = 0; i < nrows; i++)
	{
		int64		v;

		if (bit_at(c->nulls, i) || bit_at(date->nulls, i))
			continue;
		v = c->key[i] - rec.wbase * 64;
		if (cell[i] != month[m])
			m = (const int64 *) bsearch(&cell[i], month, nmonths, sizeof(int64), cmp_i64) - month;
		words[(Size) m * rec.nwords + v / 64] |= UINT64CONST(1) << (v % 64);
	}
	appendBinaryStringInfo(out, (const char *) &rec, sizeof(rec));
	appendBinaryStringInfo(out, (const char *) words, (Size) nmonths * rec.nwords * sizeof(uint64));
	pfree(words);
	return true;
}

void
sift_cdsets_build(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, int cal, const int *leaf, int nleaf, StringInfo out)
{
	const SiftCPaggCol *date = &cols[cal];
	int64	   *cell = palloc(Max(nrows, 1) * sizeof(int64)),
			   *month = palloc(Max(nrows, 1) * sizeof(int64));
	uint32		nmonths = 0,
				u = 0;
	DsetsHead	head = {0, 0, cal, 0};

	resetStringInfo(out);
	if (!date->ready || date->key == NULL || !sift_cpagg_cal_rows(s, cal, SIFT_CAL_MONTH, date, cell))
		return;
	for (uint32 i = 0; i < nrows; i++)
		if (!bit_at(date->nulls, i))
			month[nmonths++] = cell[i];
	qsort(month, nmonths, sizeof(int64), cmp_i64);
	for (uint32 i = 0; i < nmonths; i++)
		if (i == 0 || month[i] != month[i - 1])
			month[u++] = month[i];
	head.nmonths = u;
	appendBinaryStringInfo(out, (const char *) &head, sizeof(head));
	appendBinaryStringInfo(out, (const char *) month, u * sizeof(int64));
	for (int i = 0; u > 0 && i < nleaf; i++)
		head.nleaf += dsets_leaf(&cols[leaf[i]], leaf[i], cell, date, nrows, month, u, out);
	if (head.nleaf == 0)
		resetStringInfo(out);
	else
		memcpy(out->data, &head, sizeof(head));
	pfree(cell);
	pfree(month);
}

/* The head, the months and the first leaf record of a blob; false when it is too short for them */
static bool
dsets_head(const uint8 *b, uint32 len, const DsetsHead **head, const int64 **month)
{
	if (len < sizeof(DsetsHead))
		return false;
	*head = (const DsetsHead *) b;
	*month = (const int64 *) (b + sizeof(DsetsHead));
	return (*head)->nleaf > 0 && (*head)->nmonths > 0 && (uint64) (*head)->nmonths * sizeof(int64) <= len - sizeof(DsetsHead);
}

/* The next leaf record at *at, with the offset of the next one in *at; false when it does not fit in the blob */
static bool
dsets_next(const uint8 *b, uint32 len, uint32 nmonths, uint64 *at, const DsetsLeaf **rec)
{
	uint64		bytes;

	if (*at + sizeof(DsetsLeaf) > len)
		return false;
	*rec = (const DsetsLeaf *) (b + *at);
	bytes = (uint64) nmonths * (*rec)->nwords * sizeof(uint64);
	if ((*rec)->nwords == 0 || bytes > len - *at - sizeof(DsetsLeaf))
		return false;
	*at += sizeof(DsetsLeaf) + bytes;
	return true;
}

int
sift_cdsets_get(const uint8 *b, uint32 len, int cal, int leaf, int64 cell, SiftCDset *out)
{
	const DsetsHead *head;
	const int64 *month;
	uint64		at;

	if (!dsets_head(b, len, &head, &month))
		return -1;
	if ((int) head->cal != cal)
		return 0;
	at = sizeof(DsetsHead) + (uint64) head->nmonths * sizeof(int64);
	for (uint32 i = 0; i < head->nleaf; i++)
	{
		const DsetsLeaf *rec;
		const int64 *m;

		if (!dsets_next(b, len, head->nmonths, &at, &rec))
			return -1;
		if ((int) rec->leaf != leaf)
			continue;
		memset(out, 0, sizeof(*out));
		if ((m = (const int64 *) bsearch(&cell, month, head->nmonths, sizeof(int64), cmp_i64)) != NULL)
		{
			out->wbase = rec->wbase;
			out->nwords = rec->nwords;
			out->words = (const uint8 *) (rec + 1) + (Size) (m - month) * rec->nwords * sizeof(uint64);
		}
		return 1;
	}
	return 0;
}

int
sift_cdsets_leaves(const SiftColSchema *s, const uint8 *b, uint32 len, int *cal, int *leaf)
{
	const DsetsHead *head;
	const int64 *month;
	uint64		at;

	if (!dsets_head(b, len, &head, &month) || head->cal >= (uint32) s->ncols)
		return -1;
	*cal = head->cal;
	at = sizeof(DsetsHead) + (uint64) head->nmonths * sizeof(int64);
	for (uint32 i = 0; i < head->nleaf; i++)
	{
		const DsetsLeaf *rec;

		if (!dsets_next(b, len, head->nmonths, &at, &rec) || rec->leaf >= (uint32) s->ncols || !sift_cdsets_leaf_ok(s, rec->leaf) ||
			(i > 0 && (int) rec->leaf <= leaf[i - 1]))
			return -1;
		leaf[i] = rec->leaf;
	}
	return at == len ? (int) head->nleaf : -1;
}
