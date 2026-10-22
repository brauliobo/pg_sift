#!/usr/bin/env python3
"""Regenerate every generated part of README.md and docs/ from the code, the tests and the committed benchmark results.

    scripts/gen-docs.py            rewrite the generated blocks in place
    scripts/gen-docs.py --check    fail (exit 1) when a block is stale or a generated table is over the width budget; renders into a scratch copy, touches nothing, needs no server
    scripts/gen-docs.py --tables [--all]    list the tables of README.md and docs/ over the budget (--all: every table) with columns, longest row and cell

Nothing here measures: a benchmark number comes from docs/results/ (written by `bench/bench ... --docs`, which runs this too), the rest from the tree.
The parts:
  <!-- bench:NAME:begin --> ... <!-- bench:NAME:end -->   the benchmark renderers of bench/steps (readme.py, extras.py, store.py); a block whose
                                                          results file is missing or empty says it is not measured yet
  <!-- coverage:start --> ... <!-- coverage:end -->        scripts/coverage.sh --gen, from docs/coverage.tsv (also writes docs/COVERAGE.md)
  <!-- gen:NAME --> ... <!-- /gen:NAME -->                 gucs: the list of settings (DefineCustom*Variable calls of src/); guc:sift.NAME: the default of
                                                          one setting, inline; tests: the test counts, inline
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, 'bench', 'steps'))
import mdtable  # noqa: E402  (the width budget and the tables of a text)

# the results file behind each bench block; a block with a missing file is not rendered, it says so
RESULTS = {n: n + '.tsv' for n in ('sets', 'rollup', 'tablewrites', 'writepath', 'bm25size', 'leaftypes', 'fields', 'buildcost', 'gpu')}
RESULTS.update({'store': 'store.json', 'store-updates': 'store.json', 'store-compression': 'store-compression.json',
                'slim': 'articles.tsv', 'readme': 'articles.tsv', 'summary': 'articles.tsv'})
BENCH_CMD = dict.fromkeys(('store', 'store-updates', 'store-compression'), 'bench/bench store all --docs') | dict.fromkeys(('slim', 'readme', 'summary'), 'bench/bench all --docs')

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


def gucs_list(root):
    """one item per setting (a table cannot hold the descriptions): name, then default, range and who sets it in brackets, then the description"""
    return '\n'.join('- `%s` (default %s; %s%s): %s' % (name, s['default'] or 'empty', s['range'] + '; ' if s['range'] else '', s['context'], s['desc'])
                     for name, s in sorted(settings(root).items()))


# ---- counts of the tree ----------------------------------------------------------------------------------------------------------------

def count(root, pattern):
    d, _, suffix = pattern.partition('*')
    d = os.path.join(root, d)
    return sum(1 for f in os.listdir(d) if f.endswith(suffix)) if os.path.isdir(d) else 0


def tests_line(root):
    rows = sum(1 for l in read(os.path.join(root, 'docs', 'coverage.tsv')).splitlines() if l and not l.startswith(('#', 'id\t')))
    return '%d regression tests (`test/sql`), %d isolation specs, %d TAP tests (`test/t`), %d column fixtures and %d coverage rows' % (
        count(root, 'test/sql/*.sql'), count(root, 'test/isolation/specs/*.spec'), count(root, 'test/t/*.pl') + count(root, 'test/t/*.sh'),
        count(root, 'test/col/fixtures/*.sql'), rows)


# ---- the blocks --------------------------------------------------------------------------------------------------------------------------

def fill_gen_blocks(root):
    cfg = {}

    def text(name):
        if name == 'gucs':
            cfg.setdefault('gucs', gucs_list(root))
            return '\n' + cfg['gucs'] + '\n'
        if name == 'tests':
            return tests_line(root)
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


def generate(root):
    def run(*cmd):   # the renderers log to stderr; it is shown only when one fails
        p = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        if p.returncode:
            sys.exit('gen-docs: %s failed:\n%s' % (' '.join(cmd), p.stderr or p.stdout))

    steps = os.path.join('bench', 'steps')
    py = ['python3', '-B', '-I']
    if have(root, 'slim'):
        run(*py, os.path.join(steps, 'readme.py'))
    if have(root, 'store'):
        run(*py, os.path.join(steps, 'store.py'), 'render', '--readme', 'README.md', '--benchmarks', 'docs/BENCHMARKS.md')
    run(*py, os.path.join(steps, 'extras.py'), 'render')
    run(*py, os.path.join(steps, 'charts.py'))
    run(*py, os.path.join(steps, 'gap.py'), 'render', '--profile', 'docs/results/store-gap-1000000.json', '--stats', 'docs/results/store-stats-1000000.json')
    run('bash', os.path.join('scripts', 'coverage.sh'), '--gen')
    fill_missing_bench_blocks(root)
    fill_gen_blocks(root)


def check():
    with tempfile.TemporaryDirectory(prefix='sift-gen-docs.') as tmp:
        files = subprocess.run(['git', '-C', REPO, 'ls-files', '-z'], check=True, capture_output=True).stdout.split(b'\0')[:-1]
        for f in files:
            src, dst = os.path.join(REPO, os.fsdecode(f)), os.path.join(tmp, os.fsdecode(f))
            if os.path.isfile(src):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
        generate(tmp)
        stale = []
        for d, _, fs in os.walk(tmp):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), tmp)
                if rel == 'README.md' or rel.startswith('docs' + os.sep):
                    other = os.path.join(REPO, rel)
                    if not os.path.exists(other) or open(other, 'rb').read() != open(os.path.join(d, f), 'rb').read():
                        stale.append(rel)
        wide = [m for f in docs_files(tmp) for m in mdtable.lint(read(f), os.path.relpath(f, tmp), generated_only=not f.endswith(os.sep + 'README.md'))]
    if stale:
        sys.exit('gen-docs: stale (a generated block no longer matches its source): %s\nrun scripts/gen-docs.py and commit the result' % ', '.join(sorted(stale)))
    if wide:
        sys.exit('gen-docs: tables over the budget of %d columns, %d characters a row, %d a cell (AGENTS.md, "Tables in the docs"): split, shorten or fold them; for a generated block change its renderer, not the output\n%s' % (
            mdtable.MAX_COLS, mdtable.MAX_ROW, mdtable.MAX_CELL, '\n'.join(wide)))
    print('gen-docs: every generated block is current and every generated table fits')


def tables(everything):
    n = wide = 0
    for f in docs_files(REPO):
        for line, cols, row, cell, gen, head in mdtable.survey(read(f)):
            n += 1
            over = cols > mdtable.MAX_COLS or row > mdtable.MAX_ROW or cell > mdtable.MAX_CELL
            wide += over
            if over or everything:
                print('%-24s %5d %-4s columns %2d  row %4d  cell %3d  %s' % (os.path.relpath(f, REPO), line, 'gen' if gen else 'hand', cols, row, cell, head[:40]))
    print('%d tables, %d over the budget' % (n, wide))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--tables', action='store_true')
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    if a.check:
        check()
    elif a.tables:
        tables(a.all)
    else:
        generate(REPO)


if __name__ == '__main__':
    main()
