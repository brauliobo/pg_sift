#!/usr/bin/env bash
# Version and ABI: the library reports what it was built for, an index written by another format is refused with a message
# that says what to do, and the library refuses to build against MariaDB headers outside the supported window.
#   mariadb/tests/version.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=version
MANIFEST=$SIFT_MDB_ROOT/data/.sift/vv/MANIFEST
patch() {   # offset hex-bytes: overwrites bytes of the manifest (little endian)
  python3 -I - "$MANIFEST" "$1" "$2" <<'E'
import sys
p, off, hexs = sys.argv[1], int(sys.argv[2]), bytes.fromhex(sys.argv[3])
with open(p, 'r+b') as f:
    f.seek(off)
    f.write(hexs)
E
}

fresh_db vv
sql "CREATE TABLE vv (id BIGINT PRIMARY KEY, body TEXT)"
sql "INSERT INTO vv VALUES (1, 'version check'), (2, 'another document')"
sql "CALL sift_attach('vv', 'vv', 'id', 't.body', 8)" >/dev/null
sync vv
check_match "sift_version names the release, the formats and the server it was built for" "^sift-mariadb $(sed 's/\./\\./g' "$HERE/VERSION") \\(format [0-9]+, pages [0-9]+\.[0-9]+ layout [0-9]+, built for MariaDB [0-9.]+" "$(sql 'SELECT sift_version()')"
check "built for the series of the running server" "$(sql 'SELECT SUBSTRING_INDEX(VERSION(), ".", 2)')" "$(sql 'SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(sift_version(), "built for MariaDB ", -1), ".", 2)')"
check "the index is readable" 1 "$(cnt version vv)"
cp "$MANIFEST" "$MANIFEST.good"

refused() {   # name offset bytes message-regex: the index is opened by the restarted server
  "$SRV" stop; cp "$MANIFEST.good" "$MANIFEST"; patch "$2" "$3"; "$SRV" start >/dev/null
  check_match "$1" "$4" "$(sqle "SELECT sift_count('vv', 'version')")"
}
refused "an index of another manifest format is refused" 7 31 "incompatible version of sift for MariaDB.*rebuild it with sift_rebuild"
refused "an index with a newer page format is refused" 12 09000000 "page format .*rebuild it with sift_rebuild"
refused "an index with another page layout is refused" 16 ffff0000 "page layout.*rebuild it with sift_rebuild"
refused "a damaged manifest is refused" 0 4142 "corrupted"
"$SRV" stop; : >"$MANIFEST"; "$SRV" start >/dev/null
check_match "an empty manifest is refused" "corrupted" "$(sqle "SELECT sift_count('vv', 'version')")"
check_match "and sift_stats says the same" "corrupted" "$(sqle "SELECT sift_stats('vv')")"
"$SRV" stop; cp "$MANIFEST.good" "$MANIFEST"; "$SRV" start >/dev/null
check "a restored manifest opens again" 1 "$(cnt version vv)"
"$SRV" stop; patch 16 ffff0000; "$SRV" start >/dev/null
check_match "a refused index is replaced by sift_rebuild" "^2	" "$(sqle "CALL sift_rebuild('vv')" | tail -1)"
check "and answers again" 1 "$(cnt version vv)"
rm -f "$MANIFEST.good"

# the compile-time window of the MariaDB headers (10.5 .. 13.x): a copy of the headers with another version number
MDB_INC=$(make -s -C "$HERE" mdb-inc)
TMP=$(mktemp -d /tmp/sift-mdb-ver.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
for v in 100400 140000 90500; do
  mkdir -p "$TMP/$v"
  cp -rs "$MDB_INC/." "$TMP/$v/"                     # the header tree, with its own version number
  rm "$TMP/$v/mysql_version.h"
  sed "s/^#define MYSQL_VERSION_ID.*/#define MYSQL_VERSION_ID $v/" "$MDB_INC/mysql_version.h" >"$TMP/$v/mysql_version.h"
  out=$(cc -fsyntax-only -I"$TMP/$v" -I"$TMP/$v/private" -I"$HERE" "$HERE/udf.c" 2>&1)
  check_match "headers of version $v are refused at compile time" "outside 10.5 .. 13.x" "$out"
done
out=$(cc -fsyntax-only -I"$MDB_INC" -I"$MDB_INC/private" -I"$HERE" "$HERE/udf.c" "$HERE/udf_host.c" "$HERE/udf_query.c" "$HERE/udf_drain.c" 2>&1)
check "the installed headers compile cleanly" "" "$out"

# CMake builds the same library against the installed headers
if command -v cmake >/dev/null; then
  cmake -S "$HERE" -B "$TMP/cmake" -DCMAKE_BUILD_TYPE=Release >"$TMP/cmake.log" 2>&1 && nice cmake --build "$TMP/cmake" -j4 >>"$TMP/cmake.log" 2>&1
  check "CMake builds libsift_udf.so" 1 "$([[ -f $TMP/cmake/libsift_udf.so ]] && echo 1)"
  check "and exports the functions of the Makefile build" "$(nm -D "$SIFT_BUILD/libsift_udf.so" | awk '$2=="T"{print $3}' | sort | paste -sd' ' -)" \
    "$(nm -D "$TMP/cmake/libsift_udf.so" | awk '$2=="T"{print $3}' | sort | paste -sd' ' -)"
fi
finish
