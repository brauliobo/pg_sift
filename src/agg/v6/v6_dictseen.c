/*
 * agg/v6/v6_dictseen.c - the distinct strings of dictionary-coded rows (v6_dictseen.h).
 */
#include "agg/v6/v6_dictseen.h"

#define DICTSEEN_MIN_CAP		(256 * 1024)

/* A sorted run of strings: n of them, string i is bytes[off[i] .. off[i + 1]) */
typedef struct DictRun
{
	uint32		n;
	uint32	   *off;
	char	   *bytes;
	uint32		off_cap;
	Size		bytes_cap;
} DictRun;

struct SiftDictSeen
{
	MemoryContext cx;
	uint64		cap;
	bool		have;			/* a part is being read: its dictionary is in part, its seen codes in bits */
	uint64		gen;
	uint32		n;
	DictRun		part;
	uint64	   *bits;
	uint32		bits_cap;
	uint32	   *used;			/* the codes of bits, ascending, while a part is folded */
	uint32		used_cap;
	DictRun		all;			/* the strings of the parts folded so far, sorted, each once */
	DictRun		spare;			/* what a fold merges into: swapped with all */
};

static inline int
str_cmp(const char *a, uint32 la, const char *b, uint32 lb)
{
	const int	c = memcmp(a, b, Min(la, lb));

	return c != 0 ? c : (la > lb) - (la < lb);
}

/* the bits of word w of a bitmap of n rows that are rows */
static inline uint64
word_mask(uint16 n, uint32 w)
{
	return (w + 1) * 64 <= n ? ~UINT64CONST(0) : (UINT64CONST(1) << (n & 63)) - 1;
}

static inline uint32
run_len(const DictRun *r, uint32 i)
{
	return r->off[i + 1] - r->off[i];
}

static void
run_room(SiftDictSeen *d, DictRun *r, uint32 n, Size bytes)
{
	if (n + 1 > r->off_cap)
	{
		if (r->off != NULL)
			pfree(r->off);
		r->off_cap = Max(n + 1, 2 * r->off_cap);
		r->off = MemoryContextAlloc(d->cx, (Size) r->off_cap * sizeof(uint32));
	}
	if (bytes > r->bytes_cap || r->bytes == NULL)
	{
		if (r->bytes != NULL)
			pfree(r->bytes);
		r->bytes_cap = Max(Max(bytes, 2 * r->bytes_cap), 64);
		r->bytes = MemoryContextAlloc(d->cx, r->bytes_cap);
	}
}

static void
run_free(DictRun *r)
{
	if (r->off != NULL)
		pfree(r->off);
	if (r->bytes != NULL)
		pfree(r->bytes);
	*r = (DictRun) {0};
}

SiftDictSeen *
sift_dictseen_create(MemoryContext cx, uint64 cap_bytes)
{
	SiftDictSeen *d = MemoryContextAllocZero(cx, sizeof(SiftDictSeen));

	d->cx = cx;
	d->cap = Max(cap_bytes, DICTSEEN_MIN_CAP);
	run_room(d, &d->all, 0, 0);
	d->all.off[0] = 0;
	return d;
}

/* The part a batch is of: its dictionary kept (the caller's lasts only for the call) and no code seen */
static void
part_start(SiftDictSeen *d, const SiftDictSrc *src)
{
	const uint32 words = (src->n + 63) / 64;

	run_room(d, &d->part, src->n, src->off[src->n]);
	memcpy(d->part.off, src->off, ((Size) src->n + 1) * sizeof(uint32));
	memcpy(d->part.bytes, src->bytes, src->off[src->n]);
	d->part.n = src->n;
	if (words > d->bits_cap)
	{
		if (d->bits != NULL)
			pfree(d->bits);
		d->bits_cap = Max(words, 2 * d->bits_cap);
		d->bits = MemoryContextAlloc(d->cx, (Size) d->bits_cap * sizeof(uint64));
	}
	if (words > 0)
		memset(d->bits, 0, (Size) words * sizeof(uint64));
	d->have = true;
	d->gen = src->gen;
	d->n = src->n;
}

/* The codes the part has seen, ascending; false when the strings of two of them are not in increasing order (a dictionary that is not in byte order) */
static bool
collect_used(SiftDictSeen *d, uint32 *cnt)
{
	const uint32 words = (d->n + 63) / 64;
	bool		sorted = true;
	uint32		k = 0;

	if (d->n > d->used_cap)
	{
		if (d->used != NULL)
			pfree(d->used);
		d->used_cap = Max(d->n, 2 * d->used_cap);
		d->used = MemoryContextAlloc(d->cx, (Size) d->used_cap * sizeof(uint32));
	}
	for (uint32 w = 0; w < words; w++)
		for (uint64 m = d->bits[w]; m != 0; m &= m - 1)
		{
			const uint32 c = w * 64 + (uint32) __builtin_ctzll(m);

			if (k > 0 && sorted)
				sorted = str_cmp(d->part.bytes + d->part.off[d->used[k - 1]], run_len(&d->part, d->used[k - 1]), d->part.bytes + d->part.off[c], run_len(&d->part, c)) < 0;
			d->used[k++] = c;
		}
	*cnt = k;
	return sorted;
}

/* all and the used strings of the part, merged into spare (a string both have once), then swapped */
static void
merge_used(SiftDictSeen *d, uint32 cnt)
{
	Size		bytes = d->all.off[d->all.n];
	uint32		i = 0,
				j = 0,
				k = 0;
	Size		pos = 0;

	for (uint32 c = 0; c < cnt; c++)
		bytes += run_len(&d->part, d->used[c]);
	run_room(d, &d->spare, d->all.n + cnt, bytes);
	while (i < d->all.n || j < cnt)
	{
		const char *s;
		uint32		len;

		if (j < cnt)
		{
			const char *p = d->part.bytes + d->part.off[d->used[j]];
			const uint32 pl = run_len(&d->part, d->used[j]);
			const int	c = i < d->all.n ? str_cmp(d->all.bytes + d->all.off[i], run_len(&d->all, i), p, pl) : 1;

			if (c < 0)
			{
				s = d->all.bytes + d->all.off[i];
				len = run_len(&d->all, i++);
			}
			else
			{
				s = p;
				len = pl;
				j++;
				i += c == 0;
			}
		}
		else
		{
			s = d->all.bytes + d->all.off[i];
			len = run_len(&d->all, i++);
		}
		d->spare.off[k++] = (uint32) pos;
		memcpy(d->spare.bytes + pos, s, len);
		pos += len;
	}
	d->spare.off[k] = (uint32) pos;
	d->spare.n = k;
	{
		DictRun		t = d->all;

		d->all = d->spare;
		d->spare = t;
	}
}

static void
emit_run(DictRun *r, SiftDictSeenEmit emit, void *arg)
{
	for (uint32 i = 0; i < r->n; i++)
		emit(arg, r->bytes + r->off[i], run_len(r, i));
	r->n = 0;
	if (r->off != NULL)
		r->off[0] = 0;
}

/* The part's seen strings join the set; one that is not sorted goes to emit on its own */
static void
fold_part(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg)
{
	uint32		cnt;

	if (!d->have)
		return;
	d->have = false;
	if (collect_used(d, &cnt))
		merge_used(d, cnt);
	else
		for (uint32 c = 0; c < cnt; c++)
			emit(arg, d->part.bytes + d->part.off[d->used[c]], run_len(&d->part, d->used[c]));
	if (d->all.n > 0 && (uint64) d->all.off[d->all.n] + (uint64) d->all.n * sizeof(uint32) > d->cap)
		emit_run(&d->all, emit, arg);
}

bool
sift_dictseen_batch(SiftDictSeen *d, const SiftDictSrc *src, const uint32 *code, const uint64 *take, const uint64 *valid, uint16 n_raw, SiftDictSeenEmit emit, void *arg)
{
	if (src->n > SIFT_DICTSEEN_MAX_CODES || src->off[src->n] > SIFT_DICTSEEN_MAX_BYTES)
		return false;
	if (!d->have || d->gen != src->gen)
	{
		fold_part(d, emit, arg);
		part_start(d, src);
	}
	for (uint32 w = 0; w < (uint32) (n_raw + 63) / 64; w++)
		for (uint64 m = (valid != NULL ? take[w] & valid[w] : take[w]) & word_mask(n_raw, w); m != 0; m &= m - 1)
		{
			const uint32 c = code[w * 64 + (uint32) __builtin_ctzll(m)];

			Assert(c < d->n);
			d->bits[c >> 6] |= UINT64CONST(1) << (c & 63);
		}
	return true;
}

void
sift_dictseen_drain(SiftDictSeen *d, SiftDictSeenEmit emit, void *arg)
{
	fold_part(d, emit, arg);
	emit_run(&d->all, emit, arg);
}

uint64
sift_dictseen_count(const SiftDictSeen *d)
{
	return d->all.n;
}

void
sift_dictseen_free(SiftDictSeen *d)
{
	run_free(&d->part);
	run_free(&d->all);
	run_free(&d->spare);
	if (d->bits != NULL)
		pfree(d->bits);
	if (d->used != NULL)
		pfree(d->used);
	pfree(d);
}
