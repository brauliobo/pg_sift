/*
 * shim.c - the PostgreSQL backend services the engine of src/ calls, for running it inside another process.
 *
 * Memory contexts are a small size-class allocator (palloc/pfree/reset/delete as the engine expects), ereport(ERROR)
 * longjmps to sift_err_jmp, hash tables, StringInfo, List, UTF-8 and BufFile are the few things sift uses, and the
 * "buffer manager" maps a block number to a page of the index files (store.c). No PostgreSQL code is linked except
 * libpgcommon and libpgport (hash_bytes, qsort, UTF-8 tables, binaryheap).
 * The engine runs on any number of threads at once: the memory contexts and the error state are thread local.
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/generic_xlog.h"
#include "common/hashfn.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/pg_list.h"
#include "regex/regex.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "shim.h"

/*
 * The current and the top memory context are per thread: the Makefile turns every use of the two names in the engine
 * into a call of these accessors, so the engine code of src/ needs no change to run on several threads.
 */
#undef CurrentMemoryContext
#undef TopMemoryContext
static __thread MemoryContext cur_cx,
			top_cx;
#define CurrentMemoryContext	cur_cx
#define TopMemoryContext		top_cx

MemoryContext *
sift_cur_cx(void)
{
	return &cur_cx;
}

MemoryContext *
sift_top_cx(void)
{
	return &top_cx;
}

/* ------------------------------------------------ memory contexts */

#define NCLASS		10			/* 16 .. 8192 */
#define BLOCK_MIN	(8 * 1024)
#define BLOCK_MAX	(256 * 1024)
#define CLS_LARGE	0xFF

typedef struct Chunk			/* in front of every allocation */
{
	MemoryContext cx;
	uint32		cls;
	uint32		pad;
} Chunk;

typedef struct Large			/* in front of the Chunk of a large allocation */
{
	void	   *raw;
	struct Large *prev,
			   *next;
	Size		size;
} Large;

typedef struct MBlk
{
	struct MBlk *next;
	char	   *free,
			   *end;
} MBlk;

typedef struct Free
{
	struct Free *next;
} Free;

typedef struct Ctx
{
	MemoryContextData base;
	MBlk	   *blocks;
	Size		next_block;
	Free	   *free[NCLASS];
	Large		large;			/* sentinel of a ring */
} Ctx;

static inline Chunk *
chunk_of(void *p)
{
	return (Chunk *) p - 1;
}

static inline Large *
large_of(Chunk *c)
{
	return (Large *) c - 1;
}

static MemoryContext
ctx_create(MemoryContext parent, const char *name)
{
	Ctx		   *c = calloc(1, sizeof(Ctx));

	c->base.type = T_AllocSetContext;
	c->base.isReset = true;
	c->base.name = name;
	c->base.parent = parent;
	c->large.next = c->large.prev = &c->large;
	c->next_block = BLOCK_MIN;
	if (parent)
	{
		c->base.nextchild = parent->firstchild;
		if (parent->firstchild)
			parent->firstchild->prevchild = &c->base;
		parent->firstchild = &c->base;
	}
	return &c->base;
}

MemoryContext
AllocSetContextCreateInternal(MemoryContext parent, const char *name, Size minContextSize, Size initBlockSize,
							  Size maxBlockSize)
{
	return ctx_create(parent, name);
}

static void
ctx_init(void)
{
	if (TopMemoryContext == NULL)
		CurrentMemoryContext = TopMemoryContext = ctx_create(NULL, "Top");
}

static void
ctx_free_chunks(Ctx *c)
{
	for (MBlk *b = c->blocks, *n; b; b = n)
	{
		n = b->next;
		free(b);
	}
	for (Large *l = c->large.next, *n; l != &c->large; l = n)
	{
		n = l->next;
		free(l->raw);
	}
	c->blocks = NULL;
	c->next_block = BLOCK_MIN;
	memset(c->free, 0, sizeof(c->free));
	c->large.next = c->large.prev = &c->large;
}

void
MemoryContextResetOnly(MemoryContext cx)
{
	ctx_free_chunks((Ctx *) cx);
}

void
MemoryContextDelete(MemoryContext cx)
{
	while (cx->firstchild)
		MemoryContextDelete(cx->firstchild);
	ctx_free_chunks((Ctx *) cx);
	if (cx->parent)
	{
		if (cx->prevchild)
			cx->prevchild->nextchild = cx->nextchild;
		else
			cx->parent->firstchild = cx->nextchild;
		if (cx->nextchild)
			cx->nextchild->prevchild = cx->prevchild;
	}
	if (CurrentMemoryContext == cx)
		CurrentMemoryContext = cx->parent;
	free(cx);
}

void
MemoryContextReset(MemoryContext cx)
{
	while (cx->firstchild)
		MemoryContextDelete(cx->firstchild);
	ctx_free_chunks((Ctx *) cx);
}

static void *
alloc_small(Ctx *c, Size size)
{
	int			cls = size <= 16 ? 0 : 64 - __builtin_clzll(size - 1) - 4;
	Size		need = sizeof(Chunk) + ((Size) 16 << cls);
	MBlk	   *b = c->blocks;
	Chunk	   *ch;

	if (c->free[cls])
	{
		Free	   *f = c->free[cls];

		c->free[cls] = f->next;
		return f;
	}
	if (b == NULL || (Size) (b->end - b->free) < need)
	{
		Size		sz = Max(c->next_block, need + sizeof(MBlk));

		b = malloc(sz);
		b->next = c->blocks;
		b->free = (char *) (b + 1);
		b->end = (char *) b + sz;
		c->blocks = b;
		c->next_block = Min(c->next_block * 2, BLOCK_MAX);
		c->base.mem_allocated += sz;
	}
	ch = (Chunk *) b->free;
	b->free += need;
	ch->cx = &c->base;
	ch->cls = cls;
	return ch + 1;
}

static void *
alloc_large(Ctx *c, Size size, Size align)
{
	char	   *raw = malloc(sizeof(Large) + sizeof(Chunk) + size + align);
	char	   *user = (char *) TYPEALIGN(align, raw + sizeof(Large) + sizeof(Chunk));
	Chunk	   *ch = (Chunk *) user - 1;
	Large	   *l = (Large *) ch - 1;

	if (raw == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
	l->raw = raw;
	l->size = size;
	l->next = c->large.next;
	l->prev = &c->large;
	c->large.next->prev = l;
	c->large.next = l;
	ch->cx = &c->base;
	ch->cls = CLS_LARGE;
	c->base.mem_allocated += size;
	return user;
}

static void *
cx_alloc(MemoryContext cx, Size size, Size align)
{
	ctx_init();
	if (size > ((Size) 16 << (NCLASS - 1)) || align > 16)
		return alloc_large((Ctx *) cx, size, Max(align, 16));
	return alloc_small((Ctx *) cx, size);
}

void *
MemoryContextAlloc(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size, 16);
}

void *
MemoryContextAllocZero(MemoryContext cx, Size size)
{
	return memset(cx_alloc(cx, size, 16), 0, size);
}

void *
MemoryContextAllocExtended(MemoryContext cx, Size size, int flags)
{
	void	   *p = cx_alloc(cx, size, 16);

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
MemoryContextAllocHuge(MemoryContext cx, Size size)
{
	return cx_alloc(cx, size, 16);
}

void *
MemoryContextAllocAligned(MemoryContext cx, Size size, Size alignto, int flags)
{
	void	   *p = cx_alloc(cx, size, alignto);

	return (flags & MCXT_ALLOC_ZERO) ? memset(p, 0, size) : p;
}

void *
palloc(Size size)
{
	ctx_init();
	return cx_alloc(CurrentMemoryContext, size, 16);
}

void *
palloc0(Size size)
{
	return memset(palloc(size), 0, size);
}

void *
palloc_extended(Size size, int flags)
{
	return MemoryContextAllocExtended(CurrentMemoryContext, size, flags);
}

void
pfree(void *p)
{
	Chunk	   *ch = chunk_of(p);
	Ctx		   *c = (Ctx *) ch->cx;

	if (ch->cls == CLS_LARGE)
	{
		Large	   *l = large_of(ch);

		l->prev->next = l->next;
		l->next->prev = l->prev;
		c->base.mem_allocated -= l->size;
		free(l->raw);
	}
	else
	{
		((Free *) p)->next = c->free[ch->cls];
		c->free[ch->cls] = p;
	}
}

static Size
chunk_size(Chunk *ch)
{
	return ch->cls == CLS_LARGE ? large_of(ch)->size : (Size) 16 << ch->cls;
}

void *
repalloc(void *p, Size size)
{
	Chunk	   *ch = chunk_of(p);
	Size		old = chunk_size(ch);
	void	   *n;

	if (size <= old && ch->cls != CLS_LARGE)
		return p;
	n = cx_alloc(ch->cx, size, 16);
	memcpy(n, p, Min(old, size));
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
	char	   *s;
	int			n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = palloc(n + 1);
	va_start(ap, fmt);
	vsnprintf(s, n + 1, fmt, ap);
	va_end(ap);
	return s;
}

/* ------------------------------------------------ errors */

__thread sigjmp_buf *sift_err_jmp;
__thread int sift_err_code;
__thread char sift_err_msg[1024];
volatile sig_atomic_t InterruptPending;

void
ProcessInterrupts(void)
{
}

void
check_stack_depth(void)
{
}

bool
errstart(int elevel, const char *domain)
{
	if (elevel < ERROR)
		return false;
	sift_err_code = 0;
	sift_err_msg[0] = '\0';
	return true;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

int
errcode(int sqlerrcode)
{
	sift_err_code = sqlerrcode;
	return 0;
}

static void
err_append(const char *prefix, const char *fmt, va_list ap)
{
	size_t		n = strlen(sift_err_msg);

	if (n < sizeof(sift_err_msg) - 1)
	{
		n += snprintf(sift_err_msg + n, sizeof(sift_err_msg) - n, "%s", prefix);
		if (n < sizeof(sift_err_msg) - 1)
			vsnprintf(sift_err_msg + n, sizeof(sift_err_msg) - n, fmt, ap);
	}
}

int
errmsg(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	err_append("", fmt, ap);
	va_end(ap);
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	err_append("", fmt, ap);
	va_end(ap);
	return 0;
}

int
errdetail(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	err_append(". ", fmt, ap);
	va_end(ap);
	return 0;
}

int
errdetail_internal(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	err_append(". ", fmt, ap);
	va_end(ap);
	return 0;
}

int
errhint(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	err_append(". ", fmt, ap);
	va_end(ap);
	return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (sift_err_jmp == NULL)
	{
		fprintf(stderr, "sift: uncaught error at %s:%d: %s\n", filename, lineno, sift_err_msg);
		abort();
	}
	siglongjmp(*sift_err_jmp, 1);
}

/* ------------------------------------------------ StringInfo */

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
	str->len = 0;
	str->cursor = 0;
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
appendStringInfoChar(StringInfo str, char ch)
{
	appendBinaryStringInfo(str, &ch, 1);
}

int
appendStringInfoVA(StringInfo str, const char *fmt, va_list args)
{
	va_list		ap;
	int			n;

	va_copy(ap, args);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	enlargeStringInfo(str, n);
	vsnprintf(str->data + str->len, n + 1, fmt, args);
	str->len += n;
	return 0;
}

void
appendStringInfo(StringInfo str, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	appendStringInfoVA(str, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------ List */

List *
lappend(List *list, void *datum)
{
	if (list == NIL)
	{
		list = palloc(offsetof(List, initial_elements) + 4 * sizeof(ListCell));
		list->type = T_List;
		list->length = 0;
		list->max_length = 4;
		list->elements = list->initial_elements;
	}
	else if (list->length == list->max_length)
	{
		ListCell   *e = palloc(2 * list->max_length * sizeof(ListCell));

		memcpy(e, list->elements, list->length * sizeof(ListCell));
		list->elements = e;
		list->max_length *= 2;
	}
	list->elements[list->length++].ptr_value = datum;
	return list;
}

List *
list_append_unique_ptr(List *list, void *datum)
{
	for (int i = 0; list && i < list->length; i++)
		if (list->elements[i].ptr_value == datum)
			return list;
	return lappend(list, datum);
}

/* ------------------------------------------------ hash tables */

struct HTAB
{
	MemoryContext cx;
	Size		keysize,
				entrysize;
	long		n;
	uint32		nbuckets;
	HASHELEMENT **bucket;
};

#define ELEM_DATA(e)	((char *) (e) + MAXALIGN(sizeof(HASHELEMENT)))

HTAB *
hash_create(const char *tabname, long nelem, const HASHCTL *info, int flags)
{
	MemoryContext cx = (flags & HASH_CONTEXT) ? info->hcxt : CurrentMemoryContext;
	HTAB	   *h = MemoryContextAllocZero(cx, sizeof(HTAB));

	h->cx = cx;
	h->keysize = info->keysize;
	h->entrysize = info->entrysize;
	h->nbuckets = 64;
	h->bucket = MemoryContextAllocZero(cx, h->nbuckets * sizeof(HASHELEMENT *));
	return h;
}

void
hash_destroy(HTAB *h)
{
	for (uint32 b = 0; b < h->nbuckets; b++)
		for (HASHELEMENT *e = h->bucket[b], *n; e; e = n)
		{
			n = e->link;
			pfree(e);
		}
	pfree(h->bucket);
	pfree(h);
}

static void
hash_grow(HTAB *h)
{
	uint32		nb = h->nbuckets * 2;
	HASHELEMENT **bk = MemoryContextAllocZero(h->cx, nb * sizeof(HASHELEMENT *));

	for (uint32 b = 0; b < h->nbuckets; b++)
		for (HASHELEMENT *e = h->bucket[b], *n; e; e = n)
		{
			n = e->link;
			e->link = bk[e->hashvalue & (nb - 1)];
			bk[e->hashvalue & (nb - 1)] = e;
		}
	pfree(h->bucket);
	h->bucket = bk;
	h->nbuckets = nb;
}

void *
hash_search(HTAB *h, const void *key, HASHACTION action, bool *foundPtr)
{
	uint32		hv = hash_bytes(key, h->keysize);
	HASHELEMENT **pp = &h->bucket[hv & (h->nbuckets - 1)];
	HASHELEMENT *e;

	for (; (e = *pp) != NULL; pp = &e->link)
		if (e->hashvalue == hv && memcmp(ELEM_DATA(e), key, h->keysize) == 0)
			break;
	if (foundPtr)
		*foundPtr = e != NULL;
	if (action == HASH_FIND)
		return e ? ELEM_DATA(e) : NULL;
	if (action == HASH_REMOVE)
	{
		if (e == NULL)
			return NULL;
		*pp = e->link;
		h->n--;
		pfree(e);
		return ELEM_DATA(e);
	}
	if (e)
		return ELEM_DATA(e);
	if (h->n >= 2L * h->nbuckets)
		hash_grow(h);
	e = MemoryContextAllocZero(h->cx, MAXALIGN(sizeof(HASHELEMENT)) + h->entrysize);
	e->hashvalue = hv;
	memcpy(ELEM_DATA(e), key, h->keysize);
	e->link = h->bucket[hv & (h->nbuckets - 1)];
	h->bucket[hv & (h->nbuckets - 1)] = e;
	h->n++;
	return ELEM_DATA(e);
}

long
hash_get_num_entries(HTAB *h)
{
	return h->n;
}

void
hash_seq_init(HASH_SEQ_STATUS *s, HTAB *h)
{
	s->hashp = h;
	s->curBucket = 0;
	s->curEntry = NULL;
}

void *
hash_seq_search(HASH_SEQ_STATUS *s)
{
	HTAB	   *h = s->hashp;
	HASHELEMENT *e = s->curEntry ? s->curEntry->link : NULL;

	while (e == NULL && s->curBucket < h->nbuckets)
		e = h->bucket[s->curBucket++];
	s->curEntry = e;
	return e ? ELEM_DATA(e) : NULL;
}

/* ------------------------------------------------ encoding: UTF-8 only */

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

bool
pg_verify_mbstr(int encoding, const char *mbstr, int len, bool noError)
{
	if (pg_encoding_verifymbstr(encoding, mbstr, len) != len)
	{
		if (noError)
			return false;
		ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("invalid byte sequence for encoding \"UTF8\"")));
	}
	return true;
}

int
pg_mblen(const char *s)
{
	return pg_encoding_mblen(PG_UTF8, s);
}

int
pg_mblen_range(const char *s, const char *end)
{
	int			l = pg_encoding_mblen(PG_UTF8, s);

	if (l > end - s)
		ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE), errmsg("truncated character")));
	return l;
}

int
pg_mbstrlen_with_len(const char *s, int limit)
{
	int			n = 0;

	for (int i = 0; i < limit; n++)
		i += pg_encoding_mblen(PG_UTF8, s + i);
	return n;
}

int
pg_mbcliplen(const char *s, int len, int limit)
{
	int			i = 0;

	while (i < len && i + pg_encoding_mblen(PG_UTF8, s + i) <= limit)
		i += pg_encoding_mblen(PG_UTF8, s + i);
	return i;
}

int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[PG_UTF8].mb2wchar_with_len((const unsigned char *) from, to, len);
}

int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	return pg_wchar_table[PG_UTF8].wchar2mb_with_len(from, (unsigned char *) to, len);
}

/* libpgcommon's binaryheap logs through the frontend logger */
void
pg_log_generic(enum pg_log_level level, enum pg_log_part part, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(sift_err_msg, sizeof(sift_err_msg), fmt, ap);
	va_end(ap);
	if (level >= PG_LOG_ERROR)
		ereport(ERROR, (errmsg_internal("%s", sift_err_msg)));
}

/* ------------------------------------------------ BufFile: a temporary file (O_TMPFILE in sift_tmpdir) */

const char *sift_tmpdir = "/tmp";

#define BF_BUF	65536

struct BufFile
{
	int			fd;
	off_t		pos;			/* logical position */
	off_t		size;
	char	   *wbuf;			/* pending writes at [pos - wlen, pos) */
	size_t		wlen;
};

BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *f = calloc(1, sizeof(BufFile));

	f->fd = open(sift_tmpdir, O_TMPFILE | O_RDWR, 0600);
	if (f->fd < 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not create a temporary file in \"%s\": %m", sift_tmpdir)));
	f->wbuf = malloc(BF_BUF);
	return f;
}

static void
bf_flush(BufFile *f)
{
	off_t		at = f->pos - f->wlen;

	for (size_t done = 0; done < f->wlen;)
	{
		ssize_t		n = pwrite(f->fd, f->wbuf + done, f->wlen - done, at + done);

		if (n < 0)
			ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not write to a temporary file")));
		done += n;
	}
	f->wlen = 0;
}

void
BufFileClose(BufFile *f)
{
	close(f->fd);
	free(f->wbuf);
	free(f);
}

void
BufFileWrite(BufFile *f, const void *ptr, size_t size)
{
	if (f->wlen + size > BF_BUF)
		bf_flush(f);
	if (size > BF_BUF)
	{
		f->wlen = 0;
		f->pos += size;
		for (size_t done = 0; done < size;)
		{
			ssize_t		n = pwrite(f->fd, (const char *) ptr + done, size - done, f->pos - size + done);

			if (n < 0)
				ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not write to a temporary file")));
			done += n;
		}
	}
	else
	{
		memcpy(f->wbuf + f->wlen, ptr, size);
		f->wlen += size;
		f->pos += size;
	}
	f->size = Max(f->size, f->pos);
}

size_t
BufFileRead(BufFile *f, void *ptr, size_t size)
{
	size_t		done = 0;

	bf_flush(f);
	while (done < size)
	{
		ssize_t		n = pread(f->fd, (char *) ptr + done, size - done, f->pos + done);

		if (n < 0)
			ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not read from a temporary file")));
		if (n == 0)
			break;
		done += n;
	}
	f->pos += done;
	return done;
}

void
BufFileReadExact(BufFile *f, void *ptr, size_t size)
{
	if (BufFileRead(f, ptr, size) != size)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not read from a temporary file: short read")));
}

size_t
BufFileReadMaybeEOF(BufFile *f, void *ptr, size_t size, bool eofOK)
{
	size_t		n = BufFileRead(f, ptr, size);

	if (n != size && !(eofOK && n == 0))
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not read from a temporary file: short read")));
	return n;
}

int
BufFileSeek(BufFile *f, int fileno, off_t offset, int whence)
{
	bf_flush(f);
	f->pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? f->pos + offset : f->size + offset;
	return 0;
}

void
BufFileTell(BufFile *f, int *fileno, off_t *offset)
{
	*fileno = 0;
	*offset = f->pos;
}

BufFile *
BufFileCreateFileSet(FileSet *fileset, const char *name)
{
	elog(ERROR, "sift: shared file sets are not available");
	return NULL;
}

void
BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok)
{
	elog(ERROR, "sift: shared file sets are not available");
}

/* The memory a merge or a build may use before it spills sorted runs to temporary files, in kB */
int			maintenance_work_mem = 65536;

/* ------------------------------------------------ buffer manager: block numbers map to the mapped index files */

char	   *BufferBlocks;
Block	   *LocalBufferBlockPointers;

Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	return sift_buf_of(reln, blockNum);
}

void
LockBuffer(Buffer buffer, int mode)
{
}

void
UnlockReleaseBuffer(Buffer buffer)
{
}

void
ReleaseBuffer(Buffer buffer)
{
}

BlockNumber
BufferGetBlockNumber(Buffer buffer)
{
	return sift_buf_block(buffer);
}

struct GenericXLogState
{
	Buffer		buf[4];
	Page		page[4];
	int			n;
};

GenericXLogState *
GenericXLogStart(Relation relation)
{
	return palloc0(sizeof(GenericXLogState));
}

Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buffer, int flags)
{
	Page		copy = palloc(BLCKSZ);

	memcpy(copy, BufferGetPage(buffer), BLCKSZ);
	state->buf[state->n] = buffer;
	state->page[state->n] = copy;
	return state->page[state->n++];
}

XLogRecPtr
GenericXLogFinish(GenericXLogState *state)
{
	for (int i = 0; i < state->n; i++)
	{
		memcpy(BufferGetPage(state->buf[i]), state->page[i], BLCKSZ);
		pfree(state->page[i]);
	}
	pfree(state);
	return 0;
}
