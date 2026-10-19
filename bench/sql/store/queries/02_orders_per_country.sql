-- How many orders per country
-- key: 1
-- label: `count(*) GROUP BY country`
--@ pg
SELECT country, count(*) FROM orders GROUP BY country
--@ ch
SELECT country, count() FROM store.orders GROUP BY country
--@ es
POST /orders/_search?request_cache={rc}
{"size": 0, "aggs": {"c": {"terms": {"field": "country", "size": 100}}}}
rows: aggregations.c.buckets -> key doc_count
