# Streaming replication: a hot standby replays index builds, inserts,
# maintenance and VACUUM, answers queries like the primary, and a scan open on
# the standby across merge and recycling either completes correctly or fails
# with a retryable serialization error.
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
# Each standby has a physical slot that reserves the WAL when it is created, before its backup: the fast checkpoint of the backup of the next
# standby recycles the segments no slot holds, and a walsender that then finds the segment its standby asks for gone never serves it again
# ("requested WAL segment ... has already been removed", the walreceiver retries until the test times out)
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('standby', true)");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "hot_standby_feedback = off\nmax_standby_streaming_delay = 2s\nprimary_slot_name = 'standby'");
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
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (write_buffer_limit = 64)");
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
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (write_buffer_limit = 64, vacuum_truncate = false)");
	$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('standby_feedback', true)");
	$primary->backup('feedback_base');
	my $fb = PostgreSQL::Test::Cluster->new('standby_feedback');
	$fb->init_from_backup($primary, 'feedback_base', has_streaming => 1);
	$fb->append_conf('postgresql.conf', "hot_standby_feedback = on\nmax_standby_streaming_delay = 2s\nprimary_slot_name = 'standby_feedback'");
	$fb->start;
	$primary->wait_for_catchup($fb);

	my $bg = $fb->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $expected = $bg->query_safe("SELECT count(*) FROM $t WHERE sift_ref.match(body, 'w1')");
	$bg->query_safe("DECLARE c CURSOR FOR SELECT id FROM $t WHERE body @@> 'w1'");
	$bg->query_safe('FETCH 10 FROM c');
	# a standby that streams through a slot has its feedback xmin on the slot, not on its walsender
	$primary->poll_query_until('postgres', "SELECT xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'standby_feedback'", 't');

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
	$primary->safe_psql('postgres', "SELECT pg_drop_replication_slot('standby_feedback')");
};

# A backend keeps the blobs of the terms it reads (bm25/segcache.c, under the relation file and the generation of the segment): one
# backend on the standby, alive from one check to the next, answers as the primary does after the replay of a promotion, a merge,
# a VACUUM that rewrites the segment and a REINDEX, which each leave blobs of the same offsets and sizes under another generation or file
subtest 'sift: a standby backend that keeps term blobs follows promotion, merge, VACUUM and REINDEX' => sub {
	my ($t, $idx) = ('docs_a', 'docs_a_idx');
	$primary->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		INSERT INTO $t SELECT g, 'dim_a' || g % 6 || ' dim_a' || (g + 1) % 6 || ' dim_a' || (g + 2) % 6 FROM generate_series(1, 30000) g;
		CREATE INDEX $idx ON $t USING sift (body);
	});
	$primary->wait_for_catchup($standby);
	my $bg = $standby->background_psql('postgres');
	my $same = sub {
		my ($what) = @_;
		for my $q ('dim_a0', 'dim_a1 OR dim_a2')
		{
			for my $order ('id', 'id DESC')
			{
				my $sql = "SELECT string_agg(id::text, ',' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id FROM"
				  . " (SELECT id FROM $t WHERE body @@> '$q' ORDER BY sift.score(ctid) DESC, $order LIMIT 25) x) y";
				is($bg->query_safe($sql), $primary->safe_psql('postgres', $sql), "$what: $q by $order");
			}
		}
	};
	$same->('first reads');
	$same->('blobs kept');
	$primary->safe_psql('postgres', "INSERT INTO $t SELECT 1000000 + g, 'dim_a' || g % 6 || ' dim_a' || (g + 1) % 6 || ' dim_a' || (g + 2) % 6"
		. " FROM generate_series(1, 30000) g; SELECT sift.promote('$idx')");
	$primary->wait_for_catchup($standby);
	$same->('promoted segment');
	$primary->safe_psql('postgres', "SELECT sift.merge('$idx', true)");
	$primary->wait_for_catchup($standby);
	$same->('merged');
	$primary->safe_psql('postgres', "DELETE FROM $t WHERE id % 3 = 0; VACUUM $t");
	$primary->wait_for_catchup($standby);
	$same->('vacuumed');
	$primary->safe_psql('postgres', "REINDEX INDEX $idx");
	$primary->wait_for_catchup($standby);
	$same->('reindexed');
	$bg->quit;
};

subtest 'sift: INCLUDE columns on a hot standby' => sub {
	my ($t, $idx) = ('inc_repl', 'inc_repl_idx');
	$primary->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text, category_id int, created_at date) WITH (autovacuum_enabled = off);
		INSERT INTO $t SELECT g, 'w' || g % 7 || ' w' || g % 13 || ' common', CASE WHEN g % 10 = 0 THEN NULL ELSE g % 50 END,
			DATE '2026-01-01' + g % 200 FROM generate_series(1, 20000) g;
		CREATE INDEX $idx ON $t USING sift (body) INCLUDE (category_id, created_at);
		INSERT INTO $t SELECT g, 'w1 later ' || g, g % 50, DATE '2026-03-01' + g % 100 FROM generate_series(20001, 26000) g;
		SELECT sift.promote('$idx');
		UPDATE $t SET category_id = category_id + 1, created_at = created_at + 20 WHERE id % 13 = 0;
		DELETE FROM $t WHERE id % 11 = 0;
		VACUUM $t;
		SELECT sift.merge('$idx');
	});
	$primary->wait_for_catchup($standby);

	my $where = "category_id < 20 AND created_at >= '2026-04-01'";
	my $sift = "SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM $t WHERE body \@\@> 'w1' AND $where"
	  . " ORDER BY sift.score(ctid) DESC LIMIT 100000) x";
	my $heap = "SELECT string_agg(id::text, ',' ORDER BY id) FROM $t WHERE sift_ref.match(body, 'w1') AND $where";
	like($standby->safe_psql('postgres', "SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM $t WHERE body \@\@> 'w1' AND $where"
		. " ORDER BY sift.score(ctid) DESC LIMIT 100000"), qr/Include Tests/, 'the standby plans the tests of the INCLUDE columns');
	is($standby->safe_psql('postgres', $sift), $primary->safe_psql('postgres', $sift), "the standby returns the primary's rows");
	is($standby->safe_psql('postgres', $sift), $standby->safe_psql('postgres', $heap), 'the rows are the heap\'s');
	$standby->safe_psql('postgres', "SELECT sift.verify('$idx', true)");
};

$standby->stop;
$primary->stop;
done_testing();
