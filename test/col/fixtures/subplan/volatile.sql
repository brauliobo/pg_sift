-- trap: random() in the VALUES list (always true here, so that both plans agree): the list is not the same at every evaluation, the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT m.label, count(*) AS n
FROM orders o JOIN (VALUES ('web', 'Online', random() < 2), ('ios', 'Mobile', random() < 2)) AS m(source, label, ok) ON o.source = m.source AND m.ok
GROUP BY m.label ORDER BY m.label
