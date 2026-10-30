# The results the charts read (store.json, duckdb.tsv, the scale files) and the lookups on them.
# lat() and fig() give the median of a question for an engine and the figure that names its data file.

import os

import store_charts as sc
import triple
from store_charts import Store

from .base import ORDER


# ---- the results ------------------------------------------------------------------------------------------------------------------------

class Duck:
    """bench/results/duckdb.tsv (and duckdb-10000000.tsv): pg_duckdb's own run, with its own PostgreSQL and pg_sift next to it"""

    def __init__(self, path):
        self.meta, self.rows = sc.read_tsv(path)
        self.v, self.note = {}, {}
        for r in self.rows:
            k = (r['case'].split(' ')[0], r['arm'], r['metric'])
            if r['value'] not in ('NA', 'error', ''):
                self.v[k] = float(r['value'])
            elif r['value'] == 'NA':   # a measure the lane cannot give: the note says why
                self.note[k] = r['note']

    def arm(self, q, metric='warm_ms'):
        """the faster of pg_duckdb's two modes on a question, None when neither ran it"""
        have = [a for a in ('duckdb_heap', 'duckdb_parquet') if (q, a, metric) in self.v]
        return min(have, key=lambda a: self.v[q, a, metric]) if have else None

    def best(self, q, metric='warm_ms'):
        a = self.arm(q, metric)
        return self.v[q, a, metric] if a else None

    def own(self, q, arm, metric='warm_ms'):
        return self.v.get((q, arm, metric))

    def fig(self, q, arm, variant='warm'):
        """(ms, cpu_ms, mb) of a lane of the run on a question (steps/triple.py); a measure the run lacks is `NA` with the lane's own reason when it gave one"""
        t = self.v.get((q, arm, variant + '_ms'))
        miss = lambda m: triple.NA(self.note.get((q, arm, variant + '_' + m)) or triple.NOT_MEASURED)  # noqa: E731
        return t and (t, *(self.v.get((q, arm, variant + '_' + m), miss(m)) for m in ('cpu_ms', 'read_mb')))


def load(results):
    store = os.path.join(results, 'store.json')
    scaling = os.path.join(results, 'store-scaling.json')
    d = {'S': Store(store), 'SS': Store(store, scaling) if os.path.exists(scaling) else None}
    for k, f in (('D1', 'duckdb.tsv'), ('D10', 'duckdb-10000000.tsv')):
        d[k] = Duck(os.path.join(results, f)) if os.path.exists(os.path.join(results, f)) else None
    return d


def questions(S):
    return S.queries


def engines_of(S, tier, D):
    es = [e for e in ORDER if e in S.engines(tier) or (e == 'duckdb' and D)]
    return es


def lat(S, D, tier, e, q, variant='warm'):
    """(value, min, max) of a system on a question; pg_duckdb: the faster of its two modes"""
    if e == 'duckdb':
        v = D.best(q, 'warm_ms' if variant == 'warm' else 'cold_ms') if D else None
        return (v, v, v) if v else None
    r = S.lat(tier, e, q, variant)
    return (r['value'], r['min'], r['max']) if r else None


def fig(S, D, tier, e, q, variant='warm'):
    """(ms, cpu_ms, mb) of a system on a question (steps/triple.py); pg_duckdb: the faster of its two modes, whose runs record the time only"""
    if e == 'duckdb':
        a = D.arm(q, variant + '_ms') if D else None
        return a and D.fig(q, a, variant)
    return triple.fig(lambda m: S.get(tier, e, q, variant, m))


def foot_lines(S, tier, D, what):
    t = S.tiers[tier]
    s = t['servers']
    host = S.host()
    out = [f'Data: bench/results/store.json{", duckdb.tsv" if D else ""}, {t["parts"]["warm"]["date"][:10]}, {host}, {S.meta["host"]["memory"]} per server, {t["cpus"]} CPUs.',
           f'PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]}, Elasticsearch {s["es"]}, ClickHouse {s["ch"]}, Citus {s["citus_columnar"]}' + (f', pg_duckdb {D.meta["pg_duckdb"].split(",")[0]}.' if D else '.') + ' ' + what]
    return out
