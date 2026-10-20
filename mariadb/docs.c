/*
 * docs.c - the documents of an index (engine.c): the latest version of each key, the dead docids, docid -> key.
 */

#include "ix.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bm25/analyze.h"
#include "bm25/bitmap.h"
#include "core/cmp.h"
#include "core/cpu.h"
#include "bm25/kernels.h"
#include "bm25/prefix.h"

/* ------------------------------------------------ documents: latest, dead, ids/pks */

size_t
lower_bound(const int64 *a, size_t n, int64 v)
{
	size_t		lo = 0,
				hi = n;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (a[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

bool
in_sorted(const int64 *a, size_t n, int64 v)
{
	size_t		i = lower_bound(a, n, v);

	return i < n && a[i] == v;
}

bool
is_dead(const SiftIdx *ix, int64 docid)
{
	return in_sorted(ix->dead, ix->ndead, docid);
}

static size_t
slot_hash(int64 pk)
{
	return ((uint64) pk * 0x9E3779B97F4A7C15ULL) >> 20;
}

static void
latest_grow(SiftIdx *ix)
{
	Slot	   *old = ix->latest;
	size_t		oldcap = ix->caplatest;

	ix->caplatest = oldcap ? oldcap * 2 : 1024;
	ix->latest = calloc(ix->caplatest, sizeof(Slot));
	ix->nlatest = 0;
	for (size_t i = 0; i < oldcap; i++)
		if (old[i].used && old[i].val)
		{
			size_t		h = slot_hash(old[i].key);

			while (ix->latest[h % ix->caplatest].used)
				h++;
			ix->latest[h % ix->caplatest] = old[i];
			ix->nlatest++;
		}
	free(old);
}

/* The slot of pk, or the free one it would take */
static Slot *
latest_slot(const SiftIdx *ix, int64 pk)
{
	size_t		h = slot_hash(pk);

	for (;; h++)
	{
		Slot	   *s = &ix->latest[h % ix->caplatest];

		if (!s->used || s->key == pk)
			return s;
	}
}

int64
latest_get(const SiftIdx *ix, int64 pk)
{
	return ix->caplatest ? latest_slot(ix, pk)->val : 0;
}

/* Applies one event: the previous live version of pk dies, an insert becomes the live one */
void
apply_event(SiftIdx *ix, int64 docid, int64 pk, int64 op)
{
	Slot	   *s;

	if ((ix->nlatest + 1) * 2 > ix->caplatest)
		latest_grow(ix);
	s = latest_slot(ix, pk);
	if (s->used && s->val)
	{
		if (ix->ndead == ix->capdead)
			ix->dead = realloc(ix->dead, (ix->capdead = ix->capdead ? ix->capdead * 2 : 1024) * sizeof(int64));
		ix->dead[ix->ndead++] = s->val;
	}
	if (!s->used)
		ix->nlatest++;
	s->used = 1;
	s->key = pk;
	s->val = op == 'I' ? docid : 0;
	if (op == 'I')
	{
		if (ix->ndoc == ix->capdoc)
		{
			ix->capdoc = ix->capdoc ? ix->capdoc * 2 : 1024;
			ix->ids = realloc(ix->ids, ix->capdoc * sizeof(int64));
			ix->pks = realloc(ix->pks, ix->capdoc * sizeof(int64));
		}
		ix->ids[ix->ndoc] = docid;
		ix->pks[ix->ndoc++] = pk;
	}
}

void
finish_events(SiftIdx *ix)
{
	qsort(ix->dead, ix->ndead, sizeof(int64), sift_cmp_i64);
	ix->sr->any_dead = ix->ndead > 0;
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

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (ix->ids[mid] < docid)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == ix->ndoc || ix->ids[lo] != docid)
		fail("sift: document %lld has no row key in the event log", (long long) docid);
	return ix->pks[lo];
}
