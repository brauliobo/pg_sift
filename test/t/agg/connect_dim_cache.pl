# The dimension of a star join that a backend kept (agg/dim_cache.h) is served to a statement only while the table of it is what the journal says: a writer of another session that has not
# committed, one that rolled back, one that committed, a snapshot that was taken before the commit and the backend's own open transaction, each against the answer of the heap.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = sift_node('primary');
$node->append_conf('postgresql.conf', "sift.test_create_format1 = off\nshared_buffers = 64MB");
$node->start;
install_sift_test($node, 'the star join reads the dimension of the cache');

$node->safe_psql('postgres', q{
	CREATE TABLE dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
	INSERT INTO dim SELECT i, i % 7, i % 3 FROM generate_series(0, 699) i;
	CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
	INSERT INTO ev SELECT i, i % 750, i % 11 FROM generate_series(1, 3000) i;
	CREATE INDEX dim_col ON dim USING sift_col (k, a, b);
	CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)');
	VACUUM (FREEZE, ANALYZE) dim, ev;
	CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE r text; BEGIN
		PERFORM set_config('sift.enable_agg', scan::text, true);
		EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
		RETURN r;
	END $$;
	CREATE FUNCTION cache(sql text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE l text; BEGIN
		PERFORM set_config('sift.enable_agg', 'true', true);
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
			IF l LIKE '%Dimension Cache:%' THEN RETURN trim(split_part(l, ':', 2)); END IF;
		END LOOP;
		RETURN 'none';
	END $$ });

my $join = q{SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1};
my $setup = 'SET max_parallel_workers_per_gather = 0; SET jit = off; SET enable_hashagg = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_nestloop = off';
my $reader = $node->background_psql('postgres');
my $writer = $node->background_psql('postgres');
$reader->query_safe($setup);

sub cache { return $reader->query_safe("SELECT cache(\$q\$$join\$q\$)") }
sub scan { return $reader->query_safe("SELECT answer(\$q\$$join\$q\$, true)") }
sub heap { return $reader->query_safe("SELECT answer(\$q\$$join\$q\$, false)") }

sub agrees
{
	my ($what) = @_;
	is(scan(), heap(), "$what: the answer is the heap's");
}

is(cache(), '0 hit, 1 built', 'the first statement reads the dimension');
is(cache(), '1 hit, 0 built', 'the next one is served from the kept table');
my $before = heap();

$writer->query_safe('BEGIN');
$writer->query_safe('UPDATE dim SET a = a + 1 WHERE k % 3 = 0');
is(cache(), '1 hit, 0 built', 'a writer of another session that has not committed: its rows are not seen, the kept table is the table');
agrees('writer in doubt');
is(scan(), $before, 'the writer in doubt is not seen');
$writer->query_safe('ROLLBACK');
is(cache(), '1 hit, 0 built', 'the writer rolled back: the kept table is the table');
agrees('writer rolled back');

$writer->query_safe('UPDATE dim SET a = a + 1 WHERE k % 3 = 0');
is(cache(), '0 hit, 1 built', 'a committed write: the table is read');
isnt(scan(), $before, 'the committed write is seen');
agrees('writer committed');
is(cache(), '1 hit, 0 built', 'the table read after the write is kept');

$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT count(*) FROM dim');
my $held = heap();
$writer->query_safe('UPDATE dim SET b = b + 1 WHERE k % 5 = 0');
is(cache(), '0 hit, 1 built', 'a snapshot taken before a commit does not read the table kept before it');
is(scan(), $held, 'the snapshot sees the table as it was');
agrees('repeatable read');
$reader->query_safe('COMMIT');
is(cache(), '0 hit, 1 built', 'the next statement reads the table the commit left');
agrees('after the snapshot');

$reader->query_safe('BEGIN');
$reader->query_safe('UPDATE dim SET a = a + 1 WHERE k % 11 = 0');
is(cache(), '0 hit, 1 built', 'the transaction reads the table it changed');
agrees('own transaction');
$reader->query_safe('ROLLBACK');
is(cache(), '1 hit, 0 built', 'its rollback leaves the kept table as it was');
agrees('own rollback');

$reader->quit;
$writer->quit;
$node->stop;
done_testing();
