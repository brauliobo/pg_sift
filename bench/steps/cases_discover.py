#!/usr/bin/env python3
"""Candidate benchmark cases from the query corpora: the shapes pg_sift pushes down and that beat PostgreSQL, that no case of bench/cases.tsv covers, ranked by gain times how common the shape is.

    python3 -I cases_discover.py [--shapes FILE ...] [--fuzz SEEDS] [--results FILE ...] [--setup FILE] [--rows N] [--min-gain 2] [--runs 3] [--out FILE]

Sources (any number):
  --shapes FILE    statements in the layout of test/plan/shapes.sql (`-- name: x`, optional `-- weight: n`, the statement ending in `;`), measured here on --db
                   (created from --setup, default test/plan/gen.sql, when it is missing). The default source is test/plan/shapes.sql, the neutral corpus of the plan gate.
  --fuzz SEEDS    the query families of the differential fuzzer (test/fuzz/diff_fuzz.py): `1-3` seeds of --queries queries each on the fuzzer's own tables; the weight is the family's.
  --results FILE  statements measured elsewhere, no server needed: TSV `id native_ms sift_ms [weight [shape]]` (or JSONL of an API-corpus run, `--lane NAME` the sift lane in `e`), so a
                  private corpus can be ranked without its SQL leaving the machine. The shape is the text before `--` in the id when there is none.
Each statement runs with sift.enable_agg off (native) and on (sift), serial, the answers compared (a difference is listed first: it is a bug, not a candidate). A shape is the set of
features of the SQL (aggregates, DISTINCT, GROUP BY forms, top-N keys, window, joins, UNION ALL, CTE, lateral, grouping sets, calendar and sort expressions). A candidate is pushed down, at
least --min-gain times faster than native, and its shape is not that of a case of the registry. The output is a proposal for a person to accept, never a case.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as cases_mod  # noqa: E402
from duckdb import Session, same  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
FEATURES = [  # (name, regular expression over the lower-cased SQL without literals)
    ('count_distinct', r'count\s*\(\s*distinct'), ('sum_distinct', r'(sum|avg)\s*\(\s*distinct'), ('count', r'\bcount\s*\('), ('sum', r'\bsum\s*\('), ('avg', r'\bavg\s*\('),
    ('minmax', r'\b(min|max)\s*\('), ('stat', r'\b(stddev|variance|percentile_\w+|string_agg|array_agg|bool_and|bool_or)\s*\('), ('filter', r'\bfilter\s*\(\s*where'),
    ('group', r'\bgroup\s+by\b'), ('sets', r'\b(grouping\s+sets|rollup|cube)\b'), ('having', r'\bhaving\b'), ('calendar', r'date_trunc|extract\s*\('),
    ('topn', r'order\s+by[^;]*\blimit\b'), ('offset', r'\boffset\b'), ('sort_expr', r'order\s+by\s+(coalesce|lower|upper)\s*\('), ('window', r'\bover\s*\('),
    ('join', r'\bjoin\b'), ('outer', r'\b(left|right|full)\s+(outer\s+)?join\b'), ('lateral', r'\blateral\b'), ('limit1', r'limit\s+1\b'), ('union', r'\bunion\s+all\b'),
    ('cte', r'\bwith\b[^;]*\bas\s*\('), ('exists', r'\bexists\s*\('), ('insub', r'\bin\s*\(\s*select\b'), ('jsonb', r'->>?|jsonb_\w+\s*\(|#>>'), ('like', r'\blike\b|~~'),
    ('inlist', r'\bin\s*\(\s*\$?\d|\bin\s*\(\s*\?'), ('between', r'\bbetween\b|[<>]=?\s*\$?\d'), ('distinct', r'select\s+distinct\b'), ('search', r'@@>'),
]


def signature(sql):
    """the features of a statement: what it asks of the scan, not the table or the literals"""
    text = re.sub(r"'(?:[^']|'')*'", "''", sql.lower())
    text = re.sub(r'\b\d+(\.\d+)?\b', '0', text)
    return ' '.join(n for n, rx in FEATURES if re.search(rx, text))


def parse_shapes(path):
    out, name, weight, buf = [], None, 1.0, []
    for line in open(path):
        m = re.match(r'-- (name|weight): (.*)', line)
        if m and not buf:
            if m.group(1) == 'name':
                name = m.group(2).strip()
            else:
                weight = float(m.group(2))
        elif line.startswith('--') or not line.strip():
            continue
        else:
            buf.append(line.rstrip('\n'))
            if line.rstrip().endswith(';'):
                out.append(dict(id=name or 'shape%d' % (len(out) + 1), sql='\n'.join(buf).rstrip(';'), weight=weight, source=os.path.basename(path)))
                name, weight, buf = None, 1.0, []
    return out


def parse_results(path, lane):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('{'):
            o = json.loads(line)
            f = [o['id'], o['e']['pg']['ms'], o['e'][lane]['ms']] if lane in o['e'] else None
        else:
            f = line.split('\t')
        if not f or f[0] == 'id':
            continue
        out.append(dict(id=f[0], native=float(f[1]), sift=float(f[2]) if f[2] not in ('', 'NA') else None, weight=float(f[3]) if len(f) > 3 and f[3] else 1.0,
                        sig='shape:' + (f[4] if len(f) > 4 and f[4] else f[0].split('--')[0]), source=os.path.basename(path), pushed=True))
    return out


def fuzz_statements(seeds, queries, rows):
    sys.path.insert(0, os.path.join(REPO, 'test', 'fuzz'))
    import diff_fuzz as df
    out = []
    for seed in seeds:
        db = 'discover_fz%d' % seed
        df.psql('postgres', 'DROP DATABASE IF EXISTS %s WITH (FORCE)' % db)
        df.psql('postgres', 'CREATE DATABASE %s' % db)
        df.build_base(db, seed, rows, 4)
        ctx = {'rows': rows, 'next_ev': rows + 1000, 'next_pt': rows + 1000}
        for _, qs in df.episodes(seed, queries, ctx, set(), 0):
            for qid, fam, parts, _, _ in qs:
                sql, ordered = df.render_query(fam, parts)
                out.append(dict(id=qid, sql=sql.rstrip(';'), weight=float(df.FAMILIES[fam][2]), source='fuzz:' + fam, db=db, ordered=ordered))
    return out


def measure(records, db, runs, timeout):
    """native and sift milliseconds, whether the scan was used and whether the answers differ, per statement with an SQL text"""
    sessions = {}

    def session(d, lane):
        if (d, lane) not in sessions:
            s = Session(os.environ['PGPORT'], timeout, None, db=d, host=os.environ['PGHOST'])
            for st in ('SET max_parallel_workers_per_gather = 0', 'SET jit = off', 'LOAD \'pg_sift\'', 'SET sift.enable_agg = %s' % ('on' if lane == 'sift' else 'off')):
                s.run(st)
            sessions[d, lane] = s
        return sessions[d, lane]

    for r in records:
        if 'sql' not in r:
            continue
        d = r.get('db', db)
        n, s = session(d, 'native'), session(d, 'sift')
        r['sig'] = signature(r['sql'])
        r['pushed'] = 'Custom Scan (Sift' in '\n'.join(s.run('EXPLAIN (COSTS OFF) ' + r['sql'])[1])
        _, a, err, _ = n.run(r['sql'])
        if err:
            r['error'] = err
            continue
        _, b, err, _ = s.run(r['sql'])
        if err:
            r['error'] = 'sift: ' + err
            continue
        r['differs'] = not (same(a, b) if not r.get('ordered') else len(a) == len(b) and all(same([x], [y]) for x, y in zip(a, b)))
        tn, ts = [], []
        for _ in range(runs):
            tn.append(n.run(r['sql'])[0])
            ts.append(s.run(r['sql'])[0])
        r['native'], r['sift'] = statistics.median(tn), statistics.median(ts)
    for s in sessions.values():
        s.close()


def covered(registry):
    """the signatures of the statements of the registry"""
    return {signature(c.sec.get('sift', c.sec.get('pg', c.sec.get('load', '')))) for c in registry}


def rank(records, known, min_gain):
    groups = {}
    for r in records:
        if r.get('error') or r.get('differs') or not r.get('pushed') or not r.get('sift') or not r.get('native') or r['sig'] in known:
            continue
        gain = r['native'] / r['sift']
        if gain >= min_gain and r['native'] - r['sift'] >= 0.2:   # 2x of a few microseconds is noise
            groups.setdefault(r['sig'], []).append(dict(r, gain=gain))
    out = []
    for sig, rs in groups.items():
        weight = sum(r['weight'] for r in rs)
        med = statistics.median(r['gain'] for r in rs)
        best = max(rs, key=lambda r: r['gain'])
        out.append(dict(sig=sig, n=len(rs), weight=weight, median=med, max=best['gain'], score=weight * med, example=best, sources=sorted({r['source'] for r in rs})))
    return sorted(out, key=lambda g: -g['score'])


def proposals(ranked, path):
    with open(path, 'w') as f:
        f.write('# Candidate cases from the corpora (bench/steps/cases_discover.py): pushed down, at least as many times faster than native as asked, in no case of bench/cases.tsv. A person accepts one by\n'
                '# writing bench/cases/ID.sql and a row of bench/cases.tsv (the draft below), then `bench/bench cases --cases ID --docs`. score = statements x weight x median gain.\n')
        f.write('rank\tscore\tfeatures\tstatements\tweight\tmedian_gain\tmax_gain\tsources\texample\tdraft_row\texample_sql\n')
        for i, g in enumerate(ranked, 1):
            e = g['example']
            slug = re.sub(r'[^a-z0-9]+', '_', g['sig'].replace(' ', '_'))[:40]
            draft = '\t'.join(['candidate_%s' % slug, 'TODO: a phrase of the CHANGELOG line', 'orders', 'default', 'TODO: regex of the EXPLAIN', 'native,pg_duckdb', '-', 'ms', 'faster33', 'no'])
            f.write('%d\t%.0f\t%s\t%d\t%.0f\t%.1f\t%.1f\t%s\t%s\t%s\t%s\n' % (i, g['score'], g['sig'] or '-', g['n'], g['weight'], g['median'], g['max'], ','.join(g['sources']), e['id'], draft,
                                                                            re.sub(r'\s+', ' ', e.get('sql', ''))[:400].replace('\t', ' ')))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--shapes', action='append', default=[])
    ap.add_argument('--fuzz', default='')
    ap.add_argument('--queries', type=int, default=60)
    ap.add_argument('--results', action='append', default=[])
    ap.add_argument('--lane', default='s321')
    ap.add_argument('--db', default='discover')
    ap.add_argument('--setup', default=os.path.join(REPO, 'test', 'plan', 'gen.sql'))
    ap.add_argument('--rows', type=int, default=30000)
    ap.add_argument('--min-gain', type=float, default=2.0)
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--out', default=os.path.join(REPO, 'bench', 'out', 'cases-proposed.tsv'))
    a = ap.parse_args()
    records, measured_here = [], False
    shapes = a.shapes or ([] if a.results or a.fuzz else [os.path.join(REPO, 'test', 'plan', 'shapes.sql')])
    if shapes:
        cases_mod.psql('DROP DATABASE IF EXISTS %s WITH (FORCE)' % a.db, 'postgres')
        cases_mod.psql('CREATE DATABASE %s' % a.db, 'postgres')
        cases_mod.psql('CREATE EXTENSION pg_sift', a.db)
        subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', a.db, '-v', 'n=%d' % a.rows, '-f', a.setup], check=True, capture_output=True)
        cases_mod.psql('VACUUM (FREEZE, ANALYZE)', a.db)
        for path in shapes:
            records += parse_shapes(path)
    if a.fuzz:
        lo, _, hi = a.fuzz.partition('-')
        records += fuzz_statements(range(int(lo), int(hi or lo) + 1), a.queries, 3000)
    for path in a.results:
        records += parse_results(path, a.lane)
    print('discover: %d statements' % len(records), file=sys.stderr)
    measure(records, a.db, a.runs, a.timeout)
    registry = cases_mod.read_registry()
    known = covered(registry)
    differs = [r for r in records if r.get('differs')]
    errors = [r for r in records if r.get('error')]
    losses = [r for r in records if r.get('sift') and r.get('native') and r.get('pushed') and r['sift'] > r['native'] * 1.2 and r['sift'] - r['native'] > 1]
    ranked = rank(records, known, a.min_gain)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    proposals(ranked, a.out)
    pushed = [r for r in records if r.get('pushed')]
    print('discover: %d statements, %d pushed down, %d beat native by %gx or more, %d of those in a shape a case covers, %d shapes proposed (%s)' % (
        len(records), len(pushed), sum(1 for r in pushed if r.get('sift') and r['native'] / r['sift'] >= a.min_gain), a.min_gain, sum(1 for r in pushed if r.get('sig') in known and r.get('sift') and r['native'] / r['sift'] >= a.min_gain),
        len(ranked), a.out))
    for r in differs:
        print('DIFFERENT ANSWER %s (%s): %s' % (r['id'], r['source'], re.sub(r'\s+', ' ', r['sql'])[:160]))
    for r in errors:
        print('error %s: %s' % (r['id'], r['error'][:100]))
    for r in losses:
        print('loss %s: sift %.1f ms against native %.1f ms (%s)' % (r['id'], r['sift'], r['native'], r['source']))
    for i, g in enumerate(ranked[:15], 1):
        print('%2d. score %5.0f  %2d statements  median %5.1fx  max %5.1fx  [%s]  e.g. %s' % (i, g['score'], g['n'], g['median'], g['max'], g['sig'] or '-', g['example']['id']))
    sys.exit(1 if differs else 0)


if __name__ == '__main__':
    main()
