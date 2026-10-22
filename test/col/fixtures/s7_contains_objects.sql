-- S7: containment of several objects (each by some element, the same one or not), the commuted <@, an integer member, and NOT on a NOT NULL column
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders,
       count(*) FILTER (WHERE o.items @> '[{"category": "Toys"}, {"category": "Books", "qty": 2}]') AS two_objects,
       count(*) FILTER (WHERE '[{"kind": "gift"}]' <@ o.items) AS gift,
       count(*) FILTER (WHERE NOT o.notes @> '[{"type": "kind_a", "author_kind": "type_b"}]') AS no_role_b_note
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
GROUP BY 1
ORDER BY 1
