"""`clickbench.py report`: docs/results/clickbench.tsv -> the ClickBench block of docs/BENCHMARKS.md and docs/charts/clickbench{,-dark}.svg (the chart of charts/clickbench.py). No number is typed by hand."""
import math
import os

import resultfile
from charts import clickbench as chart
from charts.data import load_clickbench
from mdtable import ms, tables

LABEL = {'pg': 'PG 18', 'sift_table': '`sift_table`', 'sift_col': '`sift_col`', 'ch': 'ClickHouse', 'ch_proj': 'ClickHouse + PROJECTION', 'duckdb': 'DuckDB'}
GROUPS = [('warm_ms', 'Warm, wall ms (client clock)'), ('warm_server_ms', 'Warm, server ms (engine figure)'),
          ('cold_ms', 'Cold, wall ms'), ('cold_server_ms', 'Cold, server ms')]


def geomean(xs):
    return math.exp(sum(math.log(max(x, 0.001)) for x in xs) / len(xs)) if xs else None


def summary(rows, arms, metric):
    return {a: geomean([float(r['value']) for r in rows if r['arm'] == a and r['metric'] == metric and r['value'] not in ('', 'NA')]) for a in arms}


def block(meta, rows):
    arms = [a for a in LABEL if any(r['arm'] == a for r in rows)]
    cases = sorted({r['case'] for r in rows}, key=lambda c: int(c[1:]))
    equal = {a: sum(1 for r in rows if r['arm'] == a and r['metric'] == 'md5_equal' and r['value'] == '1') for a in arms}
    metrics = [(m, t) for m, t in GROUPS if any(r['metric'] == m and r['value'] != 'NA' for r in rows)]
    out = ['ClickBench `Q0`-`Q%d` on %s `hits` rows (%s data), %s; %s threads; pg_sift projections (`SIFT_PT_PROJDIR`) %s.' % (
        len(cases) - 1, meta['tier'], meta['source'], meta['date'], meta['threads'], meta['projections']), '',
        '`md5_equal` against stock PostgreSQL (canonical row order, `AVG` columns to 9 significant digits, everything else exact): %s.' % ', '.join(
            '%s %d/%d' % (LABEL[a], equal[a], len(cases)) for a in arms), '']
    for m, title in metrics:
        geo = summary(rows, arms, m)
        lines = tables(cases, [(title, [(a, LABEL[a]) for a in arms])], lambda c, a: ms(resultfile.num(rows, case=c, arm=a, metric=m)),
                       max_cols=len(arms), first='query')
        out += lines + ['| geometric mean | ' + ' | '.join(ms(geo[a]) for a in arms) + ' |', '']
    out += ['**How to read it.** `wall` is the client clock around one request (PostgreSQL: the `psql` round trip with the rows discarded; ClickHouse: one HTTP '
            'request, response read; DuckDB: the CLI round trip). `server` is the engine\'s own figure: PostgreSQL `EXPLAIN (ANALYZE, TIMING OFF)` Execution Time of a '
            'separate run, ClickHouse `X-ClickHouse-Summary` `elapsed_ns`, DuckDB `.timer` (1 ms resolution). Warm: the median of the timed runs after the untimed ones. Cold: '
            'the engine\'s own caches are emptied before each run (`pg_buffercache_evict_all()` or a restart, ClickHouse `SYSTEM DROP ... CACHE`, a DuckDB restart) and the '
            'operating system\'s page cache is `%s`. ClickHouse `PROJECTION`s used by the timed queries: %s.' % (
                meta['os_cache'], meta.get('clickhouse_projections_used', '-'))]
    return out


def svg(inp, mode):
    return chart.render({'clickbench': load_clickbench(inp)}, mode)


def main(a):
    meta, rows = resultfile.read(a.inp)
    if a.md:
        resultfile.replace_block(a.md, 'clickbench-%s' % meta['tier'].lower().replace(' ', ''), block(meta, rows))
    if a.svg:
        os.makedirs(os.path.dirname(os.path.abspath(a.svg)), exist_ok=True)
        stem, ext = os.path.splitext(a.svg)
        for mode, path in (('light', a.svg), ('dark', f'{stem}-dark{ext}')):
            open(path, 'w', newline='\n').write(svg(a.inp, mode))
    print('\n'.join(block(meta, rows)[:3]))
