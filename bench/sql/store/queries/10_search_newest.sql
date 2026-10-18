-- Search "wireless headphones", shipped orders: the 10 newest
--@ pg
SELECT id, created_at, title FROM orders WHERE to_tsvector('simple', title) @@ plainto_tsquery('simple', 'wireless headphones') AND status = 'shipped' ORDER BY created_at DESC, id DESC LIMIT 10
--@ sift
SELECT id, created_at, title FROM orders WHERE title @@> 'wireless headphones' AND status = 'shipped' ORDER BY created_at DESC, id DESC LIMIT 10
--@ ch
SELECT id, created_at, title FROM store.orders WHERE hasAllTokens(title, ['wireless', 'headphones']) AND status = 'shipped' ORDER BY created_at DESC, id DESC LIMIT 10
--@ es
POST /orders/_search?request_cache={rc}
{"size": 10, "track_total_hits": false, "_source": ["id", "created_at", "title"], "query": {"bool": {"must": [{"match": {"title": {"query": "wireless headphones", "operator": "and"}}}], "filter": [{"term": {"status": "shipped"}}]}}, "sort": [{"created_at": "desc"}, {"id": "desc"}]}
rows: hits.hits -> _source.id _source.created_at _source.title
