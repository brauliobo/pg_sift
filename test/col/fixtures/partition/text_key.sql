-- a text group key and a text condition: the dictionaries of the partitions are read apart
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT tag, count(*) AS n, min(id) AS first_id, max(id) AS last_id FROM t_part WHERE tag IN ('red', 'blue', 'plain') AND note <> 'n3' GROUP BY tag ORDER BY tag
