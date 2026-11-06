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
slugs = []     # (ID, slug, sentence, limit, by_design, action)
for m in re.finditer(r'^\tX\((\w+), "([a-z_]+)", "((?:[^"\\]|\\.)*)", ([01]), ([01]), "((?:[^"\\]|\\.)*)"\)', read('src/agg/refuse.h'), re.M):
    slugs.append((m.group(1), m.group(2), m.group(3), m.group(4) == '1', m.group(5) == '1', m.group(6)))
if not slugs:
    sys.exit('gen-refusals: no row found in the SIFT_AGG_REFUSALS table of src/agg/refuse.h')


def block():
    rows = ['| slug | class | by design | the query kept the normal plan because | what to do |', '|---|---|---|---|---|']
    for _, slug, text, limit, by_design, action in slugs:
        rows.append('| `%s` | %s | %s | %s | %s |' % (slug, 'limit' if limit else 'semantic', 'yes' if by_design else '', text.replace('\\"', '"'), action.replace('\\"', '"')))
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
# the folders of src/agg/ hold the planner files that were split (cost, from, expr, plan ...); NO_REFUSAL_DIRS hold the code of the scan itself and the passes that rewrite a query before it is planned (flat), which name no refusal
NO_REFUSAL_DIRS = {'blob', 'cte', 'dist', 'exec', 'flat', 'combo', 'group', 'lookup', 'part', 'preagg', 'topn', 'update'}
folders = [d for d in glob.glob('src/agg/*/') if os.path.basename(d.rstrip('/')) not in NO_REFUSAL_DIRS]
for path in sorted(glob.glob('src/agg/plan*.c') + glob.glob('src/agg/spec*.c') + [f for d in folders for f in glob.glob(d + '*.c')]):
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
for path in glob.glob('src/agg/**/*.c', recursive=True):
    used.update(re.findall(r'\bagg_(?:refuse|refused|refuse_note|report)\w*\(\s*(?:"[^"]*",\s*)?(?:AGG_REFUSE_)?([A-Z_]+)', read(path)))
    used.update(re.findall(r'\bAGG_REFUSE_([A-Z_]+)', read(path)))
for id_, slug, _, _, _, _ in slugs:
    if id_ != 'OTHER' and id_ not in used:
        errors.append('src/agg/refuse.h: the slug %s has no site in src/agg' % slug)

# ---- the EXPLAIN labels: every label the library raises is named in a document (README.md or docs/, but not the results, the perf notes and the release notes) ----
labels = set()
for path in glob.glob('src/**/*.[ch]', recursive=True):
    if re.search(r'(^|/)(test\.c|[^/]*_test[^/]*\.[ch])$', path):
        continue
    labels.update(re.findall(r'\bExplainProperty(?:Integer|Text|Float|Bool|Unit|Bytes)?\(\s*"([^"]+)"', read(path)))
    labels.update(re.findall(r'\bsift_explain_(?:begin\([^,]+,[^,]+|int\([^,]+|ms\([^,]+),\s*"([^"]+)"', read(path)))
docs = read('README.md')
for d, _, fs in os.walk('docs'):
    if re.match(r'docs/(results|perf|releases)', d):
        continue
    for f in fs:
        if f.endswith('.md'):
            docs += read(os.path.join(d, f))
for label in sorted(labels):
    if label not in docs:
        errors.append('src/: the EXPLAIN label "%s" is named in no document (a row of the table "EXPLAIN labels" in docs/COL-PLAN.md)' % label)

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
