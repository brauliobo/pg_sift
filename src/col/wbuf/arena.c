/*
 * col/wbuf/arena.c - the values of the REF records of a write buffer, in shared memory.
 *
 * The backend that inserts a row has its values: it keeps them here, under (index, buffer generation, TID, inserter xid), when it writes the 16 byte record (SIFT_COLR_REF) to the
 * buffer, so that a reader of the buffer takes them from here and never from the heap. The heap is read for a record only to build an entry again (col/wbuf/ref.c): after a restart
 * or a crash (the table is empty), on a standby (replay makes the records, no one made the entries), or when the table dropped the entries of the buffer.
 *
 * The table is a few slots in the DSM registry (nothing is allocated at postmaster start, an untouched page is not backed): a slot is the buffer of one generation, a bump area of
 * the encoded rows and an open addressing directory of their offsets. A slot is full when it holds as many bytes as write_buffer_limit says: the seal of the buffer follows
 * (sift_cwbuf_seal), so the limit counts the rows a buffer holds, not the pages of its records. A new buffer takes a free slot or the least recently used one.
 * An entry is written once and never changed until its slot is taken for another buffer (the epoch of the slot moves), so a reader copies it outside the spinlock and checks
 * the epoch after.
 */
#include "postgres.h"

#include "storage/spin.h"

#include "col/dsmreg.h"
#include "col/wbuf/wbuf_int.h"

#define ARENA_SLOTS		8
#define ARENA_DIR		(1 << 17)			/* directory entries of a slot: half of them are rows at most */
#define ARENA_ROWS		(ARENA_DIR / 2)
#define ARENA_DATA		((Size) 6 << 20)	/* bytes of rows of a slot */
#define ARENA_SLOT_SIZE	(ARENA_DIR * sizeof(uint32) + ARENA_DATA)
#define ARENA_NAME		"sift_col_wbuf_arena_1"

typedef struct ArenaSlot
{
	bool		used;
	Oid			db;
	RelFileNumber rel;
	uint64		gen;
	uint64		epoch;
	uint64		tick;
	Size		bytes;
	uint32		rows;
} ArenaSlot;

typedef struct Arena
{
	slock_t		mutex;
	uint64		tick;
	uint64		epoch;
	ArenaSlot	slot[ARENA_SLOTS];
	char		mem[FLEXIBLE_ARRAY_MEMBER];
} Arena;

typedef struct ArenaRow
{
	ItemPointerData tid;
	uint16		pad;
	TransactionId xmin;
	uint32		len;
} ArenaRow;

static Arena *arena;

static void
arena_init(SIFT_DSM_INIT_ARGS)
{
	memset(ptr, 0, offsetof(Arena, mem));
	SpinLockInit(&((Arena *) ptr)->mutex);
}

/* joined without a subtransaction (sift_dsm_attach goes without the table under a write statement, and the first row of a backend is written by one): the rows of a REF record have no other place */
static Arena *
arena_get(void)
{
	if (arena == NULL)
		arena = sift_dsm_join(ARENA_NAME, offsetof(Arena, mem) + ARENA_SLOTS * ARENA_SLOT_SIZE, arena_init);
	return arena;
}

static inline uint32 *
slot_dir(Arena *a, const ArenaSlot *s)
{
	return (uint32 *) (a->mem + (s - a->slot) * ARENA_SLOT_SIZE);
}

static inline char *
slot_data(Arena *a, const ArenaSlot *s)
{
	return a->mem + (s - a->slot) * ARENA_SLOT_SIZE + ARENA_DIR * sizeof(uint32);
}

static inline uint32
row_hash(const ItemPointerData *tid, TransactionId xmin)
{
	uint64		k = ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 32) ^ ((uint64) ItemPointerGetOffsetNumberNoCheck(tid) << 16) ^ xmin;

	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & (ARENA_DIR - 1);
}

/* the slot of the buffer; with create, a free or the least recently used one when there is none (its entries are gone). Under the spinlock */
static ArenaSlot *
slot_find(Arena *a, Relation index, uint64 gen, bool create)
{
	ArenaSlot  *v = NULL;

	for (int i = 0; i < ARENA_SLOTS; i++)
	{
		ArenaSlot  *s = &a->slot[i];

		if (s->used && s->gen == gen && s->rel == index->rd_locator.relNumber && s->db == index->rd_locator.dbOid)
			return s;
		if (v == NULL || (v->used && (!s->used || s->tick < v->tick)))
			v = s;
	}
	if (!create)
		return NULL;
	*v = (ArenaSlot) {.used = true,.db = index->rd_locator.dbOid,.rel = index->rd_locator.relNumber,.gen = gen,.epoch = ++a->epoch};
	memset(slot_dir(a, v), 0, ARENA_DIR * sizeof(uint32));
	return v;
}

static const ArenaRow *
slot_probe(Arena *a, const ArenaSlot *s, const ItemPointerData *tid, TransactionId xmin, uint32 **at)
{
	uint32	   *dir = slot_dir(a, s);

	for (uint32 h = row_hash(tid, xmin);; h = (h + 1) & (ARENA_DIR - 1))
	{
		const ArenaRow *r;

		*at = &dir[h];
		if (dir[h] == 0)
			return NULL;
		r = (const ArenaRow *) (slot_data(a, s) + dir[h] - 1);
		if (memcmp(&r->tid, tid, sizeof(ItemPointerData)) == 0 && r->xmin == xmin)
			return r;
	}
}

/* Keeps the values of a row (what the record of a row holds after its xid) in the arena of the buffer; true when the arena of the buffer holds limit bytes (the buffer is to be sealed) */
bool
wbuf_arena_put(Relation index, uint64 gen, const ItemPointerData *tid, TransactionId xmin, const char *vals, Size len, Size limit)
{
	Arena	   *a = arena_get();
	Size		need = TYPEALIGN(8, sizeof(ArenaRow) + len);
	ArenaSlot  *s;
	uint32	   *at;
	bool		full;

	SpinLockAcquire(&a->mutex);
	s = slot_find(a, index, gen, true);
	s->tick = ++a->tick;
	if (slot_probe(a, s, tid, xmin, &at) == NULL && s->bytes + need <= ARENA_DATA && s->rows < ARENA_ROWS)
	{
		ArenaRow   *r = (ArenaRow *) (slot_data(a, s) + s->bytes);

		*r = (ArenaRow) {.tid = *tid,.xmin = xmin,.len = len};
		memcpy(r + 1, vals, len);
		*at = s->bytes + 1;
		s->bytes += need;
		s->rows++;
	}
	full = s->bytes >= limit || s->rows >= ARENA_ROWS;
	SpinLockRelease(&a->mutex);
	return full;
}

/* Has the arena of the open buffer of generation gen reached limit bytes? */
bool
wbuf_arena_full(Relation index, uint64 gen, Size limit)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	bool		full = false;

	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) != NULL)
		full = s->bytes >= limit || s->rows >= ARENA_ROWS;
	SpinLockRelease(&a->mutex);
	return full;
}

/*
 * Appends the values of the rows that have an entry to pay, and marks them live with the offset and length of their values in pay. The rows without one are left as they are: the
 * caller builds them again from the heap.
 */
void
wbuf_arena_read(Relation index, uint64 gen, RefRow *rows, int n, StringInfo pay)
{
	Arena	   *a = arena_get();
	ArenaSlot  *s;
	uint64		epoch;
	const char *data;
	bool		stable;

	if (n == 0)
		return;
	SpinLockAcquire(&a->mutex);
	if ((s = slot_find(a, index, gen, false)) == NULL)
	{
		SpinLockRelease(&a->mutex);
		return;
	}
	s->tick = ++a->tick;
	epoch = s->epoch;
	data = slot_data(a, s);
	for (int i = 0; i < n; i++)
	{
		uint32	   *at;
		const ArenaRow *r = slot_probe(a, s, &rows[i].tid, rows[i].xmin, &at);

		if (r != NULL)
		{
			rows[i].off = (const char *) (r + 1) - data;
			rows[i].len = r->len;
			rows[i].live = true;
		}
	}
	SpinLockRelease(&a->mutex);
	for (int i = 0; i < n; i++)
	{
		if (!rows[i].live)
			continue;
		appendBinaryStringInfo(pay, data + rows[i].off, rows[i].len);
		rows[i].off = pay->len - rows[i].len;
	}
	SpinLockAcquire(&a->mutex);
	stable = s->epoch == epoch;
	SpinLockRelease(&a->mutex);
	if (!stable)
	{
		/* the slot was taken for another buffer while it was copied */
		resetStringInfo(pay);
		for (int i = 0; i < n; i++)
			rows[i].live = false;
	}
}
