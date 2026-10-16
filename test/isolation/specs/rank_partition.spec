# A ranked cursor over a partitioned table (MergeAppend of one ranked scan per partition) while another session
# inserts into, deletes from, promotes, merges and vacuums its partitions.
# requires: build insert vacuum scan promote merge verify score

setup
{
	CREATE TABLE iso_part (id int, body text) PARTITION BY RANGE (id);
	CREATE TABLE iso_part1 PARTITION OF iso_part FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_part2 PARTITION OF iso_part FOR VALUES FROM (1000) TO (2000) WITH (autovacuum_enabled = off);
	CREATE TABLE iso_part3 PARTITION OF iso_part FOR VALUES FROM (2000) TO (3000) WITH (autovacuum_enabled = off);
	INSERT INTO iso_part SELECT g, 'part base ' || repeat('more ', g % 5) || g FROM generate_series(1, 2400) g;
	CREATE INDEX iso_part1_idx ON iso_part1 USING sift (body) WITH (mutable_limit = 64);
	CREATE INDEX iso_part2_idx ON iso_part2 USING sift (body) WITH (mutable_limit = 64);
	CREATE INDEX iso_part3_idx ON iso_part3 USING sift (body) WITH (mutable_limit = 64);
	CREATE INDEX iso_part_idx ON iso_part USING sift (body) WITH (mutable_limit = 64);
	INSERT INTO iso_part SELECT g, 'part base second ' || g FROM generate_series(2401, 2800) g;
	SELECT sift.promote('iso_part3_idx') >= 0 AS promoted;
}

teardown
{
	DROP TABLE iso_part;
}

session s1
setup		{ CREATE TEMP TABLE IF NOT EXISTS fetched (n serial, id int, score real); TRUNCATE fetched RESTART IDENTITY; }
step s1r	{ BEGIN ISOLATION LEVEL REPEATABLE READ; DECLARE c CURSOR FOR SELECT id, sift.score(ctid) AS score FROM iso_part WHERE body @@> 'part OR second OR more' ORDER BY sift.score(ctid) DESC; }
step s1f	{ SELECT iso_fetch('c', 50) AS fetched; }
step s1rest	{ SELECT iso_fetch('c', 100000) > 0 AS more; }
step s1check	{ SELECT iso_cursor_ok('iso_part', 'part OR second OR more') AS ok; }
step s1c	{ COMMIT; }

session s2
step s2i	{ INSERT INTO iso_part SELECT g, 'part base second more more ' || g FROM generate_series(1001, 1100) g; }
step s2d	{ DELETE FROM iso_part WHERE id % 4 = 0; }
step s2v	{ VACUUM iso_part; }
step s2m	{ SELECT sift.promote('iso_part1_idx') >= 0 AND sift.merge('iso_part3_idx') >= 0 AS merged; }
step s2check	{ SELECT iso_agree('iso_part', 'part OR second OR more') AS agree, iso_verify('iso_part1_idx') AS verified1,
	iso_verify('iso_part2_idx') AS verified2, iso_verify('iso_part3_idx') AS verified3; }

permutation s1r s1f s2i s2m s1rest s1check s1c s2check
# deleted and inserted rows stay invisible to the older snapshot across merge and VACUUM
permutation s1r s1f s2d s2v s2i s2m s2v s1rest s1check s1c s2v s2check
