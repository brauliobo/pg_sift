/*
 * store.c - the storage layer of src/sift_store.h on segment files instead of an index relation.
 *
 * An index is a directory of immutable segment files, each a sequence of 8 KiB pages in the layout of
 * src/sift_page.h (block 0 unused). The files are mapped (MAP_PRIVATE) into one reserved address range, a segment per
 * slot of 4 GiB, so the engine's block numbers (slot << 19 | block of the file) are plain pointers: the "buffer" of
 * block n is page n of the range (BufferBlocks) and nothing is ever pinned or locked. There is no metapage, directory,
 * free list or WAL: the directory is the manifest of engine.c. This file also stubs what src/sift_eval.c reaches for
 * the PostgreSQL scan machinery: mutable segments (none exist), visibility, parallel and filter pushdown.
 */
#include "postgres.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "access/generic_xlog.h"
#include "utils/memutils.h"

#include "sift_analyze.h"
#include "sift_cpu.h"
#include "sift_filter.h"
#include "sift_kernels.h"
#include "sift_mutable.h"
#include "sift_parallel.h"
#include "sift_visibility.h"
#include "shim.h"

#define SLOT_BLOCKS		((uint64) 1 << SIFT_SLOT_SHIFT)
#define RUN_MIN			16
#define RUN_MAX			1024
#define FIRST_BLK		1

int			sift_max_expansions = 512;

/* ------------------------------------------------ pages */

void
PageInit(Page page, Size pageSize, Size specialSize)
{
	PageHeader	p = (PageHeader) page;

	specialSize = MAXALIGN(specialSize);
	memset(page, 0, pageSize);
	p->pd_lower = SizeOfPageHeaderData;
	p->pd_upper = pageSize - specialSize;
	p->pd_special = pageSize - specialSize;
	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);
}

void
sift_page_init(Page page, SiftPageType type, uint64 gen)
{
	PageInit(page, BLCKSZ, sizeof(SiftPageOpaqueData));
	SiftPageGetOpaque(page)->type = type;
	SiftPageGetOpaque(page)->gen = gen;
}

#define corrupt(index, ...) \
	ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), \
					errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), errdetail(__VA_ARGS__)))

void
sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen)
{
	Page		page = BufferGetPage(buf);
	PageHeader	ph = (PageHeader) page;
	SiftPageOpaqueData found = *SiftPageGetOpaque(page);

	if (ph->pd_special != BLCKSZ - MAXALIGN(sizeof(SiftPageOpaqueData)) || ph->pd_lower > ph->pd_upper)
		corrupt(index, "Block %u has an invalid page layout.", BufferGetBlockNumber(buf));
	if (found.type != type || found.gen != gen)
		corrupt(index, "Block %u: expected page type %d generation %llu, found type %d generation %llu.",
				BufferGetBlockNumber(buf), type, (unsigned long long) gen, found.type, (unsigned long long) found.gen);
}

/* ------------------------------------------------ relation and mapped files */

static uint32 next_relnumber = 1;

void
sift_shim_init(void)
{
	size_t		len = (size_t) SIFT_NGSLOTS * SLOT_BLOCKS * BLCKSZ;

	if (BufferBlocks)
		return;
	BufferBlocks = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (BufferBlocks == MAP_FAILED)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("could not reserve %zu bytes of address space", len)));
}

Relation
sift_rel_create(const char *name, const char *dir, uint32 dpp, SiftRel **out)
{
	Relation	rel = calloc(1, sizeof(RelationData));
	SiftRel    *sr = calloc(1, sizeof(SiftRel));

	sift_shim_init();
	rel->rd_rel = calloc(1, sizeof(FormData_pg_class));
	snprintf(NameStr(rel->rd_rel->relname), NAMEDATALEN, "%s", name);
	rel->rd_locator.relNumber = __atomic_fetch_add(&next_relnumber, 1, __ATOMIC_RELAXED);
	rel->rd_amcache = sr;
	snprintf(sr->name, sizeof(sr->name), "%s", name);
	snprintf(sr->dir, sizeof(sr->dir), "%s", dir);
	sr->dpp = dpp;
	sr->opts = (SiftOptions) {.k1 = SIFT_DEFAULT_K1,.b = SIFT_DEFAULT_B,.case_folding = true,.accent_folding = true,
	.mutable_limit = SIFT_DEFAULT_MUTABLE_LIMIT,.positions = true};
	memset(sr->gslot, -1, sizeof(sr->gslot));
	*out = sr;
	return rel;
}

void
sift_rel_destroy(Relation rel)
{
	SiftRel    *sr = rel->rd_amcache;

	for (int i = 0; i < SIFT_NSLOTS; i++)
		if (sr->slot_blocks[i])
			sift_slot_unmap(sr, i);
	sift_rel_view_free(rel);
}

Relation
sift_rel_view(const SiftRel *base, SiftRel **out)
{
	Relation	rel = calloc(1, sizeof(RelationData));
	SiftRel    *sr = malloc(sizeof(SiftRel));

	memcpy(sr, base, sizeof(SiftRel));
	rel->rd_rel = calloc(1, sizeof(FormData_pg_class));
	snprintf(NameStr(rel->rd_rel->relname), NAMEDATALEN, "%s", base->name);
	rel->rd_locator.relNumber = __atomic_fetch_add(&next_relnumber, 1, __ATOMIC_RELAXED);
	rel->rd_amcache = sr;
	sr->is_dead = NULL;
	sr->any_dead = false;
	*out = sr;
	return rel;
}

void
sift_rel_view_free(Relation rel)
{
	free(rel->rd_amcache);
	free(rel->rd_rel);
	free(rel);
}

/*
 * The block numbers of an index (slot << SLOT_SHIFT | block of the file) are in its manifest, so they are per index; the
 * range of the buffers is shared by every index of the process: each mapped slot takes a free slot of the range.
 */
static struct
{
	bool		used;
	int			local;
}			gowner[SIFT_NGSLOTS];

static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;

Buffer
sift_buf_of(Relation rel, BlockNumber blk)
{
	SiftRel    *sr = rel->rd_amcache;

	return (Buffer) (((BlockNumber) sr->gslot[blk >> SIFT_SLOT_SHIFT] << SIFT_SLOT_SHIFT | (blk & (SLOT_BLOCKS - 1))) + 1);
}

BlockNumber
sift_buf_block(Buffer buf)
{
	BlockNumber g = buf - 1;

	return (BlockNumber) gowner[g >> SIFT_SLOT_SHIFT].local << SIFT_SLOT_SHIFT | (g & (SLOT_BLOCKS - 1));
}

static int
gslot_take(int local)
{
	int			g = 0;

	pthread_mutex_lock(&gmu);
	while (g < SIFT_NGSLOTS && gowner[g].used)
		g++;
	if (g < SIFT_NGSLOTS)
	{
		gowner[g].used = true;
		gowner[g].local = local;
	}
	pthread_mutex_unlock(&gmu);
	if (g == SIFT_NGSLOTS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: too many mapped segments in this server")));
	return g;
}

int
sift_map_fd(int fd, int local, uint32 *blocks)
{
	off_t		size = lseek(fd, 0, SEEK_END);
	int			g = gslot_take(local);
	char	   *at = BufferBlocks + (size_t) g * SLOT_BLOCKS * BLCKSZ;

	if (size < BLCKSZ || size > (off_t) (SLOT_BLOCKS * BLCKSZ) ||
		mmap(at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED)
	{
		pthread_mutex_lock(&gmu);
		gowner[g].used = false;
		pthread_mutex_unlock(&gmu);
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("sift: could not map a segment file")));
	}
	*blocks = size / BLCKSZ;
	madvise(at, size, MADV_POPULATE_READ);	/* prefault from the page cache; MAP_POPULATE would copy every page of a private mapping */
	return g;
}

int
sift_map_path(const char *path, int local, uint32 *blocks)
{
	int			fd = open(path, O_RDONLY);
	int			g;

	if (fd < 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("sift: could not open segment file \"%s\": %m", path)));
	g = sift_map_fd(fd, local, blocks);
	close(fd);
	return g;
}

void
sift_unmap(int g, uint32 blocks)
{
	char	   *at = BufferBlocks + (size_t) g * SLOT_BLOCKS * BLCKSZ;

	mmap(at, (size_t) blocks * BLCKSZ, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	pthread_mutex_lock(&gmu);
	gowner[g].used = false;
	pthread_mutex_unlock(&gmu);
}

void
sift_slot_attach(SiftRel *sr, int slot, int g, uint32 blocks)
{
	sr->gslot[slot] = g;
	sr->slot_used[slot] = true;
	sr->slot_blocks[slot] = blocks;
}

void
sift_slot_detach(SiftRel *sr, int slot)
{
	sr->slot_used[slot] = false;
	sr->slot_blocks[slot] = 0;
	sr->gslot[slot] = -1;
}

void
sift_slot_map(SiftRel *sr, int slot, const char *path)
{
	uint32		blocks;
	int			g = sift_map_path(path, slot, &blocks);

	sift_slot_attach(sr, slot, g, blocks);
}

void
sift_slot_unmap(SiftRel *sr, int slot)
{
	int			g = sr->gslot[slot];
	uint32		blocks = sr->slot_blocks[slot];

	sift_slot_detach(sr, slot);
	sift_unmap(g, blocks);
}

/* ------------------------------------------------ options, analyzer config, directory */

const SiftOptions *
sift_index_options(Relation index)
{
	return &((SiftRel *) index->rd_amcache)->opts;
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	return opts->stemmer ? (const char *) opts + opts->stemmer : "";
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	return opts->fields ? (const char *) opts + opts->fields : "";
}

void
sift_dir_read(Relation index, SiftDirSnapshot *snap)
{
	*snap = ((SiftRel *) index->rd_amcache)->snap;
}

/* VACUUM's dirty flag of a segment: liveness here is the engine's, merges work on private copies of the pages */
void
sift_dir_update(Relation index, const SiftDirChange *chg)
{
	SiftMetaPageData meta = {0};
	SiftDirectoryData dir = {0};

	if (chg->fn)
		chg->fn(&meta, &dir, NULL, chg->arg);
}

/* ------------------------------------------------ write target: a new segment file */

struct SiftWriteTarget
{
	int			slot;
	int			fd;
	bool		mem;			/* an anonymous memory file, which the caller maps and closes */
	uint32		next;			/* next free block of the file */
	char		path[600];
};

SiftWriteTarget *
sift_wt_begin_at(const SiftRel *sr, int slot, bool mem)
{
	SiftWriteTarget *wt = palloc0(sizeof(SiftWriteTarget));

	wt->slot = slot;
	wt->mem = mem;
	wt->next = FIRST_BLK;
	snprintf(wt->path, sizeof(wt->path), "%s/new-%d.tmp", sr->dir, slot);
	wt->fd = mem ? memfd_create("sift-live", 0) : open(wt->path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (wt->fd < 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not create \"%s\": %m", wt->path)));
	return wt;
}

static void
wt_put(SiftWriteTarget *wt, uint32 blk, const void *page)
{
	if (pwrite(wt->fd, page, BLCKSZ, (off_t) blk * BLCKSZ) != BLCKSZ)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not write \"%s\": %m", wt->path)));
}

static inline BlockNumber
wt_base(const SiftWriteTarget *wt)
{
	return (BlockNumber) wt->slot << SIFT_SLOT_SHIFT;
}

void
sift_wt_finish(SiftWriteTarget *wt)
{
	if (ftruncate(wt->fd, (off_t) wt->next * BLCKSZ) != 0 || (!wt->mem && fsync(wt->fd) != 0))
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not sync \"%s\": %m", wt->path)));
	if (wt->mem)
		return;
	close(wt->fd);
	wt->fd = -1;
}

int
sift_wt_slot(const SiftWriteTarget *wt)
{
	return wt->slot;
}

const char *
sift_wt_path(const SiftWriteTarget *wt)
{
	return wt->path;
}

int
sift_wt_fd(const SiftWriteTarget *wt)
{
	return wt->fd;
}

void
sift_wt_abort(SiftWriteTarget *wt)
{
	if (wt->fd >= 0)
		close(wt->fd);
	if (!wt->mem)
		unlink(wt->path);
}

BlockNumber
sift_wt_write_page(SiftWriteTarget *wt, Page page)
{
	wt_put(wt, wt->next, page);
	return wt_base(wt) + wt->next++;
}

BlockNumber
sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks)
{
	BlockNumber first = wt_base(wt) + wt->next;

	wt->next += nblocks;
	return first;
}

void
sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page)
{
	wt_put(wt, blkno - wt_base(wt), page);
}

/* ------------------------------------------------ stream writer */

struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		pos;
	SiftExtent *ext;
	int			next,
				maxext;
	uint32		grow,
				run_next,
				run_end;		/* blocks of the file taken for the run in progress */
	uint32		blk;
	uint32		used;
	bool		have_page;
	PGIOAlignedBlock page;
};

SiftStreamWriter *
sift_sw_begin(SiftWriteTarget *wt, uint64 gen)
{
	SiftStreamWriter *sw = palloc0(sizeof(SiftStreamWriter));

	sw->wt = wt;
	sw->gen = gen;
	sw->grow = RUN_MIN;
	sw->maxext = 8;
	sw->ext = palloc(sw->maxext * sizeof(SiftExtent));
	return sw;
}

uint64
sift_sw_tell(const SiftStreamWriter *sw)
{
	return sw->pos;
}

static void
sw_flush(SiftStreamWriter *sw)
{
	if (!sw->have_page)
		return;
	SiftPageSetUsed(sw->page.data, sw->used);
	wt_put(sw->wt, sw->blk, sw->page.data);
	sw->have_page = false;
}

static void
sw_start_page(SiftStreamWriter *sw)
{
	SiftExtent *last = sw->next > 0 ? &sw->ext[sw->next - 1] : NULL;

	if (sw->run_next == sw->run_end)
	{
		sw->run_next = sw->wt->next;
		sw->run_end = sw->run_next + sw->grow;
		sw->wt->next = sw->run_end;
		sw->grow = Min(sw->grow * 2, RUN_MAX);
	}
	sw->blk = sw->run_next++;
	if (last && last->start + last->nblocks == wt_base(sw->wt) + sw->blk)
		last->nblocks++;
	else
	{
		if (sw->next == SIFT_MAX_EXTENTS)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift segment stream needs too many extents")));
		if (sw->next == sw->maxext)
		{
			sw->maxext *= 2;
			sw->ext = repalloc(sw->ext, sw->maxext * sizeof(SiftExtent));
		}
		sw->ext[sw->next++] = (SiftExtent) {wt_base(sw->wt) + sw->blk, 1};
	}
	sift_page_init(sw->page.data, SIFT_PT_DATA, sw->gen);
	sw->have_page = true;
	sw->used = 0;
}

void
sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0)
	{
		size_t		n;

		if (!sw->have_page || sw->used == SIFT_PAGE_PAYLOAD)
		{
			sw_flush(sw);
			sw_start_page(sw);
		}
		n = Min(len, SIFT_PAGE_PAYLOAD - sw->used);
		memcpy(SiftPageGetData(sw->page.data) + sw->used, p, n);
		sw->used += n;
		sw->pos += n;
		p += n;
		len -= n;
	}
}

void
sift_sw_align(SiftStreamWriter *sw, size_t align)
{
	static const char zeros[SIFT_SIMD_ALIGN];

	if (align == 0 || (align & (align - 1)) != 0 || align > SIFT_SIMD_ALIGN)
		elog(ERROR, "sift: invalid stream alignment %zu", align);
	for (;;)
	{
		size_t		room = sw->have_page ? SIFT_PAGE_PAYLOAD - sw->used : 0;
		size_t		pad = (size_t) -(SIFT_PAGE_DATA_OFFSET + (room ? sw->used : 0)) & (align - 1);

		/* pad == room would push the next byte to the unaligned start of a page */
		if (room == 0 || pad < room)
		{
			sift_sw_write(sw, zeros, pad);
			return;
		}
		sift_sw_write(sw, zeros, room);
	}
}

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	sw_flush(sw);
	sw->wt->next = sw->run_next;	/* the blocks of the run that were never filled go back */
	sw->run_end = sw->run_next;
	*nextents = sw->next;
	return sw->ext;
}

/* ------------------------------------------------ stream reader */

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents, uint64 gen)
{
	uint64		total = 0;

	memset(sr, 0, sizeof(SiftStreamReader));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	for (int i = 0; i < nextents; i++)
	{
		if (extents[i].nblocks == 0)
			corrupt(index, "Segment %llu has an empty extent.", (unsigned long long) gen);
		sr->pagestart[i] = total;
		total += extents[i].nblocks;
	}
	sr->pagestart[nextents] = sr->npages = total;
}

static const char *
sr_page(SiftStreamReader *sr, uint32 lpage)
{
	int			lo = sr->cur;
	BlockNumber blk;
	Buffer		buf;
	Page		page;

	if (lpage < sr->pagestart[lo] || lpage >= sr->pagestart[lo + 1])
	{
		int			hi = sr->nextents - 1;

		lo = 0;
		while (lo < hi)
		{
			int			mid = (lo + hi + 1) / 2;

			if (sr->pagestart[mid] <= lpage)
				lo = mid;
			else
				hi = mid - 1;
		}
		sr->cur = lo;
	}
	blk = sr->extents[lo].start + (lpage - sr->pagestart[lo]);
	buf = sift_buf_of(sr->index, blk);
	page = BufferGetPage(buf);
	if (SiftPageGetOpaque(page)->gen != sr->gen)
		sift_store_check_page(sr->index, buf, SIFT_PT_DATA, sr->gen);
	return (const char *) page;
}

const void *
sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch)
{
	uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;
	uint32		lpage = off / SIFT_PAGE_PAYLOAD;
	size_t		in = off % SIFT_PAGE_PAYLOAD;
	char	   *dst = scratch;

	if (len == 0)
		return scratch;
	if (off > size || len > size - off)
		corrupt(sr->index, "Read of %zu bytes at offset %llu is past the end of segment %llu.",
				len, (unsigned long long) off, (unsigned long long) sr->gen);
	if (in + len <= SIFT_PAGE_PAYLOAD)
		return SiftPageGetData(sr_page(sr, lpage)) + in;
	while (len > 0)
	{
		size_t		n = Min(len, SIFT_PAGE_PAYLOAD - in);

		memcpy(dst, SiftPageGetData(sr_page(sr, lpage++)) + in, n);
		dst += n;
		len -= n;
		in = 0;
	}
	return scratch;
}

void
sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len)
{
}

void
sift_sr_release(SiftStreamReader *sr)
{
}

/* ------------------------------------------------ visibility: the engine's dead documents */

void
sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask)
{
	memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	SiftRel    *sr = v->heap->rd_amcache;
	int			n = 0;

	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
	{
		for (uint64 m = offmask[w]; sr->any_dead && m; m &= m - 1)
		{
			int			off = w * 64 + __builtin_ctzll(m) + 1;

			if (sr->is_dead(sr->dead_ctx, (int64) blk * sr->dpp + off - 1))
				offmask[w] &= ~(m & -m);
		}
		n += __builtin_popcountll(offmask[w]);
	}
	return n;
}

/* ------------------------------------------------ mutable segments, parallel scans: none here */

#define NOT_HERE(what)	elog(ERROR, "sift: " what " is not available in the MariaDB build")

bool
sift_mut_bloom_has(const uint64 *bloom, uint32 hash)
{
	return false;
}

SiftMutReader *
sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftMutPageFilter filter, void *filter_arg)
{
	NOT_HERE("a mutable segment");
	return NULL;
}

bool
sift_mut_next(SiftMutReader *r, SiftMutDoc *doc)
{
	return false;
}

void
sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_mut_close(SiftMutReader *r)
{
}

int64
sift_par_units_claimed(const SiftPar *par, int64 nunit)
{
	NOT_HERE("a parallel scan");
	return 0;
}

float4
sift_par_theta(const SiftPar *par)
{
	return 0;
}

void
sift_par_raise_theta(SiftPar *par, float4 theta)
{
}

void
sift_par_publish(SiftPar *par, const SiftParHit *hits, int64 n)
{
}

bool
sift_par_next_units(SiftPar *par, int64 nunit, int64 *lo, int64 *hi)
{
	return false;
}

bool
sift_par_finish(SiftPar *par)
{
	return false;
}

SiftParHit *
sift_par_collect(SiftPar *par, int64 *n)
{
	return NULL;
}

bool
sift_par_claim_mutable(SiftPar *par)
{
	return false;
}

void
sift_par_check_units(SiftPar *par, int64 nunit, uint64 sig)
{
}

void
sift_par_attach(SiftPar *par)
{
}

/* ------------------------------------------------ filter masks of a list of documents */

static SiftFilterSB *
open_sb(SiftFilter *f, uint32 sb)
{
	SiftFilterSB *fs;

	if (f->nsb > 0 && f->sb[f->nsb - 1].sb == sb)
		return &f->sb[f->nsb - 1];
	if (f->nsb == f->capsb)
	{
		f->capsb = Max(64, 2 * f->capsb);
		f->sb = f->sb ? repalloc_huge(f->sb, f->capsb * sizeof(SiftFilterSB)) :
			MemoryContextAllocHuge(f->mcxt, f->capsb * sizeof(SiftFilterSB));
	}
	fs = &f->sb[f->nsb++];
	memset(fs, 0, sizeof(SiftFilterSB));
	fs->sb = sb;
	fs->row = f->nrow;
	return fs;
}

static uint64 *
new_row(SiftFilter *f)
{
	if (f->nrow == f->caprow)
	{
		f->caprow = Max(64, 2 * f->caprow);
		f->row = f->row ? repalloc_huge(f->row, f->caprow * sizeof(f->row[0])) :
			MemoryContextAllocHuge(f->mcxt, f->caprow * sizeof(f->row[0]));
	}
	memset(f->row[f->nrow], 0, sizeof(f->row[0]));
	return f->row[f->nrow++];
}

/* docids ascending: the pages of a superblock appear in order, each page appends one row of offset masks */
SiftFilter *
sift_filter_from_docids(const SiftRel *sr, const int64 *docids, size_t n, MemoryContext mcxt)
{
	SiftFilter *f = MemoryContextAllocZero(mcxt, sizeof(SiftFilter));
	uint64	   *row = NULL;
	BlockNumber last = 0;

	f->mcxt = mcxt;
	for (size_t i = 0; i < n; i++)
	{
		SiftKey		key = sift_docid_key(sr, docids[i]);
		BlockNumber blk = sift_key_block(key);
		int			off = (key & 0xFFFF) - 1;

		if (row == NULL || blk != last)
		{
			int			page = blk & (SIFT_SB_PAGES - 1);

			open_sb(f, sift_block_sb(blk))->pages[page / 64] |= UINT64CONST(1) << (page & 63);
			row = new_row(f);
			f->exact_pages++;
			last = blk;
		}
		row[off / 64] |= UINT64CONST(1) << (off & 63);
		f->tuples++;
	}
	return f;
}

const SiftFilterSB *
sift_filter_sb(const SiftFilter *f, uint32 sb)
{
	int64		lo = 0,
				hi = f->nsb - 1;

	while (lo <= hi)
	{
		int64		mid = lo + (hi - lo) / 2;

		if (f->sb[mid].sb == sb)
			return &f->sb[mid];
		if (f->sb[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* Exact pages of the superblock before page */
static int
exact_before(const SiftFilterSB *fs, int page)
{
	int			n = 0;

	for (int wd = 0; wd < page / 64; wd++)
		n += __builtin_popcountll(fs->pages[wd] & ~fs->lossy[wd]);
	return n + __builtin_popcountll(fs->pages[page / 64] & ~fs->lossy[page / 64] & ((UINT64CONST(1) << (page & 63)) - 1));
}

bool
sift_filter_has(const SiftFilter *f, SiftKey key)
{
	const SiftFilterSB *fs = sift_filter_sb(f, sift_block_sb(sift_key_block(key)));
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;

	if (fs == NULL || !(fs->pages[page / 64] >> (page & 63) & 1))
		return false;
	if (fs->lossy[page / 64] >> (page & 63) & 1)
		return true;
	return f->row[fs->row + exact_before(fs, page)][off / 64] >> (off & 63) & 1;
}

void
sift_filter_block(const SiftFilter *f, const SiftFilterSB *fs, int b, uint64 (*out)[SIFT_OFFMASK_WORDS])
{
	uint64		pm = fs->pages[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff,
				lm = fs->lossy[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff;
	int64		idx = fs->row + exact_before(fs, b * SIFT_BLK_PAGES);

	memset(out, 0, SIFT_BLK_PAGES * sizeof(out[0]));
	for (; pm; pm &= pm - 1)
	{
		int			pg = __builtin_ctzll(pm);

		if (lm >> pg & 1)
			memset(out[pg], 0xff, sizeof(out[0]));
		else
			memcpy(out[pg], f->row[idx++], sizeof(out[0]));
	}
}

/* ------------------------------------------------ CPU dispatch (the detection of src/sift_cpu.c without the GUC) */

SiftCpu		sift_cpu;

static SiftDispatchRebind rebinds[8];
static int	nrebinds;

void
sift_cpu_register(SiftDispatchRebind fn)
{
	rebinds[nrebinds++] = fn;
	fn(&sift_cpu);
}

void
sift_cpu_init(void)
{
	uint32		f = 0;
	const uint32 avx2 = SIFT_CPU_AVX2 | SIFT_CPU_BMI2 | SIFT_CPU_POPCNT;
	const uint32 avx512 = avx2 | SIFT_CPU_AVX512 | SIFT_CPU_AVX512_VPOPCNT | SIFT_CPU_AVX512_VBMI2;

	__builtin_cpu_init();
	f |= __builtin_cpu_supports("popcnt") ? SIFT_CPU_POPCNT : 0;
	f |= __builtin_cpu_supports("bmi2") ? SIFT_CPU_BMI2 | SIFT_CPU_BMI2_FAST : 0;
	f |= __builtin_cpu_supports("avx2") ? SIFT_CPU_AVX2 : 0;
	f |= __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") ? SIFT_CPU_AVX512 : 0;
	f |= __builtin_cpu_supports("avx512vpopcntdq") ? SIFT_CPU_AVX512_VPOPCNT : 0;
	f |= __builtin_cpu_supports("avx512vbmi2") ? SIFT_CPU_AVX512_VBMI2 : 0;
	sift_cpu.flags = f;
	sift_cpu.detected = (f & avx512) == avx512 ? SIFT_SIMD_AVX512 : (f & avx2) == avx2 ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
	sift_cpu.active = sift_cpu.detected;
}
