# kill -9 of the postmaster while a backend is busy: the server restarts at once. The backends that outlive their postmaster
# keep the shared memory until they next wait, so the restart has to wait for them (tap-004 kills its primary under load).
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $node = sift_node('postmaster_kill');
$node->start;

my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(qr/started/, "\\echo started\nSELECT count(*) FROM generate_series(1, 100000000);\n");
$node->poll_query_until('postgres', "SELECT count(*) = 1 FROM pg_stat_activity WHERE state = 'active' AND query LIKE '%generate_series%' AND pid <> pg_backend_pid()");

kill9_wait($node);
$node->start;
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'the server restarts right after its postmaster was killed');
eval { $bg->quit };
$node->stop;

done_testing();
