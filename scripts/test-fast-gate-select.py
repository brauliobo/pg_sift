#!/usr/bin/env python3
"""The code-to-test router of scripts/fast-gate.sh (Spec 84 section 3.1), in lint.sh: no PostgreSQL, no build.

  scripts/test-fast-gate-select.py

Each case runs `fast-gate.sh --dry-run` with --files, --area or --diff and reads the plan it prints: the areas, the steps, the regression tests, the arguments of each step.
The runs share a pool, so the whole table takes about a second.
"""
import os
import re
import resource
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = os.path.join(REPO, 'scripts', 'fast-gate.sh')
ENV = {k: v for k, v in os.environ.items() if k != 'CI'}

STAR, CLICK, COEXIST, JSONB = 'src/agg/sip_scan.c', 'src/agg/group/fast.c', 'src/agg/cost/model.c', 'src/col/shred/shred.c'
COL, TABLE, BM25, GPU, SQL, SMOKE = 'src/col/codec/int.h', 'src/table/am.c', 'src/bm25/amscan.c', 'src/agg/gpu/cfg.h', 'sql/pg_sift--1.0.sql', 'src/new_thing.c'
REGRESS = 'test/sql/agg/bucket.sql'
AREAS = 'star clickbench coexist jsonb col table bm25 gpu sql smoke all'.split()


def parse(args, p):
    out = p.stdout
    field = lambda name: [w for w in re.findall(rf'^{name}:\s*(.*)$', out, re.M)[0].split() if w != '-'] if re.search(rf'^{name}:', out, re.M) else []
    return dict(rc=p.returncode, out=out, err=p.stderr, areas=field('areas'), steps=field('steps'), skipped=field('skipped'),
                sql=re.sub(r'\(.*\)', '', ' '.join(field('sql'))).split(),
                cmds={m[0]: m[1] for m in re.findall(r'^  (\w[\w-]*)\s+(\S.*)$', out, re.M) if not m[1].startswith('SKIP')},
                lanes=re.findall(r'^lane \d+:', out, re.M), leaks=re.findall(r'^  leakcheck ', out, re.M))


def run(args):
    return parse(args, subprocess.run([GATE, '--dry-run', *args], capture_output=True, text=True, env=ENV, cwd=REPO))


files = lambda *paths, extra=(): ('--files', *paths, *extra)
area = lambda *a: ('--area', *a)


def has(p, *steps):
    return all(s in p['steps'] for s in steps)


def lacks(p, *steps):
    return not any(s in p['steps'] for s in steps)


def cmd(p, step, pattern):
    return bool(re.search(pattern, p['cmds'].get(step, '')))


def table():
    """(name, [argument tuples], predicate over the plans)"""
    one = lambda name, args, ok: (name, [args], lambda ps: ok(ps[0]))
    yield one('docs run nothing', files('README.md', 'docs/guide.md'), lambda p: p['rc'] == 0 and p['steps'] == [] and 'nothing to run' in p['out'])
    yield one('star', files(STAR), lambda p: p['areas'] == ['star'] and has(p, 'plangate', 'shapes_matrix') and lacks(p, 'coldiff', 'tbl_twin_fuzz'))
    yield one('clickbench', files(CLICK), lambda p: p['areas'] == ['clickbench'] and has(p, 'corpus', 'plangate', 'shapes_matrix'))
    yield one('coexist', files(COEXIST), lambda p: p['areas'] == ['coexist'] and has(p, 'plangate'))
    yield one('jsonb owns src/col/shred/shred.c, not the column index', files(JSONB), lambda p: p['areas'] == ['jsonb'] and has(p, 'coldiff'))
    yield one('col', files(COL), lambda p: p['areas'] == ['col'] and has(p, 'coldiff', 'surface_sweep', 'txn_fuzz') and lacks(p, 'coldiff-table', 'tbl_twin_fuzz'))
    yield one('table', files(TABLE), lambda p: p['areas'] == ['table'] and has(p, 'coldiff-table', 'plangate-table', 'tbl_twin_fuzz') and 'shapes_matrix' in p['steps'] + p['skipped'])
    yield one('bm25', files(BM25), lambda p: p['areas'] == ['bm25'] and has(p, 'plangate', 'shapes_matrix', 'bm25_fuzz') and lacks(p, 'coldiff'))
    yield one('gpu', files(GPU), lambda p: p['areas'] == ['gpu'] and has(p, 'plangate', 'corpus') and cmd(p, 'corpus', r'agg'))
    yield one('sql', files(SQL), lambda p: p['areas'] == ['sql'] and has(p, 'surface_sweep', 'corpus') and lacks(p, 'coldiff', 'txn_fuzz'))

    yield one('union: the areas of many paths, each once', files(STAR, CLICK, 'src/agg/topn/topn.c'), lambda p: p['areas'] == ['star', 'clickbench'])
    yield one('union: a stage runs once', files(STAR, CLICK, BM25, TABLE), lambda p: len(p['steps']) == len(set(p['steps'])))
    yield one('union: the slices merge into one plangate', files(STAR, CLICK), lambda p: cmd(p, 'plangate', r'--slice star,clickbench\b'))
    yield one('union: one shapes_matrix for the facts of two areas', files(STAR, BM25), lambda p: p['steps'].count('shapes_matrix') == 1)
    yield one('dedup: the same path twice', files(STAR, STAR), lambda p: p['areas'] == ['star'] and len(p['steps']) == len(set(p['steps'])))
    yield one('union: five areas', files(STAR, TABLE, BM25, COL, SQL),
              lambda p: p['areas'] == ['star', 'col', 'table', 'bm25', 'sql'] and has(p, 'coldiff', 'coldiff-table', 'bm25_fuzz', 'surface_sweep'))

    harnesses = (('test/fuzz/txn/cli.py', ['txn_fuzz', 'txn_fuzz-table']), ('test/fuzz/shapes_matrix.py', ['shapes_matrix']), ('test/fuzz/surface_sweep.py', ['surface_sweep']),
                 ('test/fuzz/tbl_twin.py', ['tbl_twin_fuzz']), ('test/fuzz/bm25.py', ['bm25_fuzz']), ('test/fuzz/diff/cli.py', ['diff_fuzz', 'diff_fuzz-table']),
                 ('scripts/coldiff.sh', ['coldiff', 'coldiff-table']), ('scripts/plangate.sh', ['plangate', 'plangate-table', 'plangate-4t']),
                 ('test/plan/anything.sql', ['plangate', 'plangate-table', 'plangate-4t']), ('test/corpus/anything.sql', ['corpus', 'corpus-table']), ('scripts/corpus.sh', ['corpus', 'corpus-table']))
    for path, steps in harnesses:
        yield one(f'a harness edit runs itself, and only its stages: {path}', files(path),
                  lambda p, s=steps: p['areas'] == [] and sorted(p['steps'] + p['skipped']) == sorted(s))

    yield one('a test/sql edit runs itself', files(REGRESS), lambda p: p['sql'] == ['agg/bucket'] and p['steps'] == ['regress'])
    yield one('an expected file edit runs its test', files('test/expected/agg/bucket.out'), lambda p: p['sql'] == ['agg/bucket'])
    yield one('a test without its pair is not run', files('test/sql/no_such_test.sql'), lambda p: p['sql'] == [] and lacks(p, 'regress'))
    yield one('the regression lane runs the tests named', files(REGRESS), lambda p: cmd(p, 'regress', r'pgtest\.sh regress agg/bucket$'))
    yield one('the release library skips the regression lane', files(REGRESS, extra=('--format', 'release')), lambda p: 'regress' in p['skipped'])

    yield one('unmapped src runs smoke', files(SMOKE), lambda p: p['areas'] == ['smoke'] and has(p, 'corpus', 'plangate', 'surface_sweep', 'txn_fuzz'))
    yield one('a core file runs smoke', files('src/core/cachebudget.c'), lambda p: p['areas'] == ['smoke'])
    yield one('the Makefile runs smoke', files('Makefile'), lambda p: p['areas'] == ['smoke'])
    yield one('smoke beside an area', files(STAR, SMOKE), lambda p: p['areas'] == ['star', 'smoke'])
    yield one('smoke is the smoke slice', files(SMOKE), lambda p: cmd(p, 'plangate', r'--slice smoke\b'))

    leak = [files(STAR), files(COL), files(SMOKE), files('test/fuzz/txn/cli.py'), files(REGRESS), area('all'), area('bm25')]
    yield 'every lane ends in the leak gate', leak, lambda ps: all(p['lanes'] and len(p['lanes']) == len(p['leaks']) for p in ps)
    yield one('no step, no lane, no leak gate', files('README.md'), lambda p: p['lanes'] == [] and p['leaks'] == [])

    only = [area(a) for a in ('jsonb', 'star', 'clickbench', 'gpu', 'col')]
    yield ('corpus --only globs are joined with |', only,
           lambda ps: all(not re.search(r'--only \S*,|--only \S+ [^-]', p['cmds'].get('corpus', '')) and '\\,' not in p['cmds'].get('corpus', '') for p in ps))
    yield one('a single glob stays a single glob', area('gpu'), lambda p: cmd(p, 'corpus', r'--only \\\*agg\\\*( --format \S+)?$'))
    yield one('the column index narrows surface_sweep with |', area('col'), lambda p: cmd(p, 'surface_sweep', r'--only col\\\|check'))

    yield one('--area all is the default', (), lambda p: p['areas'] == ['all'] and has(p, 'corpus', 'plangate', 'lifecycle', 'coldiff', 'surface_sweep', 'shapes_matrix', 'txn_fuzz'))
    yield one('--area is a list', area('star,table'), lambda p: p['areas'] == ['star', 'table'] and has(p, 'coldiff-table') and lacks(p, 'coldiff'))
    yield one('--area smoke is the smoke slice', area('smoke'), lambda p: cmd(p, 'plangate', r'--slice smoke\b'))
    yield one('--full widens the slices and the tiers', ('--full',), lambda p: cmd(p, 'plangate', r'--slice all\b') and cmd(p, 'shapes_matrix', r'--tier full'))
    yield ('every area of the table plans a step', [area(a) for a in AREAS], lambda ps: all(p['rc'] == 0 and p['steps'] for p in ps))
    yield one('the format reaches the plan', files(STAR, extra=('--format', 'f1')), lambda p: 'format=f1' in p['out'] and has(p, 'plangate'))

    yield one('--include-sql keeps the tests within 40 outside CI', files(STAR, CLICK, 'src/agg/topn/topn.c', 'src/agg/win/carry.c', extra=('--include-sql',)),
              lambda p: p['rc'] == 0 and len(p['sql']) <= 40 and ('regress' in p['steps']) == bool(p['sql']))
    yield one('without --include-sql a src file runs no test/sql', files(STAR), lambda p: p['sql'] == [] and lacks(p, 'regress'))
    yield one('--include-sql runs the test/sql edited', files(REGRESS, extra=('--include-sql',)), lambda p: 'agg/bucket' in p['sql'])

    yield one('--diff HEAD plans', ('--diff', 'HEAD'), lambda p: p['rc'] == 0 and 'input=diff' in p['out'])
    for why, args in (('an unknown area', area('nope')), ('--area with --files', ('--area', 'star', '--files', 'README.md')), ('--files without a path', ('--files',)),
                      ('--diff of no commit', ('--diff', 'no-such-commit')), ('--diff with --area', ('--diff', 'HEAD', '--area', 'star')), ('an unknown option', ('--bogus',))):
        yield one(f'exit 2 on {why}', args, lambda p: p['rc'] == 2)


def main():
    cases = list(table())
    unique = sorted({a for _, argsets, _ in cases for a in argsets})
    with ThreadPoolExecutor(32) as pool:
        plans = dict(zip(unique, pool.map(run, unique)))
    fails = []
    for name, argsets, ok in cases:
        ps = [plans[a] for a in argsets]
        try:
            good = ok(ps)
        except Exception as e:
            good = False
            ps[0]['out'] = f'{type(e).__name__}: {e}\n{ps[0]["out"]}'
        if not good:
            fails.append(f'FAIL: {name} ({" ".join(argsets[0])})\n    ' + '\n    '.join((ps[0]['out'] + ps[0]['err']).splitlines()[:12]))
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    subprocess.run([GATE, '--dry-run', '--files', STAR, COL, REGRESS], capture_output=True, env=ENV, cwd=REPO)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
    if cpu > 0.2:
        fails.append(f'FAIL: --dry-run takes {cpu:.2f} s of CPU, not under 0.2 s')
    print('\n'.join(fails))
    print('fast-gate-select:', 'FAILED' if fails else f'{len(cases)} routing cases: areas, union, harness edits, smoke, leak gate, corpus globs, --include-sql, exits')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
