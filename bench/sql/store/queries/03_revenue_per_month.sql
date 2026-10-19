-- Revenue per month
-- key: 1
-- label: `sum(amount) GROUP BY month`
--@ pg
SELECT date_trunc('month', created_at)::date AS month, sum(amount) FROM orders GROUP BY 1
--@ ch
SELECT toStartOfMonth(created_at) AS month, sum(amount) FROM store.orders GROUP BY month
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "aggs": {"m": {"date_histogram": {"field": "created_at", "calendar_interval": "month", "format": "yyyy-MM-dd", "min_doc_count": 1}, "aggs": {"revenue": {"sum": {"field": "amount"}}}}}}
rows: aggregations.m.buckets -> key_as_string revenue.value
