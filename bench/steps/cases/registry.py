# The registry of the cases (bench/cases.tsv) and the SQL files they name.
# A Case is a row of the registry; share_builds() lets cases with one dataset share a table.

import os
import re

from store_lib import sections

from .runcfg import DATASETS, FIELDS, FORCED, INDEX_COLUMNS, REPO, die


# --- the registry

class Case:
    def __init__(self, row, text):
        self.__dict__.update(zip(FIELDS, row))
        self.sec = sections(text)
        head = self.sec.pop('').split('\n')
        self.title = head[0][3:]
        self.label = next((m[10:] for m in head if m.startswith('-- label: ')), self.id)
        self.opt = dict(m[3:].split(': ', 1) for m in head if re.match(r'-- (ordered|vacuum|set|fresh|base): ', m))
        self.load = 'load' in self.sec
        self.baselines = [b for b in self.baselines.split(',') if b != '-']
        self.variants = [v.split(':', 1) for v in self.variants.split(',') if v != '-']
        self.fallback = self.plan.startswith('fallback:')   # the planner is expected to keep PostgreSQL's plan: `plan` is that plan, and a lane with the scan forced is measured beside it
        if self.fallback:
            self.plan = self.plan[len('fallback:'):]
            self.variants = [v for v in self.variants if v[0] != FORCED[0]] + [list(FORCED)]
        self.metrics = self.metric.split(',')
        self.headline = self.headline == 'yes'
        self.share = None   # read_registry: the name of the table that cases with the same index and setup read together

    def statement(self, lane):
        if lane == 'pg_duckdb':
            return self.sec.get('duck', self.sec['pg'])
        return self.sec['pg'] if lane == 'native' else self.sec.get('sift', self.sec['pg'])

    def schema(self):
        return DATASETS[self.dataset][0] if self.dataset in DATASETS else 'c_' + (self.share or self.id)

    def base_settings(self):
        """`-- base: a = 1`: the settings the second copy of the tables is built under, the baseline lane `base` reads it"""
        return self.opt.get('base')

    def table_index(self):
        m = re.match(r'default((?:\+\w+)*)(.*)$', self.index)
        return '(%s)%s' % (', '.join([INDEX_COLUMNS] + [c[1:] for c in re.findall(r'\+\w+', m.group(1))]), m.group(2)) if m else self.index


_REGISTRY = {}


def read_registry(root=REPO):
    """the cases of bench/cases.tsv, read once per root (a renderer asks for them a few dozen times); the list is the caller's own, the cases are shared"""
    if root not in _REGISTRY:
        _REGISTRY[root] = _read_registry(root)
    return list(_REGISTRY[root])


def _read_registry(root):
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(root, 'bench', 'cases.tsv')) if l.strip() and not l.startswith('#')]
    cases = []
    for r in rows:
        if len(r) != len(FIELDS):
            die('bench/cases.tsv: %s has %d columns, %d expected (%s)' % (r[0], len(r), len(FIELDS), ' '.join(FIELDS)))
        path = os.path.join(root, 'bench', 'cases', r[0] + '.sql')
        if not os.path.exists(path):
            die('bench/cases.tsv: %s has no file bench/cases/%s.sql' % (r[0], r[0]))
        cases.append(Case(r, open(path).read()))
    share_builds(cases)
    return cases


READ_ONLY = re.compile(r'\b(insert|update|delete|alter|create|drop|truncate|copy)\b', re.I)


def share_builds(cases):
    """the cases on `orders` that only read it and have the same index and setup are one build: `orders` is made, indexed and vacuumed once (the build is most of the time of a case at 1M
    rows), in a schema named after the first case of the group; a case that writes, loads or has its own dataset keeps a table of its own"""
    groups = {}
    for c in cases:
        if c.dataset == 'orders' and not c.load and not any(READ_ONLY.search(c.sec.get(k, '')) for k in ('pg', 'sift', 'duck')):
            groups.setdefault((c.index, c.sec.get('setup', ''), c.opt.get('vacuum', 'yes')), []).append(c)
    for g in groups.values():
        if len(g) > 1:
            for c in g:
                c.share = 's_' + g[0].id
