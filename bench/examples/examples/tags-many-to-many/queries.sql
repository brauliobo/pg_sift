-- query: items-with-any-of-tags
-- kind: set_algebra
-- join: semi
SELECT count(*) AS items FROM item i WHERE EXISTS (SELECT 1 FROM item_tag it JOIN tag t ON t.id = it.tag_id WHERE it.item_id = i.id AND t.label IN ('w1', 'w2', 'w3'));

-- query: items-with-all-of-tags
-- kind: set_algebra
-- join: inner
SELECT count(*) AS items FROM (SELECT it.item_id FROM item_tag it JOIN tag t ON t.id = it.tag_id WHERE t.label IN ('w1', 'w2') GROUP BY it.item_id HAVING count(DISTINCT t.label) = 2) x;

-- query: items-with-none-of-tags
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS items FROM item i WHERE NOT EXISTS (SELECT 1 FROM item_tag it JOIN tag t ON t.id = it.tag_id WHERE it.item_id = i.id AND t.label IN ('w1', 'w2', 'w3'));

-- query: distinct-items-by-tag-group
-- kind: count_distinct_across_join
-- join: inner
SELECT t.grp, count(DISTINCT it.item_id) AS items, count(*) AS links FROM item_tag it JOIN tag t ON t.id = it.tag_id GROUP BY t.grp;

-- query: event-amount-by-tag-group
-- kind: sum_avg
-- join: inner
SELECT t.grp, count(*) AS n, sum(e.amount) AS amount FROM event e JOIN item_tag it ON it.item_id = e.item_id JOIN tag t ON t.id = it.tag_id GROUP BY t.grp;

-- query: distinct-events-in-tag-group
-- kind: count_distinct_across_join
-- join: semi
SELECT count(*) AS events, sum(e.amount) AS amount FROM event e
WHERE EXISTS (SELECT 1 FROM item_tag it JOIN tag t ON t.id = it.tag_id WHERE it.item_id = e.item_id AND t.grp = 'w1');

-- query: top-tags-by-event-amount
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT t.id, t.label, sum(e.amount) AS amount FROM event e JOIN item_tag it ON it.item_id = e.item_id JOIN tag t ON t.id = it.tag_id
GROUP BY t.id, t.label ORDER BY amount DESC, t.id LIMIT 10;

-- query: tags-per-item-histogram
-- kind: group_by_dim_attr
-- join: left
SELECT n, count(*) AS items FROM (SELECT i.id, count(it.tag_id) AS n FROM item i LEFT JOIN item_tag it ON it.item_id = i.id GROUP BY i.id) x GROUP BY n;

-- query: kind-by-tag-filter
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT i.kind, count(*) FILTER (WHERE t.grp = 'w2') AS in_group2, count(DISTINCT it.item_id) FILTER (WHERE t.label = 'w1') AS with_w1
FROM item i JOIN item_tag it ON it.item_id = i.id JOIN tag t ON t.id = it.tag_id GROUP BY i.kind;

-- query: tags-by-group-right
-- kind: group_by_dim_attr
-- join: right
SELECT t.grp, count(it.item_id) AS links, count(*) AS rows FROM item_tag it RIGHT JOIN tag t ON t.id = it.tag_id GROUP BY t.grp;

-- query: item-link-match-counts-full
-- kind: anti_join_count
-- join: full
SELECT count(*) FILTER (WHERE it.item_id IS NULL) AS untagged_items, count(*) FILTER (WHERE i.id IS NULL) AS dangling_links, count(*) AS rows FROM item i FULL JOIN item_tag it ON it.item_id = i.id;
