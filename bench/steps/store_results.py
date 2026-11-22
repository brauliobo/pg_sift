"""docs/results/store.json and store-scaling.json as rows by (tier, engine, class, variant, metric), standard library only: the reader of the stages' time budgets (steps/budget.py)."""
import json

from store_lib import load_queries

QUESTIONS = [q.id for q in load_queries()]   # the questions in the order of their `-- order:` lines
SEARCH = ('search-count', 'search-facets', 'search-newest')   # the full-text questions of the store set: measured with the search benchmarks, not part of the aggregate set


class Store:
    """docs/results/store.json (and store-scaling.json): rows by (tier, engine, class, variant, metric)"""

    def __init__(self, *paths):
        self.rows, self.meta, self.tiers = {}, None, {}
        for p in paths:
            d = json.load(open(p))
            self.meta = self.meta or d['meta']
            self.tiers.update(d['meta']['tiers'])
            for r in d['rows']:
                self.rows[(r['tier'], r['engine'], r['class'], r['variant'], r['metric'])] = r
        self.queries = sorted({k[2] for k in self.rows if k[4] == 'latency_ms' and k[2] not in ('-',) + SEARCH}, key=QUESTIONS.index)

    def get(self, tier, engine, cls='-', variant='-', metric=None):
        return self.rows.get((tier, engine, cls, variant, metric))

    def v(self, *a, **k):
        r = self.get(*a, **k)
        return r['value'] if r else None

    def tier_of(self, rows, cpus):
        """the result set with that many rows and CPUs ('1M-c3')"""
        return next((t for t, m in self.tiers.items() if m['rows'] == rows and m.get('cpus') == cpus), None)
