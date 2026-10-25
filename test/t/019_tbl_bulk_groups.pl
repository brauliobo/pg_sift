# sift.table_bulk_groups: COPY writes column groups directly. The rows are a heap twin's on the primary and on a standby that replays the
# groups under generic WAL consistency checking, the store verifies clean, a backend killed with SIGKILL in the middle of a COPY (groups
# published, rows still waiting in the backend) leaves no row of it visible after crash recovery and nothing the next VACUUM cannot
# reclaim, and a published table keeps the write buffers.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $rows = knob('SIFT_ROWS', 60000, 40000);
my $conf = "autovacuum = off\nsift.table_bulk_groups = on\nsift.table_write_buffer_limit = 2048\nwal_consistency_checking = 'generic'";
my $digest = 'SELECT count(*) || $$:$$ || coalesce(sum(id), 0) || $$:$$ || coalesce(sum(hashtextextended(id::text || grp::text || v || pad, 0)::numeric), 0) FROM %s';
my $awk = q{awk '{ printf "%d\t%d\trow %d\t%0300d\n", $1, $1 % 7, $1, $1 }'};
my $kinds = q{SELECT count(*) FILTER (WHERE kind = 'columns') || $$:$$ || count(*) FILTER (WHERE kind LIKE '%%buffer') FROM sift.table_segments($$%s$$)};

sub digest { return $_[0]->safe_psql('postgres', sprintf($digest, $_[1])) }
sub copy_sql { my ($t, $lo, $hi) = @_; return "COPY $t FROM PROGRAM \$\$seq $lo $hi | $awk\$\$" }
sub twin_sql { my ($t, $lo, $hi) = @_; return "INSERT INTO $t SELECT g, g % 7, 'row ' || g, lpad(g::text, 300, '0') FROM generate_series($lo, $hi) g" }
sub create_sql { my ($t, $am) = @_; return "CREATE TABLE $t (id int, grp int, v text, pad text) USING $am" }
# the groups the table has and the write buffers (none: nothing went through them)
sub kinds { return $_[0]->safe_psql('postgres', sprintf($kinds, $_[1])) }

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
plan skip_all => 'the load needs the testing build' unless $primary->safe_psql('postgres', "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'");
install_sift($primary);
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('bulk_standby')");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'bulk_standby'\nhot_standby_feedback = on\n$conf");
$standby->start;

subtest 'a COPY, a CREATE TABLE AS and a materialized view are the heap twin\'s, on the primary and on a standby' => sub {
	$primary->safe_psql('postgres', create_sql('t', 'sift_table') . '; CREATE INDEX t_grp ON t (grp); ' . create_sql('twin', 'heap') . '; ' . twin_sql('twin', 1, $rows));
	$primary->safe_psql('postgres', copy_sql('t', 1, $rows));
	my $want = digest($primary, 'twin');
	is(digest($primary, 't'), $want, 'the rows of the COPY');
	like(kinds($primary, 't'), qr/^[1-9]\d*:0$/, 'groups and no write buffer');
	$primary->safe_psql('postgres', 'CREATE TABLE c USING sift_table AS SELECT * FROM twin; CREATE MATERIALIZED VIEW mv USING sift_table AS SELECT * FROM twin');
	$primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv');
	is(digest($primary, 'c'), $want, 'the rows of the CREATE TABLE AS');
	is(digest($primary, 'mv'), $want, 'the rows of the refreshed materialized view');
	like(kinds($primary, $_), qr/^[1-9]\d*:0$/, "$_: groups and no write buffer") for qw(c mv);
	is($primary->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) FROM t WHERE grp = 3"), $primary->safe_psql('postgres', 'SELECT count(*) FROM twin WHERE grp = 3'), 'the btree finds the rows by their TIDs');
	$primary->wait_for_catchup($standby);
	is(digest($standby, $_), $want, "$_ on the standby, replayed under generic WAL consistency checking") for qw(t c mv);
	cmp_ok($primary->safe_psql('postgres', "SELECT sift._test_check('t')"), '>=', 0, 'the store of the table verifies on the primary');
	cmp_ok($standby->safe_psql('postgres', "SELECT sift._test_check('t')"), '>=', 0, 'and on the standby');
	$primary->safe_psql('postgres', 'VACUUM t; VACUUM t');
	is(digest($primary, 't'), $want, 'the same rows after VACUUM');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('t')"), 0, 'and nothing is left over');
};

# the standby is not needed below, and its slot would keep the WAL of the crashes
$standby->stop;
$primary->safe_psql('postgres', "SELECT pg_drop_replication_slot('bulk_standby')");

subtest 'a backend killed in the middle of a COPY leaves none of its rows, and what VACUUM reclaims' => sub {
	$primary->safe_psql('postgres', create_sql('k', 'sift_table') . '; ' . create_sql('ktwin', 'heap') . '; ' . twin_sql('ktwin', 1, 20000));
	$primary->safe_psql('postgres', copy_sql('k', 1, 20000));
	my $want = digest($primary, 'ktwin');
	is(digest($primary, 'k'), $want, 'the committed rows');
	my $before = $primary->safe_psql('postgres', "SELECT count(*) FROM sift.table_segments('k') WHERE kind = 'columns'");
	# the program sends rows for groups to be written, then stops sending: the COPY waits with rows in the backend
	my ($out, $err) = ('', '');
	my $h = start([ 'psql', '-X', '-q', '-h', $primary->host, '-p', $primary->port, '-d', 'postgres', '-c',
			"COPY k FROM PROGRAM \$\$(seq 100001 160000 | $awk; sleep 20)\$\$" ], '>', \$out, '2>', \$err);
	ok($primary->poll_query_until('postgres', "SELECT count(*) > $before FROM sift.table_segments('k') WHERE kind = 'columns'"), 'groups of the COPY are published while it runs');
	is(digest($primary, 'k'), $want, 'and a reader sees none of its rows');
	my $pid = $primary->safe_psql('postgres', q{SELECT pid FROM pg_stat_activity WHERE query LIKE 'COPY k FROM PROGRAM%' AND pid <> pg_backend_pid()});
	ok($pid, 'the backend of the COPY');
	my $log_offset = -s $primary->logfile;
	kill_expected($pid);
	finish($h);
	# not poll_query_until: its psql can exit on 'in recovery mode' before the 'SELECT true' is written, and IPC::Run dies with a broken pipe
	ok($primary->wait_for_log(qr/database system is ready to accept connections/, $log_offset), 'the server recovered from the crash');
	is(digest($primary, 'k'), $want, 'only the committed rows after crash recovery');
	cmp_ok($primary->safe_psql('postgres', "SELECT sift._test_check('k')"), '>=', 0, 'the store verifies');
	$primary->safe_psql('postgres', 'VACUUM k; VACUUM k');
	is(digest($primary, 'k'), $want, 'the same rows after VACUUM');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('k')"), 0, 'no block is left over, the groups of the aborted COPY are gone');
	$primary->safe_psql('postgres', copy_sql('k', 20001, 30000) . '; ' . twin_sql('ktwin', 20001, 30000));
	is(digest($primary, 'k'), digest($primary, 'ktwin'), 'the table takes loads again');
};

subtest 'a commit survives an immediate stop' => sub {
	$primary->safe_psql('postgres', create_sql('s', 'sift_table') . '; ' . create_sql('stwin', 'heap') . '; ' . twin_sql('stwin', 1, $rows));
	$primary->safe_psql('postgres', copy_sql('s', 1, $rows));
	$primary->stop('immediate');
	$primary->start;
	is(digest($primary, 's'), digest($primary, 'stwin'), 'every row of the COPY after crash recovery');
	cmp_ok($primary->safe_psql('postgres', "SELECT sift._test_check('s')"), '>=', 0, 'the store verifies');
};

subtest 'a published table keeps the write buffers' => sub {
	my $logical = sift_node('logical', allows_streaming => 'logical');
	$logical->append_conf('postgresql.conf', "$conf\nshared_preload_libraries = 'pg_sift'");
	$logical->start;
	install_sift($logical);
	$logical->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
	$logical->safe_psql('postgres', create_sql('p', 'sift_table') . '; ' . create_sql('q', 'sift_table') . '; CREATE PUBLICATION pub FOR TABLE p');
	like($logical->safe_psql('postgres', "SELECT sift._test_tbl_bulk_refusal('p')"), qr/published/, 'the table says why');
	is($logical->safe_psql('postgres', "SELECT sift._test_tbl_bulk_refusal('q') IS NULL"), 't', 'an unpublished one qualifies');
	$logical->safe_psql('postgres', copy_sql('p', 1, 2000));
	is(kinds($logical, 'p'), '0:1', 'the published table went through a write buffer');
	$logical->safe_psql('postgres', copy_sql('q', 1, 2000));
	like(kinds($logical, 'q'), qr/^1:0$/, 'the other went into a group');
	$logical->stop;
};

$primary->stop;
done_testing();
