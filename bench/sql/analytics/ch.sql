-- ClickHouse layout of the comparison, tuned the way the ClickHouse documentation says for this access pattern: sorted by the filter
-- column first (store_id), LowCardinality strings, no Nullable, plain MergeTree, Float32 for prices and metrics (multiples of 0.25 and
-- 0.5: exact). Filled from the flat tables of PostgreSQL (bench/steps/analytics.sh) so that both engines hold the same rows.
DROP TABLE IF EXISTS items_tuned;
DROP TABLE IF EXISTS metrics_tuned;
DROP TABLE IF EXISTS metrics_map;
DROP TABLE IF EXISTS categories_map;
DROP TABLE IF EXISTS kinds_map;
CREATE TABLE items_tuned (
  store_id UInt32, customer_id UInt32, source LowCardinality(String), placed_at Date, order_id UInt32, idx UInt8, country_code LowCardinality(String),
  lang LowCardinality(String), channel LowCardinality(String), normalized UInt8, category LowCardinality(String), sku String,
  kind LowCardinality(String), qty UInt8, price Float32, discount Float32, overall Float32, deleted UInt8
) ENGINE = MergeTree ORDER BY (store_id, placed_at, order_id, idx);
CREATE TABLE metrics_tuned (
  store_id UInt32, customer_id UInt32, source LowCardinality(String), placed_at Date, order_id UInt32, country_code LowCardinality(String),
  lang LowCardinality(String), metric_key LowCardinality(String), value Float32, deleted UInt8
) ENGINE = MergeTree ORDER BY (store_id, placed_at, order_id);
CREATE TABLE metrics_map (source String, metric_key String, mapped_metric String, mapped_aspect String) ENGINE = MergeTree ORDER BY (source, metric_key);
CREATE TABLE categories_map (category String, mapped_category String) ENGINE = MergeTree ORDER BY category;
CREATE TABLE kinds_map (kind String, mapped_kind String) ENGINE = MergeTree ORDER BY kind;
