# sift_table sealing: autovacuum turns settled rows into column segments, parallel workers read delta pages and column groups
# together, readers of old snapshots survive a seal, and a standby reads the sealed table.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 30000, 6000);
my $kinds = "SELECT string_agg(kind || ' x' || n, ', ' ORDER BY kind) FROM (SELECT kind, count(*) n FROM sift.table_segments('%s') GROUP BY kind) x";
my $sum = 'SELECT count(*) || $$:$$ || sum(id) || $$:$$ || sum(length(note)) FROM %s';

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "autovacuum_naptime = 1s\nautovacuum_vacuum_insert_threshold = 1000\nautovacuum_vacuum_insert_scale_factor = 0\n"
	. "autovacuum_vacuum_threshold = 1000\nautovacuum_vacuum_scale_factor = 0");
$primary->start;
install_sift($primary);
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

subtest 'autovacuum seals the rows of an insert-only table' => sub {
	$primary->safe_psql('postgres', "CREATE TABLE a (id int PRIMARY KEY, note text) USING sift_table; INSERT INTO a SELECT g, 'row ' || g FROM generate_series(1, $rows) g");
	my $before = $primary->safe_psql('postgres', sprintf($sum, 'a'));
	ok($primary->poll_query_until('postgres', "SELECT count(*) > 0 FROM sift.table_segments('a') WHERE kind = 'columns'"), 'a column segment appears');
	is($primary->safe_psql('postgres', sprintf($sum, 'a')), $before, 'the same rows after sealing');
	like($primary->safe_psql('postgres', sprintf($kinds, 'a')), qr/columns x\d+/, 'segments: ' . $primary->safe_psql('postgres', sprintf($kinds, 'a')));
};

subtest 'autovacuum reclaims what updates and deletes leave' => sub {
	$primary->safe_psql('postgres', 'UPDATE a SET note = $$u$$ WHERE id % 3 = 0; DELETE FROM a WHERE id % 5 = 0');
	my $before = $primary->safe_psql('postgres', sprintf($sum, 'a'));
	ok($primary->poll_query_until('postgres', "SELECT sum(dead_rows) > 0 OR count(*) FILTER (WHERE kind = 'columns') > 1 FROM sift.table_segments('a')"),
		'dead rows are cleared in the column segments');
	is($primary->safe_psql('postgres', sprintf($sum, 'a')), $before, 'the same rows after the vacuum');
};

subtest 'parallel workers read the delta pages and the column groups' => sub {
	my $set = 'SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 3;';
	$primary->safe_psql('postgres', "INSERT INTO a SELECT g, 'late ' || g FROM generate_series($rows + 1, $rows + 3000) g");
	my $plan = $primary->safe_psql('postgres', "$set EXPLAIN (COSTS OFF) SELECT count(*) FROM a");
	like($plan, qr/Gather/, 'a parallel plan');
	my $serial = $primary->safe_psql('postgres', 'SET max_parallel_workers_per_gather = 0; ' . sprintf($sum, 'a'));
	is($primary->safe_psql('postgres', $set . ' ' . sprintf($sum, 'a')), $serial, 'the parallel scan returns every row once');
	is($primary->safe_psql('postgres', "$set SELECT count(*) FROM a WHERE note = \$\$u\$\$"),
		$primary->safe_psql('postgres', 'SET max_parallel_workers_per_gather = 0; SELECT count(*) FROM a WHERE note = $$u$$'), 'with a filter');
};

subtest 'a reader that began before a seal keeps its rows' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE r (id int, note text) USING sift_table; INSERT INTO r SELECT g, $$x$$ FROM generate_series(1, 4000) g');
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($bg->query_safe('SELECT count(*) FROM r'), 4000, 'the snapshot sees the rows');
	$primary->safe_psql('postgres', 'DELETE FROM r WHERE id <= 1000; VACUUM r');
	is($bg->query_safe('SELECT count(*) FROM r'), 4000, 'still, after the delete and VACUUM');
	$bg->query_safe('COMMIT');
	$bg->quit;
	$primary->safe_psql('postgres', 'VACUUM r');
	is($primary->safe_psql('postgres', 'SELECT count(*) FROM r'), 3000, 'a new snapshot sees the delete');
};

subtest 'a standby reads the sealed tables' => sub {
	$primary->safe_psql('postgres', 'VACUUM a');
	$primary->wait_for_catchup($standby);
	is($standby->safe_psql('postgres', sprintf($sum, 'a')), $primary->safe_psql('postgres', sprintf($sum, 'a')), 'same rows');
	is($standby->safe_psql('postgres', sprintf($kinds, 'a')), $primary->safe_psql('postgres', sprintf($kinds, 'a')), 'same segments');
	is($standby->safe_psql('postgres', 'SET enable_seqscan = off; SELECT note FROM a WHERE id = 13'), 'row 13', 'a point read by the primary key');
};

$standby->stop;
$primary->stop;
done_testing();
