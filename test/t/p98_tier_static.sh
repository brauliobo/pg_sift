#!/usr/bin/env bash
# What the tiering code of the host must not do, read from the sources: no part opened by the block of its header where it may be in object storage, no PostgreSQL allocation inside a libcurl
# callback (an error raised there would jump out of libcurl), and the library name of the platform that is not Linux.
. "$(dirname "$0")/lib.bash"

plan 3
SRC=$(cd "$(dirname "$0")/../.." && pwd)/src/col/f2
check "the scope overlay opens a part by its descriptor, which may be in object storage" test "$(grep -c 'sift_part_open(' "$SRC/f2_scope_over.c")" == 0
check "write_cb allocates nothing from PostgreSQL" test "$(sed -n '/^write_cb/,/^}/p' "$SRC/f2_tier_curl.c" | grep -cE 'palloc|repalloc|ereport|elog')" == 0
check "libcurl is looked for under its name on macOS too" grep -q 'libcurl.4.dylib' "$SRC/f2_tier_curl.c"

finish
