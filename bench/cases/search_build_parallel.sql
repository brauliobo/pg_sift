-- the parallel build of a BM25 index: the participants write the segment in ranges of terms
-- label: `CREATE INDEX .. USING sift (body)` on the documents of the dataset with 0, 2 and 4 parallel maintenance workers (lanes workers_N; the lane sift is the run's CASES_WORKERS), PostgreSQL builds `USING gin (tsv)` on the same rows. The participants write the segment together (term ranges); the check builds the index again with no worker and compares every page of the two (one segment: the same bytes)
--@ prepare
CREATE EXTENSION IF NOT EXISTS pageinspect SCHEMA public;
CREATE TABLE docs AS SELECT * FROM c_docs.docs;
CREATE FUNCTION idx_digest(r regclass) RETURNS text LANGUAGE sql AS $$ SELECT md5(string_agg(substring(get_raw_page(r::text, n::int) FROM 11), ''::bytea ORDER BY n)) FROM generate_series(0, pg_relation_size(r) / 8192 - 1) n $$;
CREATE FUNCTION same_bytes() RETURNS text LANGUAGE plpgsql AS $$ DECLARE segs int; same bool; BEGIN SELECT count(*) INTO segs FROM sift.segments('docs_sift') WHERE kind = 'immutable'; PERFORM set_config('max_parallel_maintenance_workers', '0', true); CREATE INDEX docs_ref ON docs USING sift (body); same := idx_digest('docs_ref') = idx_digest('docs_sift'); DROP INDEX docs_ref; RETURN CASE WHEN segs > 1 THEN 'layout of ' || segs || ' segments' WHEN same THEN 'same bytes' ELSE 'DIFFERENT BYTES' END; END $$;
VACUUM (FREEZE, ANALYZE) docs
--@ load
CREATE INDEX docs_gin ON docs USING gin (tsv)
--@ load_sift
CREATE INDEX docs_sift ON docs USING sift (body)
--@ check
SELECT same_bytes()
