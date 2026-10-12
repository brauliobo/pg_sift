# WAL-logged index changes are crash safe and replicate exactly:
#  - wal_consistency_checking = 'generic' on the primary makes every standby (and crash recovery) compare the
#    page it rebuilt from a Generic WAL record against the page the primary wrote, under a mixed workload of
#    inserts, updates, deletes, promotion, merge, VACUUM and REINDEX; a mismatch is a FATAL "inconsistent page"
#  - kill -9 of the primary under load, recovery, and the standby following it
#  - a pg_basebackup taken while the workload runs restores (alone, and as a standby that is then promoted)
#    to an index that sift.verify accepts and that agrees with the SQL reference
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run;
use PostgreSQL::Test::Cluster;
use Test::More;

my $secs = $ENV{SIFT_TAP_SECONDS} // 12;
my ($t, $idx) = ('wc', 'wc_idx');
my $sift = (ams())[1];

my $primary = sift_node('wc_primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "wal_consistency_checking = 'generic'\nshared_buffers = 64MB");
$primary->start;
features($primary);
load_corpus($primary, $t, 20000);
$primary->safe_psql('postgres', "CREATE SEQUENCE wc_ids START 100001; CREATE INDEX $idx ON $t USING sift (body) WITH (mutable_limit = 64)");

my $dir = $primary->basedir;
my %scripts = (
	update => [ 40, "\\set id random(1, 20000)\n\\set w random_zipfian(1, 100, 1.1)\n"
		  . "UPDATE $t SET body = 'w' || :w || ' rare upd ' || :id WHERE id = :id;\n" ],
	insert => [ 40, "\\set w random(1, 100)\nINSERT INTO $t VALUES (nextval('wc_ids'), 'w' || :w || ' w1 ins');\n" ],
	delete => [ 10, "\\set id random(1, 20000)\nDELETE FROM $t WHERE id = :id;\n" ],
	promote => [ 2, "SELECT sift.promote('$idx');\n" ],
	merge => [ 1, "SELECT sift.merge('$idx');\n" ],
	vacuum => [ 1, "VACUUM $t;\n" ],
	reindex => [ 1, "REINDEX INDEX $idx;\n" ]);

# Starts pgbench against $node for $secs seconds; mixes the scripts of %scripts. Returns the IPC::Run handle.
sub workload
{
	my ($node, $secs) = @_;
	my @cmd = ('pgbench', '-n', '-h', $node->host, '-p', $node->port, '-c', 4, '-j', 2, '-T', $secs);
	for my $name (sort keys %scripts)
	{
		my $file = "$dir/$name.sql";
		open(my $fh, '>', $file) or die $!;
		print $fh $scripts{$name}[1];
		close($fh);
		push @cmd, '-f', "$file\@$scripts{$name}[0]";
	}
	my ($out, $err) = ('', '');
	my $h = IPC::Run::start([ @cmd, 'postgres' ], '>', \$out, '2>', \$err);
	return { handle => $h, out => \$out, err => \$err };
}

sub healthy
{
	my ($node, $what) = @_;
	$node->safe_psql('postgres', $sift->{verify}->($idx));
	agree($node, $sift, $t, $what);
}

sub rows
{
	my ($node) = @_;
	return $node->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) || ':' || sum(id) FROM $t WHERE body @@> 'w1'");
}

my $no_inconsistency = qr/inconsistent page found|FATAL:\s+inconsistent/;

# phase 1: primary and hot standby under load with page consistency checking
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('wc_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

my $w = workload($primary, $secs);
$w->{handle}->finish;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
cmp_ok($primary->safe_psql('postgres', "SELECT coalesce(sum(count), 0) FROM pg_get_wal_stats(pg_current_wal_lsn() - 33554432, pg_current_wal_lsn())"
	. q{ WHERE "resource_manager/record_type" LIKE 'Generic%'}), '>', 100, 'the workload wrote Generic WAL records');
note("pgbench: " . (${ $w->{out} } =~ s/\n/ /gr) . " " . ${ $w->{err} });
$primary->wait_for_catchup($standby);
ok(!$standby->log_contains($no_inconsistency), 'the standby found no inconsistent page replaying the workload');
healthy($primary, 'primary after the workload');
healthy($standby, 'standby after the workload');
is(rows($standby), rows($primary), 'standby returns the primary rows');

# phase 2: kill -9 of the primary while it works; recovery replays and re-checks the pages
my $log_offset = -s $primary->logfile;
$w = workload($primary, $secs);
sleep 3;
$primary->kill9;
eval { $w->{handle}->finish };
$primary->start;
ok(!$primary->log_contains($no_inconsistency, $log_offset), 'crash recovery found no inconsistent page');
healthy($primary, 'primary after kill -9');
$primary->wait_for_catchup($standby);
ok(!$standby->log_contains($no_inconsistency), 'the standby found no inconsistent page after the primary crashed');
healthy($standby, 'standby after the primary crashed');
is(rows($standby), rows($primary), 'standby follows the recovered primary');
$standby->stop;

# phase 3: a base backup taken while the workload runs; a slot keeps the WAL the copy needs after the backup
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('copy_slot', true)");
$w = workload($primary, $secs);
sleep 2;
$primary->backup('during');
$w->{handle}->finish;

my $alone = PostgreSQL::Test::Cluster->new('wc_restored');
$alone->init_from_backup($primary, 'during');
$alone->start;
healthy($alone, 'backup restored on its own');
$alone->stop;

my $copy = PostgreSQL::Test::Cluster->new('wc_copy');
$copy->init_from_backup($primary, 'during', has_streaming => 1);
$copy->append_conf('postgresql.conf', "primary_slot_name = 'copy_slot'");
$copy->start;
$primary->wait_for_catchup($copy);
healthy($copy, 'standby built from the backup');
is(rows($copy), rows($primary), 'standby built from the backup returns the primary rows');
$copy->promote;
$copy->safe_psql('postgres', "INSERT INTO $t VALUES (-1, 'w1 promoted'); SELECT sift.promote('$idx'); VACUUM $t");
healthy($copy, 'promoted standby after writes');
ok(!$copy->log_contains($no_inconsistency), 'no inconsistent page on the backup copy');
$copy->stop;

$primary->stop;
done_testing();
