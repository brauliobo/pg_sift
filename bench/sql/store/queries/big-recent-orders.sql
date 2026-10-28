-- How many orders over $100 in the last 30 days
-- order: 7
-- label: `count(*) WHERE amount > 100, created_at >= ...`
--@ pg
SELECT count(*) FROM orders WHERE amount > 100 AND created_at >= '2025-12-02'
--@ ch
SELECT count() FROM store.orders WHERE amount > 100 AND created_at >= '2025-12-02'
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "track_total_hits": true, "query": {"bool": {"filter": [{"range": {"amount": {"gt": 100}}}, {"range": {"created_at": {"gte": "2025-12-02 00:00:00"}}}]}}}
rows: hits.total.value
