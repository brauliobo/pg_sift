#!/usr/bin/env bash
# No tracked source file passes 1000 lines: a longer one is split into a directory of context files (src/<area>/<context>/*.c with a small private header,
# the design notes), a pure move first. Sources are C, headers, scripts, SQL, Python, Perl, makefiles and workflows; documents (*.md), expected
# outputs, result tables and images are data and are not counted.
#
#   scripts/check-file-size.sh     exit 1 and list every source over the limit that is not allowed below
set -euo pipefail
cd "$(dirname "$0")/.."
LIMIT=1000

# Allowed for good: the full install script of a released minor (sql/pg_sift--X.Y.sql) is frozen (scripts/released-sql.sha256) and cannot be split; a new minor adds only its update script.
FROZEN='^sql/pg_sift--[0-9]+[.][0-9]+[.]sql$'

bad=0
while IFS=$'\t' read -r f n; do
	[[ $f =~ $FROZEN ]] && continue
	echo "$f: $n lines (limit $LIMIT): split it into a directory of context files"
	bad=1
done < <(git grep -c '' -- '*.c' '*.h' '*.sh' '*.bash' '*.py' '*.pl' '*.pm' '*.sql' '*.comp' '*.mk' '*Makefile' '*.yml' '*.yaml' |
	awk -F: -v l=$LIMIT '$NF > l {n = $NF; sub(/:[0-9]+$/, ""); print $0 "\t" n}')
(( ! bad )) || exit 1
echo "check-file-size: no source over $LIMIT lines"
