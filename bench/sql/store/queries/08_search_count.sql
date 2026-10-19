-- Search "wireless headphones": how many orders match
-- label: `count(*) WHERE title @@> 'a b'`
--@ pg
SELECT count(*) FROM orders WHERE to_tsvector('simple', title) @@ plainto_tsquery('simple', 'wireless headphones')
--@ sift
SELECT count(*) FROM orders WHERE title @@> 'wireless headphones'
--@ ch
SELECT count() FROM store.orders WHERE hasAllTokens(title, ['wireless', 'headphones'])
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "track_total_hits": true, "query": {"match": {"title": {"query": "wireless headphones", "operator": "and"}}}}
rows: hits.total.value
