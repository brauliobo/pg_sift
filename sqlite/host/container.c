/*
 * container.c - StringInfo, List, dynahash and the binary heap of the PostgreSQL backend, over the memory contexts of host.c.
 */
#include "host.h"

/* ---------- StringInfo ---------- */

void
initStringInfo(StringInfo str)
{
	str->maxlen = 1024;
	str->data = palloc(str->maxlen);
	resetStringInfo(str);
}

void
resetStringInfo(StringInfo str)
{
	str->data[0] = '\0';
	str->len = str->cursor = 0;
}

void
enlargeStringInfo(StringInfo str, int needed)
{
	if (str->len + needed + 1 <= str->maxlen)
		return;
	while (str->len + needed + 1 > str->maxlen)
		str->maxlen *= 2;
	str->data = repalloc(str->data, str->maxlen);
}

void
appendBinaryStringInfo(StringInfo str, const void *data, int datalen)
{
	enlargeStringInfo(str, datalen);
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;
	str->data[str->len] = '\0';
}

void
appendStringInfoString(StringInfo str, const char *s)
{
	appendBinaryStringInfo(str, s, strlen(s));
}

void
appendStringInfoChar(StringInfo str, char c)
{
	appendBinaryStringInfo(str, &c, 1);
}

void
appendStringInfo(StringInfo str, const char *fmt,...)
{
	for (;;)
	{
		va_list		ap;
		int			avail = str->maxlen - str->len,
					n;

		va_start(ap, fmt);
		n = vsnprintf(str->data + str->len, avail, fmt, ap);
		va_end(ap);
		if (n < avail)
		{
			str->len += n;
			return;
		}
		enlargeStringInfo(str, n + 1);
	}
}

/* ---------- List ---------- */

List *
lappend(List *list, void *datum)
{
	if (list == NIL)
	{
		list = palloc0(sizeof(List));
		list->max_length = 4;
		list->elements = palloc(4 * sizeof(ListCell));
	}
	else if (list->length == list->max_length)
	{
		list->max_length *= 2;
		list->elements = repalloc(list->elements, list->max_length * sizeof(ListCell));
	}
	list->elements[list->length++].ptr_value = datum;
	return list;
}

List *
list_append_unique_ptr(List *list, void *datum)
{
	for (int i = 0; i < list_length(list); i++)
		if (list->elements[i].ptr_value == datum)
			return list;
	return lappend(list, datum);
}

void *
list_nth(const List *list, int n)
{
	return list->elements[n].ptr_value;
}

/* ---------- dynahash: chained, keys of a fixed size ---------- */

typedef struct HEnt
{
	struct HEnt *next;
	uint32		hash;
	/* the entry follows, 16-aligned */
} HEnt;

struct HTAB
{
	MemoryContext cx;
	Size		keysize;
	Size		entrysize;
	HEnt	  **buckets;
	uint32		nbuckets;
	long		nentries;
};

#define HENT_HDR	16
#define HENT_ENTRY(e)	((char *) (e) + HENT_HDR)

uint32
hash_bytes(const unsigned char *k, int keylen)
{
	uint64		h = 0xcbf29ce484222325ULL;

	for (int i = 0; i < keylen; i++)
		h = (h ^ k[i]) * 0x100000001b3ULL;
	return (uint32) (h ^ (h >> 32));
}

HTAB *
hash_create(const char *name, long nelem, const HASHCTL *info, int flags)
{
	MemoryContext cx = (flags & HASH_CONTEXT) ? info->hcxt : CurrentMemoryContext;
	HTAB	   *t = MemoryContextAllocZero(cx, sizeof(HTAB));

	t->cx = cx;
	t->keysize = info->keysize;
	t->entrysize = info->entrysize;
	t->nbuckets = 64;
	t->buckets = MemoryContextAllocZero(cx, t->nbuckets * sizeof(HEnt *));
	return t;
}

void
hash_destroy(HTAB *tab)
{
	/* memory belongs to the context */
}

static void
hash_grow(HTAB *t)
{
	uint32		nb = t->nbuckets * 2;
	HEnt	  **nbk = MemoryContextAllocZero(t->cx, nb * sizeof(HEnt *));

	for (uint32 i = 0; i < t->nbuckets; i++)
		for (HEnt *e = t->buckets[i], *n; e; e = n)
		{
			n = e->next;
			e->next = nbk[e->hash & (nb - 1)];
			nbk[e->hash & (nb - 1)] = e;
		}
	pfree(t->buckets);
	t->buckets = nbk;
	t->nbuckets = nb;
}

void *
hash_search(HTAB *t, const void *key, HASHACTION action, bool *found)
{
	uint32		h = hash_bytes(key, t->keysize);
	HEnt	  **pp = &t->buckets[h & (t->nbuckets - 1)];

	for (; *pp; pp = &(*pp)->next)
		if ((*pp)->hash == h && memcmp(HENT_ENTRY(*pp), key, t->keysize) == 0)
			break;
	if (found)
		*found = *pp != NULL;
	if (*pp)
	{
		HEnt	   *e = *pp;

		if (action != HASH_REMOVE)
			return HENT_ENTRY(e);
		*pp = e->next;
		t->nentries--;
		return HENT_ENTRY(e);
	}
	if (action != HASH_ENTER && action != HASH_ENTER_NULL)
		return NULL;
	if (t->nentries > (long) t->nbuckets * 2)
	{
		hash_grow(t);
		pp = &t->buckets[h & (t->nbuckets - 1)];
	}
	{
		HEnt	   *e = MemoryContextAllocZero(t->cx, HENT_HDR + t->entrysize);

		e->hash = h;
		memcpy(HENT_ENTRY(e), key, t->keysize);
		e->next = t->buckets[h & (t->nbuckets - 1)];
		t->buckets[h & (t->nbuckets - 1)] = e;
		t->nentries++;
		return HENT_ENTRY(e);
	}
}

long
hash_get_num_entries(HTAB *t)
{
	return t->nentries;
}

void
hash_seq_init(HASH_SEQ_STATUS *s, HTAB *t)
{
	s->tab = t;
	s->bucket = 0;
	s->next = t->buckets[0];
}

void *
hash_seq_search(HASH_SEQ_STATUS *s)
{
	HEnt	   *e;

	while (s->next == NULL)
	{
		if (++s->bucket >= s->tab->nbuckets)
			return NULL;
		s->next = s->tab->buckets[s->bucket];
	}
	e = s->next;
	s->next = e->next;
	return HENT_ENTRY(e);
}

/* ---------- binary heap: the comparator puts the node that is to come out first on top ---------- */

struct binaryheap
{
	int			size,
				space;
	binaryheap_comparator cmp;
	void	   *arg;
	Datum	   *nodes;
};

binaryheap *
binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg)
{
	binaryheap *h = palloc(sizeof(binaryheap));

	h->size = 0;
	h->space = Max(capacity, 1);
	h->cmp = compare;
	h->arg = arg;
	h->nodes = palloc(h->space * sizeof(Datum));
	return h;
}

void
binaryheap_free(binaryheap *heap)
{
	pfree(heap->nodes);
	pfree(heap);
}

static void
heap_sift_down(binaryheap *h, int i)
{
	Datum		node = h->nodes[i];

	for (;;)
	{
		int			l = 2 * i + 1,
					c = l;

		if (l >= h->size)
			break;
		if (l + 1 < h->size && h->cmp(h->nodes[l + 1], h->nodes[l], h->arg) > 0)
			c = l + 1;
		if (h->cmp(h->nodes[c], node, h->arg) <= 0)
			break;
		h->nodes[i] = h->nodes[c];
		i = c;
	}
	h->nodes[i] = node;
}

static void
heap_sift_up(binaryheap *h, int i)
{
	Datum		node = h->nodes[i];

	while (i > 0 && h->cmp(node, h->nodes[(i - 1) / 2], h->arg) > 0)
	{
		h->nodes[i] = h->nodes[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	h->nodes[i] = node;
}

void
binaryheap_add_unordered(binaryheap *heap, Datum d)
{
	if (heap->size >= heap->space)
		elog(ERROR, "out of binary heap slots");
	heap->nodes[heap->size++] = d;
}

void
binaryheap_build(binaryheap *heap)
{
	for (int i = heap->size / 2 - 1; i >= 0; i--)
		heap_sift_down(heap, i);
}

void
binaryheap_add(binaryheap *heap, Datum d)
{
	binaryheap_add_unordered(heap, d);
	heap_sift_up(heap, heap->size - 1);
}

Datum
binaryheap_first(binaryheap *heap)
{
	return heap->nodes[0];
}

Datum
binaryheap_remove_first(binaryheap *heap)
{
	Datum		first = heap->nodes[0];

	heap->nodes[0] = heap->nodes[--heap->size];
	if (heap->size > 0)
		heap_sift_down(heap, 0);
	return first;
}

void
binaryheap_replace_first(binaryheap *heap, Datum d)
{
	heap->nodes[0] = d;
	if (heap->size > 1)
		heap_sift_down(heap, 0);
}

bool
binaryheap_empty(binaryheap *heap)
{
	return heap->size == 0;
}
