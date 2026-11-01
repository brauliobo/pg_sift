/*
 * col/build/publish.c - the publication of the segments a build wrote and the scan that feeds the build
 * the meta page, the progress reported to the backend
 * the scan of the table with its watch
 */
#include "postgres.h"
#include <math.h>
#ifdef SIFT_TESTING
#endif
#include "core/cmp.h"
#include "col/build/build_int.h"

typedef struct ColPublish
{
	SiftSegEntry *segs;
	int			nsegs;
} ColPublish;

static void
publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	ColPublish *p = arg;

	memcpy(dir->segs, p->segs, p->nsegs * sizeof(SiftSegEntry));
	dir->nsegs = p->nsegs;
	for (int i = 0; i < p->nsegs; i++)
	{
		meta->ndocs += p->segs[i].ndocs;
		meta->next_gen = Max(meta->next_gen, p->segs[i].gen + 1);
	}
}

static int
seg_cmp(const void *a, const void *b)
{
	const SiftSegEntry *x = a,
			   *y = b;

	if (x->sb_lo != y->sb_lo)
		return SIFT_CMP3(x->sb_lo, y->sb_lo);
	return SIFT_CMP3(x->gen, y->gen);
}

void
sift_colb_publish(Relation index, SiftSegEntry *segs, int nsegs)
{
	ColPublish	p = {segs, nsegs};

	if (nsegs == 0)
		return;
	qsort(segs, nsegs, sizeof(SiftSegEntry), seg_cmp);
	sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = &p,.extra = InvalidBuffer});
}

/* The metapage, before a SHARED target allocates a block: the participants of a parallel build and a key_cut build, which gives pages back, write through one */
void
sift_colb_init_meta(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_BULK);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
}

/* LZ4 that stores the strings in more than this share of their bytes does not shrink them (hex, ids): the column is not a choice worth a word */
#define SCAN_NO_SHRINK	0.95

/* A column that kept zstd is named when the benefit of LZ4 came within this share of its cost */
#define SCAN_CLOSE		0.75

static ColScanAcc *scan_acc;

static void
scan_add(int leaf, int kind, const SiftCcScanSum *chosen, const SiftCcScanSum *kept)
{
	if (leaf >= SIFT_COL_SCAN_LEAVES)
		return;
	SpinLockAcquire(&scan_acc->mutex);
	for (int i = 0; i < 2; i++)
	{
		SiftCcScanSum *to = &scan_acc->sum[kind][leaf][i];
		const SiftCcScanSum *from = i == 0 ? chosen : kept;

		to->n += from->n;
		to->raw += from->raw;
		to->z += from->z;
		to->l += from->l;
	}
	SpinLockRelease(&scan_acc->mutex);
}

void
sift_colb_scan_watch(ColScanAcc *acc)
{
	scan_acc = acc;
	sift_cenc_scan_hook = acc ? scan_add : NULL;
}

/* Rounded down to two digits: the sizes the compressors give vary by a few hundred bytes between library versions, and a size just under a round figure must not flip the text */
static char *
scan_bytes(double b)
{
	if (b >= 1e6)
		return psprintf("%.1f MB", floor(b / 1e5) / 10);
	return psprintf("about %.0f kB", b >= 1e4 ? floor(b / 1e4) * 10 : floor(b / 1e3));
}

/*
 * One NOTICE per CREATE INDEX for the strings the formula of the design notes decided: the columns that took LZ4, and those that kept zstd when the benefit of LZ4 came within
 * a quarter of the cost of its extra bytes. The terms are those of the sums of the leaf: instructions a MB of text costs a test with each algorithm, and B against C.
 */
void
sift_colb_scan_report(Relation index, ColScanAcc *acc)
{
	TupleDesc	desc = RelationGetDescr(index);
	StringInfoData detail;
	const char *iname = quote_identifier(RelationGetRelationName(index));
	const char *first = NULL;
	int			nlz4 = 0,
				nkept = 0;
	bool		dict = false;

	initStringInfo(&detail);
	for (int kind = 0; kind < SIFT_SCAN_KINDS; kind++)
		for (int leaf = 0; leaf < Min(desc->natts, SIFT_COL_SCAN_LEAVES); leaf++)
		{
			const SiftCcScanSum *lz4 = &acc->sum[kind][leaf][0],
					   *kept = &acc->sum[kind][leaf][1];
			SiftCcScanTerms t;
			const char *name = quote_identifier(NameStr(TupleDescAttr(desc, leaf)->attname));
			double		z = lz4->z + kept->z;

			if (lz4->n + kept->n == 0)
				continue;
			sift_cc_scan_terms(z, lz4->l + kept->l, lz4->raw + kept->raw, &t);
			if (lz4->n == 0 && (lz4->l + kept->l > SCAN_NO_SHRINK * (lz4->raw + kept->raw) || t.benefit < SCAN_CLOSE * t.cost))
				continue;
			if (lz4->n > 0)
				nlz4++;
			else
				nkept++;
			dict |= kind == SIFT_SCAN_DICT && lz4->n > 0;
			first = first ? first : name;
			appendStringInfo(&detail, "%s%s (%s): %s; a test reads %.1f M instructions a MB of text with zstd and %.1f M with lz4 (%.1f times fewer), lz4 takes %s more (%+.0f%%): benefit %.0f against cost %.0f.",
							 detail.len ? "\n" : "", name, kind == SIFT_SCAN_DICT ? "dictionary" : "long strings",
							 lz4->n == 0 ? "zstd kept" : kept->n == 0 ? "lz4" : psprintf("lz4 in %llu of %llu parts, zstd in the rest", (unsigned long long) lz4->n, (unsigned long long) (lz4->n + kept->n)),
							 t.zipb, t.lipb, t.speedup, scan_bytes(lz4->l + kept->l - z), t.growth, t.benefit, t.cost);
		}
	if (first == NULL)
		return;
	if (dict)
		appendStringInfoString(&detail, "\nLZ4 blocks of dictionaries need column format minor 29, which releases before 3.6 refuse.");
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": compression = 'auto' takes lz4 for %d and keeps zstd for %d of the %d text column%s", RelationGetRelationName(index), nlz4, nkept, nlz4 + nkept, nlz4 + nkept == 1 ? "" : "s"),
			 errdetail_internal("%s", detail.data),
			 errhint("To keep zstd for a column, run ALTER INDEX %s SET (compression = 'auto; %s=zstd') and REINDEX; 'auto; %s=lz4' takes lz4.%s", iname, first, first,
					 dict ? " To write no LZ4 blocks of dictionaries, SET sift.col_format_minor = 14 before CREATE INDEX." : "")));
}
