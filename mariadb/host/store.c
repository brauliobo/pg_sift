/*
 * store.c - the storage layer of src/core/store.h on segment files instead of an index relation (store.h).
 *
 * An index is a directory of immutable segment files, each a sequence of 8 KiB pages in the layout of src/core/page.h (block 0
 * unused). Each file is mapped on its own, at the address the kernel gives it, and the process keeps that address per mapped
 * slot, so a block number resolves to a pointer and the "buffer" of a block is its page. There is no metapage, directory, free
 * list or WAL: the directory is the manifest of engine.c.
 */
#include "store.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define SLOT_BLOCKS		((uint64) 1 << SIFT_SLOT_SHIFT)
#define SLOT_BYTES		(SLOT_BLOCKS * BLCKSZ)
#define RUN_MIN			16
#define RUN_MAX			1024
#define FIRST_BLK		1

#define corrupt(index, ...) \
	ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), \
					errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), errdetail(__VA_ARGS__)))

/* ------------------------------------------------ the mapped segments */

static uint32 next_relnumber = 1;

/*
 * The block numbers of an index (slot << SLOT_SHIFT | block of the file) are in its manifest, so they are per index; the
 * table of the mappings is shared by every index of the process: each mapped segment takes a free entry of it.
 */
static struct
{
	bool		used;
	int			local;
	char	   *base;
	size_t		len;
}			gmap[SIFT_NGSLOTS];

static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;

static inline Page
page_of(const SiftRel *sr, BlockNumber blk)
{
	return gmap[sr->gslot[blk >> SIFT_SLOT_SHIFT]].base + (size_t) (blk & (SLOT_BLOCKS - 1)) * BLCKSZ;
}

Buffer
ReadBuffer(Relation rel, BlockNumber blkno)
{
	return page_of(rel->store, blkno);
}

void
sq_unpin(Buffer buf)
{
}

BlockNumber
BufferGetBlockNumber(Buffer buf)
{
	for (int g = 0; g < SIFT_NGSLOTS; g++)
		if (gmap[g].used && buf >= gmap[g].base && buf < gmap[g].base + gmap[g].len)
			return (BlockNumber) gmap[g].local << SIFT_SLOT_SHIFT | (size_t) (buf - gmap[g].base) / BLCKSZ;
	elog(ERROR, "sift: buffer outside the mapped segments");
}

/* Pages are edited in the private mapping, there is no log */
GenericXLogState *
GenericXLogStart(Relation rel)
{
	return NULL;
}

Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buf, int flags)
{
	return buf;
}

void
GenericXLogFinish(GenericXLogState *state)
{
}

void
GenericXLogAbort(GenericXLogState *state)
{
}

/* MEM[MEM-PORT-08]: MariaDB store mmap arena + prefault (1024*4GiB = 4TiB VmSize + MADV_POPULATE_READ) */
/* STOR[STOR-PORT-08]: single-stream contiguous file alloc + drop unconditional MADV_POPULATE_READ on open */
static int
gslot_take(int local, char *base, size_t len)
{
	int			g = 0;

	pthread_mutex_lock(&gmu);
	while (g < SIFT_NGSLOTS && gmap[g].used)
		g++;
	if (g < SIFT_NGSLOTS)
		gmap[g] = (typeof(gmap[0])) {.used = true,.local = local,.base = base,.len = len};
	pthread_mutex_unlock(&gmu);
	return g;
}

static void
gslot_free(int g)
{
	pthread_mutex_lock(&gmu);
	gmap[g].used = false;
	pthread_mutex_unlock(&gmu);
}

int
sift_map_fd(int fd, int local, uint32 *blocks)
{
	off_t		size = lseek(fd, 0, SEEK_END);
	char	   *at = size < BLCKSZ || size > (off_t) SLOT_BYTES ? MAP_FAILED :
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	int			g;

	if (at == MAP_FAILED)
		elog(ERROR, "sift: could not map a segment file");
	g = gslot_take(local, at, size);
	if (g == SIFT_NGSLOTS)
	{
		munmap(at, size);
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: too many mapped segments in this server")));
	}
	*blocks = size / BLCKSZ;
	return g;
}

int
sift_map_path(const char *path, int local, uint32 *blocks)
{
	int			fd = open(path, O_RDONLY);
	int			g;

	if (fd < 0)
		elog(ERROR, "sift: could not open segment file \"%s\": %m", path);
	g = sift_map_fd(fd, local, blocks);
	close(fd);
	return g;
}

void
sift_unmap(int g, uint32 blocks)
{
	munmap(gmap[g].base, (size_t) blocks * BLCKSZ);
	gslot_free(g);
}

/* ------------------------------------------------ relation and slots */

Relation
sift_rel_create(const char *name, const char *dir, uint32 dpp, SiftRel **out)
{
	SiftRel    *sr = calloc(1, sizeof(SiftRel));

	sr->rel.store = sr;
	snprintf(sr->rel.relname, sizeof(sr->rel.relname), "%s", name);
	sr->rel.rd_locator.relNumber = __atomic_fetch_add(&next_relnumber, 1, __ATOMIC_RELAXED);
	snprintf(sr->dir, sizeof(sr->dir), "%s", dir);
	sr->dpp = dpp;
	sr->opts = (SiftOptions) {.k1 = SIFT_DEFAULT_K1,.b = SIFT_DEFAULT_B,.case_folding = true,.accent_folding = true,
	.write_buffer_limit = SIFT_DEFAULT_WRITE_BUFFER_LIMIT,.positions = true};
	memset(sr->gslot, -1, sizeof(sr->gslot));
	*out = sr;
	return &sr->rel;
}

void
sift_rel_destroy(Relation rel)
{
	SiftRel    *sr = rel->store;

	sift_host_segcache_drop_rel(sr->rel.rd_locator.relNumber);
	for (int i = 0; i < SIFT_NSLOTS; i++)
		if (sr->slot_blocks[i])
			sift_slot_unmap(sr, i);
	sift_rel_view_free(rel);
}

Relation
sift_rel_view(const SiftRel *base, SiftRel **out)
{
	SiftRel    *sr = malloc(sizeof(SiftRel));

	memcpy(sr, base, sizeof(SiftRel));
	sr->rel.store = sr;
	memset(sr->dead, 0, sizeof(sr->dead));
	memset(sr->ndead, 0, sizeof(sr->ndead));
	*out = sr;
	return &sr->rel;
}

void
sift_rel_view_free(Relation rel)
{
	free(rel->store);
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

/* ------------------------------------------------ pages, directory */

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

SiftDirSnapshot *
sift_dir_alloc(int n)
{
	return calloc(1, offsetof(SiftDirSnapshot, segs) + n * sizeof(SiftSegEntry));
}

SiftDirSnapshot *
sift_dir_copy(const SiftDirSnapshot *snap)
{
	SiftDirSnapshot *copy = sift_dir_alloc(snap->nsegs);

	memcpy(copy, snap, sift_dir_bytes(snap));
	return copy;
}

SiftDirSnapshot *
sift_dir_read(Relation index)
{
	return sift_dir_alloc(0);
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
		elog(ERROR, "could not create \"%s\": %m", wt->path);
	return wt;
}

static void
wt_put(SiftWriteTarget *wt, uint32 blk, const void *page)
{
	if (pwrite(wt->fd, page, BLCKSZ, (off_t) blk * BLCKSZ) != BLCKSZ)
		elog(ERROR, "could not write \"%s\": %m", wt->path);
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
		elog(ERROR, "could not sync \"%s\": %m", wt->path);
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

/* The pages of a stream are taken from the file in runs of 16 to 1024 blocks, so that streams written side by side stay contiguous */
struct SiftStreamWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	uint64		pos;
	SiftExtentList ext;
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
	sift_extlist_init(&sw->ext);
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
	if (sw->run_next == sw->run_end)
	{
		sw->run_next = sw->wt->next;
		sw->run_end = sw->run_next + sw->grow;
		sw->wt->next = sw->run_end;
		sw->grow = Min(sw->grow * 2, RUN_MAX);
	}
	sw->blk = sw->run_next++;
	sift_extlist_add(&sw->ext, wt_base(sw->wt) + sw->blk);
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

SiftExtent *
sift_sw_finish(SiftStreamWriter *sw, int *nextents)
{
	sw_flush(sw);
	sw->wt->next = sw->run_next;	/* the blocks of the run that were never filled go back */
	sw->run_end = sw->run_next;
	*nextents = sw->ext.n;
	return sw->ext.ext;
}

/* ------------------------------------------------ stream reader */

void
sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents, int nextents, uint64 gen)
{
	memset(sr, 0, sizeof(SiftStreamReader));
	sr->index = index;
	sr->extents = extents;
	sr->nextents = nextents;
	sr->gen = gen;
	sr->buf = InvalidBuffer;
	sr->pagestart = palloc((nextents + 1) * sizeof(uint32));
	sr->npages = sift_extents_pagestart(extents, nextents, sr->pagestart);
}

static inline const char *
sr_page(SiftStreamReader *sr, uint32 lpage)
{
	Page		page = page_of(sr->index->store, sift_extents_block(sr->extents, sr->pagestart, sr->nextents, lpage, &sr->cur));

	if (SiftPageGetOpaque(page)->gen != sr->gen)
		sift_store_check_page(sr->index, page, SIFT_PT_DATA, sr->gen);
	return page;
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

/* no ring to read through */
void
sift_sr_set_strategy(SiftStreamReader *sr, struct BufferAccessStrategyData *bas)
{
}

#define PREFETCH_MIN_RUN	(4 * BLCKSZ)

static void
prefetch_run(char *at, size_t len)
{
#ifdef MADV_WILLNEED
	if (len >= PREFETCH_MIN_RUN)
		madvise(at, len, MADV_WILLNEED);
#endif
}

/* The pages are mapped; the runs that lie side by side are asked of the kernel in one advice each (no page is read here: 0) */
uint64
sift_sr_prefetch_multi(const SiftMultiRange *ranges, int n)
{
	char	   *run = NULL;
	size_t		len = 0;

	for (int i = 0; i < n; i++)
	{
		SiftStreamReader *sr = (SiftStreamReader *) ranges[i].sr;
		uint64		size = (uint64) sr->npages * SIFT_PAGE_PAYLOAD;

		if (ranges[i].len == 0 || ranges[i].off >= size)
			continue;
		for (uint32 p = ranges[i].off / SIFT_PAGE_PAYLOAD, e = (Min(ranges[i].off + ranges[i].len, size) - 1) / SIFT_PAGE_PAYLOAD; p <= e; p++)
		{
			char	   *page = (char *) sr_page(sr, p);

			if (run != NULL && page == run + len)
				len += BLCKSZ;
			else
			{
				if (run != NULL)
					prefetch_run(run, len);
				run = page;
				len = BLCKSZ;
			}
		}
	}
	if (run != NULL)
		prefetch_run(run, len);
	return 0;
}

void
sift_sr_unlock(SiftStreamReader *sr)
{
}

void
sift_sr_release(SiftStreamReader *sr)
{
}
