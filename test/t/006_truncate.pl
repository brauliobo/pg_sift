# VACUUM giving the free blocks at the end of the index file back (sift_store.h P7):
#  - a hot standby replays the truncation (generic WAL of the metapage and free list, then the file truncation) to a file of the
#    same size, with wal_consistency_checking = 'generic' comparing every page it rebuilds, and cancels a query that holds the index
#    when it replays the exclusive lock the truncation took, instead of letting it read blocks that are gone
#  - kill -9 of VACUUM at moments spread over its run (a VACUUM this size lasts milliseconds: the kills that land in the truncation are
#    a matter of luck), and an immediate shutdown after it, which recovery replays, leave an index sift.verify accepts that agrees with
#    the SQL reference
#  - a file longer than the high-water mark (a crash between the record and the truncation) is accepted, written over, and cut by the
#    next VACUUM
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 60000, 30000);
my $rounds = knob('SIFT_TRUNCATE_ROUNDS', 12, 6);
my $sift = sift_am();
my ($t, $idx) = ('tr', 'tr_idx');
my $no_inconsistency = qr/inconsistent page found|FATAL:\s+inconsistent/;

# Table $t with its index, 95% of the rows deleted and merged down by VACUUM and sift.merge: the survivors are in front of the
# segments they replaced, so the end of the file is free and the next VACUUM gives it back (the VACUUM of the set-up does not cut:
# one that may moves the survivors itself and cuts in the same run)
sub fragmented
{
	my ($node) = @_;
	$node->safe_psql('postgres', "DROP TABLE IF EXISTS $t");
	load_corpus($node, $t, $rows);
	$node->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (build_segments = 8)");
	$node->safe_psql('postgres', "ALTER INDEX $idx SET (vacuum_truncate = false); DELETE FROM $t WHERE id % 20 <> 0; VACUUM $t;"
		. " SELECT sift.merge('$idx', true); ALTER INDEX $idx RESET (vacuum_truncate)");
}

# relation_blocks and high_water of the index
sub extent
{
	my ($node) = @_;
	return $node->safe_psql('postgres', "SELECT relation_blocks || ':' || high_water FROM sift.index_info('$idx')");
}

sub blocks { return (split /:/, extent(@_))[0] }

sub healthy
{
	my ($node, $what) = @_;
	verify_agree($node, $sift, $t, $idx, $what);
}

my $primary = sift_node('trunc_primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "wal_consistency_checking = 'generic'");
$primary->start;
install_sift($primary);
# the full images wal_consistency_checking logs are many: a slot keeps the WAL the standby has not replayed
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('trunc_slot', true)");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('trunc_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'trunc_slot'\nhot_standby_feedback = off\nmax_standby_streaming_delay = 2s\nwal_consistency_checking = 'generic'");
$standby->start;

subtest 'the standby replays the truncation' => sub {
	fragmented($primary);
	my $before = blocks($primary);
	$primary->safe_psql('postgres', "VACUUM $t");
	my ($after, $hwm) = split /:/, extent($primary);
	cmp_ok($after, '<', $before / 4, "VACUUM gave the end of the file back ($before to $after blocks)");
	is($hwm, $after, 'the high-water mark follows the file');
	$primary->wait_for_catchup($standby);
	is(extent($standby), "$after:$hwm", 'the standby file has the size of the primary');
	healthy($standby, 'standby after the truncation');
	healthy($primary, 'primary after the truncation');
	ok(!$standby->log_contains($no_inconsistency), 'the standby found no inconsistent page replaying the truncation');

	# the file grows again over the same blocks, on both
	$primary->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 rare grown ' || g FROM generate_series(100001, 100000 + $rows) g;"
		. " SELECT sift.promote('$idx')");
	$primary->wait_for_catchup($standby);
	cmp_ok(blocks($primary), '>', $after, 'the index grows again');
	is(extent($standby), extent($primary), 'the standby file follows it');
	healthy($standby, 'standby after the file grew');
	ok(!$standby->log_contains($no_inconsistency), 'no inconsistent page when the file grew again');
};

# one VACUUM merges, writes the survivors into the blocks in front of the old segments (what it released is reusable at once under the
# exclusive lock, which the standby replays before the writes) and cuts the file
subtest 'the standby replays segments moved by one VACUUM' => sub {
	$primary->safe_psql('postgres', "DROP TABLE IF EXISTS $t");
	load_corpus($primary, $t, $rows);
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (build_segments = 8)");
	my $built = blocks($primary);
	$primary->safe_psql('postgres', "DELETE FROM $t WHERE id % 20 <> 0; VACUUM $t");
	my ($after, $hwm) = split /:/, extent($primary);
	cmp_ok($after, '<', $built / 4, "one VACUUM gave the space back ($built to $after blocks)");
	is($hwm, $after, 'the high-water mark follows the file');
	$primary->wait_for_catchup($standby);
	is(extent($standby), "$after:$hwm", 'the standby file has the size of the primary');
	healthy($standby, 'standby after the segments moved');
	healthy($primary, 'primary after the segments moved');
	ok(!$standby->log_contains($no_inconsistency), 'the standby found no inconsistent page replaying the moves');
};

subtest 'a standby query holding the index is cancelled by the truncation' => sub {
	fragmented($primary);
	$primary->wait_for_catchup($standby);
	my $bg = $standby->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SET enable_seqscan = off');
	$bg->query_safe("DECLARE c CURSOR FOR SELECT id FROM $t WHERE body @@> 'w1'");
	$bg->query_safe('FETCH 5 FROM c');

	my $log_offset = -s $standby->logfile;
	my $before = blocks($primary);
	$primary->safe_psql('postgres', "VACUUM $t");
	cmp_ok(blocks($primary), '<', $before, 'the primary truncated: nothing on it held the index');
	$primary->wait_for_catchup($standby);

	my ($out, $err) = ('', '');
	eval { ($out, $err) = $bg->query('FETCH FORWARD ALL FROM c') };
	ok($standby->log_contains(qr/terminating connection due to conflict with recovery/, $log_offset),
		'the standby cancelled the query that held the index');
	unlike($out, qr/could not read/, 'it never read a block that was gone');
	eval { $bg->quit };
	is(extent($standby), extent($primary), 'the standby file has the size of the primary');
	healthy($standby, 'standby after the conflict');
};

subtest 'a reader on the primary keeps VACUUM from truncating, without waiting' => sub {
	fragmented($primary);
	my $before = blocks($primary);
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe("BEGIN; SET enable_seqscan = off; SELECT count(*) FROM $t WHERE body @@> 'w1'");
	$primary->safe_psql('postgres', "VACUUM $t");
	is(blocks($primary), $before, 'the file is as it was while a reader holds the index');
	$bg->query_safe('COMMIT');
	$bg->quit;
	$primary->safe_psql('postgres', "VACUUM $t");
	cmp_ok(blocks($primary), '<', $before / 4, 'the next VACUUM gave it back');
	healthy($primary, 'primary after the reader left');
};

$standby->stop;
$primary->stop;

my $node = sift_node('trunc_crash');
$node->append_conf('postgresql.conf', "wal_consistency_checking = 'generic'");
$node->start;
install_sift($node);

subtest 'kill -9 of VACUUM while it runs' => sub {
	my $hits = 0;
	for my $round (1 .. $rounds)
	{
		fragmented($node);
		my $before = blocks($node);
		# dead rows left for VACUUM to remove make it last a few milliseconds, the truncation at its end
		$node->safe_psql('postgres', "DELETE FROM $t WHERE id % 40 = 0");
		$hits += crash_during($node, "VACUUM $t", $round * 3 % 31);
		healthy($node, "recovered after a kill -9 $round");
		my ($size, $hwm) = split /:/, extent($node);
		cmp_ok($size, '>=', $hwm, "round $round: the file covers the high-water mark");
		$node->safe_psql('postgres', "VACUUM $t");
		($size, $hwm) = split /:/, extent($node);
		is($size, $hwm, "round $round: the next VACUUM leaves a file of exactly the high-water mark");
		cmp_ok(blocks($node), '<', $before / 4, "round $round: and gave the end back ($before to " . blocks($node) . ')');
		healthy($node, "after the VACUUM of round $round");
	}
	note("$hits kills landed while VACUUM was running");
};

# the whole history since the checkpoint, the truncation included, is replayed by recovery
subtest 'an immediate shutdown after the truncation' => sub {
	$node->safe_psql('postgres', 'CHECKPOINT');
	fragmented($node);
	my $before = blocks($node);
	$node->safe_psql('postgres', "VACUUM $t");
	my $after = blocks($node);
	cmp_ok($after, '<', $before / 4, 'VACUUM truncated');
	$node->stop('immediate');
	$node->start;
	is(extent($node), "$after:$after", 'recovery leaves the file as it was');
	healthy($node, 'after crash recovery');
};

subtest 'a file longer than the high-water mark' => sub {
	fragmented($node);
	$node->safe_psql('postgres', "VACUUM $t");
	my ($size, $hwm) = split /:/, extent($node);
	my $path = $node->data_dir . '/' . $node->safe_psql('postgres', "SELECT pg_relation_filepath('$idx')");
	$node->stop;
	my $bytes = -s $path;
	open(my $fh, '+<', $path) or die "open $path: $!";
	truncate($fh, $bytes + 100 * 8192) or die "extend $path: $!";
	close($fh);
	$node->start;
	is(extent($node), ($size + 100) . ":$hwm", 'the file is 100 blocks past the mark, as after a crash between the record and the truncation');
	healthy($node, 'with the longer file');

	# allocation extends over the blocks that are there
	$node->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 rare over ' || g FROM generate_series(100001, 100000 + $rows) g;"
		. " SELECT sift.promote('$idx')");
	healthy($node, 'after the file was written over');
	$node->safe_psql('postgres', "DELETE FROM $t WHERE id > 100000; VACUUM $t");
	my ($after, $mark) = split /:/, extent($node);
	is($after, $mark, 'VACUUM cut the file to the high-water mark');
	healthy($node, 'after the file was cut');
};

ok(!$node->log_contains($no_inconsistency), 'crash recovery found no inconsistent page');
$node->stop;
done_testing();
