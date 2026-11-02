#!/usr/bin/env python3
"""Regenerate every generated part of README.md and docs/ from the code, the tests and the committed benchmark results.

    scripts/gen-docs.py            rewrite the generated blocks in place
    scripts/gen-docs.py --check    fail (exit 1) when a block is stale; renders into a scratch copy, touches nothing, needs no server
    scripts/gen-docs.py --resolve  in a rebase or merge with conflicts: settle the generated files (see resolve()), then regenerate; run `git add -A` and continue

The options regions (scripts/gen-options.sh) are part of it, so `--check` covers them.
Nothing here measures: a benchmark number comes from docs/results/ (written by `bench/bench ... --docs`, which runs this too), the rest from the tree.
The parts:
  <!-- bench:NAME:begin --> ... <!-- bench:NAME:end -->   the benchmark renderers of bench/steps (readme.py, extras.py, store.py, highlights.py, aggregates_page.py; charts.py
                                                          draws docs/img); a block whose
                                                          results file is missing or empty says it is not measured yet
  <!-- coverage:start --> ... <!-- coverage:end -->        scripts/coverage.sh --gen, from docs/coverage.tsv (also writes docs/COVERAGE.md)
  <!-- gen:NAME --> ... <!-- /gen:NAME -->                 gucs: the settings table (DefineCustom*Variable calls of src/); guc:sift.NAME: the default of
                                                          one setting, inline
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# the results file behind each bench block; a block with a missing file is not rendered, it says so
RESULTS = {n: n + '.tsv' for n in ('sets', 'rollup', 'tablewrites', 'writepath', 'bm25size', 'leaftypes', 'fields', 'buildcost', 'gpu', 'duckdb')}
RESULTS.update({'store': 'store.json', 'store-updates': 'store.json', 'store-compression': 'store-compression.json',
                'slim': 'articles.tsv', 'readme': 'articles.tsv', 'summary': 'articles.tsv'})
BENCH_CMD = dict.fromkeys(('store', 'store-updates', 'store-compression'), 'bench/bench store all --docs') | dict.fromkeys(('slim', 'readme', 'summary'), 'bench/bench all --docs') | {'duckdb': 'bench/bench duckdb --docs'}

CONTEXT = {'PGC_USERSET': 'any user', 'PGC_SUSET': 'superuser', 'PGC_SIGHUP': 'reload', 'PGC_POSTMASTER': 'restart', 'PGC_BACKEND': 'connection', 'PGC_SU_BACKEND': 'connection'}
# argument positions after the name: (short, long, var, boot, ...) and where the context and the bounds are
SIGNATURE = {'Int': dict(lo=5, hi=6, ctx=7, flags=8), 'Real': dict(lo=5, hi=6, ctx=7, flags=8), 'Bool': dict(ctx=5, flags=6),
             'String': dict(ctx=5, flags=6), 'Enum': dict(opts=5, ctx=6, flags=7), 'HostEnum': dict(opts=5)}
OPEN_END = {'INT_MAX', 'MAX_KILOBYTES', 'PG_INT32_MAX'}


def docs_files(root):
    return [os.path.join(root, 'README.md')] + sorted(os.path.join(d, f) for d, _, fs in os.walk(os.path.join(root, 'docs')) for f in fs if f.endswith('.md'))


def read(path):
    with open(path) as f:
        return f.read()


def write_if_changed(path, text):
    if read(path) != text:
        with open(path, 'w') as f:
            f.write(text)


# ---- settings from the C source ---------------------------------------------------------------------------------------------------------

def c_files(root):
    return [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(root, 'src')) for f in sorted(fs) if f.endswith(('.c', '.h'))]


def split_args(text, i):
    """the top level arguments of the call whose '(' ends at text[i-1]"""
    depth, args, cur, quoted = 1, [], '', False
    while depth:
        c = text[i]
        if quoted:
            cur += c
            if c == '\\':
                i += 1
                cur += text[i]
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted, cur = True, cur + c
        elif c in '([{':
            depth, cur = depth + 1, cur + c
        elif c in ')]}':
            depth -= 1
            cur += c if depth else ''
        elif c == ',' and depth == 1:
            args.append(cur.strip())
            cur = ''
        else:
            cur += c
        i += 1
    return args + [cur.strip()]


def string_of(tok):
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', tok)
    return re.sub(r'\\(.)', lambda m: ' ' if m.group(1) == 'n' else m.group(1), ''.join(parts)) if parts else ''


def number_of(tok, defs, depth=0):
    tok = tok.strip()
    if tok in defs and depth < 5:
        return number_of(defs[tok], defs, depth + 1)
    tok = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: defs.get(m.group(0), m.group(0)) if depth < 5 else m.group(0), tok)
    if re.fullmatch(r'[0-9+\-*/()< ]+', tok):
        return int(eval(tok.replace('/', '//'), {'__builtins__': {}}))   # digits and operators only
    return None


def fmt_int(v, flags):
    if 'GUC_UNIT_KB' in flags:
        return '%d MB' % (v // 1024) if v and v % 1024 == 0 else '%d kB' % v
    if 'GUC_UNIT_MS' in flags:
        return '%d s' % (v // 1000) if v and v % 1000 == 0 else '%d ms' % v
    if 'GUC_UNIT_S' in flags:
        return '%d s' % v
    return format(v, ',') if abs(v) >= 10000 else str(v)


def settings(root):
    """the sift.* settings of src/ (without the test hooks), by name: kind, default, range, context, description"""
    texts = [read(f) for f in c_files(root)]
    defs = dict(m.groups() for t in texts for m in re.finditer(r'^#define\s+(\w+)\s+([^\n/]+?)\s*(?:/\*.*)?$', t, re.M))
    for t in texts:   # enum constants count as numbers (C numbering: from 0, or from the last explicit value)
        for body in re.findall(r'\benum\s*\w*\s*\{([^}]*)\}', t):
            n = 0
            for item in re.sub(r'/\*.*?\*/', '', body, flags=re.S).split(','):
                name, _, val = item.partition('=')
                if name.strip():
                    n = number_of(val, defs) if val.strip() and number_of(val, defs) is not None else n
                    defs.setdefault(name.strip(), str(n))
                    n += 1
    out = {}
    for t in texts:
        for m in re.finditer(r'\b(?:DefineCustom(Int|Bool|Real|String|Enum)Variable|(sift_host_enum_option))\s*\(', t):
            kind = m.group(1) or 'HostEnum'
            a = split_args(t, m.end())
            if len(a) < 5 or not a[0].startswith('"sift.') or a[0].startswith('"sift.test_'):
                continue
            sig = SIGNATURE[kind]
            flags = a[sig['flags']] if 'flags' in sig else ''
            name, boot = string_of(a[0]), a[4]
            rng = ''
            if kind in ('Int', 'Real'):
                lo, hi = number_of(a[sig['lo']], defs), number_of(a[sig['hi']], defs)
                dflt = fmt_int(number_of(boot, defs), flags) if kind == 'Int' and number_of(boot, defs) is not None else boot
                if lo is not None:
                    rng = ('at least %s' if a[sig['hi']] in OPEN_END or hi is None else '%s to ' + fmt_int(hi, flags)) % fmt_int(lo, flags)
            elif kind == 'Bool':
                dflt = 'on' if boot == 'true' else 'off'
            elif kind == 'String':
                dflt = string_of(boot) or 'empty'
            else:
                table = re.search(r'\b%s\s*\[\]\s*=\s*\{(.*?)\n\};' % re.escape(a[sig['opts']]), t, re.S)
                entries = [(n, v) for n, v, hidden in re.findall(r'\{"(\w+)",\s*([\w ]+?),\s*(\w+)\}', table.group(1)) if hidden != 'true'] if table else []
                dflt = next((n for n, v in reversed(entries) if number_of(v, defs) == number_of(boot, defs)), boot)   # the last label of the value: auto
                rng = ', '.join('`%s`' % n for n, _ in entries)
            desc = ' '.join(s for s in (string_of(a[1]), string_of(a[2])) if s)
            out[name] = dict(default=dflt, range=rng, context=CONTEXT.get(a[sig['ctx']] if 'ctx' in sig else 'PGC_USERSET', 'any user'), desc=desc)
    return out


def gucs_table(root):
    esc = lambda s: s.replace('|', '\\|')
    rows = ['| setting | default | range | set by | meaning |', '|---|---|---|---|---|']
    for name, s in sorted(settings(root).items()):
        rows.append('| `%s` | %s | %s | %s | %s |' % (name, esc(s['default']), esc(s['range']), s['context'], esc(s['desc'])))
    return '\n'.join(rows)


# ---- the parity of the two column engines -----------------------------------------------------------------------------------------------

def providers(root):
    """the callbacks each SiftSrcProvider initializer of the tree sets: {'sift_col': {...}, 'sift_table': {...}}"""
    out = {}
    for engine, path in (('sift_col', 'src/agg/col.c'), ('sift_table', 'src/table/src.c')):
        m = re.search(r'static const SiftSrcProvider \w+ = \{(.*?)\n\};', read(os.path.join(root, path)), re.S)
        out[engine] = set(re.findall(r'^\s*\.(\w+)\s*=', m.group(1), re.M))
    return out


def parity_table(root):
    """docs/parity.tsv as a table; a status that the providers contradict (works without every callback of the row, missing with one), or a callback only sift_col sets that no row lists, stops the run"""
    have = providers(root)
    rows = [l.split('\t') for l in read(os.path.join(root, 'docs', 'parity.tsv')).splitlines() if l.strip() and not l.startswith('#')]
    head, rows = rows[0], rows[1:]
    assert head == ['feature', 'sift_col', 'sift_table', 'seam', 'note'], 'docs/parity.tsv: unexpected header'
    out = ['| Feature | `sift_col` | `sift_table` | Note |', '|---|---|---|---|']
    listed = set()
    for feat, col, tbl, seam, note in rows:
        for engine, status in (('sift_col', col), ('sift_table', tbl)):
            if status not in ('works', 'partial', 'missing', 'n/a'):
                sys.exit('gen-docs: docs/parity.tsv: "%s": status "%s" of %s' % (feat, status, engine))
            cbs = [] if seam == '-' else seam.split('+')
            for cb in cbs:
                if cb not in have[engine] and cb not in have['sift_col'] | have['sift_table']:
                    sys.exit('gen-docs: docs/parity.tsv: "%s": no provider sets the callback %s' % (feat, cb))
            present = [cb for cb in cbs if cb in have[engine]]
            if cbs and status == 'works' and len(present) != len(cbs):
                sys.exit('gen-docs: docs/parity.tsv: "%s" is works for %s but its provider lacks %s' % (feat, engine, ', '.join(sorted(set(cbs) - set(present)))))
            if cbs and status == 'missing' and present:
                sys.exit('gen-docs: docs/parity.tsv: "%s" is missing for %s but its provider sets %s' % (feat, engine, ', '.join(present)))
        listed |= set(cbs)
        out.append('| %s | %s | %s | %s |' % (feat, col, tbl, note))
    unlisted = sorted(have['sift_col'] - have['sift_table'] - listed)
    if unlisted:
        sys.exit('gen-docs: docs/parity.tsv: sift_col sets %s, which no row lists: add the feature (a callback only the index sets is a feature the table lacks)' % ', '.join(unlisted))
    return '\n'.join(out)


# ---- the blocks --------------------------------------------------------------------------------------------------------------------------

def fill_gen_blocks(root):
    cfg = {}

    def text(name):
        if name == 'gucs':
            cfg.setdefault('gucs', gucs_table(root))
            return '\n' + cfg['gucs'] + '\n'
        if name == 'parity':
            return '\n\n' + parity_table(root) + '\n\n'
        if name.startswith('guc:'):
            s = settings(root).get(name[4:])
            if s is None:
                sys.exit('gen-docs: no setting %s in src/' % name[4:])
            return s['default']
        sys.exit('gen-docs: unknown block gen:%s' % name)

    pat = re.compile(r'(<!-- gen:(\S+)(?: [^\n]*?)? -->)(.*?)(<!-- /gen:\2 -->)', re.S)
    for path in docs_files(root):
        old = read(path)
        write_if_changed(path, pat.sub(lambda m: m.group(1) + text(m.group(2)) + m.group(4), old))


def have(root, name):
    p = os.path.join(root, 'docs', 'results', RESULTS[name])
    if not os.path.exists(p):
        return False
    rows = [l for l in read(p).splitlines() if l.strip() and not l.startswith('#')]
    return len(rows) > (1 if p.endswith('.tsv') else 0)   # a tsv has its header line


def fill_missing_bench_blocks(root):
    """a bench block whose results file is missing or empty says so, instead of keeping numbers that no file backs"""
    sys.path.insert(0, os.path.join(root, 'bench', 'steps'))
    from readme import replace_block
    for path in docs_files(root):
        for name in re.findall(r'<!-- bench:([\w-]+):begin', read(path)):
            if name in RESULTS and not have(root, name):
                replace_block(path, name, ['Not measured yet: `%s` writes docs/results/%s and this block.' % (BENCH_CMD.get(name, 'bench/bench extras %s --docs' % name), RESULTS[name])])


# what the benchmark renderers read and write (checked with strace): a tree whose files here are as a finished run left them gives the same files again, so a write skips them
RENDERER_WORLD = ('bench', 'docs', 'README.md', 'CHANGELOG.md', 'pg_sift.control', 'mariadb/README.md', 'sqlite/README.md', 'scripts/gen-docs.py', 'scripts/genlib.py')


BENCH_BLOCK = re.compile(r'<!-- bench:[\w-]+:begin.*?<!-- bench:[\w-]+:end', re.S)


GENERATED_PAGES = (b'docs/perf/AGGREGATES.md',)   # pages a renderer writes whole (the others it only replaces benchmark blocks in)


def renderer_view(name, data):
    """what the renderers see of a file, None when nothing: a page is only the benchmark blocks they replace (a change of its prose gives the same files again), a page with no
    block and not written whole is not theirs (strace of each renderer: README.md, docs/BENCHMARKS.md, the READMEs of the ports, docs/perf/AGGREGATES.md and docs/img/)"""
    if name.endswith(b'.md') and name != b'CHANGELOG.md' and name not in GENERATED_PAGES:
        blocks = BENCH_BLOCK.findall(data.decode('utf-8', 'replace'))
        return '\0'.join(blocks).encode() if blocks else None
    return data


def world_stamp(root):
    """the stamp of the files the renderers read and write, or None when the write is not to be cached (CI, the check, SIFT_LINT_NOCACHE, no cache directory)"""
    if root != REPO or os.environ.get('CI') or os.environ.get('SIFT_LINT_NOCACHE'):
        return None
    d = os.environ.get('SIFT_GEN_DOCS_CACHE') or os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'pg_sift', 'gen-docs')
    try:
        os.makedirs(d, exist_ok=True)
        names = subprocess.run(['git', '-C', root, 'ls-files', '-z', '-co', '--exclude-standard', '--', *RENDERER_WORLD], check=True, capture_output=True).stdout.split(b'\0')[:-1]
    except (OSError, subprocess.CalledProcessError):
        return None
    h = hashlib.sha256()
    for n in sorted(names):
        try:
            with open(os.path.join(root, os.fsdecode(n)), 'rb') as f:
                view = renderer_view(n, f.read())
        except OSError:
            view = b''
        if view is not None:
            h.update(n + b'\0' + view + b'\0')
    return os.path.join(d, h.hexdigest()[:40])


def generate(root):
    def run(*cmd):   # the renderers log to stderr; it is shown only when one fails
        p = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        if p.returncode:
            sys.exit('gen-docs: %s failed:\n%s' % (' '.join(cmd), p.stderr or p.stdout))

    steps = os.path.join('bench', 'steps')
    py = ['python3', '-B', '-I']
    stamp = world_stamp(root)
    if stamp and os.path.exists(stamp):
        print('gen-docs: the benchmark renderers are skipped (their files are as a run left them)', file=sys.stderr)
    else:
        if have(root, 'slim'):
            run(*py, os.path.join(steps, 'readme.py'))
        if have(root, 'store'):
            run(*py, os.path.join(steps, 'store.py'), 'render', '--readme', 'README.md', '--benchmarks', 'docs/BENCHMARKS.md')
        run(*py, os.path.join(steps, 'extras.py'), 'render')
        if have(root, 'slim'):
            run(*py, os.path.join(steps, 'charts.py'))
        if have(root, 'store'):
            run(*py, os.path.join(steps, 'highlights.py'))
            run(*py, os.path.join(steps, 'aggregates_page.py'))
        run(*py, os.path.join(steps, 'release_record.py'))
    run('bash', os.path.join('scripts', 'coverage.sh'), '--gen')
    run('bash', os.path.join('scripts', 'gen-options.sh'))   # the options regions of README.md, docs/USAGE.md and docs/OPTIONS.md (docs/options.tsv and src/)
    fill_missing_bench_blocks(root)
    fill_gen_blocks(root)
    stamp = world_stamp(root)   # the files as this run leaves them: the next run that finds them so skips the renderers
    if stamp:
        open(stamp, 'w').close()


UNPUBLISHED = ('docs/COVERAGE.md',)   # generated for the maintainers, left out of the public tree (public-repo-allowlist): a checkout without it does not compare it, and says so


def check():
    with tempfile.TemporaryDirectory(prefix='sift-gen-docs.') as tmp:
        files = subprocess.run(['git', '-C', REPO, 'ls-files', '-z'], check=True, capture_output=True).stdout.split(b'\0')[:-1]
        for f in files:
            src, dst = os.path.join(REPO, os.fsdecode(f)), os.path.join(tmp, os.fsdecode(f))
            if os.path.isfile(src):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
        generate(tmp)
        stale, unpublished = [], []
        for d, _, fs in os.walk(tmp):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), tmp)
                if rel == 'README.md' or rel.startswith('docs' + os.sep):
                    other = os.path.join(REPO, rel)
                    if rel in UNPUBLISHED and not os.path.exists(other):
                        unpublished.append(rel)
                    elif not os.path.exists(other) or open(other, 'rb').read() != open(os.path.join(d, f), 'rb').read():
                        stale.append(rel)
    if stale:
        sys.exit('gen-docs: stale (a generated block no longer matches its source): %s\nrun scripts/gen-docs.py and commit the result' % ', '.join(sorted(stale)))
    print('gen-docs: every generated block is current' + ('; not compared, the page is not in this checkout: %s' % ', '.join(unpublished) if unpublished else ''))


ROW_KEY = {'docs/results/cases.tsv': 4, 'docs/results/cases-scale.tsv': 4, 'docs/results/cases-time.tsv': 2, 'bench/cases.tsv': 1, 'scripts/bench-cases.allow': 1}   # the leading columns that name a row of the files cases add to
OPEN = re.compile(r'<!-- (?:bench:[\w-]+:begin|coverage:start|sift-[\w-]+:start)')   # the markers of the generated regions of README.md and docs/
CLOSE = re.compile(r'<!-- (?:bench:[\w-]+:end|coverage:end|sift-[\w-]+:end)')
BLOCK = re.compile(OPEN.pattern + '|' + CLOSE.pattern + r'|<!-- /?gen:')


def region_after(inblock, lines):
    """whether the text ends inside a generated region, given whether it began in one"""
    for l in lines:
        if OPEN.search(l):
            inblock = True
        elif CLOSE.search(l):
            inblock = False
    return inblock


def git_out(*a):
    return subprocess.run(['git', '-C', REPO, *a], capture_output=True, text=True).stdout


def stage(path, n):
    return git_out('show', ':%d:%s' % (n, path))


def merge_rows(path, key):
    """the tip's rows, with the rows the branch changed or added (against the merge base) on top and the rows it deleted gone; a rebase has the tip as stage 2 and the branch as stage 3, a merge the reverse"""
    tip, mine = (2, 3) if rebasing() else (3, 2)
    side = lambda n: ([l for l in stage(path, n).splitlines() if l.strip()])
    ident = lambda l: tuple(l.split('\t')[:key]) if not l.startswith('#') else None
    base = {ident(l): l for l in side(1) if ident(l)}
    out = side(tip)
    at = {ident(l): i for i, l in enumerate(out) if ident(l)}
    kept = set()
    for l in side(mine):
        k = ident(l)
        kept.add(k)
        if k is None or base.get(k) == l:
            continue
        if k in at:
            out[at[k]] = l
        else:
            at[k] = len(out)
            out.append(l)
    gone = {k for k, l in base.items() if k not in kept}   # the branch deleted them: they go unless the tip changed them
    out = [l for l in out if not (ident(l) in gone and base[ident(l)] == l)]
    write_if_changed(os.path.join(REPO, path), '\n'.join(out) + '\n')


def rebasing():
    return any(os.path.isdir(os.path.join(REPO, git_out('rev-parse', '--git-path', d).strip())) for d in ('rebase-merge', 'rebase-apply'))


def take_tip_hunks(path):
    """the conflict hunks inside (or touching) a generated block take the tip's side; a hunk in prose stays a conflict and the path is returned"""
    tip_first = rebasing()   # the first side of the markers is the tip in a rebase
    out, lines, i, inblock, left = [], read(os.path.join(REPO, path)).splitlines(True), 0, False, False
    while i < len(lines):
        if not lines[i].startswith('<<<<<<<'):
            inblock = region_after(inblock, [lines[i]])
            out.append(lines[i]); i += 1
            continue
        j = i + 1
        a = []
        while not lines[j].startswith('======='):
            a.append(lines[j]); j += 1
        k = j + 1
        b = []
        while not lines[k].startswith('>>>>>>>'):
            b.append(lines[k]); k += 1
        if inblock or any(BLOCK.search(l) for l in a + b):
            out += a if tip_first else b
            inblock = region_after(inblock, a if tip_first else b)
        else:
            out += lines[i:k + 1]; left = True
        i = k + 1
    write_if_changed(os.path.join(REPO, path), ''.join(out))
    return left


def resolve():
    """settle the generated files of a conflicted rebase or merge, then regenerate: result tables, the case registry and the allow list keep the tip's rows plus the rows this branch added or changed, charts take the
    tip's, the generated blocks of README.md and docs/ take the tip's; the rest of a conflict is left for you. Run `git add -A` and continue."""
    left = []
    for path in git_out('diff', '--name-only', '--diff-filter=U').split():
        if path in ROW_KEY:
            merge_rows(path, ROW_KEY[path])
        elif path.startswith('docs/img/'):
            subprocess.run(['git', '-C', REPO, 'checkout', '--ours' if rebasing() else '--theirs', '--', path], check=True)
        elif path.endswith('.md') and (path == 'README.md' or path.startswith('docs/')):
            if take_tip_hunks(path):
                left.append(path)
        else:
            left.append(path)
    if left:
        sys.exit('gen-docs: prose conflicts, resolve by hand: ' + ' '.join(left) + '\nthen run scripts/gen-docs.py (it regenerates every block, the options regions too)')
    generate(REPO)
    print('gen-docs: generated files settled; git add -A, then continue the rebase or merge')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--resolve', action='store_true')
    a = ap.parse_args()
    if a.resolve:
        resolve()
    elif a.check:
        check()
    else:
        generate(REPO)


if __name__ == '__main__':
    main()
