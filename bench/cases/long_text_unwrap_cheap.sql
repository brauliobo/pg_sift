-- a substring test of a column of long text (300 bytes of words, stored inline): the strings are LZ4 where the default formula says so, and a test unwraps them in every query
-- label: `count(*)` of `WHERE note LIKE '%<two words>%'` over 300-byte notes of a skewed vocabulary, no btree and no trigram index on `note`
-- ordered: no
--@ setup
DROP INDEX orders_col;
ALTER TABLE orders ADD COLUMN note text;
WITH v AS (SELECT array_agg(substr(md5('w' || k), 1, 2 + k % 6) ORDER BY k) AS a FROM generate_series(0, 399) k)
UPDATE orders SET note = (SELECT string_agg(v.a[1 + floor(400 * power((hashint8extended(orders.id * 64 + w, 0) & 16777215) / 16777216.0, 2))::int], ' ') FROM generate_series(1, 60) w) FROM v;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, note sift.col_text_ops, amount sift.col_numeric_ops)
--@ pg
SELECT count(*) AS n FROM orders WHERE note LIKE '%' || substr(md5('w3'), 1, 5) || ' ' || substr(md5('w4'), 1, 6) || '%'
