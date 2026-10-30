# The charts of the README benchmarks as a package; the names bench/steps/charts.py and the other steps import (the docstring below says what each chart is).
# Each module states its own responsibility in its first two lines.
"""The charts of the README's benchmarks, standard library only: wide, one message each, direct labels, light and dark.

Called by steps/charts.py (bench/bench charts), which writes every `NAME-light.svg` and `NAME-dark.svg` into docs/img. The systems keep the colour and
marker of store_charts.SYSTEMS in every chart (pg_duckdb is one more system among them); every chart names its data files and the host in its foot. Same results, same bytes.

  agg-latency      the seven aggregate questions, one dot per system on a log axis (store.json, duckdb.tsv)
  agg-speedup      the same medians as a factor over PostgreSQL, warm and cold (duckdb.tsv has its own PostgreSQL for its factor)
  scale-growth     how much longer the median question takes at 400 times the rows, per system, with the log-log slope (store.json, store-scaling.json, duckdb*.tsv)
  scale-questions  the same data, one panel per question, with the slope of each line
  cases-speedup    every case of bench/cases.tsv: PostgreSQL's time over pg_sift's, by family (cases.tsv)
  cases-ecdf       the share of cases (and of the real query shapes) that are at least as fast as x times PostgreSQL
  shapes-speedup   the real query shapes by the size of the filter set (analytics.json)
  cases-scale      time against rows for the case families, with the log-log slope of pg_sift and of PostgreSQL (cases-scale.tsv)
  cases-workmem    pg_sift and PostgreSQL at work_mem 64MB and at a large 4GB (cases-wm4g.tsv)
  cost-per-row     index bytes per row, load time per row and update rate (store.json)
"""
from .base import HEADER, ORDER, SHORT, SYSTEMS, W, Wide, dot
from .results import Duck, engines_of, fig, foot_lines, lat, load, questions
from .aggregates import agg_latency, agg_speedup
from .scale import growth_of, rows_label, scale_growth, scale_points, scale_questions, scale_takeaway, slope
from .cases_charts import FAMILY, case_ratios, cases_ecdf, cases_speedup, ecdf_points, family, shape_ratios, shapes_latency, shapes_speedup, shapes_tier
from .cases_scale import cases_scale, cases_workmem, cost_per_row, scale_series
from .all_charts import make
