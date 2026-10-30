# The result files of the orders benchmark and the cell formatters every table uses.
# A Results reads the stored runs; ms, seconds, size and md_table make the cells.
import json
import os
from store_charts import QUESTIONS, SEARCH


class Results:
    def __init__(self, path):
        d = json.load(open(path))
        self.dir = os.path.dirname(os.path.abspath(path))
        self.meta, self.rows = d['meta'], d['rows']
        self.scaling = os.path.exists(os.path.join(os.path.dirname(path), 'store-scaling.json'))   # the sizes other than 1,000,000 orders are measured in their own file
        tm = self.meta['tiers']
        self.all_tiers = sorted(tm, key=lambda t: (tm[t]['rows'], self.cpus(t)))   # a result set per size and thread budget (`1M-c3`; older results: one per size)
        self.tiers = [t for t in self.all_tiers if 'load' in tm[t]['parts']]       # one per size: the set that holds the load, the plans and the updates
        self.sets = {t: [t] for t in self.tiers}   # published: the set of the load (3 CPUs, the budget of the search tables); the other budgets stay in the results file (`all_tiers`)

    def cpus(self, tier):
        old = str(self.meta.get('host', {}).get('cpus', ''))   # results made before the thread budget: the one quota of the host record
        return self.meta['tiers'][tier].get('cpus') or (int(old) if old.isdigit() else 0)

    def get(self, tier, engine, cls='-', variant='-', metric=None):
        for r in self.rows:
            if (r['tier'], r['engine'], r['class'], r['variant'], r['metric']) == (tier, engine, cls, variant, metric):
                return r
        return None

    def num(self, *a, **k):
        r = self.get(*a, **k)
        return r['value'] if r else None

    def engines(self, tier, among):
        return [e for e in among if any(r['tier'] == tier and r['engine'] == e and r['metric'] in ('latency_ms', 'load_s') for r in self.rows)]

    def queries(self, tier, search=False):
        """the aggregate questions of a result set; search=True: the full-text ones, which the search benchmarks own"""
        out = {}
        for r in self.rows:
            if r['tier'] == tier and r['metric'] == 'latency_ms' and (r['class'] in SEARCH) == search:
                out[r['class']] = None
        return sorted(out, key=QUESTIONS.index)


def ms(v):
    if v is None:
        return 'n/a'
    if v >= 1000:
        return f'{v:,.0f}'
    return f'{v:.3g}' if v < 100 else f'{v:.0f}'


def md_table(head, body, align=None):
    align = align or ['---'] + ['---:'] * (len(head) - 1)
    return ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|'] + ['| ' + ' | '.join(r) + ' |' for r in body]


def seconds(v):
    return 'n/a' if v is None else f'{v:.0f} s' if v >= 10 else f'{v:.1f} s'


def size(v, free=None):
    s = 'n/a' if v is None else f'{v / 1024:.1f} GiB' if v >= 10240 else f'{v:,.0f} MiB'
    return s + (f' ({free:,.0f} free)' if free else '')


def best_bold(values, cells):
    """bold the cells whose value is within 5% of the best (lowest) one"""
    present = [v for v in values if v is not None]
    best = min(present) if present else None
    return [f'**{c}**' if v is not None and v <= best * 1.05 else c for v, c in zip(values, cells)]


def join_and(names):
    return ', '.join(names[:-1]) + (' and ' if len(names) > 1 else '') + names[-1]
