# sift_table: UPDATEs that keep the indexed columns are HOT updates (no index entry for the new version; the entries of the old one lead to it
# through the tombstone log). The chain must survive what the log goes through: a snapshot that began before the updates while VACUUM runs
# (the old version is not removable, then it is), an immediate shutdown and recovery, and a hot standby that replays the log, its
# collapse and the removal of the chains.
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
$primary->safe_psql('postgres', 'CREATE TABLE r (id int PRIMARY KEY, g int, v int, note text) USING sift_table WITH (autovacuum_enabled = false);'
	. ' INSERT INTO r SELECT i, i % 5, i, $$n$$ FROM generate_series(1, 5000) i; CREATE INDEX r_g ON r (g); VACUUM r');
is($primary->safe_psql('postgres', "SELECT count(*) FROM sift.table_segments('r') WHERE kind = 'columns'"), 1, 'the rows are in a column group');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# the rows of g = 3 and the id lookups, by the table and by the indexes
sub answers
{
	my ($node) = @_;
	my $q = 'SELECT count(*), sum(v), sum(length(note)) FROM r WHERE g = 3; SELECT v FROM r WHERE id = 8; SELECT v FROM r WHERE id IN (13, 4998)';
	my $seq = $node->safe_psql('postgres', "SET enable_indexscan = off; SET enable_bitmapscan = off; $q");
	my $idx = $node->safe_psql('postgres', "SET enable_seqscan = off; SET enable_bitmapscan = off; $q");
	my $bmp = $node->safe_psql('postgres', "SET enable_seqscan = off; SET enable_indexscan = off; $q");
	is($idx, $seq, 'an index scan reads the rows a sequential scan does');
	is($bmp, $seq, 'a bitmap scan reads the rows a sequential scan does');
	return $seq;
}

subtest 'a snapshot older than the updates keeps its versions across VACUUM' => sub {
	my $old = $primary->background_psql('postgres');
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $q = 'SELECT count(*), sum(v) FROM r WHERE g = 3';
	my $before = $old->query_safe("SET enable_seqscan = off; SET enable_bitmapscan = off; $q");
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 1000, note = $$m$$ WHERE g = 3; UPDATE r SET v = v + 1 WHERE g = 3; VACUUM r');
	is($old->query_safe($q), $before, 'the old snapshot reads its versions through the index');
	$old->query_safe('COMMIT');
	$old->quit;
	my $now = answers($primary);
	like($now, qr/^1000\|/, 'the new snapshot reads the updated rows');
	$primary->safe_psql('postgres', 'VACUUM r');
	is(answers($primary), $now, 'the chains collapse and go with the old versions without a change of the answers');
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 1 WHERE g = 3; DELETE FROM r WHERE id % 50 = 3; VACUUM r');
	answers($primary);
};

subtest 'an immediate shutdown after HOT updates and VACUUM' => sub {
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 5 WHERE g = 3; UPDATE r SET v = v + 5, note = $$q$$ WHERE g = 3 AND id % 2 = 0');
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN; UPDATE r SET v = -1 WHERE g = 3');
	my $before = $primary->safe_psql('postgres', 'SELECT count(*), sum(v) FROM r');
	$primary->stop('immediate');
	$bg->quit;
	$primary->start;
	is($primary->safe_psql('postgres', 'SELECT count(*), sum(v) FROM r'), $before, 'the committed updates are back and the in-flight one is gone');
	answers($primary);
	$primary->safe_psql('postgres', 'VACUUM r');
	answers($primary);
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 1 WHERE g = 3');
	$primary->stop('immediate');
	$primary->start;
	answers($primary);
};

subtest 'a hot standby follows the chains, their collapse and their removal' => sub {
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 2 WHERE g = 3');
	$primary->wait_for_catchup($standby);
	is(answers($standby), answers($primary), 'the standby reads the chains');
	$primary->safe_psql('postgres', 'UPDATE r SET v = v + 2 WHERE g = 3; VACUUM r');
	$primary->wait_for_catchup($standby);
	is(answers($standby), answers($primary), 'the standby reads the collapsed chains');
	$primary->safe_psql('postgres', 'DELETE FROM r WHERE g = 3; VACUUM r; VACUUM r');
	$primary->wait_for_catchup($standby);
	is($standby->safe_psql('postgres', 'SET enable_seqscan = off; SELECT count(*) FROM r WHERE g = 3'), 0, 'a chain that ended in a delete is gone for the standby too');
	is($standby->safe_psql('postgres', 'SELECT count(*) FROM r'), $primary->safe_psql('postgres', 'SELECT count(*) FROM r'), 'the same rows');
};

$standby->stop;
$primary->stop;
done_testing();
