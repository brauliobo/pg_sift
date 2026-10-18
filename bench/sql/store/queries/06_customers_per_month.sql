-- How many different customers ordered each month
-- key: 1
--@ pg
SELECT date_trunc('month', created_at)::date AS month, count(DISTINCT customer_id) FROM orders GROUP BY 1
--@ ch
SELECT toStartOfMonth(created_at) AS month, count(DISTINCT customer_id) FROM store.orders GROUP BY month
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "aggs": {"m": {"date_histogram": {"field": "created_at", "calendar_interval": "month", "format": "yyyy-MM-dd", "min_doc_count": 1}, "aggs": {"customers": {"cardinality": {"field": "customer_id", "precision_threshold": 40000}}}}}}
rows: aggregations.m.buckets -> key_as_string customers.value
tol: 0.02
