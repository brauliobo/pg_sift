from .config import MEDIAN_ENTITY_ID, MEGA_ENTITY_ID, MEGA_PARENT_ID, MID_PARENT_ID


QUERIES = {
    "Q1_scope_whole": """
        SELECT count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
    """,
    "Q2_scope_month": """
        SELECT count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
          AND e.event_date >= DATE '2024-06-01'
          AND e.event_date <  DATE '2024-07-01'
    """,
    "Q3_scope_groupby_parent": """
        SELECT ep.parent_id, count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        GROUP BY ep.parent_id
    """,
    "Q4_highcard_groupby": """
        SELECT group_key, count(*), sum(amount), avg(amount)
        FROM events
        GROUP BY group_key
    """,
    "Q5_count_distinct": """
        SELECT count(DISTINCT distinct_tag)
        FROM events
    """,
    "Q6_jsonb_shred_array": """
        SELECT e.metrics->>'channel' AS channel,
               elem->>'kind' AS item_kind,
               count(*),
               sum((elem->>'weight')::float8),
               avg((e.metrics->>'latency')::float8)
        FROM events e,
             LATERAL jsonb_array_elements(e.analysis->'items') AS elem
        GROUP BY 1, 2
    """,
    "Q7_scope_top20_unfilt": """
        SELECT e.id, e.entity_id, e.event_date, e.amount
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
        ORDER BY e.event_date DESC
        LIMIT 20
    """,
    "Q8_scope_top20_1pct": """
        SELECT e.id, e.entity_id, e.event_date, e.amount
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42 AND e.status = 0
        ORDER BY e.event_date DESC
        LIMIT 20
    """,
}

SCALING_QUERY_KEYS = [
    "Q1_scope_whole",
    "Q2_scope_month",
    "Q3_scope_groupby_parent",
    "Q7_scope_top20_unfilt",
    "Q8_scope_top20_1pct",
]

CTE6_PROBE_SQL = """
    WITH
      c1 AS (
        SELECT entity_id, count(*) AS cnt, sum(amount) AS amt
        FROM events WHERE status < 50 GROUP BY entity_id
      ),
      c2 AS (
        SELECT entity_id, avg(score) AS avg_s
        FROM events WHERE event_date >= DATE '2024-06-01' GROUP BY entity_id
      ),
      c3 AS (
        SELECT c1.entity_id, c1.cnt, c1.amt, c2.avg_s
        FROM c1 JOIN c2 ON c1.entity_id = c2.entity_id
      ),
      c4 AS (
        SELECT ep.parent_id, count(*) AS hi_cnt
        FROM events e JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE e.score > 50 GROUP BY ep.parent_id
      ),
      c5 AS (
        SELECT ep.parent_id, sum(c3.amt) AS p_amt, avg(c3.avg_s) AS p_avg
        FROM c3 JOIN entity_parents ep ON ep.entity_id = c3.entity_id
        GROUP BY ep.parent_id
      ),
      c6 AS (
        SELECT e.metrics->>'channel' AS ch, count(*) AS ch_cnt,
               avg((e.metrics->>'latency')::float8) AS avg_lat
        FROM events e WHERE jsonb_typeof(e.metrics->'latency') = 'number'
        GROUP BY 1
      )
    SELECT
      (SELECT count(*) FROM c5 JOIN c4 ON c4.parent_id = c5.parent_id WHERE c5.p_amt > 1000),
      (SELECT max(avg_lat) FROM c6)
"""

VARIANT_EXC_SQL = """
    SELECT metrics->>'channel' AS channel,
           count(*),
           avg((metrics->>'latency')::float8)
    FROM events
    WHERE jsonb_typeof(metrics->'latency') = 'number'
    GROUP BY 1
"""

F4_SOFT_DEL_NULL_SQL = f"""
    SELECT count(*), sum(e.amount), avg(e.score)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MID_PARENT_ID}
      AND e.deleted_at IS NULL
"""

F4_SOFT_DEL_NOT_NULL_SQL = f"""
    SELECT count(*), avg(e.score)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NOT NULL
"""

F5_SINGLE_ENTITY_SQL = f"""
    SELECT count(*), sum(amount), avg(score)
    FROM events
    WHERE entity_id = {MEDIAN_ENTITY_ID}
      AND deleted_at IS NULL
"""

F5_MEGA_ENTITY_SQL = f"""
    SELECT count(*), min(event_date), max(event_date), avg(score)
    FROM events
    WHERE entity_id = {MEGA_ENTITY_ID}
      AND deleted_at IS NULL
"""

F6_MATERIALIZED_CTE_SQL = f"""
    WITH ids AS MATERIALIZED (
        SELECT entity_id FROM entity_parents WHERE parent_id = {MEGA_PARENT_ID}
    )
    SELECT count(*), sum(e.amount), avg(e.score)
    FROM events e
    JOIN ids USING (entity_id)
    WHERE e.deleted_at IS NULL
"""

F7_JSONB_EXISTS_SQL = f"""
    SELECT count(*)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NULL
      AND e.analysis ? 'flag'
"""

F8_UPDATED_AT_TOP20_SQL = f"""
    SELECT e.id, e.entity_id, e.updated_at, e.amount
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NULL
    ORDER BY e.updated_at DESC
    LIMIT 20
"""


F_PROBE_SQL = {
    "F4_soft_del_null": F4_SOFT_DEL_NULL_SQL,
    "F4_soft_del_not_null": F4_SOFT_DEL_NOT_NULL_SQL,
    "F5_single_entity": F5_SINGLE_ENTITY_SQL,
    "F5_mega_entity": F5_MEGA_ENTITY_SQL,
    "F6_materialized_cte": F6_MATERIALIZED_CTE_SQL,
    "F7_jsonb_exists": F7_JSONB_EXISTS_SQL,
    "F8_updated_at_top20": F8_UPDATED_AT_TOP20_SQL,
}
TOPN_ORDER_KEY = {"Q7_scope_top20_unfilt": "event_date", "Q8_scope_top20_1pct": "event_date", "F8_updated_at_top20": "updated_at"}
