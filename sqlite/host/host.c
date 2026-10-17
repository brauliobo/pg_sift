/*
 * shim.c - the PostgreSQL backend services the portable sift sources call, over the C library: memory contexts,
 * ereport, StringInfo, List, dynahash, UTF-8 multibyte helpers and spool files. Only what src/ uses.
 */
#include "host.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>

/* ---------- memory contexts: bump blocks for small chunks, malloc for large ones, both freed with the context ---------- */

#define SQ_BLOCK		(64 * 1024)
#define SQ_SMALL		1024
#define SQ_F_LARGE		1
#define SQ_F_ALIGNED	2

typedef struct SqChunk
{
	struct SqChunk *prev,
			   *next;
	void	   *base;					/* what free() takes; differs from the chunk for aligned ones */
	Size		size;
	MemoryContext cx;
	uint32		flags;
	uint32		pad[3];
} SqChunk;								/* 64 bytes: the payload is 64-byte aligned like a PG IO buffer */

struct MemoryContextData
{
	MemoryContext parent,
				child,
				nextsib,
				prevsib;
	SqChunk		large;					/* list head of large chunks, and of bump blocks */
	char	   *bump;
	char	   *bump_end;
};

_Thread_local MemoryContext CurrentMemoryContext;
MemoryContext TopMemoryContext;

static pthread_mutex_t top_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t top_once = PTHREAD_ONCE_INIT;

static void
top_create(void)
{
	TopMemoryContext = AllocSetContextCreate(NULL, "top", 0, 0, 0);
}

/* Every thread that calls into the engine starts in the top context */
void
sq_mem_init(void)
{
	pthread_once(&top_once, top_create);
	if (CurrentMemoryContext == NULL)
		CurrentMemoryContext = TopMemoryContext;
}

size_t
sq_strlcpy(char *dst, const char *src, size_t size)
{
	size_t		n = strlen(src);

	if (size > 0)
	{
		memcpy(dst, src, Min(n, size - 1));
		dst[Min(n, size - 1)] = '\0';
	}
	return n;
}

MemoryContext
AllocSetContextCreate(MemoryContext parent, const char *name, size_t min, size_t init, size_t max)
{
	MemoryContext cx = calloc(1, sizeof(struct MemoryContextData));

	cx->large.next = cx->large.prev = &cx->large;
	cx->parent = parent;
	if (parent)
	{
		if (parent == TopMemoryContext)
			pthread_mutex_lock(&top_lock);
		cx->nextsib = parent->child;
		if (parent->child)
			parent->child->prevsib = cx;
		parent->child = cx;
		if (parent == TopMemoryContext)
			pthread_mutex_unlock(&top_lock);
	}
	return cx;
}

static void
chunk_link(MemoryContext cx, SqChunk *c)
{
	c->next = cx->large.next;
	c->prev = &cx->large;
	cx->large.next->prev = c;
	cx->large.next = c;
}

static void
cx_free_chunks(MemoryContext cx)
{
	for (SqChunk *c = cx->large.next, *n; c != &cx->large; c = n)
	{
		n = c->next;
		free(c->base);
	}
	cx->large.next = cx->large.prev = &cx->large;
	cx->bump = cx->bump_end = NULL;
}

void
MemoryContextResetChildren(MemoryContext cx)
{
	while (cx->child)
		MemoryContextDelete(cx->child);
}

void
MemoryContextReset(MemoryContext cx)
{
	MemoryContextResetChildren(cx);
	cx_free_chunks(cx);
}

void
MemoryContextDelete(MemoryContext cx)
{
	MemoryContextResetChildren(cx);
	cx_free_chunks(cx);
	if (cx->parent)
	{
		if (cx->parent == TopMemoryContext)
			pthread_mutex_lock(&top_lock);
		if (cx->prevsib)
			cx->prevsib->nextsib = cx->nextsib;
		else
			cx->parent->child = cx->nextsib;
		if (cx->nextsib)
			cx->nextsib->prevsib = cx->prevsib;
		if (cx->parent == TopMemoryContext)
			pthread_mutex_unlock(&top_lock);
	}
	if (CurrentMemoryContext == cx)
		CurrentMemoryContext = cx->parent;
	free(cx);
}

size_t
MemoryContextMemAllocated(MemoryContext cx, bool recurse)
{
	size_t		n = 0;

	for (SqChunk *c = cx->large.next; c != &cx->large; c = c->next)
		n += c->size;
	if (recurse)
		for (MemoryContext ch = cx->child; ch; ch = ch->nextsib)
			n += MemoryContextMemAllocated(ch, true);
	return n;
}

static void *
cx_alloc_large(MemoryContext cx, Size size, Size align)
{
	char	   *raw = malloc(sizeof(SqChunk) + size + align);
	SqChunk    *c;

	if (raw == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory allocating %zu bytes", size)));
	c = (SqChunk *) TYPEALIGN(align, raw + sizeof(SqChunk));
	c = (SqChunk *) ((char *) c - sizeof(SqChunk));
	c->base = raw;
	c->size = size;
	c->cx = cx;
	c->flags = SQ_F_LARGE;
	chunk_link(cx, c);
	return c + 1;
}

static void *
cx_alloc(MemoryContext cx, Size size)
{
	SqChunk    *c;
	Size		need;

	if (size > SQ_SMALL)
		return cx_alloc_large(cx, size, 64);
	need = sizeof(SqChunk) + TYPEALIGN(64, size);
	if (cx->bump == NULL || cx->bump + need > cx->bump_end)
	{
		char	   *blk = cx_alloc_large(cx, SQ_BLOCK, 64);

		cx->bump = blk;
		cx->bump_end = blk + SQ_BLOCK;
	}
	c = (SqChunk *) cx->bump;
	cx->bump += need;
	c->size = size;
	c->cx = cx;
	c->flags = 0;
	return c + 1;
}

void *
MemoryContextAlloc(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size);
}

void *
MemoryContextAllocZero(MemoryContext cx, Size size)
{
	return memset(cx_alloc(cx, size), 0, size);
}

void *
MemoryContextAllocHuge(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size);
}

void *
MemoryContextAllocExtended(MemoryContext cx, Size size, int flags)
{
	void	   *p = cx_alloc(cx, size);

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
MemoryContextAllocAligned(MemoryContext cx, Size size, Size alignto, int flags)
{
	void	   *p = cx_alloc_large(cx, size, Max(alignto, 64));

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
palloc(Size size)
{
	sq_mem_init();
	return cx_alloc(CurrentMemoryContext, size);
}

void *
palloc0(Size size)
{
	return memset(palloc(size), 0, size);
}

void *
palloc_extended(Size size, int flags)
{
	return (flags & MCXT_ALLOC_ZERO) ? palloc0(size) : palloc(size);
}

MemoryContext
GetMemoryChunkContext(void *p)
{
	return ((SqChunk *) p - 1)->cx;
}

void
pfree(void *p)
{
	SqChunk    *c = (SqChunk *) p - 1;

	if (c->flags & SQ_F_LARGE)
	{
		c->prev->next = c->next;
		c->next->prev = c->prev;
		free(c->base);
	}
}

void *
repalloc(void *p, Size size)
{
	SqChunk    *c = (SqChunk *) p - 1;
	void	   *n;

	if (size <= c->size)
		return p;
	n = cx_alloc(c->cx, size);
	memcpy(n, p, c->size);
	pfree(p);
	return n;
}

void *
repalloc_huge(void *p, Size size)
{
	return repalloc(p, size);
}

char *
pstrdup(const char *s)
{
	return strcpy(palloc(strlen(s) + 1), s);
}

char *
pnstrdup(const char *s, Size len)
{
	char	   *d;

	len = strnlen(s, len);
	d = palloc(len + 1);
	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

char *
psprintf(const char *fmt,...)
{
	va_list		ap;
	int			n;
	char	   *p;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = palloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(p, n + 1, fmt, ap);
	va_end(ap);
	return p;
}

text *
cstring_to_text_with_len(const char *s, int len)
{
	text	   *t = palloc(VARHDRSZ + len + 1);

	SET_VARSIZE(t, VARHDRSZ + len);
	memcpy(VARDATA_ANY(t), s, len);
	VARDATA_ANY(t)[len] = '\0';
	return t;
}

/* ---------- errors ---------- */

_Thread_local SqErrorFrame *sq_error_top;

static _Thread_local int err_level;
static _Thread_local int err_code;
static _Thread_local char err_msg[1024];
static _Thread_local char err_detail[512];

bool
errstart(int elevel, const char *domain)
{
	err_level = elevel;
	err_code = ERRCODE_INTERNAL_ERROR;
	err_msg[0] = err_detail[0] = '\0';
	return elevel >= ERROR;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

int
errcode(int sqlerrcode)
{
	err_code = sqlerrcode;
	return 0;
}

static int
fmt_into(char *dst, size_t n, const char *fmt, va_list ap)
{
	vsnprintf(dst, n, fmt, ap);
	return 0;
}

#define DEFINE_ERR(name, dst) \
	int name(const char *fmt,...) \
	{ \
		va_list ap; \
		va_start(ap, fmt); \
		fmt_into(dst, sizeof(dst), fmt, ap); \
		va_end(ap); \
		return 0; \
	}

DEFINE_ERR(errmsg, err_msg)
DEFINE_ERR(errmsg_internal, err_msg)
DEFINE_ERR(errdetail, err_detail)
DEFINE_ERR(errdetail_internal, err_detail)

int
errhint(const char *fmt,...)
{
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (err_detail[0] && strlen(err_msg) + strlen(err_detail) + 4 < sizeof(err_msg))
		strcat(strcat(err_msg, ": "), err_detail);
	if (sq_error_top == NULL)
	{
		fprintf(stderr, "sift: %s (%s:%d)\n", err_msg, filename, lineno);
		abort();
	}
	siglongjmp(sq_error_top->jmp, 1);
}

/* After a cleanup in SQ_CATCH: the same error goes on to the next handler */
void
sq_error_rethrow(void)
{
	if (sq_error_top == NULL)
		abort();
	siglongjmp(sq_error_top->jmp, 1);
}

const char *
sq_errmsg(void)
{
	return err_msg;
}

int
sq_errcode(void)
{
	return err_code;
}

void
sq_error_set(int code, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(err_msg, sizeof(err_msg), fmt, ap);
	va_end(ap);
	err_code = code;
	err_detail[0] = '\0';
	errfinish(__FILE__, __LINE__, __func__);
}

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

/* ---------- multibyte: UTF-8 ---------- */

int
GetDatabaseEncoding(void)
{
	return PG_UTF8;
}

const char *
GetDatabaseEncodingName(void)
{
	return "UTF8";
}

int
pg_database_encoding_max_length(void)
{
	return 4;
}

static int
utf8_len(unsigned char c)
{
	return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

int
pg_mblen(const char *s)
{
	return utf8_len((unsigned char) *s);
}

int
pg_mblen_range(const char *s, const char *end)
{
	return Min(utf8_len((unsigned char) *s), (int) (end - s));
}

int
pg_mbstrlen_with_len(const char *s, int len)
{
	int			n = 0;

	for (int i = 0; i < len; i += pg_mblen(s + i))
		n++;
	return n;
}

int
pg_mbcliplen(const char *s, int len, int limit)
{
	int			n = 0;

	while (n < len)
	{
		int			l = pg_mblen(s + n);

		if (n + l > limit || n + l > len)
			break;
		n += l;
	}
	return n;
}

int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	int			n = 0;

	while (len > 0 && *from)
	{
		unsigned char c = (unsigned char) *from;
		int			l = Min(utf8_len(c), len);

		if (l == 1)
			to[n++] = c;
		else if (l == 2)
			to[n++] = ((c & 0x1F) << 6) | (from[1] & 0x3F);
		else if (l == 3)
			to[n++] = ((c & 0x0F) << 12) | ((from[1] & 0x3F) << 6) | (from[2] & 0x3F);
		else
			to[n++] = ((c & 0x07) << 18) | ((from[1] & 0x3F) << 12) | ((from[2] & 0x3F) << 6) | (from[3] & 0x3F);
		from += l;
		len -= l;
	}
	to[n] = 0;
	return n;
}

int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	char	   *p = to;

	for (; len > 0 && *from; len--, from++)
	{
		pg_wchar	c = *from;

		if (c < 0x80)
			*p++ = c;
		else if (c < 0x800)
		{
			*p++ = 0xC0 | (c >> 6);
			*p++ = 0x80 | (c & 0x3F);
		}
		else if (c < 0x10000)
		{
			*p++ = 0xE0 | (c >> 12);
			*p++ = 0x80 | ((c >> 6) & 0x3F);
			*p++ = 0x80 | (c & 0x3F);
		}
		else
		{
			*p++ = 0xF0 | (c >> 18);
			*p++ = 0x80 | ((c >> 12) & 0x3F);
			*p++ = 0x80 | ((c >> 6) & 0x3F);
			*p++ = 0x80 | (c & 0x3F);
		}
	}
	*p = '\0';
	return p - to;
}

/* no overlongs, surrogates or code points above U+10FFFF, as PostgreSQL */
static int
utf8_valid_len(const unsigned char *s, int len)
{
	unsigned char c = s[0];
	int			l = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;

	if (l == 0 || l > len)
		return 0;
	for (int i = 1; i < l; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	if (l == 3 && ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] > 0x9F)))
		return 0;
	if (l == 4 && ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] > 0x8F)))
		return 0;
	return l;
}

void
pg_verify_mbstr(int encoding, const char *s, int len, bool noError)
{
	for (int i = 0; i < len;)
	{
		int			l = utf8_valid_len((const unsigned char *) s + i, len - i);

		if (l == 0)
		{
			if (noError)
				return;
			ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("invalid byte sequence for encoding \"UTF8\"")));
		}
		i += l;
	}
}

/* ---------- strings ---------- */

int
pg_strcasecmp(const char *s1, const char *s2)
{
	return strcasecmp(s1, s2);
}

int
pg_strncasecmp(const char *s1, const char *s2, size_t n)
{
	return strncasecmp(s1, s2, n);
}

/* PostgreSQL prints the shortest digits that read back as the same float (Ryu), in %g like form with E exponents */
int
float_to_shortest_decimal_buf(float f, char *result)
{
	for (int prec = 1; prec <= 9; prec++)
	{
		snprintf(result, 32, "%.*g", prec, (double) f);
		if (strtof(result, NULL) == f)
			break;
	}
	for (char *p = result; *p; p++)
		if (*p == 'e')
			*p = 'E';
	return strlen(result);
}

/* ---------- spool files ---------- */

struct BufFile
{
	FILE	   *f;
};

BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *b = palloc(sizeof(BufFile));

	b->f = tmpfile();
	if (b->f == NULL)
		elog(ERROR, "could not create a temporary file");
	return b;
}

void
BufFileWrite(BufFile *file, const void *ptr, size_t size)
{
	if (fwrite(ptr, 1, size, file->f) != size)
		elog(ERROR, "could not write to a temporary file");
}

void
BufFileReadExact(BufFile *file, void *ptr, size_t size)
{
	if (fread(ptr, 1, size, file->f) != size)
		elog(ERROR, "could not read from a temporary file");
}

size_t
BufFileRead(BufFile *file, void *ptr, size_t size)
{
	return fread(ptr, 1, size, file->f);
}

BufFile *
BufFileCreateFileSet(FileSet *fileset, const char *name)
{
	elog(ERROR, "sift: no shared file sets in this host");
	return NULL;
}

BufFile *
BufFileOpenFileSet(FileSet *fileset, const char *name, int mode, bool missing_ok)
{
	elog(ERROR, "sift: no shared file sets in this host");
	return NULL;
}

void
BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok)
{
	elog(ERROR, "sift: no shared file sets in this host");
}

int			maintenance_work_mem = 64 * 1024;

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

int
BufFileSeek(BufFile *file, int fileno, long offset, int whence)
{
	return fseek(file->f, offset, whence);
}

void
BufFileClose(BufFile *file)
{
	fclose(file->f);
	pfree(file);
}
