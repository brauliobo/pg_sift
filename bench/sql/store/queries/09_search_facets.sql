-- Search "wireless headphones", shipped orders of $50 or more: orders per category
-- key: 1
-- label: `count(*) WHERE title @@> 'a b', ... GROUP BY category`
--@ pg
SELECT category, count(*) AS n FROM orders WHERE to_tsvector('simple', title) @@ plainto_tsquery('simple', 'wireless headphones') AND status = 'shipped' AND amount >= 50 GROUP BY category ORDER BY n DESC, category LIMIT 10
--@ duck
SELECT category, count(*) AS n FROM orders WHERE lower(title) LIKE '%wireless%' AND lower(title) LIKE '%headphones%' AND status = 'shipped' AND amount >= 50 GROUP BY category ORDER BY n DESC, category LIMIT 10
--@ sift
SELECT category, count(*) AS n FROM orders WHERE title @@> 'wireless headphones' AND status = 'shipped' AND amount >= 50 GROUP BY category ORDER BY n DESC, category LIMIT 10
--@ ch
SELECT category, count() AS n FROM store.orders WHERE hasAllTokens(title, ['wireless', 'headphones']) AND status = 'shipped' AND amount >= 50 GROUP BY category ORDER BY n DESC, category LIMIT 10
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "track_total_hits": false, "query": {"bool": {"must": [{"match": {"title": {"query": "wireless headphones", "operator": "and"}}}], "filter": [{"term": {"status": "shipped"}}, {"range": {"amount": {"gte": 50}}}]}}, "aggs": {"c": {"terms": {"field": "category", "size": 10, "shard_size": 1000, "order": [{"_count": "desc"}, {"_key": "asc"}]}}}}
rows: aggregations.c.buckets -> key doc_count
