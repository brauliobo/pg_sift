"""
pg_sift 6.0 focused scaling benchmark and telemetry harness (bench/bench v6-core).

Measures scaling growth factors across a 3-point series (default 10k -> 40k -> 160k rows,
4x jumps; --scales / --release-scale change the ladder) under tight memory budgets
(maintenance_work_mem=4MB, work_mem=4MB), plus OLTP mutations, 2% dirty-state resilience,
analytical queries Q1-Q8 with 1-vs-4 worker parallel scaling, idle pooled backend memory,
index size, page utilization, WAL per row and compaction write amplification.

Runs against a running cluster that has pg_sift preloaded (bench/run_round.sh builds a
throwaway one). Writes <out-dir>/<round>.tsv and .md; with --docs also docs/results/v6-core.tsv,
the file scripts/gen-docs.py reads (bench/compare_rounds.py diffs two such files).

Uses strictly neutral public-repo naming (events, entity_id, parent_id,
entity_parents, metrics, notes, analysis) and zero external Python dependencies.
"""

from .client import PgClient
from .config import INDEX_COLS, INDEX_KEY_ORDER, INDEX_PATHS
from .schema import events_insert_sql, events_table_ddl
