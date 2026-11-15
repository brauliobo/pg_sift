#!/usr/bin/env bash
# Every tracked file of src/ is classified once: in src/portable.list or src/portable-col.list (reaches its host only through core/host.h, compiled by the SQLite and MariaDB hosts or by
# `make -C sqlite col-smoke`) or in src/pg-only.list (the PostgreSQL extension only). A new file is a decision: left out of the lists it shows up as an undefined symbol in the CI job of a host.
#   scripts/check-classified.sh             the tracked files of src/
#   scripts/check-classified.sh --selftest
set -euo pipefail
cd "$(dirname "$0")/.."

# check ROOT: prints one line for each file or list entry that breaks the rule; exit 1 if there is one
check() {
	python3 -I - "$1" <<'PY'
import os, subprocess, sys

root = sys.argv[1]
KEEP = {'areas.mk', 'portable.list', 'portable-col.list', 'pg-only.list'}


def entries(name):
    with open(os.path.join(root, name)) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith('#')]


portable = set(entries('portable.list')) | set(entries('portable-col.list'))
pg = entries('pg-only.list')
files = [f for f in subprocess.check_output(['git', '-C', root, 'ls-files', '--', '.'], text=True).split('\n') if f and f not in KEEP]
used = set()
bad = []
for f in files:
    hits = [e for e in pg if f == e or (e.endswith('/') and f.startswith(e))]
    used.update(hits)
    if f in portable and hits:
        bad.append(f'src/{f}: in a portable list and in pg-only.list; a file is one or the other')
    elif f not in portable and not hits:
        bad.append(f'src/{f}: not classified. Add it to src/portable.list or src/portable-col.list when it needs only core/host.h, else to src/pg-only.list; '
                   'a portable file that calls it needs the callee in the portable core or a function in sqlite/host, not a PostgreSQL-only file')
bad += [f'src/pg-only.list: {e} matches no file' for e in pg if e not in used]
print('\n'.join(bad))
sys.exit(1 if bad else 0)
PY
}

selftest() {
	local t out; t=$(mktemp -d); trap "rm -rf $t" RETURN
	mkdir -p "$t/s/core" "$t/s/col/am"
	(cd "$t" && git init -q && git config user.email t@example.com && git config user.name t)
	touch "$t/s/core/host.h" "$t/s/core/ops.c" "$t/s/col/am/a.c" "$t/s/col/am/b.c" "$t/s/col/enc.c"
	printf 'core/host.h\ncol/enc.c\n' >"$t/s/portable.list"; : >"$t/s/portable-col.list"
	printf 'core/ops.c\ncol/am/\n' >"$t/s/pg-only.list"
	git -C "$t" add -A
	check "$t/s" >/dev/null || { echo "selftest: a fully classified tree was refused"; return 1; }
	touch "$t/s/core/new.c"; git -C "$t" add -A
	out=$(check "$t/s" || true); grep -q 'core/new.c: not classified' <<<"$out" || { echo "selftest: an unclassified file passed"; return 1; }
	echo core/new.c >>"$t/s/portable.list"; echo core/new.c >>"$t/s/pg-only.list"
	out=$(check "$t/s" || true); grep -q 'in a portable list and in pg-only.list' <<<"$out" || { echo "selftest: a file in both lists passed"; return 1; }
	grep -v 'core/new.c' "$t/s/pg-only.list" >"$t/s/pg-only.new"; mv "$t/s/pg-only.new" "$t/s/pg-only.list"; echo col/gone.c >>"$t/s/pg-only.list"
	out=$(check "$t/s" || true); grep -q 'col/gone.c matches no file' <<<"$out" || { echo "selftest: a stale entry passed"; return 1; }
	echo "check-classified selftest: ok"
}

case ${1:-} in
	--selftest) selftest ;;
	'') out=$(check src) || { echo "$out"; exit 1; }; echo "check-classified: every file of src/ is in portable.list, portable-col.list or pg-only.list" ;;
	*) echo "usage: $0 [--selftest]" >&2; exit 2 ;;
esac
