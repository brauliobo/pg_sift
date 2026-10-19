-- Average value of the shipped orders of one country in one year
-- label: `avg(amount) WHERE status='shipped', ...`
--@ pg
SELECT avg(amount) FROM orders WHERE status = 'shipped' AND country = 'DE' AND created_at >= '2025-01-01' AND created_at < '2026-01-01'
--@ ch
SELECT avg(amount) FROM store.orders WHERE status = 'shipped' AND country = 'DE' AND created_at >= '2025-01-01' AND created_at < '2026-01-01'
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "track_total_hits": false, "query": {"bool": {"filter": [{"term": {"status": "shipped"}}, {"term": {"country": "DE"}}, {"range": {"created_at": {"gte": "2025-01-01 00:00:00", "lt": "2026-01-01 00:00:00"}}}]}}, "aggs": {"a": {"avg": {"field": "amount"}}}}
rows: aggregations.a.value
