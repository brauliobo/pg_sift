#!/usr/bin/env python3
"""check-codec-bytes.py [--selftest] [TEST...]   an exact size of a sift index in an expected file is a size of the version of LZ4 and zstd that wrote it.

The default compression (auto) chooses LZ4 or zstd for each chunk by the size each library gives, and the bytes of both change from one release of the
library to the next (hosted Ubuntu and a development host differ by a page at 6000 rows). A regression test whose expected output prints the bytes or pages of
an index built so passes on the machine that wrote the file and fails on another. Lint fails on a test (test/sql/NAME.sql with test/expected/NAME.out) that
  - builds a sift_col index or a sift_table that does not pin a codec without a library (compression = 'none', or encodings only: 'for+delta+rle+dict+alp+scale'),
  - reads the size of one of them (pg_relation_size, pg_table_size, pg_indexes_size, pg_total_relation_size, sift.col_segments, col_leaves, col_index_info, col_space,
    col_sections, col_extents), and
  - prints a number above 1 in a column named bytes, kb, mb, size, pages or blocks (not of a heap).
A TAP test (test/t/*.sh) that builds such an index and compares a size to a number with == or -eq fails the same way.
Fix it in the test: pin the codec of its indexes (the expected file then gets the new sizes, once, and they are the same under every library). Never change the
number in the expected file to the one of the failing machine. A size that is no size of an index (the pages of a heap, a memory budget, a count named size) is listed in
scripts/codec-bytes.allow, one `file:column<TAB>hits<TAB>reason` a row: the exact number of cells, so a new one in the same column fails and is read."""
import glob
import os
import re
import sys

PINNED = {'none', 'for', 'delta', 'rle', 'dict', 'alp', 'scale'}
SIZE_COLUMN = re.compile(r'^(?!.*heap)(?:.*_)?(bytes?|nbytes|kb|mb|size|pages?|blocks)(?:_.*)?$', re.I)
SIZE_FUNC = re.compile(r"pg_(?:relation|table|total_relation|indexes)_size\(\s*'?\"?(\w+)")
SIFT_FUNC = re.compile(r"sift\.col_(?:segments|leaves|index_info|space|sections|extents)\(\s*'?\"?(\w+)")
CREATE = re.compile(r"create\s+(?:unique\s+)?(?:index|table)\s+(?:concurrently\s+)?(?:if\s+not\s+exists\s+)?([\w.]+)(.*?)(?=;|create\s+(?:unique\s+)?(?:index|table)\s|\Z)", re.I | re.S)
ON_TABLE = re.compile(r"\bon\s+(?:only\s+)?([\w.]+)", re.I)
USING = re.compile(r"using\s+sift_(?:col|table)\b", re.I)
COMPRESSION = re.compile(r"\bcompression\s*=\s*'([^']*)'", re.I)
TABLE_COMPRESSION = re.compile(r"sift\.table_compression\s*(?:=|to)\s*'([^']*)'", re.I)
SEP = re.compile(r'^-+(\+-+)*$')
ROW_END = re.compile(r'^\(\d+ rows?\)$')
RECORD = re.compile(r'^-\[ RECORD \d+ \]-+')
TAP_SIZE = re.compile(r'(pg_(?:relation|table|total_relation|indexes)_size|\bchunk_bytes\b|\bbytes\b).*(?:==|-eq)\s*"?\d{3,}')


def deterministic(spec):
    """True when no chunk of the policy goes through LZ4 or zstd: none, or the encodings alone, in every entry of 'default; column=policy'."""
    entries = [e.split('=', 1)[-1] for e in spec.split(';') if e.strip()]
    return bool(entries) and all(t.strip() in PINNED for e in entries for t in e.split('+'))


def sift_relations(sql):
    """(name, pinned, table) of every sift_col index and sift_table the text creates; the text of a format() string counts as a statement."""
    text = re.sub(r'--[^\n]*', '', sql).replace("''", "'")
    text = re.sub(r'\s+', ' ', text)
    table_pin = [deterministic(m.group(1)) for m in TABLE_COMPRESSION.finditer(text)]
    out = []
    for m in CREATE.finditer(text):
        window = m.group(2)[:600]
        if not USING.search(window):
            continue
        spec = COMPRESSION.search(window)
        on = ON_TABLE.search(window)
        is_index = bool(re.match(r'create\s+(unique\s+)?index', m.group(0), re.I))
        pinned = deterministic(spec.group(1)) if spec else (not is_index and bool(table_pin) and all(table_pin))
        out.append((m.group(1).split('.')[-1], pinned, on.group(1).split('.')[-1] if on and is_index else m.group(1).split('.')[-1]))
    return out


def size_sources(sql, relations):
    """The unpinned sift relations whose size the test reads."""
    text = re.sub(r'--[^\n]*', '', sql).replace("''", "'")
    unpinned = {n for n, pinned, _ in relations if not pinned}
    tables = {t for n, pinned, t in relations if not pinned}
    found = {m.group(1) for m in SIFT_FUNC.finditer(text) if m.group(1) in unpinned}
    for m in SIZE_FUNC.finditer(text):
        whole = m.group(0).startswith(('pg_indexes_size', 'pg_total_relation_size'))
        if m.group(1) in unpinned or (whole and m.group(1) in tables):
            found.add(m.group(1))
    return sorted(found)


def cells(line):
    return [c.strip() for c in line.split('|')]


def numbers(text):
    """Yield (line number, column, cell) of every number above 1 in a size column of the tables and records of an expected file."""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if RECORD.match(l):
            i += 1
            while i < len(lines) and lines[i].strip() and not RECORD.match(lines[i]):
                name, bar, val = lines[i].partition('|')
                if bar and SIZE_COLUMN.search(name.strip()) and re.fullmatch(r'-?\d+', val.strip()) and abs(int(val)) > 1:
                    yield (i + 1, name.strip(), val.strip())
                i += 1
            continue
        if i + 1 < len(lines) and SEP.match(lines[i + 1]) and l.strip() and not l.startswith(('--', '+')) and len(lines[i + 1]) >= len(l.rstrip()) - 1:
            names = cells(l)
            wide = [k for k, n in enumerate(names) if SIZE_COLUMN.search(n)]
            i += 2
            while i < len(lines) and lines[i].strip() and not ROW_END.match(lines[i]):
                row = cells(lines[i])
                if len(row) == len(names):
                    for k in wide:
                        if re.fullmatch(r'-?\d+', row[k]) and abs(int(row[k])) > 1:
                            yield (i + 1, names[k], row[k])
                i += 1
            continue
        i += 1


def scan(sql, expected):
    """([relations whose size is read], [(line, column, cell)]) of one regression test; the numbers only count when an unpinned relation is read."""
    relations = sift_relations(sql)
    sources = size_sources(sql, relations)
    return sources, list(numbers(expected)) if sources else []


def scan_tap(text):
    relations = sift_relations(text)
    if not any(not pinned for _, pinned, _ in relations):
        return []
    return [(n, l.strip()[:100]) for n, l in enumerate(text.split('\n'), 1) if TAP_SIZE.search(l)]


def allowed(root):
    p = os.path.join(root, 'scripts/codec-bytes.allow')
    out = {}
    if os.path.exists(p):
        for l in open(p):
            if l.strip() and not l.startswith('#'):
                k, _, rest = l.rstrip('\n').partition('\t')
                n, _, why = rest.partition('\t')
                if not n.isdigit() or not why.strip():
                    sys.exit(f'{p}: "{k}" wants file:column<TAB>hits<TAB>reason')
                out[k] = (int(n), why)
    return out


def selftest():
    table = ' relation | bytes \n----------+-------\n idx      |  8192\n(1 row)\n'
    create = "CREATE INDEX idx ON t USING sift_col (a);\nSELECT 'idx' AS relation, sum(bytes) AS bytes FROM sift.col_segments('idx');\n"
    cases = [  # (sql, expected, hits)
        (create, table, 1),
        (create.replace('(a);', "(a) WITH (compression = 'none');"), table, 0),
        (create.replace('(a);', "(a) WITH (compression = 'for+delta+rle+dict+alp+scale');"), table, 0),
        (create.replace('(a);', "(a) WITH (compression = 'auto');"), table, 1),
        (create.replace('(a);', "(a) WITH (compression = 'zstd:3');"), table, 1),
        (create.replace('(a);', "(a) WITH (compression = 'for+zstd');"), table, 1),
        (create.replace('(a);', "(a) WITH (compression = 'auto; w=zstd');"), table, 1),
        (create.replace('(a);', "(a) WITH (compression = 'none; w=lz4');"), table, 1),
        (create, ' relation | bytes \n----------+-------\n idx      |     1\n(1 row)\n', 0),   # a size of at most one is no page
        (create, ' relation | ok \n----------+----\n idx      | 8192\n(1 row)\n', 0),
        (create.replace('sift.col_segments', 'other'), table, 0),                            # the size of nothing of sift
        (create.replace("col_segments('idx')", "col_segments('other_idx')"), table, 0),
        (create.replace('USING sift_col', 'USING btree'), table, 0),                         # no sift index
        ("CREATE INDEX idx ON t USING sift_col (a) WITH (scope = 'k: a -> t(a, b)', compression = 'none');\nSELECT pg_relation_size('idx') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),
        ("CREATE INDEX idx ON t USING sift_col (a);\nSELECT pg_relation_size('idx') AS size;", ' size \n------\n 8192\n(1 row)\n', 1),
        ("CREATE INDEX idx ON t USING sift_col (a);\nSELECT pg_relation_size('t') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),    # the heap
        ("CREATE INDEX idx ON t USING sift_col (a);\nSELECT pg_total_relation_size('t') AS size;", ' size \n------\n 8192\n(1 row)\n', 1),
        ("CREATE TABLE s (a int) USING sift_table;\nSELECT pg_table_size('s') AS size;", ' size \n------\n 8192\n(1 row)\n', 1),
        ("SET sift.table_compression = 'none';\nCREATE TABLE s (a int) USING sift_table;\nSELECT pg_table_size('s') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),
        ("CREATE TABLE s (a int) USING sift_table WITH (compression = 'none');\nSELECT pg_table_size('s') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),
        ("-- CREATE INDEX idx ON t USING sift_col (a);\nSELECT pg_relation_size('idx') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),
        ("SELECT s FROM unnest(ARRAY['CREATE INDEX idx ON t USING sift_col (a) WITH (compression = ''none'')']) s \\gexec\n"
         "SELECT pg_relation_size('idx') AS size;", ' size \n------\n 8192\n(1 row)\n', 0),
        ("SELECT s FROM unnest(ARRAY['CREATE INDEX idx ON t USING sift_col (a)']) s \\gexec\nSELECT pg_relation_size('idx') AS size;", ' size \n------\n 8192\n(1 row)\n', 1),
        (create, '-[ RECORD 1 ]---\nrelation | idx\nbytes    | 8192\n', 1),
        (create, ' bytes \n-------\n  8192\n(1 row)\n\n bytes \n-------\n 16384\n(1 row)\n', 2),
    ]
    bad = 0
    for n, (sql, expected, want) in enumerate(cases):
        sources, hits = scan(sql, expected)
        if len(hits) != want:
            print(f'selftest case {n}: {len(hits)} hits, want {want}', file=sys.stderr)
            bad += 1
    tap = [("CREATE INDEX idx ON t USING sift_col (a)\ntest \"$(pg_relation_size idx)\" -eq 12288", 1),
           ("CREATE INDEX idx ON t USING sift_col (a) WITH (compression = 'none')\ntest \"$(pg_relation_size)\" -eq 12288", 0),
           ("CREATE INDEX idx ON t USING sift_col (a)\ntest \"$(sql x \"SELECT pg_relation_size('idx')\")\" -le 12288", 0),
           ("CREATE INDEX idx ON t USING sift_col (a)\ntest \"$(sql x \"SELECT pg_relation_size('idx')\")\" == 12288", 1)]
    for n, (text, want) in enumerate(tap):
        got = len(scan_tap(text))
        if got != want:
            print(f'selftest tap case {n}: {got} hits, want {want}', file=sys.stderr)
            bad += 1
    print('check-codec-bytes: selftest ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def read(path):
    return open(path, encoding='utf-8', errors='replace').read() if os.path.exists(path) else ''


def main(a):
    if a[:1] == ['--selftest']:
        return selftest()
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    names = a or sorted(os.path.basename(f)[:-4] for f in glob.glob('test/expected/*.out'))
    allow = allowed('.')
    seen, tap_hits, hits = {}, [], 0
    for t in names:
        sources, found = scan(read(f'test/sql/{t}.sql'), read(f'test/expected/{t}.out'))
        for n, col, cell in found:
            seen.setdefault(f'test/expected/{t}.out:{col}', []).append((n, cell, sources))
    for f in sorted(glob.glob('test/t/*.sh')) if not a else []:
        tap_hits += [(f, n, l) for n, l in scan_tap(read(f))]
    for k, h in seen.items():
        if k in allow and allow[k][0] == len(h):
            continue
        hits += len(h)
        path, _, col = k.rpartition(':')
        for n, cell, sources in h:
            print(f'{path}:{n}: column "{col}" = {cell} prints a size of {", ".join(sources)}, built with a compression that goes through LZ4 or zstd')
        if k in allow:
            print(f'{k}: scripts/codec-bytes.allow lists {allow[k][0]} hit(s), the file has {len(h)}')
    for f, n, l in tap_hits:
        hits += 1
        print(f'{f}:{n}: {l}: an exact size of an index built with a compression that goes through LZ4 or zstd')
    if not a:
        for k in allow:
            if k not in seen:
                hits += 1
                print(f'{k}: scripts/codec-bytes.allow lists a column that has no hit any more: remove the row')
    if hits:
        print(f'check-codec-bytes: {hits} hit(s): the bytes of LZ4 and zstd depend on the version of the library, so a test that prints the exact size of an index must pin the codec of its '
              "indexes in the test (compression = 'none', or encodings only: 'for+delta+rle+dict+alp+scale'; for a sift_table SET sift.table_compression the same way); do not change the expected "
              'number to the one of the machine that fails. A column that is no size of an index is listed in scripts/codec-bytes.allow with the exact number of cells and the reason', file=sys.stderr)
    return 1 if hits else 0


sys.exit(main(sys.argv[1:]))
