# The rows of a joined table kept in shared memory (sift.agg_lookup_shared_kb): a backend killed while it reads them takes the server through crash recovery,
# which discards the directory and the segments, and the next queries load the table again and get the rows of the normal plan.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $node = sift_node('lookup_shared');
$node->append_conf('postgresql.conf', "sift.agg_lookup_shared_kb = 8192\nsift.agg_lookup_cache_kb = 0\nmax_parallel_workers_per_gather = 0\nseq_page_cost = 10000\nrandom_page_cost = 10000");
$node->start;
install_sift($node);
$node->safe_psql('postgres', <<'SQL');
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 3000, i % 100 FROM generate_series(1, 30000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, amount);
CREATE TABLE ents (id int, label text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, 'e' || i % 50 FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) ev;
VACUUM (FREEZE, ANALYZE) ents;
CREATE FUNCTION cached(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; kept int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	RETURN kept;
END $$;
SQL

my $q = 'SELECT e.label, count(*), sum(amount) FROM ev JOIN ents e ON e.id = ev.dim_a_id GROUP BY 1';
my $cached = sub { $node->safe_psql('postgres', "SELECT cached(\$\$$q\$\$)") };
my $differences = sub {
	$node->safe_psql('postgres', "SET sift.enable_agg = off; CREATE TEMP TABLE r_off AS $q; RESET sift.enable_agg; CREATE TEMP TABLE r_on AS $q;"
		. " SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d");
};

is($cached->(), '0', 'the first query loads the table');
is($cached->(), '1', 'and the next one, of another backend, reads it');

# a backend that is killed while a cursor holds the segment mapped
crash_during($node, "BEGIN; DECLARE c CURSOR FOR $q; FETCH 1 FROM c; SELECT pg_sleep(60)", 300);
is($cached->(), '0', 'crash recovery discarded the segments: the table is loaded again');
is($cached->(), '1', 'and kept again');
is($differences->(), '0', 'the rows are those of the normal plan');

# a write: the rows of the old state are not read
$node->safe_psql('postgres', 'UPDATE ents SET label = $$moved$$ WHERE id = 7');
is($cached->(), '0', 'a table written to is loaded again');
$node->safe_psql('postgres', 'VACUUM ents');
is($cached->(), '0', 'and once vacuumed, the pages are not those of the rows kept before');
is($cached->(), '1', 'and is then kept');
is($differences->(), '0', 'the rows are those of the normal plan after the write');

$node->stop;
done_testing();
