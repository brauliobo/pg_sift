"""bench/engines.tsv as rows: the one reader of the engine registry for the Python steps (steps/lib.sh reads it for the shell ones). Standard library only."""
import importlib.util
import os

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELDS = ['id', 'label', 'md', 'color', 'chart', 'default', 'reference', 'kind', 'section']


def rows(kind='text'):
    """the engines of the registry as dicts, in table order: those of KIND (text, analytic; `both` belongs to either), every one for None"""
    with open(os.path.join(BENCH, 'engines.tsv')) as f:
        out = [dict(zip(FIELDS, l.rstrip('\n').split('\t'))) for l in f if l.strip() and not l.startswith('#')]
    return [r for r in out if kind is None or r['kind'] in (kind, 'both')]


def adapter(eid):
    """the module bench/engines/ID.py of an analytic engine"""
    spec = importlib.util.spec_from_file_location('engine_' + eid, os.path.join(BENCH, 'engines', eid + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
