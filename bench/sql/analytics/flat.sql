-- What an application does today without a columnar index: the arrays and the metrics flattened into child tables, one row per item,
-- per metric and per note, with the columns of the order that the queries filter and group by, and the btree that serves them.
\set ON_ERROR_STOP on
DROP TABLE IF EXISTS pg_items, pg_metrics, pg_notes;
CREATE TABLE pg_items AS
SELECT o.id AS order_id, (i.ord - 1)::int AS idx, o.tenant_id, o.customer_id, o.source, o.placed_at, o.country_code, o.lang,
       o.attributes->>'channel' AS channel, (o.attributes->>'normalized') IS NOT NULL AS normalized, i.v->>'category' AS category,
       i.v->>'sku' AS sku, i.v->>'kind' AS kind, (i.v->>'qty')::int AS qty, (i.v->>'price')::float8 AS price,
       (i.v->>'discount')::float8 AS discount, (o.metrics->>'m_total')::float8 AS m_total, o.deleted_at
FROM orders o, jsonb_array_elements(o.items) WITH ORDINALITY AS i(v, ord);
CREATE TABLE pg_metrics AS
SELECT o.id AS order_id, o.tenant_id, o.customer_id, o.source, o.placed_at, o.country_code, o.lang, kv.key AS metric_key,
       (kv.value #>> '{}')::float8 AS value, o.deleted_at
FROM orders o, jsonb_each(o.metrics) kv;
CREATE TABLE pg_notes AS
SELECT o.id AS order_id, (n.ord - 1)::int AS idx, n.v->>'type' AS type, n.v->>'author_kind' AS author_kind
FROM orders o, jsonb_array_elements(o.notes) WITH ORDINALITY AS n(v, ord);
CREATE INDEX pg_items_store_placed ON pg_items (tenant_id, placed_at);
CREATE INDEX pg_metrics_store_placed ON pg_metrics (tenant_id, placed_at);
CREATE INDEX pg_notes_order ON pg_notes (order_id);
VACUUM (ANALYZE) pg_items;
VACUUM (ANALYZE) pg_metrics;
VACUUM (ANALYZE) pg_notes;
