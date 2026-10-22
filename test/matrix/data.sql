-- The fixed part of the data of the variant matrices (test/matrix/matrix.py builds the fact tables, one column per key type the server takes):
-- the helpers, the dimension tables of the joins and a second indexed table. psql -f data.sql with :rows replaced by the size of the fact tables.
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.async_maintenance = off;

CREATE FUNCTION mx_h(i int, salt int) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT hashint4(i * 7 + salt) & 2147483647 $$;
CREATE FUNCTION mx_words(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
							 'thanks','wrong','size','color','cable','wire','box','label'])[1 + mx_h(seed * 31 + w, 1) % 20], ' ')
	FROM generate_series(1, 3 + seed % 5) w $$;
CREATE FUNCTION mxf_imm(i int) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT i % 7 $$;
CREATE FUNCTION mxf_stab(i int) RETURNS int LANGUAGE sql STABLE AS $$ SELECT i % 7 $$;
CREATE FUNCTION mxf_vol(i int) RETURNS int LANGUAGE sql VOLATILE AS $$ SELECT i % 7 $$;

CREATE TABLE dims (id int PRIMARY KEY, kind text, label text, w numeric, grp int);
INSERT INTO dims SELECT i, 'kind_' || chr(97 + i % 5), 's' || lpad(i::text, 6, '0'), i % 9, i % 4 FROM generate_series(0, 44) i;
CREATE TABLE dims_h AS SELECT * FROM dims WHERE id < 20;
ALTER TABLE dims_h ADD PRIMARY KEY (id);
CREATE TABLE dims2 (id int PRIMARY KEY, name text);
INSERT INTO dims2 SELECT i, 'g' || i FROM generate_series(0, 3) i;
CREATE TABLE dims_p (id int, kind text) PARTITION BY HASH (id);
CREATE TABLE dims_p_0 PARTITION OF dims_p FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE dims_p_1 PARTITION OF dims_p FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO dims_p SELECT i, 'kind_' || chr(97 + i % 5) FROM generate_series(0, 44) i;
CREATE VIEW dims_v AS SELECT id, kind FROM dims WHERE id < 40;
-- a second indexed table, to join two
CREATE TABLE orders2 (id int PRIMARY KEY, cat int, amount numeric(10,2));
INSERT INTO orders2 SELECT i, mx_h(i, 20) % 40, (mx_h(i, 21) % 5000)::numeric / 10 FROM generate_series(1, :rows / 4) i;
CREATE INDEX orders2_col ON orders2 USING sift_col (id, cat, amount);
ANALYZE dims; ANALYZE dims_h; ANALYZE dims2; ANALYZE dims_p; ANALYZE orders2;
