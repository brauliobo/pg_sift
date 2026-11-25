-- typeof: the type of a path tested where PostgreSQL guards a read of its values (a FILTER, a CASE) keeps the normal plan, which skips the strings
-- the leaf cannot hold, and so does a path that is no leaf of the index
-- shape: quals
-- expect: plain
-- key: -
-- scenario: yes
SELECT sum((attributes->>'weight')::float8) FILTER (WHERE jsonb_typeof(attributes->'weight') = 'number') AS total,
       sum(CASE WHEN jsonb_typeof(attributes->'tier') = 'string' THEN length(attributes->>'tier') END) AS tier_chars,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'nokey') IS NULL) AS no_key
FROM orders
