-- Fixture of the DRY performance gate beyond the corpus lab (test/corpus/lab.sql): documents for BM25, a table with an open write buffer, a sift_table, and the sources of the
-- build / promote / merge / vacuum cases. Deterministic (no random()), generic names, sizes follow :rows (default 100000). Loaded once per base commit into the data template.
-- Every case that changes state resets it first (TRUNCATE / DROP INDEX outside the measured window), so each run starts from the same bytes.
\set ON_ERROR_STOP on
\if :{?rows} \else \set rows 100000 \endif
set client_min_messages = warning;
set sift.col_auto_merge = off;
set sift.async_maintenance = off;
set sift.table_auto_defrag = off;
set sift.table_write_buffer_limit = 2048;
create extension if not exists pg_buffercache;

-- documents: a common term, a rare term (1 row in 97), a two-word phrase, 13 repeat lengths, 43 + 2000 + 300 mid and low frequency terms
create table docs (id int, g int, amount bigint, body text);
insert into docs
select i, i % 16, case when i % 31 = 0 then null else i % 1000 end,
       'common wire ' || case when i % 97 = 3 then 'rare ' else '' end || repeat('clean ', 1 + i % 13) || 'word' || i % 43 || ' t' || (i::bigint * 7919) % 2000 || ' u' || (i::bigint * 104729) % 300
from generate_series(1, :rows) i;
create index docs_bm25 on docs using sift (body);
create index docs_col on docs using sift_col (id, g, amount);

-- open write buffer: a promoted base plus rows that stay buffered in both indexes
create table wb (id int, g int, amount bigint, body text);
create index wb_col on wb using sift_col (id, g, amount);
create index wb_bm25 on wb using sift (body);
insert into wb select id, g, amount, body from docs where id <= :rows / 5;
select sift.col_promote('wb_col') >= 0 as promoted_col, sift.promote('wb_bm25') >= 0 as promoted_bm25;
insert into wb select id + 1000000, g, amount, body from docs where id <= 3000;

-- scratch tables of the write cases (empty here; each case truncates and fills them before its measured statement)
create table wbp (id int, g int, amount bigint, body text);
create index wbp_col on wbp using sift_col (id, g, amount);
create index wbp_bm25 on wbp using sift (body);
create table mt (id int, g int, amount bigint);
create index mt_col on mt using sift_col (id, g, amount);

-- sift_table: a read table and a mutable one (the layout of bench/perf/fixture.sql)
create table seed (id int, g int, amount bigint, body text, pad text);
alter table seed alter column pad set storage plain;
insert into seed select id, g, amount, body, repeat('p', 100) from docs;
create table store (like seed) using sift_table;
insert into store select * from seed order by id;
create index store_pk on store (id);
create table mutable (like seed) using sift_table;
create index mutable_pk on mutable (id);

-- sources of the build cases (plain heap, no index)
create table cb as
select i as id, i % 16 as g, i % 1000 as g2, i as v, (i % 977) * 0.37::float8 as x, (i % 5000)::numeric / 7 as n, 'tag' || i % 50 as t,
       jsonb_build_object('a', i % 100, 'b', (array['x','y','z'])[1 + i % 3]) as j
from generate_series(1, :rows) i;
create table cbb as select id, body from docs where id <= :rows / 3;

vacuum (analyze) docs, wb, wbp, mt, seed, store, mutable, cb, cbb;
checkpoint;
