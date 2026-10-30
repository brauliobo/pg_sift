# make() draws every chart of docs/img for the results that exist.
# A chart whose data file is missing is left out.

import os

from store_charts import THEMES

from .results import load
from .aggregates import agg_latency, agg_speedup
from .scale import scale_growth, scale_questions
from .cases_charts import cases_ecdf, cases_speedup, shapes_latency, shapes_speedup
from .cases_scale import cases_scale, cases_workmem, cost_per_row


# ---- the set ----------------------------------------------------------------------------------------------------------------------------

def make(results):
    out = {}
    if not os.path.exists(os.path.join(results, 'store.json')):
        return out
    d = load(results)
    have_cases = os.path.exists(os.path.join(results, 'cases.tsv'))
    have_shapes = os.path.exists(os.path.join(results, 'analytics.json'))
    for theme in THEMES:
        add = lambda name, svg: svg and out.__setitem__(f'{name}-{theme}.svg', svg)
        add('agg-latency', agg_latency(d, theme))
        add('agg-speedup', agg_speedup(d, theme))
        add('cost-per-row', cost_per_row(d, theme))
        if d['SS']:
            add('scale-growth', scale_growth(d, theme))
            add('scale-questions', scale_questions(d, theme))
        if have_cases:
            add('cases-speedup', cases_speedup(results, theme))
            add('cases-ecdf', cases_ecdf(results, theme))
            add('cases-scale', cases_scale(results, theme))
            add('cases-workmem', cases_workmem(results, theme))
        if have_shapes:
            add('shapes-speedup', shapes_speedup(results, theme))
            add('shapes-latency', shapes_latency(results, theme))
    return out
