# Streaming replication: a hot standby replays index builds, inserts,
# maintenance and VACUUM, answers queries like the primary, and a scan open on
# the standby across merge and recycling either completes correctly or fails
# with a retryable serialization error (DESIGN.md (d)).
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->start;
install_sift($primary);
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "hot_standby_feedback = off\nmax_standby_streaming_delay = 2s");
$standby->start;

for my $am (ams())
{
	my $n = $am->{name};
	subtest "$n: replay on a hot standby" => sub {
		my ($t, $idx) = ("repl_$n", "repl_${n}_idx");
		load_corpus($primary, $t, 20000);
		$primary->safe_psql('postgres', $am->{index}->($t, $idx));
		$primary->safe_psql('postgres', qq{
			INSERT INTO $t SELECT g, 'w1 rare replica ' || g FROM generate_series(30001, 40000) g;
			UPDATE $t SET body = 'w12 updated ' || id WHERE id % 17 = 0;
			DELETE FROM $t WHERE id % 11 = 0;
		});
		$primary->safe_psql('postgres', $am->{maint}->($idx));
		$primary->safe_psql('postgres', "SELECT sift.merge('$idx')") if $n eq 'sift';
		$primary->safe_psql('postgres', "VACUUM $t");
		$primary->wait_for_catchup($standby);

		agree($standby, $am, $t, 'standby');
		$standby->safe_psql('postgres', $am->{verify}->($idx));
		my $q = "SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM $t WHERE "
		  . $am->{match}->('w100');
		is($standby->safe_psql('postgres', $q), $primary->safe_psql('postgres', $q),
			"$n: standby returns the primary's rows");
	};
}

subtest 'sift: standby scan across merge and segment recycling' => sub {
	my ($t, $idx) = ('recycle', 'recycle_idx');
	load_corpus($primary, $t, 20000);
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (mutable_limit = 64)");
	$primary->wait_for_catchup($standby);

	my $bg = $standby->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('\set VERBOSITY verbose');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $expected = $bg->query_safe("SELECT count(*) FROM $t WHERE sift_ref.match(body, 'w1')");
	$bg->query_safe("DECLARE c CURSOR FOR SELECT id FROM $t WHERE body @@> 'w1'");
	$bg->query_safe('FETCH 10 FROM c');

	my $log_offset = -s $standby->logfile;
	for my $round (1 .. 3)
	{
		$primary->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 churn $round ' || g"
			. " FROM generate_series(100000 * $round, 100000 * $round + 4000) g; SELECT sift.promote('$idx')");
		$primary->safe_psql('postgres', "DELETE FROM $t WHERE id % 5 = $round");
		$primary->safe_psql('postgres', "VACUUM $t; SELECT sift.merge('$idx'); VACUUM $t");
	}
	$primary->wait_for_catchup($standby);

	# a recovery conflict ends the session (SQLSTATE 40001); otherwise the scan sees its snapshot
	my ($out, $err);
	if (eval { ($out, $err) = $bg->query("FETCH FORWARD ALL IN c"); 1 } && $err !~ /ERROR|FATAL/)
	{
		is(scalar(() = $out =~ /^\d+$/mg), $expected - 10, 'scan returned the rows of its snapshot');
	}
	else
	{
		ok($standby->log_contains(qr/terminating connection due to conflict with recovery/, $log_offset),
			'scan was cancelled by a recovery conflict');
	}
	eval { $bg->quit };
	$standby->safe_psql('postgres', "SELECT sift.verify('$idx', true)");
	agree($standby, sift_am(), $t, 'standby after recycling');
};

# with hot_standby_feedback the primary's horizon holds back VACUUM and recycling for the open scan: it always completes.
# VACUUM's truncation of the end of the file is not held back by feedback, like a heap's: its exclusive lock cancels the scan
# when the standby replays it (test/t/006_truncate.pl), so the index here keeps its file
subtest 'sift: hot_standby_feedback keeps a standby scan alive across merge and recycling' => sub {
	my ($t, $idx) = ('feedback', 'feedback_idx');
	load_corpus($primary, $t, 20000);
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (mutable_limit = 64, vacuum_truncate = false)");
	$primary->backup('feedback_base');
	my $fb = PostgreSQL::Test::Cluster->new('standby_feedback');
	$fb->init_from_backup($primary, 'feedback_base', has_streaming => 1);
	$fb->append_conf('postgresql.conf', "hot_standby_feedback = on\nmax_standby_streaming_delay = 2s");
	$fb->start;
	$primary->wait_for_catchup($fb);

	my $bg = $fb->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $expected = $bg->query_safe("SELECT count(*) FROM $t WHERE sift_ref.match(body, 'w1')");
	$bg->query_safe("DECLARE c CURSOR FOR SELECT id FROM $t WHERE body @@> 'w1'");
	$bg->query_safe('FETCH 10 FROM c');
	$primary->poll_query_until('postgres', "SELECT backend_xmin IS NOT NULL FROM pg_stat_replication WHERE application_name = 'standby_feedback'", 't');

	for my $round (1 .. 3)
	{
		$primary->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 churn $round ' || g"
			. " FROM generate_series(100000 * $round, 100000 * $round + 4000) g; SELECT sift.promote('$idx')");
		$primary->safe_psql('postgres', "DELETE FROM $t WHERE id % 5 = $round");
		$primary->safe_psql('postgres', "VACUUM $t; SELECT sift.merge('$idx'); VACUUM $t");
	}
	$primary->wait_for_catchup($fb);

	is(scalar(() = $bg->query_safe("FETCH FORWARD ALL IN c") =~ /^\d+$/mg), $expected - 10,
		'the open scan returned the rows of its snapshot');
	unlike(slurp_file($fb->logfile), qr/conflict with recovery/, 'no recovery conflict was raised');
	$bg->quit;
	$fb->safe_psql('postgres', "SELECT sift.verify('$idx', true)");
	$fb->stop;
};

$standby->stop;
$primary->stop;
done_testing();
