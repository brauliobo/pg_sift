# The command line of the differential fuzz: seeds, the selftest and the totals.
# The docstring of test/fuzz/diff_fuzz.py is its help text.

import argparse
import os
import random
import re
import sys
import time

from . import tablemode
from .db import REF, build_base, create_db, data_sql, psql
from .tablemode import TABLE_SKIP
from .registry import FAMILIES, LEADER, base_of
from .run import CRASH, diverged, episodes, query_script, render_query, run_script
from .shrink import handle

def main(doc):
    ap = argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--queries', type=int, default=40)
    ap.add_argument('--rows', type=int, default=3000)
    ap.add_argument('--parts', type=int, default=4)
    ap.add_argument('--families', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--no-shrink', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--stop-after', type=int, default=0, help='stop after the seed in which this many queries crashed, diverged or failed to generate (0: all seeds); the full tier uses it so that a red job does not run the rest of its seeds')
    ap.add_argument('--dml', type=int, default=4, help='most DML statements before a group of queries')
    ap.add_argument('--leader-on', action='store_true', help='parallel_leader_participation stays on (a worker crash of leaderless parallel scans hides other findings)')
    ap.add_argument('--show', action='store_true', help='print every query')
    ap.add_argument('--table', action='store_true', help='the fact tables are sift_tables (see above)')
    ap.add_argument('--selftest', action='store_true', help='no server: every family generates and renders queries for seeds 0-19, with and without --table (the lint job)')
    a = ap.parse_args()
    tablemode.TABLE = a.table
    if a.selftest:
        for tablemode.TABLE in (False, True):
            ctx = {'rows': a.rows, 'parts': a.parts, 'next_ev': a.rows + 1000, 'next_pt': a.rows + 1000, 'next_kc': a.rows + 1000, 'next_nz': a.rows + 1000, 'next_ar': a.rows + 1000, 'next_ex': a.rows + 1000, 'next_tr': a.rows + 1000, 'next_go': a.rows + 1000, 'next_kt': a.rows * 40 + 1000, 'next_dc': a.rows * 2 + 1000}
            sql = data_sql(1, a.rows, a.parts)
            if '-- @' in sql or '@' in re.sub(r"'[^']*'", '', sql) or ('USING sift_col' in sql) == tablemode.TABLE:
                sys.exit(f'diff_fuzz selftest: the data of {"--table" if tablemode.TABLE else "the heap mode"} is not as expected')
            fams = {f: v for f, v in FAMILIES.items() if not (tablemode.TABLE and f in TABLE_SKIP)}
            for fam, (gen, render, _) in fams.items():
                for seed in range(20):
                    sql, _ = render(gen(random.Random(seed), dict(ctx)))
                    if not sql.strip():
                        sys.exit(f'diff_fuzz selftest: family {fam} rendered no query for seed {seed}')
            for seed in range(20):
                for st, qs in episodes(seed, 40, dict(ctx), set()):
                    for qid, fam, parts, vs, nl in qs:
                        render_query(fam, parts)
            print(f"diff_fuzz selftest{' --table' if tablemode.TABLE else ''}: {len(fams)} families, 20 queries each, 20 scripts of 40 queries")
            for fam in FAMILIES if tablemode.TABLE else ():
                if fam in TABLE_SKIP:
                    print(f'  skipped on a sift_table: {fam}: {TABLE_SKIP[fam]}')
        return 0
    seeds = []
    for part in a.seeds.split(','):
        lo, _, hi = part.partition('-')
        seeds += range(int(lo), int(hi or lo) + 1)
    only = set(filter(None, a.families.split(',')))
    if tablemode.TABLE and only & set(TABLE_SKIP):
        sys.exit('diff_fuzz: not run on a sift_table: ' + ', '.join(f'{f} ({TABLE_SKIP[f]})' for f in sorted(only & set(TABLE_SKIP))))
    if a.leader_on:
        LEADER[:] = ['on']
    total = {'CRASH': 0, 'ok': 0, 'DIFF': 0, 'GENERR': 0, 'timeout': 0, 'queries': 0}
    pushed = {f: [0, 0] for f in FAMILIES}
    found = []
    t0 = time.time()
    for seed in seeds:
        tag = f"{seed}{'t' if tablemode.TABLE else ''}"
        base_db = f'fz_{tag}_base'
        build_base(base_db, seed, a.rows, a.parts)
        ctx = {'rows': a.rows, 'parts': a.parts, 'next_ev': a.rows + 1000, 'next_pt': a.rows + 1000, 'next_kc': a.rows + 1000, 'next_nz': a.rows + 1000, 'next_ar': a.rows + 1000, 'next_ex': a.rows + 1000, 'next_tr': a.rows + 1000, 'next_go': a.rows + 1000, 'next_kt': a.rows * 40 + 1000, 'next_dc': a.rows * 2 + 1000}
        eps = episodes(seed, a.queries, ctx, only, a.dml)
        run_db = f'fz_{tag}_run'
        create_db(run_db, base_db)
        hist = []
        for st, qs in eps:
            entries, qinfo = [], {}
            for qid, fam, parts, vs, nl in qs:
                sql, ordered = render_query(fam, parts)
                base = base_of(parts, nl)
                entries.append(query_script(qid, sql, ordered, base, vs))
                qinfo[qid] = (fam, parts, vs, nl, sql)
                if a.show:
                    print(f'-- {qid} {fam}: {sql}')
            res, err = run_script(run_db, st, entries)
            hist += st
            lo = 0
            while CRASH.search(err):
                # a backend died: the first query without all its lines is the one; the queries after it run again
                bad_i = next((i for i in range(lo, len(qs)) if len(res.get(qs[i][0], {})) < 1 + len(qs[i][3])), None)
                if bad_i is None:
                    break
                qid, fam, parts, vs, nl = qs[bad_i]
                got = res.get(qid, {})
                where_ = 'reference' if 'ref' not in got else next((n for n, _ in vs if n not in got), '?')
                print(f'CRASH {qid} {fam} in {where_} (settings {dict(vs).get(where_, REF)}) after {len(hist)} statements\n  {qinfo[qid][4]}')
                total['CRASH'] += 1
                found.append((seed, qid))
                if a.out:
                    os.makedirs(a.out, exist_ok=True)
                    open(f'{a.out}/crash_{qid}.sql', 'w').write('\n'.join(h + ';' for h in hist) + f'\n-- {where_}: {dict(vs).get(where_, REF)}\n{qinfo[qid][4]};\n')
                res[qid] = dict(got, ref=('CRASH', ''))
                lo = bad_i + 1
                if lo >= len(qs):
                    break
                res2, err = run_script(run_db, [], entries[lo:])
                res.update(res2)
            for qid, (fam, parts, vs, nl, sql) in qinfo.items():
                total['queries'] += 1
                q = res.get(qid)
                if q is None or 'ref' not in q:
                    total['timeout'] += 1
                    print(f'TIMEOUT {qid} {fam} seed {seed} in reference: {sql}')
                    continue
                if q['ref'][0] == 'CRASH':
                    continue
                if q['ref'][0] == 'GENERR':
                    total['GENERR'] += 1
                    print(f'GENERATOR ERROR {qid} {fam}: {sql}\n  {err.strip()[:300]}')
                    continue
                vars_ = [n for n, _ in vs]
                for n in vars_:
                    if n in q and q[n][0] != 'SKIP':
                        pushed[fam][1] += 1
                        pushed[fam][0] += q[n][1] == '1'
                    elif n not in q:
                        total['timeout'] += 1
                        print(f'TIMEOUT {qid} {fam} seed {seed} in {n} (settings {dict(vs)[n]}): {sql}')
                bad = diverged(q)
                if bad:
                    total['DIFF'] += 1
                    print(f'DIVERGENCE {qid} {fam} variants {sorted(bad)}\n  {sql}\n  {list(bad.values())[0][2][:500]}')
                    found.append((seed, qid))
                    if not a.no_shrink:
                        s2, name, detail, path, nh, nh0 = handle(seed, a.rows, a.parts, qid, fam, parts, vs, nl, hist, q, a.out, tag)
                        print(f'  shrunk to ({name}, {nh}/{nh0} statements): {s2}\n  {path or ""}')
                else:
                    total['ok'] += 1
        psql('postgres', f'DROP DATABASE IF EXISTS {run_db} WITH (FORCE)')
        if not a.keep:
            psql('postgres', f'DROP DATABASE IF EXISTS {base_db} WITH (FORCE)')
        if a.stop_after and total['CRASH'] + total['DIFF'] + total['GENERR'] >= a.stop_after:
            print(f'diff_fuzz: {a.stop_after} bad queries by seed {seed}: the remaining seeds are not run')
            break
    print('families, share of variant runs that used SiftAgg: ' + ', '.join(f'{f} {100 * p[0] // max(p[1], 1)}% of {p[1]}' for f, p in pushed.items() if p[1]))
    print(f"diff_fuzz: {total['queries']} queries in {time.time() - t0:.0f}s: {total['ok']} equal, {total['DIFF']} divergent, {total['GENERR']} generator errors, {total['CRASH']} crashes, "
          f"{total['timeout']} timeouts")
    return 1 if total['DIFF'] or total['GENERR'] or total['CRASH'] else 0
