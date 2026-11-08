/*
 * col/scan/view.c - the directory a source read, for the other participants of a parallel scan
 * The items of a parallel scan are ranges of superblocks and mean what the copy of the directory says, so the participants open their sources on one copy.
 */
#include "postgres.h"
#include "col/scan_int.h"

/* the bytes of a directory copy: the segments it lists, not the room for the most the directory holds */
static Size
dir_bytes(const SiftDirSnapshot *dir)
{
	return offsetof(SiftDirSnapshot, segs) + dir->nsegs * sizeof(SiftSegEntry);
}

/* The directory of the source: the copy another source exported (view), else the directory now */
void
scan_dir_load(SiftColSource *src, const void *view)
{
	if (view != NULL)
		memcpy(&src->dir, view, dir_bytes(view));
	else
		sift_dir_read(src->index, &src->dir);
}

Size
sift_col_view_size(const SiftColSource *src)
{
	return dir_bytes(&src->dir);
}

void
sift_col_view_export(const SiftColSource *src, void *buf)
{
	memcpy(buf, &src->dir, dir_bytes(&src->dir));
}
