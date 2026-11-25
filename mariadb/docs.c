/*
 * docs.c - the documents of an index (engine.c): the latest version of each key, the dead docids, docid -> key.
 */

#include "ix.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bm25/analyze/analyze.h"
#include "bm25/bitmap/bitmap.h"
#include "core/cmp.h"
#include "core/hash.h"
#include "core/cpu.h"
#include "bm25/kernels/kernels.h"
#include "bm25/prefix.h"

/* ------------------------------------------------ documents: latest, dead, ids/pks */

size_t
lower_bound(const int64 *a, size_t n, int64 v)
{
	size_t		lo = 0,
				hi = n;

	SIFT_BISECT(size_t, mid, lo, hi, a[mid] < v);
	return lo;
}

bool
in_sorted(const int64 *a, size_t n, int64 v)
{
	size_t		i = lower_bound(a, n, v);

	return i < n && a[i] == v;
}

/* The first position of a[0, n) at or after pos whose value is not below v: a gallop from pos, then a bisection of its last step */
size_t
lower_bound_from(const int64 *a, size_t n, size_t pos, int64 v)
{
	size_t		lo = pos,
				step = 1,
				hi;

	if (pos >= n || a[pos] >= v)
		return pos;
	while (lo + step < n && a[lo + step] < v)
	{
		lo += step;
		step <<= 1;
	}
	hi = Min(lo + step, n);
	lo++;
	SIFT_BISECT(size_t, mid, lo, hi, a[mid] < v);
	return lo;
}

bool
is_dead(const SiftIdx *ix, int64 docid)
{
	return in_sorted(ix->dead, ix->ndead, docid);
}

static size_t
slot_hash(int64 pk)
{
	return sift_fib64((uint64) pk) >> 20;
}

/* The slot of pk in table t of cap slots, or the free one it would take */
Slot *
slot_find(Slot *t, size_t cap, int64 pk)
{
	size_t		h = slot_hash(pk);

	for (;; h++)
	{
		Slot	   *s = &t[h % cap];

		if (!s->used || s->key == pk)
			return s;
	}
}

/*
 * The table of the live keys of ix->latest with room for `extra` more, built without changing the index (readers may be
 * using it); NULL when the table has the room already. The slots of removed keys are not carried over: the table grows only
 * for the live ones, and a rolling window of keys stays the size of the window.
 */
Slot *
latest_reserve(const SiftIdx *ix, size_t extra, size_t *cap, size_t *nused)
{
	size_t		nlive = 0,
				want,
				c = ix->caplatest ? ix->caplatest : 1024;
	Slot	   *t;

	if ((ix->nlatest + extra) * 2 <= ix->caplatest)
		return NULL;
	for (size_t i = 0; i < ix->caplatest; i++)
		nlive += ix->latest[i].used && ix->latest[i].val;
	want = (nlive + extra + 1) * 4;
	while (c < want)
		c *= 2;
	while (c > 1024 && want * 2 < c)
		c /= 2;
	t = xcalloc(c, sizeof(Slot));
	for (size_t i = 0; i < ix->caplatest; i++)
		if (ix->latest[i].used && ix->latest[i].val)
			*slot_find(t, c, ix->latest[i].key) = ix->latest[i];
	*cap = c;
	*nused = nlive;
	return t;
}

static void
latest_grow(SiftIdx *ix)
{
	size_t		cap,
				nused;
	Slot	   *t = latest_reserve(ix, 1, &cap, &nused);

	if (t == NULL)
		return;
	free(ix->latest);
	ix->latest = t;
	ix->caplatest = cap;
	ix->nlatest = nused;
}

int64
latest_get(const SiftIdx *ix, int64 pk)
{
	return ix->caplatest ? slot_find(ix->latest, ix->caplatest, pk)->val : 0;
}

/* Applies an event to the documents that have the room (latest_reserve, a doc table of capdoc): no allocation, no dead docid */
void
apply_live(SiftIdx *ix, int64 docid, int64 pk, int64 op)
{
	Slot	   *s = slot_find(ix->latest, ix->caplatest, pk);

	if (!s->used)
		ix->nlatest++;
	s->used = 1;
	s->key = pk;
	s->val = op == 'I' ? docid : 0;
	if (op == 'I')
	{
		ix->ids[ix->ndoc] = docid;
		ix->pks[ix->ndoc++] = pk;
	}
}

/* Applies one event: the previous live version of pk dies, an insert becomes the live one */
void
apply_event(SiftIdx *ix, int64 docid, int64 pk, int64 op)
{
	int64		prev;

	latest_grow(ix);
	prev = latest_get(ix, pk);
	if (prev)
	{
		if (ix->ndead == ix->capdead)
			ix->dead = xrealloc(ix->dead, (ix->capdead = ix->capdead ? ix->capdead * 2 : 1024) * sizeof(int64));
		ix->dead[ix->ndead++] = prev;
	}
	/* MEM[MEM-PORT-09]: MariaDB SiftIdx resident arrays (ids+pks 16-32B/doc + latest 48-96B/key + dead) */
	if (op == 'I' && ix->ndoc == ix->capdoc)
	{
		ix->capdoc = ix->capdoc ? ix->capdoc + ix->capdoc / 2 : 1024;
		ix->ids = xrealloc(ix->ids, ix->capdoc * sizeof(int64));
		ix->pks = xrealloc(ix->pks, ix->capdoc * sizeof(int64));
	}
	apply_live(ix, docid, pk, op);
}

/* The dead docids of ix and of new_dead (ascending, no common docid) as one ascending list */
static int64 *
merge_dead(const SiftIdx *ix, const int64 *add, size_t nadd)
{
	int64	   *out = xmalloc((ix->ndead + nadd + 1) * sizeof(int64));
	size_t		i = 0,
				j = 0,
				k = 0;

	while (i < ix->ndead && j < nadd)
		out[k++] = ix->dead[i] < add[j] ? ix->dead[i++] : add[j++];
	while (i < ix->ndead)
		out[k++] = ix->dead[i++];
	while (j < nadd)
		out[k++] = add[j++];
	return out;
}

void
prepare_events(const SiftIdx *ix, const Ev *ev, size_t n, Prep *p)
{
	size_t		ovcap = 16,
				k = 0,
				nins = 0;
	Slot	   *ov;
	int64	   *add = xmalloc((n + 1) * sizeof(int64));

	while (ovcap < n * 2)
		ovcap *= 2;
	ov = xcalloc(ovcap, sizeof(Slot));		/* the keys of the events so far: what the table will say when they are applied */
	for (size_t i = 0; i < n; i++)
	{
		Slot	   *o = slot_find(ov, ovcap, ev[i].pk);
		int64		prev = o->used ? o->val : latest_get(ix, ev[i].pk);

		if (prev)
			add[k++] = prev;
		*o = (Slot) {ev[i].pk, ev[i].op == 'I' ? ev[i].docid : 0, 1};
		nins += ev[i].op == 'I';
	}
	qsort(add, k, sizeof(int64), sift_cmp_i64);
	*p = (Prep) {.dead = merge_dead(ix, add, k),.ndead = ix->ndead + k};
	p->latest = latest_reserve(ix, n, &p->caplatest, &p->nlatest);
	if (ix->ndoc + nins > ix->capdoc)
	{
		p->capdoc = Max(Max(ix->capdoc + ix->capdoc / 2, ix->ndoc + nins), 1024);
		p->ids = xmalloc(p->capdoc * sizeof(int64));
		p->pks = xmalloc(p->capdoc * sizeof(int64));
		memcpy(p->ids, ix->ids, ix->ndoc * sizeof(int64));
		memcpy(p->pks, ix->pks, ix->ndoc * sizeof(int64));
	}
	free(add);
	free(ov);
}

#define SWAP(a, b) do { __typeof__(a) t_ = (a); (a) = (b); (b) = t_; } while (0)

void
install_prepared(SiftIdx *ix, const Ev *ev, size_t n, Prep *p)
{
	SWAP(ix->dead, p->dead);
	ix->ndead = ix->capdead = p->ndead;
	if (p->latest)
	{
		SWAP(ix->latest, p->latest);
		ix->caplatest = p->caplatest;
		ix->nlatest = p->nlatest;
	}
	if (p->ids)
	{
		SWAP(ix->ids, p->ids);
		SWAP(ix->pks, p->pks);
		ix->capdoc = p->capdoc;
	}
	for (size_t i = 0; i < n; i++)
		apply_live(ix, ev[i].docid, ev[i].pk, ev[i].op);
}

void
release_prepared(Prep *p)
{
	free(p->dead);
	free(p->latest);
	free(p->ids);
	free(p->pks);
}

void
finish_events(SiftIdx *ix)
{
	qsort(ix->dead, ix->ndead, sizeof(int64), sift_cmp_i64);
}

void
reset_docs(SiftIdx *ix)
{
	free(ix->ids);
	free(ix->pks);
	free(ix->dead);
	free(ix->latest);
	ix->ids = ix->pks = ix->dead = NULL;
	ix->latest = NULL;
	ix->ndoc = ix->capdoc = ix->ndead = ix->capdead = ix->caplatest = ix->nlatest = 0;
}

int64
pk_of(const SiftIdx *ix, int64 docid)
{
	size_t		lo = 0,
				hi = ix->ndoc;

	SIFT_BISECT(size_t, mid, lo, hi, ix->ids[mid] < docid);
	if (lo == ix->ndoc || ix->ids[lo] != docid)
		fail("sift: document %lld has no row key in the event log", (long long) docid);
	return ix->pks[lo];
}
