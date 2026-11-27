/*
 * test/unit/bm25/doctext_window.c - the sorted docs of a promotion and the window of texts it reads ahead in the order the rows lie (bm25/doctext.c):
 *   - the window stops reading once its texts take the memory it may, and the docs it did not read are read when their turn comes (it read every doc of
 *     the window first and checked the memory after: 16384 docs of 256 kB each held 4 GB under any maintenance budget);
 *   - the terms of a doc past 1 GB of SiftTermFreq are taken with the huge allocation ("invalid memory alloc request size").
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/doctext.c"

#define NDOCS		64
#define TEXT_BYTES	(64 * 1024)
#define BUDGET		(1024 * 1024)

TransactionId CheckXidAlive = InvalidTransactionId;
bool		bsysscan = false;
volatile sig_atomic_t InterruptPending = false;

static int	ndoc,
			pos,
			mark,
			nread,
			nwindow,
			nrow;
static text *body;
static char *big;

Size
sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators)
{
	return BUDGET;
}

void
tuplesort_markpos(Tuplesortstate *state)
{
	mark = pos;
}

void
tuplesort_restorepos(Tuplesortstate *state)
{
	pos = mark;
}

/* The docs by key, without terms (they come from the rows) */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward, bool copy, Datum *val, bool *isNull, Datum *abbrev)
{
	static char packed[VARHDRSZ + sizeof(uint64) + sizeof(uint32) + sizeof(uint16) + sizeof(uint32)];
	uint64		key;

	if (big)
	{
		*val = PointerGetDatum(big);
		*isNull = false;
		return true;
	}
	if (pos == ndoc)
		return false;
	key = pg_hton64(((uint64) (pos++ + 1)) << 16 | 1);
	memset(packed, 0, sizeof(packed));
	SET_VARSIZE(packed, sizeof(packed));
	memcpy(VARDATA(packed), &key, sizeof(key));
	*val = PointerGetDatum(packed);
	*isNull = false;
	return true;
}

/* The rows lie in the reverse order of the keys */
static uint64
fetch_order(IndexFetchTableData *fetch, const ItemPointerData *tid)
{
	return UINT64_MAX - ItemPointerGetBlockNumber(tid);
}

static bool
index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
	return true;
}

void
FormIndexDatum(IndexInfo *indexInfo, TupleTableSlot *slot, EState *estate, Datum *values, bool *isnull)
{
	nread++;
	values[0] = PointerGetDatum(body);
	isnull[0] = false;
}

struct varlena *
pg_detoast_datum_packed(struct varlena *datum)
{
	return datum;
}

Snapshot
GetTransactionSnapshot(void)
{
	return NULL;
}

void
PushActiveSnapshot(Snapshot snapshot)
{
}

void
PopActiveSnapshot(void)
{
}

int
sift_analyze_doc(SiftAnalyzer *a, const char *text, int len, SiftTermFreq **out, uint32 *dl)
{
	nwindow++;
	return 0;
}

bool
sift_bm25_row_terms(SiftAnalyzer *an, SiftDocKeys *keys, const Datum *values, const bool *isnull, SiftTermFreq **terms, int *nterms, uint32 *dl,
					int *len)
{
	nrow++;
	return true;
}

/* A doc whose terms come with it, more of them than a plain palloc of SiftTermFreq takes */
static void
next_huge_doc(void *arg)
{
	DocCursor  *c = arg;
	uint32		nterms = MaxAllocSize / sizeof(SiftTermFreq) + 1;
	Size		head = VARHDRSZ + sizeof(uint64) + sizeof(uint32) + sizeof(uint16);

	big = calloc(1, head + sizeof(nterms) + (Size) nterms * PACK_TERM);
	SET_VARSIZE(big, head + sizeof(nterms) + (Size) nterms * PACK_TERM);
	memcpy(big + head, &nterms, sizeof(nterms));
	SIFT_CHECK(sift_doc_next(c));
	SIFT_CHECKF(c->nterms == nterms, "a doc of %u terms of %u", c->nterms, nterms);
}

static void
check_huge_doc(void)
{
	DocCursor	c = {.terms = palloc(sizeof(SiftTermFreq)),.maxterms = 1};

	SIFT_CHECKF(pgshim_try(next_huge_doc, &c), "the terms of a doc past 1 GB: %s", pgshim_errmsg);
	free(big);
	big = NULL;
}

int
main(void)
{
	TableAmRoutine am = {.index_fetch_tuple = index_fetch_tuple};
	RelationData rel = {.rd_tableam = &am};
	SiftTableFormatOps ops = {.fetch_order = fetch_order};
	SiftTblFetchHeader fetch = {.base = {.rel = &rel},.ops = &ops};
	ExprContext econtext = {.ecxt_per_tuple_memory = CurrentMemoryContext};
	EState		estate = {.es_per_tuple_exprcontext = &econtext};
	DocText		d = {.estate = &estate,.fetch = &fetch.base,.nblocks = MaxBlockNumber,.wcx = CurrentMemoryContext,.wmax = NDOCS};
	DocCursor	c = {.terms = palloc(sizeof(SiftTermFreq)),.maxterms = 1};
	int			primed;

	body = palloc0(VARHDRSZ + TEXT_BYTES);
	SET_VARSIZE(body, VARHDRSZ + TEXT_BYTES);
	d.wkey = palloc(DOCTEXT_WINDOW * sizeof(SiftKey));
	d.wtext = palloc(DOCTEXT_WINDOW * sizeof(text *));
	ndoc = NDOCS;
	sift_doctext_prime(&d, &c);
	primed = nread;
	SIFT_CHECKF(d.nwin == NDOCS, "a window of %u docs", d.nwin);
	SIFT_CHECKF((Size) primed * TEXT_BYTES <= BUDGET / 4 + TEXT_BYTES, "the window read %d texts of %d bytes under a budget of %d bytes", primed, TEXT_BYTES,
				BUDGET / 4);
	while (d.wpos < d.nwin && sift_doc_next(&c))
	{
		ItemPointerData tid;
		SiftTermFreq *terms;
		int			nterms,
					len;
		uint32		dl;

		sift_key_to_tid(c.key, &tid);
		SIFT_CHECKF(sift_doctext_terms(&d, &tid, &terms, &nterms, &dl, &len), "doc %u of the window has no text", d.wpos);
	}
	SIFT_CHECKF(nwindow == primed && nrow == NDOCS - primed && nread == NDOCS, "%d docs from the window, %d from their rows, %d rows read of %d docs",
				nwindow, nrow, nread, NDOCS);
	check_huge_doc();
	return sift_check_summary();
}
