#!/usr/bin/env bash
# The regex engine of the MariaDB host tells the query parser (src/bm25/query_analyze.c compile_regex) whether a pattern has a back-reference:
# re_info of the regex_t, which the parser palloc's and never clears. The engine must set it for every pattern: a pattern without a
# back-reference was refused (and one with it accepted) depending on what the heap held.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -O2 -g -std=gnu11 -fwrapv -fno-strict-aliasing -D_GNU_SOURCE -Wall -Wno-declaration-after-statement \
	-I"$HERE/../host" -I"$HERE/../../src" -o "$W/regex_test" "$HERE/../host/host_regex.c" "$HERE/regex_test.c" -lm
got=$(printf '%s\n' '^w.nd$' 'a+b' '(a)(b)' '(a)\1' '(a|b)x\1' '[' | "$W/regex_test" info | tr '\n' ' ')
want='0 0 0 1 1 error '
[[ $got == "$want" ]] || { echo "re_info: got [$got], want [$want]"; exit 1; }
echo "regex_info: ok"
