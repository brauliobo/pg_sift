# The key journal of a parent index of format major 2 is WAL-logged page by page: a standby replays it (every Generic record is compared with the page it wrote:
# wal_consistency_checking = 'generic') and crash recovery after kill -9 gives the journal that was committed, its lineage and its records, with the child joining the parent as the heap does.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "wal_consistency_checking = 'generic'\nsift.test_create_format1 = off\nshared_buffers = 64MB");
$primary->start;
install_sift_test($primary, 'the journal is read through sift.col_connection_state');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

$primary->safe_psql('postgres', q{
	CREATE TABLE dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
	INSERT INTO dim SELECT i, i % 7, i % 3 FROM generate_series(0, 999) i;
	CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
	INSERT INTO ev SELECT i, i % 1100, i % 11 FROM generate_series(1, 4000) i;
	CREATE INDEX dim_col ON dim USING sift_col (k, a, b);
	CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)') });

my $join = 'SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1';
my $heap = "SET sift.enable_agg = off; $join";
my $agg = "SET sift.agg_force = on; $join";
my $journal = q{SELECT lineage || ':' || records FROM sift.col_connection_state('dim_col')};
my $problems = q{SELECT count(*) FROM sift.check('dim_col', heapallindexed => true) c1, sift.check('ev_col', heapallindexed => true) c2};

sub same_journal
{
	my ($what) = @_;
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
	is($standby->safe_psql('postgres', $journal), $primary->safe_psql('postgres', $journal), "$what: the standby has the journal of the primary");
	is($standby->safe_psql('postgres', $agg), $standby->safe_psql('postgres', $heap), "$what: the child joins the parent on the standby as the heap does");
}

is($primary->safe_psql('postgres', "SELECT format_major FROM sift.status('dim_col')"), 2, 'the parent is of format major 2');
same_journal('built');

subtest 'parent writes of every kind, a rollback and a journal that wraps' => sub {
	$primary->safe_psql('postgres', q{
		UPDATE dim SET a = 6 WHERE k % 31 = 0;
		UPDATE dim SET k = k + 100000 WHERE k % 97 = 0;
		DELETE FROM dim WHERE k % 53 = 0;
		INSERT INTO dim SELECT i, 1, 1 FROM generate_series(1000, 1049) i;
		BEGIN; UPDATE dim SET b = 2 WHERE k % 17 = 0; ROLLBACK });
	same_journal('writes');
	$primary->safe_psql('postgres', 'UPDATE dim SET a = a + 1');
	same_journal('wrap');
	is($primary->safe_psql('postgres', $problems), 0, 'sift.check finds no problem');
};

subtest 'VACUUM and sift.maintain() trim the journal on the primary and the standby' => sub {
	$primary->safe_psql('postgres', q{UPDATE dim SET b = 1 WHERE k = 11; VACUUM (INDEX_CLEANUP ON) dim});
	same_journal('vacuum');
	$primary->safe_psql('postgres', q{SELECT busy FROM sift.maintain('dim_col')});
	same_journal('maintain');
};

subtest 'crash recovery gives the journal that was committed' => sub {
	$primary->safe_psql('postgres', q{UPDATE dim SET a = 3 WHERE k % 13 = 0; INSERT INTO dim SELECT i, 2, 2 FROM generate_series(1100, 1119) i});
	my $before = $primary->safe_psql('postgres', $journal);
	my $want = $primary->safe_psql('postgres', $heap);
	kill9_wait($primary);
	$primary->start;
	is($primary->safe_psql('postgres', $journal), $before, 'kill -9: the lineage and the records are those of the commit');
	is($primary->safe_psql('postgres', $agg), $want, 'the child joins the parent as before the crash');
	is($primary->safe_psql('postgres', $problems), 0, 'sift.check finds no problem');
	$primary->safe_psql('postgres', 'UPDATE dim SET a = 4 WHERE k = 5');
	same_journal('after recovery');
};

subtest 'a rebuilt parent is a journal of its own on both nodes' => sub {
	my $lineage = $primary->safe_psql('postgres', q{SELECT lineage FROM sift.col_connection_state('dim_col')});
	$primary->safe_psql('postgres', 'REINDEX INDEX dim_col');
	isnt($primary->safe_psql('postgres', q{SELECT lineage FROM sift.col_connection_state('dim_col')}), $lineage, 'REINDEX starts another lineage');
	same_journal('reindex');
	is($primary->safe_psql('postgres', $problems), 0, 'sift.check finds no problem');
};

$primary->stop;
$standby->stop;
done_testing();
