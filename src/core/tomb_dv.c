/*
 * core/tomb_dv.c - the deletion record of the tombstone log (core/tomb_dv.h): how rows are written as slots of the log and read back
 */
#include "postgres.h"

#include "core/tomb_dv.h"

#define SLOT_PAYLOAD(e)		((uint8 *) &(e)->xid)

StaticAssertDecl(offsetof(SiftTomb, kind) - offsetof(SiftTomb, xid) == SIFT_DV_SLOT_BYTES, "a body slot carries 16 bytes between its key and its kind");

static uint32
data_slots(uint32 bytes)
{
	return (bytes + SIFT_DV_SLOT_BYTES - 1) / SIFT_DV_SLOT_BYTES;
}

static uint32
position_bytes(uint32 count)
{
	return count * sizeof(uint16);
}

static uint32
bitmap_bytes(uint32 span)
{
	return (span + 7) / 8;
}

/* The rows [0, n) the one record that starts at rows[0] takes: as many as fit in a page by a list of positions or a bitmap */
static uint32
record_rows(const uint32 *rows, uint32 n)
{
	uint32		take = 1;

	while (take < n)
	{
		uint32		span = rows[take] - rows[0] + 1;
		bool		list_fits = rows[take] - rows[0] <= PG_UINT16_MAX && position_bytes(take + 1) <= SIFT_DV_DATA_MAX;

		if (!list_fits && bitmap_bytes(span) > SIFT_DV_DATA_MAX)
			break;
		take++;
	}
	return take;
}

static SiftTomb *
slots_room(SiftTomb **out, uint32 *nout, uint32 *cap, MemoryContext cxt, uint32 more)
{
	if (*nout + more > *cap)
	{
		*cap = Max(Max(64, *cap * 2), *nout + more);
		*out = *out == NULL ? MemoryContextAlloc(cxt, (Size) *cap * sizeof(SiftTomb)) : repalloc_huge(*out, (Size) *cap * sizeof(SiftTomb));
	}
	return *out + *nout;
}

typedef struct Form
{
	bool		bitmap;
	uint32		span;
	uint32		bytes;
} Form;

static Form
record_form(const uint32 *rows, uint32 count)
{
	Form		f = {.span = rows[count - 1] - rows[0] + 1};

	f.bitmap = bitmap_bytes(f.span) < position_bytes(count) || rows[count - 1] - rows[0] > PG_UINT16_MAX;
	f.bytes = f.bitmap ? bitmap_bytes(f.span) : position_bytes(count);
	return f;
}

static void
encode_one(SiftTomb *slots, uint64 gen, uint32 part, TransactionId xid, CommandId cid, const uint32 *rows, uint32 count, Form f)
{
	uint32		nbody = 1 + data_slots(f.bytes);
	SiftDvDesc	desc = {part, rows[0], count, f.span};
	uint8	   *data = palloc0((Size) data_slots(f.bytes) * SIFT_DV_SLOT_BYTES);

	for (uint32 i = 0; i < count; i++)
	{
		uint32		off = rows[i] - rows[0];

		if (f.bitmap)
			data[off / 8] |= 1 << (off % 8);
		else
			memcpy(data + i * sizeof(uint16), &(uint16) {off}, sizeof(uint16));
	}
	memset(slots, 0, (1 + nbody) * sizeof(SiftTomb));
	slots[0] = (SiftTomb) {.key = SIFT_DV_KEY(gen, part),.xid = xid,.cid = cid,.succ = gen,.kind = SIFT_TOMB_DVEC,.flags = f.bitmap ? SIFT_DV_BITMAP : 0};
	for (uint32 i = 1; i <= nbody; i++)
	{
		slots[i].key = slots[0].key;
		slots[i].kind = SIFT_TOMB_DVEC_BODY;
		slots[i].flags = i;
		memcpy(SLOT_PAYLOAD(&slots[i]), i == 1 ? (const uint8 *) &desc : data + (i - 2) * SIFT_DV_SLOT_BYTES, SIFT_DV_SLOT_BYTES);
	}
	pfree(data);
}

uint32
sift_dv_count(const uint32 *rows, uint32 n)
{
	uint32		slots = 0;

	while (n > 0)
	{
		uint32		take = record_rows(rows, n);

		slots += 2 + data_slots(record_form(rows, take).bytes);
		rows += take;
		n -= take;
	}
	return slots;
}

uint32
sift_dv_encode(SiftTomb **out, uint32 *nout, uint32 *cap, MemoryContext cxt, uint64 gen, uint32 part, TransactionId xid, CommandId cid, const uint32 *rows, uint32 n)
{
	uint32		start = *nout;

	while (n > 0)
	{
		uint32		take = record_rows(rows, n);
		Form		f = record_form(rows, take);
		uint32		nslots = 2 + data_slots(f.bytes);

		encode_one(slots_room(out, nout, cap, cxt, nslots), gen, part, xid, cid, rows, take, f);
		*nout += nslots;
		rows += take;
		n -= take;
	}
	return *nout - start;
}

uint32
sift_dv_slots_of(const SiftTomb *e, uint32 avail)
{
	SiftDvDesc	desc;
	uint32		bytes;

	if (avail < 2 || e[0].kind != SIFT_TOMB_DVEC || e[1].kind != SIFT_TOMB_DVEC_BODY)
		return 0;
	memcpy(&desc, SLOT_PAYLOAD(&e[1]), sizeof(desc));
	bytes = (e[0].flags & SIFT_DV_BITMAP) ? bitmap_bytes(desc.span) : position_bytes(desc.count);
	return 2 + data_slots(bytes) <= avail ? 2 + data_slots(bytes) : 0;
}

void
sift_dv_rekey_slots(SiftTomb *slots, uint32 n, uint64 gen, uint32 part)
{
	SiftDvDesc	desc;

	memcpy(&desc, SLOT_PAYLOAD(&slots[1]), sizeof(desc));
	desc.part = part;
	memcpy(SLOT_PAYLOAD(&slots[1]), &desc, sizeof(desc));
	slots[0].succ = gen;
	for (uint32 i = 0; i < n; i++)
		slots[i].key = SIFT_DV_KEY(gen, part);
}

uint32
sift_dv_head_rows(const SiftTomb *e)
{
	SiftDvDesc	desc;

	memcpy(&desc, SLOT_PAYLOAD(&e[1]), sizeof(desc));
	return desc.count;
}

void
sift_dv_decode(const SiftTomb *e, uint32 nslots, SiftDvRec *r)
{
	r->gen = e[0].succ;
	r->xid = e[0].xid;
	r->cid = e[0].cid;
	r->bitmap = (e[0].flags & SIFT_DV_BITMAP) != 0;
	memcpy(&r->desc, SLOT_PAYLOAD(&e[1]), sizeof(r->desc));
	for (uint32 i = 2; i < nslots; i++)
		memcpy(r->data + (i - 2) * SIFT_DV_SLOT_BYTES, SLOT_PAYLOAD(&e[i]), SIFT_DV_SLOT_BYTES);
}

bool
sift_dv_next(SiftTombKeyWalk *w, SiftDvRec *r)
{
	SiftTomb	slots[SIFT_TOMB_CAP];
	const SiftTomb *e;
	uint32		have = 0,
				want = 0;

	while (want == 0 || have < want)
	{
		if ((e = sift_tomb_key_next(w)) == NULL)
			break;
		if (have == 0 && e->kind != SIFT_TOMB_DVEC)
			continue;
		slots[have++] = *e;
		if (have == 2)
		{
			want = sift_dv_slots_of(slots, SIFT_TOMB_CAP);
			SIFT_INVARIANT(want != 0, "sift: a deletion record of the tombstone log is damaged");
		}
	}
	if (have == 0)
		return false;
	SIFT_INVARIANT(have == want, "sift: a deletion record of the tombstone log is cut short");
	sift_dv_decode(slots, have, r);
	return true;
}

void
sift_dv_or_bits(const SiftDvRec *r, uint64 *bits)
{
	if (r->bitmap)
		for (uint32 i = 0; i < r->desc.span; i++)
			if (r->data[i / 8] >> (i % 8) & 1)
				bits[(r->desc.row_lo + i) / 64] |= UINT64CONST(1) << ((r->desc.row_lo + i) % 64);
	for (uint32 i = 0; !r->bitmap && i < r->desc.count; i++)
	{
		uint16		off;

		memcpy(&off, r->data + i * sizeof(uint16), sizeof(off));
		bits[(r->desc.row_lo + off) / 64] |= UINT64CONST(1) << ((r->desc.row_lo + off) % 64);
	}
}

void
sift_dv_rows(const SiftDvRec *r, uint32 *rows)
{
	uint32		k = 0;

	if (r->bitmap)
		for (uint32 i = 0; i < r->desc.span; i++)
			if (r->data[i / 8] >> (i % 8) & 1)
				rows[k++] = r->desc.row_lo + i;
	for (uint32 i = 0; !r->bitmap && i < r->desc.count; i++)
	{
		uint16		off;

		memcpy(&off, r->data + i * sizeof(uint16), sizeof(off));
		rows[k++] = r->desc.row_lo + off;
	}
}
