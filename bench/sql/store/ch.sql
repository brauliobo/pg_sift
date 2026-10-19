-- ClickHouse, the way its documentation lays out a table for these filters: one MergeTree partition per month, the rows sorted by the
-- columns the filters name (low-cardinality ones first), LowCardinality strings, Decimal for money, a text index for the title search
-- and a Bloom filter for the lookup by id (the sorting key does not start with it). `status` and `amount` are not in the sorting key: ClickHouse
-- cannot UPDATE a key column, and the update benchmark changes both. The two settings are what lightweight UPDATE and DELETE (patch parts)
-- need. Sections: `table` before the load, `after` once loaded.
--@ table
CREATE DATABASE IF NOT EXISTS store;
CREATE TABLE store.orders (
  id          UInt64,
  customer_id UInt32,
  country     LowCardinality(String),
  category    LowCardinality(String),
  status      LowCardinality(String),
  amount      Decimal(10, 2),
  quantity    UInt8,
  created_at  DateTime('UTC'),
  title       String,
  INDEX id_bloom id TYPE bloom_filter(0.001) GRANULARITY 1,
  INDEX title_tokens title TYPE text(tokenizer = 'splitByNonAlpha') GRANULARITY 1
) ENGINE = MergeTree PARTITION BY toYYYYMM(created_at) ORDER BY (country, created_at, id)
SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1;
--@ after
OPTIMIZE TABLE store.orders FINAL;
--@ rmt
-- the table of the update benchmark that keeps changes as new row versions (ReplacingMergeTree, read with FINAL): the same layout plus a version
-- and a deleted flag; the sorting key is the old one, which no update changes
CREATE TABLE store.orders_rmt (
  id          UInt64,
  customer_id UInt32,
  country     LowCardinality(String),
  category    LowCardinality(String),
  status      LowCardinality(String),
  amount      Decimal(10, 2),
  quantity    UInt8,
  created_at  DateTime('UTC'),
  title       String,
  version     UInt64,
  is_deleted  UInt8,
  INDEX id_bloom id TYPE bloom_filter(0.001) GRANULARITY 1,
  INDEX title_tokens title TYPE text(tokenizer = 'splitByNonAlpha') GRANULARITY 1
) ENGINE = ReplacingMergeTree(version, is_deleted) PARTITION BY toYYYYMM(created_at) ORDER BY (country, created_at, id)
SETTINGS clean_deleted_rows = 'Always';
