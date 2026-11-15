# The command line of the transaction fuzz. The docstring of test/fuzz/txn_fuzz.py is its help text.

import argparse
import json
import sys
import time

from .gen import Config, config, script, setup_sql
from . import run as run_mod
from .run import ERRORS, build_base, check_sql, release_prepared, run
from .session import drop_db, psql
from .shapes import KINDS, ShapeGen, shape_config
from .shrink import shrink, write


def seeds_of(spec):
    out = []
    for part in spec.split(','):
        lo, _, hi = part.partition('-')
        out += range(int(lo), int(hi or lo) + 1)
    return out


def main(doc):
    ap = argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--actions', type=int, default=60, help='actions of a script (statements, reconnects and checks)')
    ap.add_argument('--tracks', default='auto,key,off,ctid', help='the track_changes settings the seeds draw from (v4.1.0 and before: auto,key,off)')
    ap.add_argument('--out', default='/tmp/txn_fuzz')
    ap.add_argument('--db', default='tf', help='prefix of the databases')
    ap.add_argument('--no-spi', action='store_true', help='no statement that runs SQL through SPI (a volatile function, a DO block, a trigger that updates t): they hide other findings')
    ap.add_argument('--no-oc', action='store_true', help='no INSERT .. ON CONFLICT (a finding of 4.2: aborted speculative inserts are counted by the scan)')
    ap.add_argument('--no-truncate', action='store_true', help='VACUUM never truncates the heap (a finding of the first runs: an index scan reads a block the truncation cut off)')
    ap.add_argument('--no-shrink', action='store_true')
    ap.add_argument('--variants', default='', help='the plans compared with the plain heap plan, a subset of scan,default,index,workers (default: all; what shows which plan finds a bug)')
    ap.add_argument('--shapes', action='store_true', help='the shapes of shapes.py: cursors, prepared statements, partitioned tables, upserts and COPY with SPI (another set of scripts for a seed)')
    ap.add_argument('--only', default='', choices=('',) + KINDS, help='with --shapes: the checks of one shape in place of the plain ones (what finds a bug that the plain checks find too, by this shape)')
    ap.add_argument('--concurrently', action='store_true', help='with --shapes: REINDEX INDEX CONCURRENTLY and CREATE INDEX CONCURRENTLY of a second index between the statements (outside a transaction block)')
    ap.add_argument('--parts', type=int, default=None, choices=(0, 3), help='with --shapes: every table partitioned (3) or none (0)')
    ap.add_argument('--stop-after', type=int, default=0, help='stop after this many failing seeds (0: all seeds)')
    ap.add_argument('--budget', type=int, default=0, help='stop starting seeds after this many seconds (0: no limit)')
    ap.add_argument('--repro', default='', help='replay the .json of a reproducer')
    ap.add_argument('--keep', action='store_true', help='keep the databases of the seeds (a run of thousands of seeds fills a tmpfs)')
    ap.add_argument('--show', action='store_true', help='print every statement')
    ap.add_argument('--selftest', action='store_true', help='no server: every seed from 0 to 39 generates a script (the lint job)')
    a = ap.parse_args()
    tracks = a.tracks.split(',')
    if a.variants:
        run_mod.VARIANTS[:] = [v for v in run_mod.VARIANTS if v[0] == 'plain' or v[0] in a.variants.split(',')]

    def make(seed):
        if a.shapes:
            c = shape_config(seed, prepared, tracks, not a.no_spi, not a.no_oc, not a.no_truncate, a.parts)
            return c, ShapeGen(c, a.actions, a.only, a.concurrently).build()
        c = config(seed, prepared, tracks, not a.no_spi, not a.no_oc, not a.no_truncate)
        return c, script(c, a.actions)
    if a.selftest:
        prepared = False
        n = 0
        for shapes in (False, True):
            a.shapes = shapes
            for seed in range(40):
                c, acts = make(seed)
                if not setup_sql(c).strip() or not any(x['t'] == 'check' for x in acts) or sum(x['t'] == 'sql' for x in acts) < 5:
                    sys.exit(f'txn_fuzz selftest: seed {seed} made no script')
                for x in acts:
                    if x['t'] == 'check':
                        check_sql(c, x, x['s'] == 'A')
                        check_sql(c, x)
                n += 1
        for only in KINDS:
            a.shapes, a.only = True, only
            if not any(x.get('kind', 'plain') != 'plain' for seed in range(10) for x in make(seed)[1]):
                sys.exit(f'txn_fuzz selftest: --only {only} made no check')
        a.shapes, a.only, a.concurrently, a.parts = True, '', True, 0
        if not any('CONCURRENTLY' in x.get('sql', '') for seed in range(10) for x in make(seed)[1]):
            sys.exit('txn_fuzz selftest: --concurrently made no CONCURRENTLY statement')
        print(f'txn_fuzz selftest: {n} scripts')
        return 0
    prepared = int(psql('postgres', 'SHOW max_prepared_transactions')[0].strip() or 0) > 0
    if a.repro:
        j = json.load(open(a.repro))
        c = Config(**j['config'])
        build_base(c, f'{a.db}_repro')
        fail, nchecks, nerr = run(c, j['actions'], f'{a.db}_repro')
        print(f'repro: {fail}' if fail else 'repro: no failure')
        return 1 if fail else 0
    release_prepared()
    failed, start = 0, time.time()
    for seed in seeds_of(a.seeds):
        if a.budget and time.time() - start > a.budget:
            break
        c, acts = make(seed)
        db = f'{a.db}_{seed}'
        t0 = time.time()
        try:
            build_base(c, db)
        except SystemExit as e:
            print(f'seed {seed}: the configuration is not supported here ({str(e)[:160]})')
            continue
        if a.show:
            for x in acts:
                print('   ', x['s'], x['t'], x.get('sql', '')[:200].replace('\n', ' '))
        fail, nchecks, nerr = run(c, acts, db)
        print(f"seed {seed}: {'FAIL' if fail else 'ok'} {len(acts)} actions, {nchecks} checks, {nerr} statement errors, {time.time() - t0:.0f} s [{c.keytype} {c.rows} track={c.track} wbuf={c.wbuf} vacuum={c.vacuum} trigger={c.trigger}]", flush=True)
        if fail and fail[1].startswith('ENVIRONMENT'):
            sys.exit(f'txn_fuzz: seed {seed}: {fail[1]}')
        if fail:
            failed += 1
            print(f'  action {fail[0]}: {fail[1][:600]}', flush=True)
            if not a.no_shrink:
                c2, acts2 = shrink(c, acts, fail[0], f'{a.db}_s{seed}')
                print(f"  shrunk to {len(acts2)} actions: {write(a.out, c2, acts2, fail[1])}.sql", flush=True)
        if not a.keep:
            drop_db(db, f'{a.db}_s{seed}_tpl', f'{a.db}_s{seed}_w')
        if fail and a.stop_after and failed >= a.stop_after:
            break
    for e, n in ERRORS.most_common(8):
        print(f'  {n:5} statement errors: {e}')
    print(f'txn_fuzz: {failed} failing seeds')
    return 1 if failed else 0
