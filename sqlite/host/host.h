/*
 * host.h - the SQLite implementation of the host interface of the portable core (src/sift_host.h): the vocabulary
 * the core uses (memory contexts, ereport, StringInfo, List, hash tables, UTF-8 and regex helpers, atomics and
 * locks, spool files), over the C library. The sources of src/portable.list compile unmodified against it; they
 * reach it only through src/sift_host.h (SIFT_HOST_SQLITE, -I sqlite/host).
 *
 * Memory is a context tree over malloc (host.c); ereport(ERROR) longjmps to the handler a caller pushed with
 * SQ_TRY (host.c), so no engine state needs unwinding. Pages, buffers and relations are in host_store.h.
 */
#ifndef SQ_HOST_H
#define SQ_HOST_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>

/* ---------- base types and macros ---------- */

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef size_t Size;
typedef float float4;
typedef double float8;
typedef unsigned int Oid;
typedef uint32 BlockNumber;
typedef uint16 OffsetNumber;
typedef uintptr_t Datum;
typedef int64 TimestampTz;
typedef uint32 pg_wchar;
typedef struct { uint64 value; } FullTransactionId;

#define InvalidOid				((Oid) 0)
#define InvalidBlockNumber		((BlockNumber) 0xFFFFFFFF)
#define InvalidOffsetNumber		((OffsetNumber) 0)
#define PG_UINT16_MAX			(0xFFFF)
#define PG_UINT32_MAX			(0xFFFFFFFFU)
#define PG_UINT64_MAX			UINT64_MAX
#define PG_INT32_MAX			(0x7FFFFFFF)
#define PG_INT32_MIN			(-0x7FFFFFFF - 1)
#define PGDLLIMPORT
#define PGDLLEXPORT
#define BLCKSZ					8192
#define FLEXIBLE_ARRAY_MEMBER
#define MAXIMUM_ALIGNOF			8
#define BITS_PER_BYTE			8
#define HIGHBIT					0x80

#define qsort_arg				qsort_r	/* the same (base, n, size, cmp(a, b, arg), arg) */
#define Max(x, y)				((x) > (y) ? (x) : (y))
#define Min(x, y)				((x) < (y) ? (x) : (y))
#define Abs(x)					((x) >= 0 ? (x) : -(x))
#define lengthof(array)			(sizeof(array) / sizeof((array)[0]))
#define TYPEALIGN(a, len)		(((uintptr_t) (len) + ((a) - 1)) & ~((uintptr_t) ((a) - 1)))
#define MAXALIGN(len)			TYPEALIGN(MAXIMUM_ALIGNOF, (len))
#define StaticAssertDecl(c, m)	_Static_assert(c, m)
#define StaticAssertStmt(c, m)	_Static_assert(c, m)
#define StaticAssertExpr(c, m)	((void) sizeof(struct { int sq_assert_: (c) ? 1 : -1; }))
#define Assert(c)				((void) 0)
#define AssertArg(c)			((void) 0)
#define likely(x)				__builtin_expect((x) != 0, 1)
#define unlikely(x)				__builtin_expect((x) != 0, 0)
#define pg_unreachable()		__builtin_unreachable()
#define pg_attribute_aligned(a)	__attribute__((aligned(a)))
#define pg_attribute_always_inline __attribute__((always_inline)) inline
#define pg_attribute_unused()	__attribute__((unused))
#define pg_attribute_noreturn()	__attribute__((noreturn))
#define pg_noinline				__attribute__((noinline))
#define pg_attribute_printf(f, a) __attribute__((format(printf, f, a)))
#define INT64CONST(x)			(x##L)
#define UINT64CONST(x)			(x##UL)
#define INT64_FORMAT			"%ld"
#define UINT64_FORMAT			"%lu"
#define CHECK_FOR_INTERRUPTS()	((void) 0)
#define pg_attribute_cold
#define unconstify(t, v)		((t) (v))
#define pgstat_report_wait_start(x)	((void) 0)

/* ---------- bit utilities (port/pg_bitutils.h, common/int.h) ---------- */

/* sift_pc64: baseline x86-64 code must not call libgcc's __popcountdi2, which a distribution may build with POPCNT */
#include "sift_popcount.h"
static inline int pg_popcount32(uint32 x) { return sift_pc64(x); }
static inline int pg_popcount64(uint64 x) { return sift_pc64(x); }
static inline int pg_rightmost_one_pos32(uint32 x) { return __builtin_ctz(x); }
static inline int pg_rightmost_one_pos64(uint64 x) { return __builtin_ctzll(x); }
static inline int pg_leftmost_one_pos32(uint32 x) { return 31 - __builtin_clz(x); }
static inline int pg_leftmost_one_pos64(uint64 x) { return 63 - __builtin_clzll(x); }
static inline uint32 pg_nextpower2_32(uint32 n) { return n <= 1 ? 1 : 1U << (32 - __builtin_clz(n - 1)); }
static inline uint32 pg_bswap32(uint32 x) { return __builtin_bswap32(x); }
static inline uint64 pg_bswap64(uint64 x) { return __builtin_bswap64(x); }
static inline int pg_cmp_u64(uint64 a, uint64 b) { return (a > b) - (a < b); }
static inline int pg_cmp_u32(uint32 a, uint32 b) { return (a > b) - (a < b); }
static inline bool pg_add_u64_overflow(uint64 a, uint64 b, uint64 *r) { return __builtin_add_overflow(a, b, r); }
static inline bool pg_mul_u64_overflow(uint64 a, uint64 b, uint64 *r) { return __builtin_mul_overflow(a, b, r); }
static inline unsigned char pg_ascii_tolower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline unsigned char pg_ascii_toupper(unsigned char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

extern int pg_strcasecmp(const char *s1, const char *s2);
extern int pg_strncasecmp(const char *s1, const char *s2, size_t n);
extern uint32 hash_bytes(const unsigned char *k, int keylen);

/* ---------- memory contexts ---------- */

typedef struct MemoryContextData *MemoryContext;

/* Per thread: the context in use. Top is the one process-wide root; only contexts created under it are shared. */
extern _Thread_local MemoryContext CurrentMemoryContext;
extern MemoryContext TopMemoryContext;

#define ALLOCSET_DEFAULT_SIZES	0, 8192, 8 * 1024 * 1024
#define ALLOCSET_SMALL_SIZES	0, 1024, 8192
#define ALLOCSET_DEFAULT_MINSIZE 0
#define MCXT_ALLOC_HUGE			0x01
#define MCXT_ALLOC_NO_OOM		0x02
#define MCXT_ALLOC_ZERO			0x04

extern MemoryContext AllocSetContextCreate(MemoryContext parent, const char *name, size_t min, size_t init, size_t max);
extern void MemoryContextDelete(MemoryContext cx);
extern void MemoryContextReset(MemoryContext cx);
extern void MemoryContextResetChildren(MemoryContext cx);
extern size_t MemoryContextMemAllocated(MemoryContext cx, bool recurse);
extern void *MemoryContextAlloc(MemoryContext cx, Size size);
extern void *MemoryContextAllocZero(MemoryContext cx, Size size);
extern void *MemoryContextAllocHuge(MemoryContext cx, Size size);
extern void *MemoryContextAllocExtended(MemoryContext cx, Size size, int flags);
extern void *MemoryContextAllocAligned(MemoryContext cx, Size size, Size alignto, int flags);
extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *repalloc(void *p, Size size);
extern void *repalloc_huge(void *p, Size size);
extern void pfree(void *p);
extern char *pstrdup(const char *s);
extern char *pnstrdup(const char *s, Size len);
extern MemoryContext GetMemoryChunkContext(void *p);
extern void sq_mem_init(void);

/* glibc only has strlcpy since 2.38 */
extern size_t sq_strlcpy(char *dst, const char *src, size_t size);
#define strlcpy sq_strlcpy

static inline MemoryContext
MemoryContextSwitchTo(MemoryContext cx)
{
	MemoryContext old = CurrentMemoryContext;

	CurrentMemoryContext = cx;
	return old;
}

/* ---------- errors: ereport(ERROR) longjmps to the innermost SQ_TRY ---------- */

#define DEBUG1		14
#define LOG			15
#define INFO		17
#define NOTICE		18
#define WARNING		19
#define ERROR		21
#define FATAL		22
#define PANIC		23

#define MAKE_SQLSTATE(a, b, c, d, e)	((a) | ((b) << 6) | ((c) << 12) | ((d) << 18) | ((e) << 24))
#define ERRCODE_INTERNAL_ERROR				1
#define ERRCODE_SYNTAX_ERROR				2
#define ERRCODE_PROGRAM_LIMIT_EXCEEDED		3
#define ERRCODE_INDEX_CORRUPTED				4
#define ERRCODE_FEATURE_NOT_SUPPORTED		5
#define ERRCODE_INVALID_PARAMETER_VALUE		6
#define ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE 7
#define ERRCODE_INVALID_REGULAR_EXPRESSION	8
#define ERRCODE_CHARACTER_NOT_IN_REPERTOIRE	9
#define ERRCODE_T_R_SERIALIZATION_FAILURE	10
#define ERRCODE_DATA_CORRUPTED				11
#define ERRCODE_OUT_OF_MEMORY				12

extern bool errstart(int elevel, const char *domain);
extern bool errstart_cold(int elevel, const char *domain);
extern void errfinish(const char *filename, int lineno, const char *funcname);
extern int errcode(int sqlerrcode);
extern int errmsg(const char *fmt,...) pg_attribute_printf(1, 2);
extern int errmsg_internal(const char *fmt,...) pg_attribute_printf(1, 2);
extern int errdetail(const char *fmt,...) pg_attribute_printf(1, 2);
extern int errdetail_internal(const char *fmt,...) pg_attribute_printf(1, 2);
extern int errhint(const char *fmt,...) pg_attribute_printf(1, 2);

#define ereport(elevel, ...) \
	do { \
		const int elevel_ = (elevel); \
		if (errstart(elevel_, NULL)) \
			__VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while (0)

#define elog(elevel, ...) \
	do { \
		const int elevel_ = (elevel); \
		if (errstart(elevel_, NULL)) \
			errmsg_internal(__VA_ARGS__), errfinish(__FILE__, __LINE__, __func__); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while (0)

/* The last ERROR: message, and a handler stack. SQ_TRY { ... } SQ_CATCH { sq_errmsg() } SQ_END */
typedef struct SqErrorFrame
{
	sigjmp_buf	jmp;
	struct SqErrorFrame *prev;
	MemoryContext cx;				/* restored on catch */
} SqErrorFrame;

extern _Thread_local SqErrorFrame *sq_error_top;
extern const char *sq_errmsg(void);
extern int sq_errcode(void);
extern void sq_error_set(int code, const char *fmt,...) pg_attribute_printf(2, 3);
extern void sq_error_rethrow(void) pg_attribute_noreturn();

#define SQ_TRY() \
	do { \
		SqErrorFrame sq_frame_; \
		sq_frame_.prev = sq_error_top; \
		sq_frame_.cx = CurrentMemoryContext; \
		sq_error_top = &sq_frame_; \
		if (sigsetjmp(sq_frame_.jmp, 0) == 0) {

#define SQ_CATCH() \
			sq_error_top = sq_frame_.prev; \
		} else { \
			sq_error_top = sq_frame_.prev; \
			CurrentMemoryContext = sq_frame_.cx;

#define SQ_END() \
		} \
	} while (0)

/* ---------- StringInfo ---------- */

typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
	int			cursor;
} StringInfoData;

typedef StringInfoData *StringInfo;

extern void initStringInfo(StringInfo str);
extern void resetStringInfo(StringInfo str);
extern void appendStringInfo(StringInfo str, const char *fmt,...) pg_attribute_printf(2, 3);
extern void appendStringInfoString(StringInfo str, const char *s);
extern void appendStringInfoChar(StringInfo str, char c);
extern void appendBinaryStringInfo(StringInfo str, const void *data, int datalen);
extern void enlargeStringInfo(StringInfo str, int needed);

/* ---------- List (pointer lists only) ---------- */

typedef union ListCell
{
	void	   *ptr_value;
	int			int_value;
} ListCell;

typedef struct List
{
	int			length;
	int			max_length;
	ListCell   *elements;
} List;

#define NIL						((List *) NULL)
#define lfirst(lc)				((lc)->ptr_value)
#define lfirst_int(lc)			((lc)->int_value)
#define list_length(l)			((l) ? (l)->length : 0)
#define list_head(l)			((l) ? &(l)->elements[0] : NULL)
#define lnext(l, c)				(((c) + 1 < &(l)->elements[(l)->length]) ? (c) + 1 : NULL)
#define foreach(cell, lst)		for ((cell) = list_head(lst); (cell) != NULL; (cell) = lnext(lst, cell))
#define foreach_ptr(type, var, lst) \
	for (int var##__i = 0; var##__i < list_length(lst); var##__i++) \
		for (type *var = (type *) (lst)->elements[var##__i].ptr_value, *var##__once = (type *) 1; var##__once; var##__once = NULL)

extern List *lappend(List *list, void *datum);
extern List *list_append_unique_ptr(List *list, void *datum);
extern void *list_nth(const List *list, int n);

/* ---------- hash tables (utils/hsearch.h): fixed-size keys, chained ---------- */

typedef struct HTAB HTAB;

typedef struct HASHCTL
{
	Size		keysize;
	Size		entrysize;
	MemoryContext hcxt;
} HASHCTL;

typedef enum { HASH_FIND, HASH_ENTER, HASH_REMOVE, HASH_ENTER_NULL } HASHACTION;
#define HASH_ELEM		0x01
#define HASH_BLOBS		0x02
#define HASH_CONTEXT	0x04

typedef struct HASH_SEQ_STATUS
{
	HTAB	   *tab;
	uint32		bucket;
	void	   *next;
} HASH_SEQ_STATUS;

extern HTAB *hash_create(const char *name, long nelem, const HASHCTL *info, int flags);
extern void hash_destroy(HTAB *tab);
extern void *hash_search(HTAB *tab, const void *key, HASHACTION action, bool *found);
extern long hash_get_num_entries(HTAB *tab);
extern void hash_seq_init(HASH_SEQ_STATUS *status, HTAB *tab);
extern void *hash_seq_search(HASH_SEQ_STATUS *status);

/* ---------- varlena / text: a plain int32 length (incl. header) then bytes ---------- */

typedef struct varlena
{
	int32		vl_len_;
	char		vl_dat[FLEXIBLE_ARRAY_MEMBER];
} varlena;

typedef varlena text;
#define VARHDRSZ				((int32) sizeof(int32))
#define VARDATA_ANY(p)			(((varlena *) (p))->vl_dat)
#define VARSIZE_ANY_EXHDR(p)	((Size) (((varlena *) (p))->vl_len_ - VARHDRSZ))
#define SET_VARSIZE(p, len)		(((varlena *) (p))->vl_len_ = (len))
extern text *cstring_to_text_with_len(const char *s, int len);

/* ---------- ItemPointer (storage/itemptr.h): block and offset, no alignment games ---------- */

typedef struct ItemPointerData
{
	uint32		blk;
	uint16		off;
} ItemPointerData;

typedef ItemPointerData *ItemPointer;
#define ItemPointerSet(p, b, o)				((p)->blk = (b), (p)->off = (o))
#define ItemPointerGetBlockNumberNoCheck(p)	((p)->blk)
#define ItemPointerGetOffsetNumberNoCheck(p) ((p)->off)

#define MaxHeapTuplesPerPage	291

/* ---------- multibyte (mb/pg_wchar.h): UTF-8 only ---------- */

#define PG_SQL_ASCII	0
#define PG_UTF8			6
#define C_COLLATION_OID	950

extern int GetDatabaseEncoding(void);
extern const char *GetDatabaseEncodingName(void);
extern int pg_database_encoding_max_length(void);
extern int pg_mblen(const char *s);
extern int pg_mblen_range(const char *s, const char *end);
extern int pg_mbstrlen_with_len(const char *s, int len);
extern int pg_mbcliplen(const char *s, int len, int limit);
extern int pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len);
extern int pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len);
extern void pg_verify_mbstr(int encoding, const char *s, int len, bool noError);

/* ---------- regex (regex/regex.h) over the C library, see shim_regex.c ---------- */

typedef struct regex_t
{
	void	   *posix;				/* regex_t of <regex.h> */
	int			magic;
} regex_t;

#define REG_ADVANCED	0x0001
#define REG_NOSUB		0x0002
#define REG_OKAY		0
#define REG_NOMATCH		1
#define REG_EXACT		2
#define REG_PREFIX		3
#define REG_ESPACE		4

extern int pg_regcomp(regex_t *re, const pg_wchar *pattern, size_t len, int cflags, Oid collation);
extern int pg_regexec(regex_t *re, const pg_wchar *data, size_t len, size_t search_start,
					  void *details, size_t nmatch, void *pmatch, int eflags);
extern size_t pg_regerror(int errcode, const regex_t *re, char *errbuf, size_t errbuf_size);
extern void pg_regfree(regex_t *re);
extern int pg_regprefix(regex_t *re, pg_wchar **string, size_t *slength);

/* ---------- BufFile (temporary spool files) ---------- */

typedef struct BufFile BufFile;
extern BufFile *BufFileCreateTemp(bool interXact);
extern void BufFileWrite(BufFile *file, const void *ptr, size_t size);
extern void BufFileReadExact(BufFile *file, void *ptr, size_t size);
extern size_t BufFileRead(BufFile *file, void *ptr, size_t size);
extern int BufFileSeek(BufFile *file, int fileno, long offset, int whence);
extern void BufFileClose(BufFile *file);

/* named files shared between processes: there is one process, the runs of a fileset are never asked for */
typedef struct FileSet { int unused; } FileSet;
typedef struct SharedFileSet { FileSet fs; } SharedFileSet;
extern BufFile *BufFileCreateFileSet(FileSet *fileset, const char *name);
extern void BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok);

/* the memory (kB) a rebuild of segments may use: the budget of the run merge (lib/binaryheap.h follows) */
extern int maintenance_work_mem;

#define PointerGetDatum(X)		((Datum) (X))
#define DatumGetPointer(X)		((void *) (X))

typedef struct binaryheap binaryheap;
typedef int (*binaryheap_comparator) (Datum a, Datum b, void *arg);
extern binaryheap *binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg);
extern void binaryheap_free(binaryheap *heap);
extern void binaryheap_add_unordered(binaryheap *heap, Datum d);
extern void binaryheap_build(binaryheap *heap);
extern void binaryheap_add(binaryheap *heap, Datum d);
extern Datum binaryheap_first(binaryheap *heap);
extern Datum binaryheap_remove_first(binaryheap *heap);
extern void binaryheap_replace_first(binaryheap *heap, Datum d);
extern bool binaryheap_empty(binaryheap *heap);

/* ---------- small odds and ends ---------- */

extern char *psprintf(const char *fmt,...) pg_attribute_printf(1, 2);
#define linitial(l)				((l)->elements[0].ptr_value)
#define check_stack_depth()		((void) 0)
extern int float_to_shortest_decimal_buf(float f, char *result);

static inline uint64
hash_combine64(uint64 a, uint64 b)
{
	a ^= b + UINT64CONST(0x49a0f4dd15e5a8e3) + (a << 54) + (a >> 7);
	return a;
}

typedef union PGIOAlignedBlock
{
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGIOAlignedBlock __attribute__((aligned(64)));

/* the tidbitmap iteration of src/sift_compat.h: only the planner builds a filter, which a virtual table never does */
typedef struct TIDBitmap TIDBitmap;
typedef struct SiftTbmIter SiftTbmIter;
extern SiftTbmIter *sift_tbm_begin(TIDBitmap *tbm);
extern bool sift_tbm_next(SiftTbmIter *it, BlockNumber *blockno, bool *lossy, OffsetNumber *offs, int *n);
extern void sift_tbm_end(SiftTbmIter *it);

/* ---------- atomics, spin locks and "shared memory" of sift_parallel.c: the participants are threads of one process ---------- */

#include <pthread.h>
#include <stdatomic.h>

typedef struct { _Atomic uint32 v; } pg_atomic_uint32;
typedef struct { _Atomic uint64 v; } pg_atomic_uint64;

static inline void pg_atomic_init_u32(pg_atomic_uint32 *p, uint32 x) { atomic_init(&p->v, x); }
static inline void pg_atomic_init_u64(pg_atomic_uint64 *p, uint64 x) { atomic_init(&p->v, x); }
static inline uint32 pg_atomic_read_u32(const pg_atomic_uint32 *p) { return atomic_load_explicit((_Atomic uint32 *) &p->v, memory_order_acquire); }
static inline uint64 pg_atomic_read_u64(const pg_atomic_uint64 *p) { return atomic_load_explicit((_Atomic uint64 *) &p->v, memory_order_acquire); }
static inline uint64 pg_atomic_fetch_add_u64(pg_atomic_uint64 *p, int64 n) { return atomic_fetch_add(&p->v, n); }
static inline bool pg_atomic_compare_exchange_u32(pg_atomic_uint32 *p, uint32 *expected, uint32 x)
{
	return atomic_compare_exchange_strong(&p->v, expected, x);
}

typedef pthread_mutex_t slock_t;
#define SpinLockInit(l)		pthread_mutex_init((l), NULL)
#define SpinLockAcquire(l)	pthread_mutex_lock(l)
#define SpinLockRelease(l)	pthread_mutex_unlock(l)

typedef struct dsa_area dsa_area;
typedef uint64 dsa_pointer;
#define InvalidDsaPointer		((dsa_pointer) 0)
#define DsaPointerIsValid(p)	((p) != InvalidDsaPointer)
#define DSA_ALLOC_HUGE			0x01
static inline dsa_pointer dsa_allocate_extended(dsa_area *a, Size n, int flags) { return (dsa_pointer) (uintptr_t) malloc(Max(n, 1)); }
static inline void *dsa_get_address(dsa_area *a, dsa_pointer p) { return (void *) (uintptr_t) p; }
static inline void dsa_free(dsa_area *a, dsa_pointer p) { free((void *) (uintptr_t) p); }

static inline Size add_size(Size a, Size b) { return a + b; }
static inline Size mul_size(Size a, Size b) { return a * b; }

/* ---------- pages, buffers and relations over SQLite rows: sqstore.h ---------- */

#include "host_store.h"

#endif							/* SQ_HOST_H */
