-- the elements of an array column (docs/COL-SHRED.md): an index with a path name[] text keeps the elements of a text[] column as the list of a jsonb array, with a
-- point directory that lists the rows that own a value. Every query runs with and without the pushdown and the rows must be the same; the shapes with 'scan'
-- ran the pushdown. The same after every kind of write, and col_verify checks the index against the chunks each time. About 17 s: 54 shapes after each of 9 states
-- on ae, the index that keeps no image of the arrays, and on ad, the one that does in the states built, orders, vacuum and reindex (ad gets the same writes in between),
-- first on Format 2.0 indexes, then on Format 1 ones: only Format 1 keeps the lengths of the arrays as lanes (tags#1, tags#2), and there the element tests of ae run in
-- the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;
SET work_mem = '16MB';

SET sift.test_create_format1 = off;
\ir test/common/sql/col/array_elems.sql
SET sift.test_create_format1 = on;
\ir test/common/sql/col/array_elems.sql
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
