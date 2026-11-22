#!/usr/bin/env python3
"""The cache of the rival engines of the search benchmark (stdlib only; `python3 -I`): a rival is every engine that is not a build of sift. It does not depend on the pg_sift
sources, so its rows are measured once per host, dataset, tier, class set and adapter and reused by every later run.

    rivals.py plan     --cache DIR --kind articles|places --context TEXT --engines "a b c"     prints the engines to measure, then the cached ones (two lines)
    rivals.py assemble --cache DIR --kind K --context TEXT --engines "a b" --dest RESULTDIR      appends the cached rows to RESULTDIR/{cells,builds,cold}.tsv
    rivals.py save     --cache DIR --kind K --context TEXT --engines "a b" --dest RESULTDIR      stores the rows of the engines measured in RESULTDIR

--context is what the run depends on besides the engine: the host fingerprint, dataset, tier, mode, classes and settings. The key of an entry adds the engine, the content of its
adapter (bench/engines/ENGINE.*), bench/versions.env and the cache epoch; `--remeasure-all` is the caller's: it skips `plan`.
"""
import argparse
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from benchlib import key_of  # noqa: E402

BENCH = os.path.dirname(HERE)
EPOCH = 1
ENGINE_COLUMN = {'articles': {'cells': 3, 'builds': 1, 'cold': 2}, 'places': {'cells': 4, 'builds': 0, 'cold': 2}}   # the 0-based column of the engine in each file
FILES = ('cells', 'builds', 'cold')


def is_rival(engine):
    return not engine.startswith('sift')


def adapter_hash(engine):
    files = sorted(glob.glob(os.path.join(BENCH, 'engines', engine + '.*')))
    return key_of([[os.path.basename(f), open(f, 'rb').read().hex()] for f in files] + [open(os.path.join(BENCH, 'versions.env')).read()])


def entry_dir(a, engine):
    return os.path.join(a.cache, a.kind, engine, key_of({'epoch': EPOCH, 'engine': engine, 'adapter': adapter_hash(engine), 'context': a.context}))


def rows_of(path, column, engine):
    if not os.path.exists(path):
        return None
    lines = open(path).read().splitlines()
    return [l for l in lines[1:] if l.split('\t')[column] == engine] if path.endswith(('cells.tsv', 'cold.tsv')) else [l for l in lines if l.split('\t')[column] == engine]


def cmd_plan(a):
    fresh, cached = [], []
    for e in a.engines.split():
        (cached if is_rival(e) and os.path.exists(os.path.join(entry_dir(a, e), 'complete')) else fresh).append(e)
    if not fresh:
        fresh, cached = cached, []
    print(' '.join(fresh))
    print(' '.join(cached))


def cmd_save(a):
    for e in (e for e in a.engines.split() if is_rival(e)):
        d = entry_dir(a, e)
        os.makedirs(d, exist_ok=True)
        for f in FILES:
            rows = rows_of(os.path.join(a.dest, f + '.tsv'), ENGINE_COLUMN[a.kind][f], e)
            if rows is not None:
                with open(os.path.join(d, f + '.tsv'), 'w') as out:
                    out.write(''.join(r + '\n' for r in rows))
        with open(os.path.join(d, 'complete'), 'w') as out:
            out.write(a.context + '\n')


def cmd_assemble(a):
    for e in a.engines.split():
        d = entry_dir(a, e)
        for f in FILES:
            src, dest = os.path.join(d, f + '.tsv'), os.path.join(a.dest, f + '.tsv')
            if os.path.exists(src) and os.path.exists(dest) and not rows_of(dest, ENGINE_COLUMN[a.kind][f], e):
                with open(dest, 'a') as out:
                    out.write(open(src).read())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=['plan', 'assemble', 'save'])
    p.add_argument('--cache', required=True)
    p.add_argument('--kind', required=True, choices=sorted(ENGINE_COLUMN))
    p.add_argument('--context', required=True)
    p.add_argument('--engines', required=True)
    p.add_argument('--dest')
    a = p.parse_args()
    {'plan': cmd_plan, 'assemble': cmd_assemble, 'save': cmd_save}[a.command](a)


if __name__ == '__main__':
    main()
