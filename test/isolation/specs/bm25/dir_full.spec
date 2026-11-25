# Writes go on while the directory of the index is full and a reader holds its snapshot: merges fail (injected), the limit of 64 entries is
# reached (sift.test_limits), inserts and the promotion keep working, the error of a promotion is the failed step's and not a segment count,
# and the reader's count does not change. Then the merges run and the index verifies.
# requires: build insert scan promote merge verify gates

setup
{
	CREATE TABLE iso_dir (id serial, body text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_dir (body) SELECT 'seed doc ' || g FROM generate_series(1, 200) g;
	CREATE INDEX iso_dir_idx ON iso_dir USING sift (body) WITH (write_buffer_limit = 64);
	CREATE FUNCTION iso_big(n int) RETURNS text LANGUAGE sql AS
		$$ SELECT string_agg('t' || n || 'x' || g, ' ') FROM generate_series(1, 12000) g $$;
}

teardown
{
	DROP TABLE iso_dir;
	DROP FUNCTION iso_big(int);
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1q	{ SET enable_seqscan = off; SELECT count(*) AS seed_docs FROM iso_dir WHERE body @@> 'seed'; }
step s1c	{ COMMIT; }

session s2
step s2s	{ LOAD 'pg_sift'; SET sift.async_maintenance = off; SET client_min_messages = error; SET sift.test_limits = 'max_segments=64'; }
step s2f	{ SET sift.test_fail_pages = 1; }
step s2w	{ DO $$ BEGIN FOR i IN 1..90 LOOP INSERT INTO iso_dir (body) SELECT 'room ' || iso_big(i); END LOOP; END $$; }
step s2n	{ SELECT count(*) = 64 AS at_limit, count(*) FILTER (WHERE kind = 'buffer') AS open_buffers FROM sift.segments('iso_dir_idx'); }
step s2i	{ INSERT INTO iso_dir (body) SELECT 'room more ' || iso_big(1000 + g) FROM generate_series(1, 5) g; }
step s2p	{ SELECT sift.promote('iso_dir_idx') >= 0 AS promoted; }
step s2r	{ RESET sift.test_fail_pages; SELECT sift.promote('iso_dir_idx') >= 0 AS promoted; }
step s2m	{ SELECT sift.merge('iso_dir_idx') >= 0 AS merged; SELECT count(*) < 40 AS merged_down FROM sift.segments('iso_dir_idx'); }

session s3
step s3q	{ SELECT iso_agree('iso_dir', 'seed') AS agree_seed, iso_agree('iso_dir', 'room') AS agree_room, iso_verify('iso_dir_idx') AS verified; }

permutation s1b s1q s2s s2f s2w s2n s2i s2p s1q s2r s2m s1q s1c s3q
