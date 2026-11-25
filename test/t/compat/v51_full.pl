# CI-02, Spec 30: a cluster written by the v5.1.0 release is opened by the 6.0 library with no REINDEX and no rewrite.
# Every Format 1 relation (sift_col plain, JSONB paths, tracked and scoped; sift_table; sift BM25 1.x) answers as it
# did under v5.1.0 (golden digests), takes INSERT, UPDATE, DELETE, VACUUM and SiftAgg, is replayed by a standby, and is
# moved to Format 2.0 online: REINDEX INDEX CONCURRENTLY with reads and writes running (sift_col), sift.upgrade_table()
# and VACUUM FULL (sift_table). The BM25 index stays on its Format 1 minor. Rows are checked against the heap, never against the
# index under test.
#
# The v5.1.0 library comes from scripts/build-v510-fixture.sh (SIFT_V510_LIBDIR, default /tmp/sift-v510-lib;
# SIFT_V510_BUILD=1 builds it here); the library under test from SIFT_V60_LIBDIR or the staged root SIFT_ROOT.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest ();
use SiftCompat;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my ($v510, $why) = v510_root();
plan skip_all => $why unless defined $v510;
my $v60 = v60_root();
plan skip_all => 'no 6.0 library: run through scripts/tap.sh (SIFT_ROOT) or set SIFT_V60_LIBDIR' unless defined $v60;

my $tree = PostgreSQL::Test::Utils::tempdir('v51tree');
$ENV{PATH} = make_server_tree($tree) . ":$ENV{PATH}";
delete $ENV{TEMP_CONFIG};

# ------------------------------------------------------------------ the cluster of v5.1.0
install_sift_version($tree, $v510);
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_sift'\njit = off\nmax_wal_size = 256MB\npg_sift.install_notice = off\n");
$node->start;
sql($node, 'CREATE EXTENSION pg_sift');
is(sql($node, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"), '5.1', 'the cluster is written by v5.1.0');

sql($node, fixture_sql());
sql($node, dml(0, @ALL_DML));
vacuum_all($node);
sql($node, 'CHECKPOINT');

for my $rel (@FORMAT1) { like(format_of($node, $rel), qr/^1\.\d+$/, "$rel is Format 1 under v5.1.0") }
like(format_of($node, $BM25), qr/^1\.\d+$/, "$BM25 is Format 1 under v5.1.0");
agree_with_heap($node, 'v5.1.0');
my $golden    = snapshot($node);
my $nodes0    = filenodes($node);
my $formats0  = formats($node, @FORMAT1, $BM25);
my $st_vf0    = sql($node, q{SELECT count(*)::text || ':' || sum(v) FROM st_vf});
$node->stop;

# ------------------------------------------------------------------ the 6.0 library over the files of v5.1.0
install_sift_version($tree, $v60);
$node->start;
is(sql($node, 'SHOW sift.parallel_scan'), 'on', 'the 6.0 library is loaded (sift.parallel_scan defaults on)');
is(sql($node, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"), '5.1', 'the SQL is still the 5.1 one: the rolling window of a binary upgrade');
is_deeply(snapshot($node), $golden, 'binary swapped, extension not updated: every query answers as under v5.1.0');

sql($node, "ALTER EXTENSION pg_sift UPDATE TO '6.0'");
is(sql($node, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"), '6.0', 'ALTER EXTENSION UPDATE TO 6.0');
is(filenodes($node), $nodes0, 'no relation was rewritten by the update: zero reindex');
is_deeply(formats($node, @FORMAT1, $BM25), $formats0, 'every relation is still on the format it was written in');
is_deeply(snapshot($node), $golden, 'after ALTER EXTENSION: every query answers as under v5.1.0');
is(sql($node, "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
	WHERE a.amname IN ('sift', 'sift_col') AND NOT (i.indisvalid AND i.indisready)"), '0', 'every sift index is valid and ready');
is(sql($node, "SELECT count(*) FROM sift.health() WHERE severity <> 'info'"), '0', 'sift.health() knows Format 1 as information only');
is(sql($node, "SELECT string_agg(relation::text || ' ' || state, ',' ORDER BY relation::text) FROM sift.indexes WHERE format ~ '^1[.]'"),
	join(',', map { "$_ ready" } sort(@FORMAT1, $BM25)), 'sift.indexes lists the Format 1 relations as ready');

# ------------------------------------------------------------------ a standby of the 6.0 server replays Format 1
$node->backup('compat_base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'compat_base', has_streaming => 1);
$standby->start;
is_deeply(snapshot($standby), $golden, 'the standby reads Format 1 as v5.1.0 answered');

sub catch_up { $node->wait_for_catchup($standby) }

sub same_on_standby
{
	my ($label) = @_;
	catch_up();
	is_deeply(snapshot($standby), snapshot($node), "$label: the standby answers as the primary");
}

# ------------------------------------------------------------------ writes on Format 1 under 6.0
sql($node, dml(1, @ALL_DML));
vacuum_all($node);
agree_with_heap($node, '6.0 writes on Format 1');
is(sql($node, "SELECT count(*) FROM sift.check(NULL, true)"), '0', '6.0 writes on Format 1: sift.check(heapallindexed) finds no problem');
is_deeply(formats($node, @FORMAT1, $BM25), $formats0, '6.0 writes on Format 1: writes and VACUUM keep every relation on its format');
same_on_standby('6.0 writes on Format 1');
for my $plan (qw(ev_group sc_tenant))
{
	my $explain = sql($node, 'EXPLAIN (COSTS OFF) ' . $HEAP_QUERIES{$plan});
	note("plan of $plan on Format 1:\n$explain");
}
sql($node, "SELECT count(*) FROM sift.maintain('$_')") for (@FORMAT1, $BM25);
agree_with_heap($node, '6.0 maintenance on Format 1');
is_deeply(formats($node, @FORMAT1, $BM25), $formats0, '6.0 maintenance on Format 1: a maintenance pass keeps every relation on its format');

# ------------------------------------------------------------------ online upgrade of sift_col: REINDEX INDEX CONCURRENTLY
{
	my $holder = $node->background_psql('postgres', on_error_stop => 1);
	$holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$holder->query_safe('SELECT count(*) FROM ev');
	my $reindex = $node->background_psql('postgres', on_error_stop => 1);
	$reindex->query_until(qr/reindex_started/, "\\echo reindex_started\nREINDEX INDEX CONCURRENTLY ev_col;\n");
	ok($node->poll_query_until('postgres',
			"SELECT count(*) = 1 FROM pg_stat_activity WHERE query LIKE 'REINDEX INDEX CONCURRENTLY ev_col%' AND wait_event_type = 'Lock'"),
		'REINDEX INDEX CONCURRENTLY is built and waits for the older snapshot');

	my $timeout = "SET statement_timeout = '20s'";
	for my $name (sort keys %HEAP_QUERIES)
	{
		is($node->psql('postgres', "$timeout; SELECT count(*) FROM ($HEAP_QUERIES{$name}) t"), 0, "during REINDEX: $name is not blocked");
	}
	is($node->psql('postgres', "$timeout; " . dml(2, 'ev')), 0, 'during REINDEX: INSERT, UPDATE and DELETE are not blocked');
	is(digest($node, $HEAP_QUERIES{ev_group}), digest($node, $HEAP_QUERIES{ev_group}, 1), 'during REINDEX: the old index answers as the heap after the writes');

	$holder->query_safe('COMMIT');
	$reindex->query_safe('SELECT 1');
	$holder->quit;
	$reindex->quit;
}
is(format_of($node, 'ev_col'), '2.0', 'ev_col is Format 2.0 after REINDEX INDEX CONCURRENTLY');
is(sql($node, "SELECT count(*) FROM pg_class WHERE relname ~ '_cc(new|old)'"), '0', 'REINDEX left no _ccnew or _ccold index');
is(sql($node, "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = 'ev_col'::regclass"), 't', 'ev_col is valid');
agree_with_heap($node, 'ev_col Format 2.0');

sql($node, "REINDEX INDEX CONCURRENTLY $_") for qw(ev_trk pairs_journal sc_idx);
is(format_of($node, $_), '2.0', "$_ is Format 2.0 after REINDEX INDEX CONCURRENTLY") for qw(ev_trk sc_idx);
is(format_of($node, 'pairs_journal'), $formats0->{pairs_journal}, 'pairs_journal, the journal of the scope of sc_idx, stays Format 1 after REINDEX INDEX CONCURRENTLY');
is(format_of($node, $BM25), $formats0->{$BM25}, "$BM25 stays on the Format 1 minor v5.1.0 wrote");
agree_with_heap($node, 'every sift_col Format 2.0');
same_on_standby('sift_col Format 2.0');
is(format_of($standby, 'ev_col'), '2.0', 'the standby has ev_col on Format 2.0');

# ------------------------------------------------------------------ online upgrade of sift_table
is(sql($node, "SELECT sift.upgrade_table('st')"), 't', 'sift.upgrade_table() rewrites the Format 1 sift_table');
is(format_of($node, 'st'), '2.0', 'st is Format 2.0 after sift.upgrade_table()');
$node->psql('postgres', "SELECT sift.upgrade_table('st')", stdout => \my $again, stderr => \my $notice);
is($again, 'f', 'sift.upgrade_table() of a Format 2.0 table does nothing');
like($notice, qr/already on Format 2\.0/, 'and says so');
agree_with_heap($node, 'st Format 2.0');
is(sql($node, "SELECT count(*) FROM pg_index WHERE indrelid = 'st'::regclass AND indisvalid"), '2', 'the primary key and the btree on st were rebuilt and are valid');

sql($node, 'VACUUM FULL st_vf');
is(format_of($node, 'st_vf'), '2.0', 'VACUUM FULL moves st_vf to Format 2.0');
is(sql($node, q{SELECT count(*)::text || ':' || sum(v) FROM st_vf}), $st_vf0, 'VACUUM FULL keeps the rows of st_vf');

# ------------------------------------------------------------------ the upgraded relations and the BM25 index take writes
sql($node, dml(3, @ALL_DML));
vacuum_all($node);
agree_with_heap($node, 'Format 2.0 and Format 1 writes');
is(sql($node, "SELECT count(*) FROM sift.check(NULL, true)"), '0', 'sift.check(heapallindexed) finds no problem after the upgrades');
my %format1 = map { $_ => 1 } ($BM25, 'pairs_journal');
is(sql($node, 'SELECT count(*) FROM sift.health()'), '0', 'sift.health() reports nothing: every Format 1 relation but the BM25 index and the journal of the scope is Format 2.0');
is(sql($node, "SELECT string_agg(relation::text || ' ' || format, ',' ORDER BY relation::text) FROM sift.indexes"),
	join(',', map { "$_ " . ($format1{$_} ? $formats0->{$_} : '2.0') } sort(@FORMAT1, $BM25)), 'sift.indexes: Format 2.0 everywhere, the v5.1.0 minor for the BM25 index and the journal');
same_on_standby('Format 2.0 and Format 1 writes');
for my $rel (@FORMAT1) { is(format_of($standby, $rel), $format1{$rel} ? $formats0->{$rel} : '2.0', "the standby has $rel on " . ($format1{$rel} ? 'its Format 1 minor' : 'Format 2.0')) }

$standby->stop;
$node->stop;
done_testing();
