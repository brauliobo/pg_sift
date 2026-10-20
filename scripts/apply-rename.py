#!/usr/bin/env python3
"""Apply the naming vocabulary (rename-map.tsv, next to this file or --map / $SIFT_RENAME_MAP) to a tree: deterministic, whole-word, idempotent.

  apply-rename.py [--check] [--list] DIR

Modes of a row (column `mode`):
  X  as W, the old name is a regular expression (a prefix of names)
  W  a token that is not English prose: replaced everywhere in the text files of test/ bench/ tools/ scripts/ .github/ sql/ docs/
  C  an English word that is also a column or key: replaced in code only (test/ bench/ sql/ .sql .out .spec .tsv .py .sh), never in a comment line, never
     in a PostgreSQL message line of an expected output (ERROR:, NOTICE:, ...), never in a trailing `--` comment
  F  as C, and not when a `(` follows at once (the name of a function)
  Q  an English value: between quotes in code; in an expected output (which prints it unquoted) only where the test source quotes it
The column `skip` lists path globs a row never touches. --check changes nothing: it prints the occurrences of old names and exits 1 if there are any.
"""
import csv, fnmatch, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEXT = ('.sql', '.out', '.py', '.sh', '.tsv', '.spec', '.pl', '.pm', '.txt', '.md', '.yml', '.yaml', '.json', '.c', '.h', '.mk', '.service', '.cfg', '.env', '')
ROOTS = ('test/', 'bench/', 'tools/', 'scripts/', '.github/', 'sql/', 'docs/')
CODE_ROOTS = ('test/', 'bench/', 'sql/', 'scripts/coldiff.sh')    # coldiff.sh holds the index definitions the fixtures are checked against
SRC_TEST = re.compile(r'^src/(.*/)?(test/|[a-z0-9_]*_test\.[ch]$|test\.c$)')
MSG = re.compile(r'^(ERROR|DETAIL|HINT|NOTICE|WARNING|CONTEXT|LINE \d+|STATEMENT|PANIC|FATAL|INFO|LOG):')
WORD = r'(?<![A-Za-z0-9_-]){}(?![A-Za-z0-9_])'


def load(path):
    rows = []
    for r in csv.DictReader(open(path), delimiter='\t'):
        skip = [g for g in r['skip'].split(';') if g]
        o = re.escape(r['old'])
        if r['mode'] == 'X':
            rx = (re.compile(r['old']), r['new'])
            raw = None
        elif r['mode'] == 'Q':
            rx = (re.compile(r'([\'"])' + o + r'\1'), r'\1' + r['new'] + r'\1')
            raw = re.compile(r'(?<![A-Za-z0-9_-])' + o + r'(?![A-Za-z0-9_-])')
        else:
            rx = (re.compile(WORD.format(o) + (r'(?!\()' if r['mode'] == 'F' else '')), r['new'])
            raw = None
        rows.append(dict(old=r['old'], new=r['new'], mode=r['mode'], skip=skip, rx=rx, raw=raw, pat=r['old'] if r['mode'] == 'X' else o))
    global ANY
    ANY = re.compile('|'.join(r['pat'] for r in rows))
    return rows


def is_comment(l, path):
    s = l.lstrip()
    if path.endswith(('.sql', '.out', '.spec')):
        return s.startswith(('--', '/*', '*'))
    return s.startswith('#')


def code_end(l):
    q = None
    for i, c in enumerate(l):
        if q:
            if c == q:
                q = None
        elif c in '\'"':
            q = c
        elif c == '-' and l.startswith('--', i):
            return i
    return len(l)


def sibling_of(path):
    m = re.fullmatch(r'test/expected/(.+?)(?:_\d+)?\.out', path)
    if m:
        return f'test/sql/{m[1]}.sql'
    m = re.fullmatch(r'test/isolation/expected/(.+?)(?:_\d+)?\.out', path)
    return f'test/isolation/specs/{m[1]}.spec' if m else None


def skipped(row, path):
    return any(fnmatch.fnmatch(path, g) for g in row['skip'])


NUM = re.compile(r'^[-+]?(\d[\d.,]*|\.\d+|NaN|Infinity)([eE][-+]?\d+)?$')
FOOT = re.compile(r'^\(\d+ rows?\)$')
SEP = re.compile(r'^-+(\+-+)*$')


def reflow(block, ren):
    """block: [header, separator, rows...] of an aligned psql table as printed before the renames; the lines of the table after them, with the widths of the
    columns recomputed (header centered, numbers to the right, the rest to the left). None when the block is not a plain table (multi-line cells, wide characters)."""
    head, sep, rows = block[0], block[1], block[2:]
    w = [len(x) - 2 for x in sep.split('+')]
    off = [sum(v + 3 for v in w[:i]) for i in range(len(w))]
    n = len(w)

    def cut(line, i, last_ok=False):
        seg = line[off[i] + 1:off[i] + 1 + w[i]]
        if i < n - 1:
            if len(line) <= off[i] + w[i] + 2 or line[off[i] + w[i] + 2] != '|' or line[off[i]] != ' ' or line[off[i] + w[i] + 1] != ' ':
                raise ValueError
        elif len(line) > off[i] + w[i] + 2 or (len(line) == off[i] + w[i] + 2 and line[-1] != ' '):
            raise ValueError
        return seg
    try:
        if not all(l.isascii() for l in block):
            return None
        hs = [cut(head, i).strip() for i in range(n)]
        cells = [[cut(r, i) for i in range(n)] for r in rows]
    except ValueError:
        return None
    align = []
    for i in range(n):
        col = [c[i] for c in cells]
        vals = [x.strip() for x in col if x.strip()]
        left_ev = any(x and x[0] != ' ' and len(x.rstrip(' ')) < w[i] for x in col)
        # a column of NULLs: the last one shows its alignment in the length of the line (a right-aligned NULL is padded)
        empty_pad = i == n - 1 and not vals and any(len(r) > off[i] + 1 for r in rows)
        align.append('right' if (vals and all(NUM.match(v) for v in vals) and not left_ev) or empty_pad else 'left')
    raw = (hs, cells)
    hs = [ren(x) for x in hs]
    cells = [[ren(x.rstrip(' ') if al == 'left' else x.strip(' '), True) for x, al in zip(r, align)] for r in cells]
    if hs == raw[0] and all(a == [x.rstrip(' ') if al == 'left' else x.strip(' ') for x, al in zip(b, align)] for a, b in zip(cells, raw[1])):
        return list(block)                            # nothing renamed in the table: it stays as the server printed it
    nw = [max([len(hs[i])] + [len(r[i]) for r in cells]) for i in range(n)]

    def hdr(t, k):
        left = (nw[k] - len(t)) // 2
        return ' ' + ' ' * left + t + ' ' * (nw[k] - len(t) - left) + ' '
    out = ['|'.join(hdr(t, k) for k, t in enumerate(hs)), '+'.join('-' * (v + 2) for v in nw)]
    for r in cells:
        parts = []
        for k, t in enumerate(r):
            pad = t.rjust(nw[k]) if align[k] == 'right' else t.ljust(nw[k])
            parts.append(' ' + (pad.rstrip(' ') if k == n - 1 and align[k] == 'left' else pad) + ('' if k == n - 1 else ' '))
        out.append('|'.join(parts))
    return out


def in_scope(path):
    # everything but the extension source, which names PostgreSQL's own fields (a test build's own files are in scope)
    if path.startswith('sql/pg_sift'):
        return False                                     # the released install and upgrade scripts are checksummed
    return not path.startswith('src/') or bool(SRC_TEST.search(path))


def apply(rows, path, text, sibling=None):
    if not in_scope(path) or not ANY.search(text):
        return text
    ws = [r for r in rows if r['mode'] in 'WX' and not skipped(r, path)]

    def wren(t):
        for r in ws:
            t = r['rx'][0].sub(r['rx'][1], t)
        return t
    if not path.startswith(CODE_ROOTS) or path.endswith(('.c', '.h', '.md', '.yml')):
        return wren(text)
    cs = [r for r in rows if r['mode'] in 'CF' and not skipped(r, path)]
    qs = [r for r in rows if r['mode'] == 'Q' and not skipped(r, path)]
    is_out = path.endswith('.out')
    quoted = set(re.findall(r'[\'"](\w+)[\'"]', sibling or ''))
    sqlish = path.endswith(('.sql', '.out', '.spec'))

    def ren(l, cell=False):
        l = wren(l)
        for r in cs:
            if r['old'] in l:
                l = r['rx'][0].sub(r['rx'][1], l)
        for r in qs:
            if r['old'] in l:
                l = r['rx'][0].sub(r['rx'][1], l)
            # an expected output prints a value without quotes in a cell; the header of a column is its alias, as the test source wrote it
            if is_out and cell and r['old'] in quoted and r['old'] in l:
                l = r['raw'].sub(r['new'], l)
        return l
    lines = text.split('\n')
    done = [None] * len(lines)
    if is_out:
        # aligned tables: the text of the cells is renamed and the columns are laid out again
        k = 0
        while k < len(lines):
            if SEP.match(lines[k]) and k > 0 and len(lines[k]) > 2:
                e = k + 1
                while e < len(lines) and not FOOT.match(lines[e]) and lines[e] != '':
                    e += 1
                if e < len(lines) and FOOT.match(lines[e]):
                    out = reflow(lines[k - 1:e], ren)
                    if out is not None:
                        done[k - 1:e] = out
                        # same number of lines, so the indices stay
                    k = e
            k += 1
    res = []
    for i, l in enumerate(lines):
        if done[i] is not None:
            res.append(done[i])
            continue
        if not l.strip() or is_comment(l, path) or (is_out and MSG.match(l)):
            res.append(wren(l))
            continue
        tail = ''
        if sqlish:
            k = code_end(l)
            l, tail = l[:k], l[k:]
        res.append(ren(l) + wren(tail))
    return '\n'.join(res)


def files(root):
    try:
        out = subprocess.run(['git', '-C', root, 'ls-files', '-z'], capture_output=True, check=True).stdout.decode().split('\0')
        return sorted(p for p in out if p)
    except Exception:
        return sorted(os.path.relpath(os.path.join(d, f), root) for d, ds, fs in os.walk(root) if '.git' not in d.split(os.sep) for f in fs)


def main(argv):
    mp = os.environ.get('SIFT_RENAME_MAP', str(HERE / 'rename-map.tsv'))
    check = '--check' in argv
    args = []
    it = iter(argv)
    for a in it:
        if a == '--map':
            mp = next(it)
        elif not a.startswith('--'):
            args.append(a)
    if len(args) != 1 or not Path(mp).exists():
        sys.exit(__doc__ if len(args) != 1 else f'apply-rename: no map at {mp}')
    root = args[0]
    rows = load(mp)
    changed = 0
    for p in files(root):
        if not in_scope(p) or not p.endswith(TEXT) and '.' in p.rsplit('/', 1)[-1]:
            continue
        fp = os.path.join(root, p)
        if not os.path.isfile(fp) or os.path.islink(fp):
            continue
        raw = open(fp, 'rb').read()
        if b'\0' in raw:
            continue
        text = raw.decode('utf-8', 'surrogateescape')
        sib = sibling_of(p)
        sibt = open(os.path.join(root, sib), errors='replace').read() if sib and os.path.exists(os.path.join(root, sib)) else None
        new = apply(rows, p, text, sibt)
        if new != text:
            changed += 1
            if check:
                print(p)
            else:
                open(fp, 'wb').write(new.encode('utf-8', 'surrogateescape'))
    print(f'apply-rename: {changed} file(s) {"with old names" if check else "changed"}', file=sys.stderr)
    return 1 if check and changed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
