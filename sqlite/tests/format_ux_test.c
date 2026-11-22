/*
 * format_ux_test.c - pins the texts of src/core/format_ux.c: a duration, a size, a count and a ratio read the same in every message, and the summaries of the operations a user starts
 * are silent for a small quick one. C library only; built by the portable flags of sqlite/Makefile (make format-ux-test), run by test.sh.
 */
#include <stdio.h>
#include <string.h>

#include "core/format_ux.h"

static int	bad;

static void
expect(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0)
	{
		fprintf(stderr, "format_ux_test: %s: got \"%s\", want \"%s\"\n", what, got, want);
		bad++;
	}
}

static void
quantities(void)
{
	char		b[SIFT_UX_LINE];
	const struct
	{
		int64_t		us;
		const char *want;
	}			dur[] = {{0, "1 ms"}, {400, "1 ms"}, {84000, "84 ms"}, {999000, "999 ms"}, {1000000, "1.00 s"}, {3420000, "3.42 s"},
	{59990000, "59.99 s"}, {60000000, "1m 0.0s"}, {134500000, "2m 14.5s"}};
	const struct
	{
		uint64_t	n;
		const char *bytes,
				   *count;
	}			qty[] = {{0, "0 B", "0"}, {512, "512 B", "512"}, {1023, "1023 B", "1,023"}, {1024, "1.0 KB", "1,024"},
	{65536, "64.0 KB", "65,536"}, {14889779, "14.2 MB", "14,889,779"}, {1481763717, "1.38 GB", "1,481,763,717"},
	{UINT64_C(1000000), "976.6 KB", "1,000,000"}, {UINT64_MAX, "17179869184.00 GB", "18,446,744,073,709,551,615"}};

	for (size_t i = 0; i < sizeof(dur) / sizeof(dur[0]); i++)
		expect("duration", sift_fmt_duration(b, sizeof(b), dur[i].us), dur[i].want);
	for (size_t i = 0; i < sizeof(qty) / sizeof(qty[0]); i++)
	{
		expect("bytes", sift_fmt_bytes(b, sizeof(b), qty[i].n), qty[i].bytes);
		expect("count", sift_fmt_count(b, sizeof(b), qty[i].n), qty[i].count);
	}
	expect("ratio", sift_fmt_ratio(b, sizeof(b), 363 * UINT64_C(1048576) / 10, 3 * UINT64_C(1048576)), "12.1x vs heap");
	expect("ratio of a small heap", sift_fmt_ratio(b, sizeof(b), 4096, 100), "");
	expect("ratio of an empty index", sift_fmt_ratio(b, sizeof(b), UINT64_C(1) << 30, 0), "");
	expect("size summary", sift_fmt_size_summary(b, sizeof(b), 33345024, 1000000, 380000000), "31.8 MB, 33.3 B/row, 11.4x vs heap");
	expect("size summary of no rows", sift_fmt_size_summary(b, sizeof(b), 8192, 0, 0), "8.0 KB");
}

static void
small_buffers(void)
{
	char		b[8];

	expect("a quantity cut to the buffer", sift_fmt_count(b, sizeof(b), UINT64_C(1234567890)), "1,234,5");
	expect("an empty buffer is not written", sift_fmt_bytes(b, 1, 5), "");
	if (strlen(sift_fmt_bytes(b, sizeof(b), UINT64_C(1) << 40)) >= sizeof(b))
		bad++;
}

static void
gate(void)
{
	const struct
	{
		int64_t		us;
		uint64_t	rows;
		bool		chose,
					changed;
		bool		want;
	}			g[] = {{4000, 500, false, false, false}, {99999, 9999, false, false, false}, {100000, 10, false, false, true}, {4000, 10000, false, false, true},
	{4000, 10, true, false, true}, {4000, 10, false, true, true}};

	for (size_t i = 0; i < sizeof(g) / sizeof(g[0]); i++)
		if (sift_ux_should_emit_build(g[i].us, g[i].rows, g[i].chose, g[i].changed) != g[i].want)
		{
			fprintf(stderr, "format_ux_test: gate %zu\n", i);
			bad++;
		}
	if (sift_ux_should_emit_timing(99999) || !sift_ux_should_emit_timing(100000))
		bad++;
}

static void
lines(void)
{
	char		b[SIFT_UX_LINE];
	SiftUxOp	build = {.am = "sift_col",.name = "idx_events_col",.rows = 1000000,.elapsed_us = 2410000,.bytes = 33345024,.heap_bytes = 380000000};
	SiftUxOp	docs = {.am = "sift",.name = "idx_docs_bm25",.noun = "docs",.rows = 250000,.elapsed_us = 1850000,.bytes = 44667659,.extra = "2 segments, Format 1.7"};
	SiftUxOp	up = {.am = "sift_col",.name = "idx_events_col",.rows = 1000000,.elapsed_us = 2680000,.format_before = "1.51",.format_after = "2.0",
	.bytes_before = 119646822,.bytes = 33345024};
	SiftUxOp	vac = {.am = "sift_col",.name = "idx_events_col",.dead_rows = 48210,.parts_before = 6,.parts_after = 2,.granules = 123,.elapsed_us = 412000,
	.bytes_before = 40056217,.bytes = 33345024,.pages_before = 4888,.pages_after = 4069};
	SiftUxOp	deferred = {.am = "sift_col",.name = "idx_events_col",.dead_rows = 48210,.parts_before = 6,.parts_after = 2,.granules = 123,.elapsed_us = 390000,
	.bytes = 40056217,.shrink_deferred = "streaming standby",.free_blocks = 819};
	SiftUxOp	mnt = {.name = "idx_events_col",.parts_before = 8,.parts_after = 3,.bytes_before = 46333747,.bytes = 33345024,.elapsed_us = 640000};
	SiftUxOp	compact = {.name = "idx_events_col",.parts_before = 3,.parts_after = 3,.bytes_before = 33345024,.bytes = 33345024,.elapsed_us = 120000};
	SiftUxOp	tbl = {.am = "sift_table",.name = "events",.format_before = "1.x",.format_after = "2.0",.rows = 1000000,.parts_after = 4,
	.bytes_before = 124146278,.bytes = 33657037,.elapsed_us = 3180000};
	SiftUxOp	chk = {.name = "idx_events_col",.format_after = "2.0",.parts_after = 4,.granules = 123,.rows = 1000000,.elapsed_us = 310000};

	expect("build", sift_ux_build_line(b, sizeof(b), &build), "sift_col \"idx_events_col\": built 1,000,000 rows in 2.41 s (31.8 MB, 33.3 B/row, 11.4x vs heap)");
	expect("bm25 build", sift_ux_build_line(b, sizeof(b), &docs), "sift \"idx_docs_bm25\": built 250,000 docs in 1.85 s (42.6 MB, 178.7 B/row, 2 segments, Format 1.7)");
	expect("reindex upgrade", sift_ux_build_line(b, sizeof(b), &up), "sift_col \"idx_events_col\": upgraded Format 1.51 -> 2.0 (1,000,000 rows, 114.1 MB -> 31.8 MB) in 2.68 s");
	expect("vacuum", sift_ux_vacuum_line(b, sizeof(b), &vac),
		   "sift_col \"idx_events_col\": removed 48,210 dead rows, compacted 6->2 parts (123 granules), size 31.8 MB (reclaimed 6.4 MB, 4888->4069 pages) in 412 ms");
	expect("vacuum with the shrink deferred", sift_ux_vacuum_line(b, sizeof(b), &deferred),
		   "sift_col \"idx_events_col\": removed 48,210 dead rows, compacted 6->2 parts (123 granules), size 38.2 MB (tail shrink deferred: streaming standby, 819 free blocks remain) in 390 ms");
	expect("maintain", sift_ux_maintain_line(b, sizeof(b), &mnt), "sift.maintain(\"idx_events_col\"): compacted 8->3 parts, reclaimed 12.4 MB (44.2 MB -> 31.8 MB) in 640 ms");
	expect("maintain with nothing to do", sift_ux_maintain_line(b, sizeof(b), &compact), "sift.maintain(\"idx_events_col\"): already compact (0 parts changed)");
	expect("upgrade_table", sift_ux_upgrade_line(b, sizeof(b), &tbl),
		   "sift_table \"events\": upgraded Format 1.x -> 2.0 (1,000,000 rows, 4 parts, 118.4 MB -> 32.1 MB, 3.7x smaller) in 3.18 s");
	expect("check", sift_ux_check_line(b, sizeof(b), &chk), "sift.check(\"idx_events_col\"): ok (Format 2.0, 4 parts, 123 granules, 1,000,000 rows verified in 310 ms)");
}

int
main(void)
{
	quantities();
	small_buffers();
	gate();
	lines();
	if (bad)
		return 1;
	puts("format_ux_test: ok");
	return 0;
}
