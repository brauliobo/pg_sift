# A visibility map record over frozen rows replays on a hot standby: the rows stand for no transaction, so the record names no snapshot conflict horizon
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
install_sift($primary);
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

$primary->safe_psql('postgres', q{
	CREATE TABLE fz (id int PRIMARY KEY, k int, pad text) USING sift_table;
	INSERT INTO fz SELECT g, g, repeat('x', 40) FROM generate_series(1, 70000) g;
	CREATE INDEX fz_k ON fz (k);
	VACUUM (FREEZE) fz;});
cmp_ok($primary->safe_psql('postgres', q{SELECT pg_relation_size('fz', 'vm')}), '>', 0, 'VACUUM set the visibility map of the frozen rows');
$primary->wait_for_catchup($standby);

is($standby->safe_psql('postgres', 'SELECT count(*) FROM fz'), 70000, 'the standby replayed the visibility map of frozen rows');
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 't', 'and is still in recovery');
done_testing();
