/*
 * col/f2/preagg_join.c - col/f2/preagg_v2_read.c: the join of the pre-aggregate sections of parts reports a section whose header names bytes it does not have (the cubes, the sets of distinct values)
 * as a damaged segment, and reads nothing past it.
 */
#include "postgres.h"

#include "utils/rel.h"

#include "col/f2/part_format.h"
#include "col/f2/preagg_v2.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define SEC_BYTES	512

static FormData_pg_class fake_class;
static RelationData fake_rel = {.rd_rel = &fake_class};
static char *section;

const char *
sift_index_ident(Relation index)
{
	return "preagg_join_idx";
}

void *
sift_part_read_section(Relation rel, const SiftPart *part, uint16 tag, uint64 *len)
{
	char	   *copy = palloc(SEC_BYTES);

	memcpy(copy, section, SEC_BYTES);
	*len = SEC_BYTES;
	return copy;
}

const SiftSecDirEntry *
sift_part_section(const SiftPart *part, uint16 tag)
{
	pgshim_unreached("sift_part_section");
}

void
sift_part_read_section_bytes(Relation rel, const SiftPart *part, const SiftSecDirEntry *sec, uint64 off, uint64 len, void *dst)
{
	pgshim_unreached("sift_part_read_section_bytes");
}

void *
sift_preagg_v2_assemble(int ncubes, const SiftPreaggCubeDesc *cube, uint32 ng, uint32 flags, uint32 *const *first, const struct StringInfoData *cells, const struct StringInfoData *dist, uint64 *len)
{
	pgshim_unreached("sift_preagg_v2_assemble");
}

typedef struct JoinArgs
{
	SiftPart	part;
	SiftPart   *src[1];
	uint64		len;
	void	   *out;
} JoinArgs;

static void
join_it(void *arg)
{
	JoinArgs   *a = arg;

	a->out = sift_preagg_v2_join(&fake_rel, a->src, 1, &a->len);
}

/* A section of one cube of no cells whose header says where the cubes and the sets of distinct values are */
static void
make_section(uint64 cubes_off, uint64 dist_off)
{
	SiftPreaggSecHdr hdr = {.magic = SIFT_PREAGG_MAGIC,.version = SIFT_PREAGG_VERSION,.ncubes = 1,.ngranules = 4,.cubes_off = cubes_off,.dist_off = dist_off};
	SiftPreaggCubeDesc cube = {.gran_dir_off = 128,.cells_off = 256};

	memset(section, 0, SEC_BYTES);
	memcpy(section, &hdr, sizeof(hdr));
	if (cubes_off <= SEC_BYTES - sizeof(cube))
		memcpy(section + cubes_off, &cube, sizeof(cube));
}

static void
expect_damaged(uint64 cubes_off, uint64 dist_off)
{
	static JoinArgs a;

	memset(&a, 0, sizeof(a));
	a.part.hdr.ngranules = 4;
	a.part.hdr.part_gen = 7;
	a.src[0] = &a.part;
	make_section(cubes_off, dist_off);
	SIFT_CHECK(!pgshim_try(join_it, &a));
	SIFT_CHECK(strstr(pgshim_errmsg, "is corrupted") != NULL);
}

int
main(void)
{
	pgshim_keep();
	section = malloc(SEC_BYTES);
	strlcpy(fake_class.relname.data, "idx", NAMEDATALEN);
	expect_damaged(SEC_BYTES + 64, 0);
	expect_damaged(UINT64_MAX - 31, 0);
	expect_damaged(sizeof(SiftPreaggSecHdr), SEC_BYTES + 100);
	expect_damaged(sizeof(SiftPreaggSecHdr), UINT64_MAX);
	free(section);
	puts("test_preagg_join: ok");
	return 0;
}
