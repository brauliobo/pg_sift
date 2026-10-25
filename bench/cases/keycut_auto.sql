-- a lookup of the rows of one tenant on an index created with no `key_cut`: CREATE INDEX samples the heap and cuts the units by the tenant when its rows are scattered over the units (15% of them outside their range of the heap), no other leaf follows the table and the index does not grow by more than 3%
-- label: `count(*), sum(a1) WHERE tenant_id = <one tenant>`, 400,000 rows of 320 tenants (85% of a tenant's rows together) in a table of fillfactor 10, an 11-column index with no `key_cut` option, no btree on the column
--@ setup
CREATE TABLE keyed (id int, tenant_id int, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, a7 int, a8 int, a9 int, a10 int, pad text) WITH (fillfactor = 10);
INSERT INTO keyed SELECT i, CASE WHEN hashint4(i) % 100 < 15 THEN abs(hashint4(i + 1)) % 320 ELSE i / 1250 END, (i * 31) % 1000, (i * 37) % 1000, (i * 41) % 1000, (i * 43) % 1000, (i * 47) % 1000, (i * 53) % 1000, (i * 59) % 1000, (i * 61) % 1000, (i * 67) % 1000, (i * 71) % 1000, repeat('x', 100) FROM generate_series(1, 400000) i;
VACUUM (FREEZE, ANALYZE) keyed;
CREATE INDEX keyed_col ON keyed USING sift_col (tenant_id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10)
--@ pg
SELECT count(*) AS n, sum(a1) AS total FROM keyed WHERE tenant_id = 137
