#!/usr/bin/env bash
# The refusal registry (src/agg/refuse.h): the slug tables of docs/USAGE.md and docs/COL-PLAN.md are generated from it, and the planner sources are
# checked against it. Needs python3, no PostgreSQL.
#
#   scripts/gen-refusals.sh           write the generated blocks (between <!-- refusals:start --> and <!-- refusals:end -->)
#   scripts/gen-refusals.sh --check   check the sources and that the blocks are the generated ones (lint)
#
# The checks fail when
#   - a `return false` of src/agg/plan*.c or spec*.c is neither a refusal (`return agg_refuse*(..)`, or `agg_refused*(..);` right before it) nor in a
#     function that scripts/refusals.allow lists with its reason (a walker, a predicate or a "try" whose caller names the refusal: a reason recorded in
#     a try would be the first of the arm and hide the one that follows it),
#   - scripts/refusals.allow lists a function that has no such `return false` any more (or does not exist),
#   - a slug of the table has no site (agg_refuse*, agg_refused*, agg_report, or AGG_REFUSE_<ID> in refuse.c) in src/agg, except `other`.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check ]] || { echo "usage: $0 [--check]" >&2; exit 2; }
exec python3 -I - "$mode" <<'PY'
import glob
import os
import re
import sys

mode = sys.argv[1]
errors = []


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


# ---- the table ----
slugs = []     # (ID, slug, sentence, limit, by_design)
for m in re.finditer(r'^\tX\((\w+), "([a-z_]+)", "((?:[^"\\]|\\.)*)", ([01]), ([01])\)', read('src/agg/refuse.h'), re.M):
    slugs.append((m.group(1), m.group(2), m.group(3), m.group(4) == '1', m.group(5) == '1'))
if not slugs:
    sys.exit('gen-refusals: no row found in the SIFT_AGG_REFUSALS table of src/agg/refuse.h')


def block():
    rows = ['| slug | class | by design | the query kept the normal plan because |', '|---|---|---|---|']
    for _, slug, text, limit, by_design in slugs:
        rows.append('| `%s` | %s | %s | %s |' % (slug, 'limit' if limit else 'semantic', 'yes' if by_design else '', text.replace('\\"', '"')))
    return '\n'.join(rows)


# ---- the sources ----
allow = {}      # (file, function) -> reason
for n, line in enumerate(read('scripts/refusals.allow').split('\n'), 1):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    m = re.match(r'(\S+\.c):\s+([\w ]+?)\s+#\s*(\S.*)$', line)
    if not m:
        errors.append('scripts/refusals.allow:%d: expected "file.c: function function # reason"' % n)
        continue
    for fn in m.group(2).split():
        if (m.group(1), fn) in allow:
            errors.append('scripts/refusals.allow:%d: %s %s listed twice' % (n, m.group(1), fn))
        allow[(m.group(1), fn)] = m.group(3)

seen = set()
for path in sorted(glob.glob('src/agg/plan*.c') + glob.glob('src/agg/spec*.c')):
    lines = read(path).split('\n')
    name = os.path.basename(path)
    fn = None
    for i, l in enumerate(lines):
        m = re.match(r'^(\w+)\(', l)
        if m and i > 0 and lines[i - 1].strip() and not lines[i - 1].startswith(('\t', ' ', '#', '/', '*')):
            fn = m.group(1)
        if not re.search(r'\breturn\s+false\b', l) or re.match(r'\s*(/\*|\*|//)', l):
            continue
        j = i - 1
        while j >= 0 and not lines[j].strip():
            j -= 1
        if 'agg_refus' in l or (j >= 0 and re.match(r'\s*agg_refused', lines[j])):
            continue
        if (name, fn) in allow:
            seen.add((name, fn))
        else:
            errors.append('%s:%d: `return false` in %s() names no refusal: use agg_refuse*(SLUG) or agg_refused*(SLUG) before it, or list the function in scripts/refusals.allow' % (path, i + 1, fn))
for key in sorted(set(allow) - seen):
    errors.append('scripts/refusals.allow: %s %s() has no unnamed `return false` (remove it)' % key)

used = set()
for path in glob.glob('src/agg/*.c'):
    used.update(re.findall(r'\bagg_(?:refuse|refused|refuse_note|report)\w*\(\s*(?:"[^"]*",\s*)?(?:AGG_REFUSE_)?([A-Z_]+)', read(path)))
    used.update(re.findall(r'\bAGG_REFUSE_([A-Z_]+)', read(path)))
for id_, slug, _, _, _ in slugs:
    if id_ != 'OTHER' and id_ not in used:
        errors.append('src/agg/refuse.h: the slug %s has no site in src/agg' % slug)

# ---- the generated blocks ----
START, END = '<!-- refusals:start -->', '<!-- refusals:end -->'
DOCS = ['docs/USAGE.md', 'docs/COL-PLAN.md']
for doc in DOCS:
    text = read(doc)
    m = re.search(re.escape(START) + r'\n(?:.*?\n)?' + re.escape(END), text, re.S)
    if not m:
        errors.append('%s: no %s .. %s block' % (doc, START, END))
        continue
    new = text[:m.start()] + START + '\n' + block() + '\n' + END + text[m.end():]
    if new != text:
        if mode == '--check':
            errors.append('%s: the refusals block is not the generated one (scripts/gen-refusals.sh)' % doc)
        else:
            with open(doc, 'w', encoding='utf-8') as f:
                f.write(new)

if errors:
    print('\n'.join(errors), file=sys.stderr)
    sys.exit(1)
print('refusals: ok, %d slugs (%d limits), %d functions that name none' % (len(slugs), sum(1 for s in slugs if s[3]), len(allow)))
PY
