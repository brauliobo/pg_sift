/*
 * col/seg/verify.c - the checks of the sections a segment keeps for ordered and listed
 * reads: the global order and the point directory against the units
 * they were built from; run by the verify functions, never by a scan
 */
#include "core/host.h"
#include "col/seg_int.h"

/* The global order of a leaf holds every row of the segment once, the values by key then address, the rows with no value by address */
static void
verify_gorder(SiftColSegment *seg, int col, uint32 total)
{
	GOrdLeaf   *g = seg_gord_leaf(seg, col);
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
	uint32		have = UINT32_MAX;
	int64		pk = 0;
	uint64		pa = 0;
	uint32		nranges;
	SiftCGOrdGrp *grp = NULL;

	if (g == NULL)
		return;
	if (g->hdr.strbytes != 0)
		(void) seg_gord_strs(seg, g, col);
	seg_gord_dir_read(seg, g);
	if (g->hdr.gcol < 0 ? g->hdr.entries != total : g->hdr.entries > total)
		corrupt(seg, "The global order of leaf %d holds %u entries for %u rows.", col, g->hdr.entries, total);
	/* the chunks follow one another, and the first keys of a block that is not grouped ascend */
	{
		uint64		next = SIFT_CGORD_HDR + (uint64) g->hdr.chunks * SIFT_CGORD_DIRENT;
		uint32		vchunks = (g->hdr.values + g->hdr.crows - 1) / g->hdr.crows;
		int64		pf = 0;

		for (uint32 c = 0; c < g->hdr.chunks; c++)
		{
			SiftCGOrdDir d = seg_gord_dirent(seg, g, col, c);

			if (d.off != next || (g->hdr.gcol < 0 && c > 0 && c < vchunks && d.first < pf))
				corrupt(seg, "The global order of leaf %d has a malformed directory entry %u.", col, c);
			pf = d.first;
			next = (uint64) d.off + ALIGN8((uint64) d.klen) + ALIGN8((uint64) d.alen);
		}
	}
	/* the entries of a range ascend by (key, address), those of a NULL range by address; a block that is not grouped is two ranges, the values and the NULL rows */
	if (g->hdr.gcol >= 0)
	{
		uint8	   *gb = palloc((Size) g->hdr.ngroups * SIFT_CGORD_GRPENT);

		grp = palloc((Size) g->hdr.ngroups * sizeof(SiftCGOrdGrp));
		seg_read(seg, g->at + g->gat, (Size) g->hdr.ngroups * SIFT_CGORD_GRPENT, gb);
		if (!sift_cgord_groups(gb, &g->hdr, (uint64) g->hdr.ngroups * SIFT_CGORD_GRPENT, grp))
			corrupt(seg, "The global order of leaf %d has malformed groups.", col);
		pfree(gb);
	}
	nranges = g->hdr.gcol < 0 ? 2 : 2 * g->hdr.ngroups;
	for (uint32 r = 0; r < nranges; r++)
	{
		bool		isnull = g->hdr.gcol < 0 ? r == 1 : r % 2 == 1;
		uint32		from,
					count;

		if (g->hdr.gcol < 0)
		{
			from = isnull ? g->hdr.values : 0;
			count = isnull ? g->hdr.entries - g->hdr.values : g->hdr.values;
		}
		else
		{
			const SiftCGOrdGrp *gr = &grp[r / 2];

			from = isnull ? gr->nstart : gr->vstart;
			count = isnull ? gr->ncount : gr->vcount;
		}
		for (uint32 p = from; p < from + count; p++)
		{
			if (p / g->hdr.crows != have)
			{
				have = p / g->hdr.crows;
				(void) sift_cseg_gorder_chunk(seg, col, have, key, addr);
			}
			if (p != from && (isnull ? addr[p % g->hdr.crows] <= pa : key[p % g->hdr.crows] < pk || (key[p % g->hdr.crows] == pk && addr[p % g->hdr.crows] <= pa)))
				corrupt(seg, "The global order of leaf %d is out of order at entry %u.", col, p);
			pk = key[p % g->hdr.crows];
			pa = addr[p % g->hdr.crows];
			if (!isnull && g->hdr.strbytes != 0 && (uint64) pk >= g->nstr)
				corrupt(seg, "The global order of leaf %d has the rank %lld of %u strings at entry %u.", col, (long long) pk, g->nstr, p);
			if (p % 4096 == 0)
				CHECK_FOR_INTERRUPTS();
		}
	}
	/* the strings of a text block: every one is there, and each is at most a dictionary entry long */
	if (g->hdr.strbytes != 0)
	{
		uint32		len;

		for (uint32 r = 0; r < g->nstr; r++)
		{
			(void) sift_cseg_gorder_str(seg, col, r, &len);
			if (r % 4096 == 0)
				CHECK_FOR_INTERRUPTS();
		}
	}
}

/*
 * The entries of a packed table: the directory ascends from 0 to the entries, the remainders ascend in a bucket and the codes in a run of
 * one remainder, a row is a row of the segment and an address is in a unit of it. Returns the distinct k.
 */
static uint32
verify_packed(SiftColSegment *seg, int c, const PointLeaf *pl, uint32 total)
{
	const SiftCPackPar *par = &pl->pk;
	uint32		width = par->rbits + par->pbits,
				keys = 0,
				chunk = 4096;
	uint64		dir = ((uint64) (par->nbuckets + 1) * 4 + 7) & ~UINT64CONST(7),
				prem = 0,
				pcode = 0;
	uint32	   *starts = palloc((uint64) (par->nbuckets + 1) * sizeof(uint32));
	uint8	   *buf = palloc((uint64) chunk * width / 8 + 24);
	uint32		b = 0;

	seg_read(seg, pl->at, (uint64) (par->nbuckets + 1) * sizeof(uint32), starts);
	if (starts[0] != 0 || starts[par->nbuckets] != pl->nslots)
		corrupt(seg, "The packed point directory of leaf %d starts at entry %u and ends at %u of %u.", c, starts[0], starts[par->nbuckets], pl->nslots);
	for (uint32 i = 0; i < par->nbuckets; i++)
		if (starts[i] > starts[i + 1])
			corrupt(seg, "The packed point directory of leaf %d has the bucket %u from entry %u to %u.", c, i, starts[i], starts[i + 1]);
	for (uint32 e = 0; e < pl->nslots; e += chunk)
	{
		uint32		m = Min(chunk, pl->nslots - e);
		uint64		first = (uint64) e * width / 8,
					last = ((uint64) (e + m) * width + 7) / 8;

		memset(buf, 0, last - first + 8);
		seg_read(seg, pl->at + dir + first, last - first, buf);
		for (uint32 j = 0; j < m; j++)
		{
			uint64		at = (uint64) (e + j) * width - first * 8,
						rem = sift_cpoint_pk_get(buf, at, par->rbits),
						code = sift_cpoint_pk_get(buf, at + par->rbits, par->pbits);

			while (starts[b + 1] <= e + j)
				b++;
			if (e + j != starts[b] && rem < prem + (code <= pcode && rem == prem))
				corrupt(seg, "The packed point directory of leaf %d has entry %u out of order.", c, e + j);
			keys += e + j == starts[b] || rem != prem;
			prem = rem;
			pcode = code;
			if (pl->tids)
			{
				uint64		key = sift_cpoint_pk_tid(par, code);
				uint32		sb = (uint32) (key >> 16) >> SIFT_SB_SHIFT,
							u = sift_cseg_unit_of_sb(seg, sb);

				if ((key >> 16) > UINT32_MAX || u >= seg->nsb || (key & 0xFFFF) == 0 || (key & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
					corrupt(seg, "The packed point directory of leaf %d lists the address %llx, which is in no unit of the segment.", c, (unsigned long long) key);
			}
			else if (code >= total)
				corrupt(seg, "The packed point directory of leaf %d lists row %llu of %u.", c, (unsigned long long) code, total);
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(starts);
	pfree(buf);
	return keys;
}

void
sift_cseg_verify_points(SiftColSegment *seg)
{
	const PointLeaf *pl = seg_points_read(seg);
	uint32		total = seg->nsb > 0 ? seg->sbdir[seg->nsb - 1].rank_base + seg->sbdir[seg->nsb - 1].nrows : 0;
	uint64		win[1024];
	uint32		rows[1024];

	for (int c = 0; pl && c < seg->schema->ncols; c++)
	{
		uint64		listed = 0,
					words = 0;
		uint32		keys = 0;
		uint64		lists = pl[c].at + (uint64) pl[c].nslots * sizeof(uint64);

		if (pl[c].nslots == 0)
			continue;
		if (pl[c].packed)
		{
			if (verify_packed(seg, c, &pl[c], total) != pl[c].nkeys)
				corrupt(seg, "The packed point directory of leaf %d has a different number of distinct keys than its record %u.", c, pl[c].nkeys);
			continue;
		}
		if (pl[c].exact && seg->entry.format < SIFT_COL_FORMAT_EXACTSLOT)
			corrupt(seg, "The point directory of leaf %d has an exact slot count in a segment of column format 1.%d.", c, seg->entry.format);
		((PointLeaf *) pl)[c].ndense = 0;
		for (uint32 p = 0; p < pl[c].nslots; p += lengthof(win))
		{
			uint32		m = Min(lengthof(win), pl[c].nslots - p);

			seg_read(seg, pl[c].at + (uint64) p * sizeof(uint64), m * sizeof(uint64), win);
			for (uint32 j = 0; j < m; j++)
			{
				uint32		payload = (uint32) win[j],
							cnt = 1;

				if (win[j] == SIFT_CPOINT_EMPTY)
					continue;
				keys++;
				if (pl[c].tids)
				{
					TidAcc		a = {.seg = seg,.col = c,.pl = &pl[c]};

					seg_tid_slot_read(&a, win[j]);
					for (uint32 i = 1; i < a.n; i++)
						if (a.t[i] <= a.t[i - 1])
							corrupt(seg, "The point directory of leaf %d lists an address twice or out of order.", c);
					cnt = a.n;
					words += a.words;
					pfree(a.t);
				}
				else if ((payload & SIFT_CPOINT_LIST) && pl[c].zrows)
				{
					uint32		nw;

					pfree(seg_point_list_read(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST, total - 1, UINT32_MAX, &cnt, &nw));
					words += nw;
				}
				else if (payload & SIFT_CPOINT_LIST)
				{
					uint32		prev = 0;

					cnt = seg_point_list_count(seg, c, &pl[c], payload & ~SIFT_CPOINT_LIST);
					words += 1 + cnt;
					for (uint32 i = 0, got; i < cnt; i += got)
					{
						got = Min(lengthof(rows), cnt - i);
						seg_read(seg, lists + (uint64) ((payload & ~SIFT_CPOINT_LIST) + 1 + i) * sizeof(uint32), got * sizeof(uint32), rows);
						for (uint32 k = 0; k < got; k++)
						{
							if (rows[k] >= total || (i + k > 0 && rows[k] <= prev))
								corrupt(seg, "The point directory of leaf %d lists row %u of %u out of order.", c, rows[k], total);
							prev = rows[k];
						}
					}
				}
				else if (payload == SIFT_CPOINT_DENSE)
				{
					cnt = 1;
					((PointLeaf *) pl)[c].ndense++;
				}
				else if (payload >= total)
					corrupt(seg, "The point directory of leaf %d lists row %u of %u.", c, payload, total);
				listed += cnt;
			}
			CHECK_FOR_INTERRUPTS();
		}
		if (keys != pl[c].nkeys || listed != pl[c].count || words != pl[c].words)
			corrupt(seg, "The point directory of leaf %d lists %u keys, %llu rows and %llu words, its record %u, %u and %u.", c, keys, (unsigned long long) listed,
					(unsigned long long) words, pl[c].nkeys, pl[c].count, pl[c].words);
	}
	for (int c = 0; c < 2 * seg->schema->ncols; c++)
		verify_gorder(seg, c, total);
}
