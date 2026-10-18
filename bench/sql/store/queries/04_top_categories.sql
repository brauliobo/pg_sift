-- The 10 categories with the most revenue
--@ pg
SELECT category, sum(amount) AS revenue FROM orders GROUP BY category ORDER BY revenue DESC, category LIMIT 10
--@ ch
SELECT category, sum(amount) AS revenue FROM store.orders GROUP BY category ORDER BY revenue DESC, category LIMIT 10
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "aggs": {"c": {"terms": {"field": "category", "size": 10, "order": [{"revenue": "desc"}, {"_key": "asc"}]}, "aggs": {"revenue": {"sum": {"field": "amount"}}}}}}
rows: aggregations.c.buckets -> key revenue.value
