# VACUUM does not mark a block all visible while a snapshot older than a narrow UPDATE is open: the xid bound of the column delta chain that
# plan_partition (table/f2/vm/vm.c) settles on is the newest writer of the chain as its pages say, not the anchor's max_xid, which an UPDATE
# that fits the newest page leaves behind (core/f2/col_delta_anchor.h)
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_sift'\nsift.test_create_format1 = off\nautovacuum = off");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift');

$primary->safe_psql('postgres', q{
	CREATE TABLE cd (id int PRIMARY KEY, k int, v int, pad text, w int) USING sift_table;
	INSERT INTO cd SELECT g, g, g, repeat('x', 40), g FROM generate_series(1, 2000) g;
	CREATE INDEX cd_k ON cd (k);
	VACUUM cd;
	UPDATE cd SET v = -1 WHERE id = 1;
	VACUUM cd;});
my $reader = $primary->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM cd;');
$primary->safe_psql('postgres', 'UPDATE cd SET v = 777 WHERE id = 1; VACUUM cd');
my $plan = $primary->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k FROM cd WHERE k BETWEEN 1 AND 100});
$reader->quit;
like($plan, qr/Index Only Scan/, 'the planner reads the index alone');
my ($fetches) = $plan =~ /Heap Fetches: (\d+)/;
cmp_ok($fetches, '>', 0, 'the block of the update is not all visible while a snapshot older than it is open');
done_testing();
