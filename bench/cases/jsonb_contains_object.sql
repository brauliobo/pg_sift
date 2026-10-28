-- doc @> '{...}' on a jsonb attribute whose paths are leaves of the index: the members of the object are tests of the leaves
-- label: `count(*) WHERE doc @> '{"k": 42, "o": {"a": 3}}'`, two scalar members, one in a nested object
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN doc jsonb;
UPDATE orders SET doc = jsonb_build_object('k', id % 100, 'o', jsonb_build_object('a', id % 7), 'tags', jsonb_build_array('t' || id % 50));
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, doc) WITH (paths = 'doc.k int4, doc.o.a int4, doc.tags[] text')
--@ pg
SELECT count(*) AS n FROM orders WHERE doc @> '{"k": 42, "o": {"a": 3}}'
