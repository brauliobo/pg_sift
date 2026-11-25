# kill -9 of the postmaster while a backend is busy: the server restarts at once. The backends that outlive their postmaster
# keep the shared memory until they next wait, so the restart has to wait for them (tap-004 kills its primary under load). The select-list
# generate_series streams its rows: in FROM, PostgreSQL would spool all 100 million of them to a temporary file (1.4 GB of scratch).
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $node = sift_node('postmaster_kill');
$node->start;

my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(qr/started/, "\\echo started\nSELECT count(*) FROM (SELECT generate_series(1, 100000000)) s;\n");
$node->poll_query_until('postgres', "SELECT count(*) = 1 FROM pg_stat_activity WHERE state = 'active' AND query LIKE '%generate_series%' AND pid <> pg_backend_pid()");

kill9_wait($node);
$node->start;
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'the server restarts right after its postmaster was killed');
eval { $bg->quit };
$node->stop;

# kill9_wait polls the children of a killed postmaster, which init reaps while it reads their /proc entries: reading the entry of a process
# that is gone fails with ESRCH, which must be "not running" and not a fatal warning (tap-004 under ASan, whose processes exit slowly)
for my $round (1 .. 150)
{
	pipe(my $r, my $w);
	my $parent = fork();
	if (!$parent)
	{
		my @kids = map { fork() || do { my $x = 0; $x += $_ for 1 .. 30000; exit 0 } } 1 .. 20;
		print $w "@kids\n";
		exit 0;
	}
	my @kids = split ' ', scalar <$r>;
	waitpid($parent, 0);
	while (@kids)
	{
		@kids = grep { SiftTest::running($_) } @kids;
	}
}
pass('150 rounds of children reaped by init while their /proc entries are read');

done_testing();
