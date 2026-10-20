/*
 * persist.c - what an index keeps on disk (engine.c): the manifest, the event log and its replay, the orphans of an
 * interrupted drain, the segment slots and generations.
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

/* ------------------------------------------------ persistence */

void
write_manifest(SiftIdx *ix, const Manifest *m, const ManSeg *segs)
{
	char		tmp[600],
				final[600];
	int			fd;

	path_of(ix, tmp, sizeof(tmp), "MANIFEST.tmp");
	path_of(ix, final, sizeof(final), "MANIFEST");
	fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not create %s: %s", tmp, strerror(errno));
	write_all(fd, m, sizeof(Manifest), "the manifest");
	write_all(fd, segs, m->nsegs * sizeof(ManSeg), "the manifest");
	write_all(fd, ix->cfg, m->cfg_len, "the manifest");
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", tmp, strerror(errno));
	close(fd);
	if (rename(tmp, final) != 0)
		fail("sift: could not rename %s: %s", tmp, strerror(errno));
	sync_dir(ix->dir);
}

void
log_path(const SiftIdx *ix, uint32 logno, char *out, size_t n)
{
	snprintf(out, n, "%s/events-%u.log", ix->dir, logno);
}

void
append_events(SiftIdx *ix, const Ev *ev, size_t n)
{
	char		path[600];
	int			fd;

	log_path(ix, ix->man.logno, path, sizeof(path));
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not open %s: %s", path, strerror(errno));
	if (lseek(fd, ix->man.log_len, SEEK_SET) < 0 || ftruncate(fd, ix->man.log_len) != 0)
		fail("sift: could not position %s: %s", path, strerror(errno));
	write_all(fd, ev, n * sizeof(Ev), path);
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", path, strerror(errno));
	close(fd);
}

static void
replay_log(SiftIdx *ix)
{
	char		path[600];
	int			fd;
	Ev			ev[4096];
	uint64		left = ix->man.log_len;

	log_path(ix, ix->man.logno, path, sizeof(path));
	fd = open(path, O_RDONLY);
	if (fd < 0 && left > 0)
		fail("sift: event log %s is missing", path);
	while (left > 0)
	{
		ssize_t		got = read(fd, ev, Min(left, sizeof(ev)));

		if (got <= 0 || got % sizeof(Ev))
			fail("sift: event log %s is truncated", path);
		for (size_t i = 0; i < got / sizeof(Ev); i++)
			apply_event(ix, ev[i].docid, ev[i].pk, ev[i].op);
		left -= got;
	}
	if (fd >= 0)
		close(fd);
	finish_events(ix);
}

/* The log of the documents n given: an insert for each, then the delete of every key that has none left alive */
void
write_compact_log(SiftIdx *ix, uint32 logno, const int64 *ids, const int64 *pks, size_t nd, uint64 *len)
{
	char		path[600];
	int			fd;
	Ev			ev[4096];
	size_t		n = 0;

	log_path(ix, logno, path, sizeof(path));
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not create %s: %s", path, strerror(errno));
	*len = 0;
	for (int pass = 0; pass < 2; pass++)
		for (size_t i = 0; i < nd; i++)
		{
			if (pass == 0)
				ev[n++] = (Ev) {ids[i], pks[i], 'I'};
			else if (latest_get(ix, pks[i]) == 0)
				ev[n++] = (Ev) {0, pks[i], 'D'};
			else
				continue;
			if (n == lengthof(ev))
			{
				write_all(fd, ev, n * sizeof(Ev), path);
				*len += n * sizeof(Ev);
				n = 0;
			}
		}
	write_all(fd, ev, n * sizeof(Ev), path);
	*len += n * sizeof(Ev);
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", path, strerror(errno));
	close(fd);
}

static void
load_manifest(SiftIdx *ix)
{
	char		path[600];
	int			fd;
	Manifest   *m = &ix->man;

	path_of(ix, path, sizeof(path), "MANIFEST");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail("sift: index \"%s\" does not exist (%s)", ix->name, path);
	if (read(fd, m, sizeof(Manifest)) != sizeof(Manifest) || memcmp(m->magic, "SIFTMDB", 7) != 0)
		fail("sift: manifest %s is corrupted", path);
	if (memcmp(m->magic, MAN_MAGIC, 8) != 0 || m->format != MDB_FORMAT)
		fail("sift: index \"%s\" was built by an incompatible version of sift for MariaDB (manifest format %c, this build "
			 "reads %d): rebuild it with sift_rebuild", ix->name, m->magic[7], MDB_FORMAT);
	if (m->sift_format >> 16 != SIFT_FORMAT_MAJOR || (m->sift_format & 0xFFFF) > SIFT_FORMAT_MINOR)
		fail("sift: index \"%s\" has page format %u.%u, this build reads up to %d.%d: rebuild it with sift_rebuild",
			 ix->name, m->sift_format >> 16, m->sift_format & 0xFFFF, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR);
	if (m->page_layout != PAGE_LAYOUT_ID)
		fail("sift: index \"%s\" has page layout %u, this build uses %u: rebuild it with sift_rebuild", ix->name,
			 m->page_layout, (unsigned) PAGE_LAYOUT_ID);
	ix->cfg = malloc(m->cfg_len + 1);
	if (m->nsegs > MAX_SEGS ||
		read(fd, ix->segs, m->nsegs * sizeof(ManSeg)) != (ssize_t) (m->nsegs * sizeof(ManSeg)) ||
		read(fd, ix->cfg, m->cfg_len) != (ssize_t) m->cfg_len)
		fail("sift: manifest %s is corrupted", path);
	ix->cfg[m->cfg_len] = '\0';
	close(fd);
}

/* Files the manifest does not name are leftovers of an interrupted drain or merge */
static void
remove_orphans(SiftIdx *ix)
{
	DIR		   *d = opendir(ix->dir);
	struct dirent *e;
	char		keep_log[48];

	snprintf(keep_log, sizeof(keep_log), "events-%u.log", ix->man.logno);
	while (d && (e = readdir(d)) != NULL)
	{
		bool		keep = e->d_name[0] == '.' || strcmp(e->d_name, "MANIFEST") == 0 || strcmp(e->d_name, keep_log) == 0;
		char		path[600];

		for (uint32 i = 0; i < ix->man.nsegs; i++)
			keep |= strcmp(e->d_name, ix->segs[i].file) == 0;
		if (!keep)
		{
			path_of(ix, path, sizeof(path), e->d_name);
			unlink(path);
		}
	}
	if (d)
		closedir(d);
}

static void
map_segments(SiftIdx *ix)
{
	ix->bytes = 0;
	for (uint32 i = 0; i < ix->man.nsegs; i++)
	{
		char		path[600];

		path_of(ix, path, sizeof(path), ix->segs[i].file);
		sift_slot_map(ix->sr, ix->segs[i].slot, path);
		ix->bytes += (int64) ix->sr->slot_blocks[ix->segs[i].slot] * BLCKSZ;
		ix->slot_reserved[ix->segs[i].slot] = true;
	}
}

void
do_open(SiftIdx *ix)
{
	load_manifest(ix);
	remove_orphans(ix);
	init_rel(ix);
	map_segments(ix);
	replay_log(ix);
}

/* ------------------------------------------------ slots, generations */

int
slot_reserve(SiftIdx *ix)
{
	int			slot = 0;

	pthread_mutex_lock(&ix->wmu);
	while (slot < SIFT_DELTA_SLOT && (ix->sr->slot_used[slot] || ix->slot_reserved[slot]))
		slot++;
	if (slot == SIFT_DELTA_SLOT)
	{
		pthread_mutex_unlock(&ix->wmu);
		fail("sift: index \"%s\" has too many segments: merge it", ix->name);
	}
	ix->slot_reserved[slot] = true;
	pthread_mutex_unlock(&ix->wmu);
	return slot;
}

void
slot_release(SiftIdx *ix, int slot)
{
	pthread_mutex_lock(&ix->wmu);
	ix->slot_reserved[slot] = false;
	pthread_mutex_unlock(&ix->wmu);
}

uint64
gen_take(SiftIdx *ix)
{
	uint64		g;

	pthread_mutex_lock(&ix->wmu);
	g = ix->man.next_gen++;
	pthread_mutex_unlock(&ix->wmu);
	return g;
}

/* Names the finished segment file, maps it; the caller attaches the slot when it publishes */
void
rename_segment(SiftIdx *ix, SiftWriteTarget *wt, uint64 gen, ManSeg *ms, int *g, uint32 *blocks)
{
	char		final[600];

	snprintf(ms->file, sizeof(ms->file), "seg-%llu.sift", (unsigned long long) gen);
	path_of(ix, final, sizeof(final), ms->file);
	if (rename(sift_wt_path(wt), final) != 0)
		fail("sift: could not rename %s: %s", sift_wt_path(wt), strerror(errno));
	sync_dir(ix->dir);
	ms->slot = sift_wt_slot(wt);
	*g = sift_map_path(final, ms->slot, blocks);
}

void
delta_release(Delta *d)
{
	if (d == NULL || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	if (d->g >= 0)
		sift_unmap(d->g, d->blocks);
	for (size_t i = 0; i < d->nd; i++)
		free(d->text[i]);
	free(d->dead);
	free(d->dids);
	free(d->dpks);
	free(d->text);
	free(d->tlen);
	free(d);
}

/* Forgets the cached live segment (a writer published: the queue rows it was built over are in the segments) */
void
delta_drop(SiftIdx *ix)
{
	Delta	   *d;

	pthread_mutex_lock(&ix->dmu);
	d = ix->delta;
	ix->delta = NULL;
	pthread_mutex_unlock(&ix->dmu);
	delta_release(d);
}
