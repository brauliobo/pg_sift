# EXT-09, EXT-10, EXT-17: Format 2.0 objects survive pg_dump and pg_restore, and ALTER ... SET TABLESPACE.
#
#   - pg_dump | psql in one transaction, and pg_dump -Fc | pg_restore -j 4 with three sift_col indexes on one table
#     (the parallel CREATE INDEX of one table must leave one internal tracking trigger per index, no error, no
#     deadlock), and the two-phase restore of the options of a sift_table: every restore answers as its source, takes
#     writes and keeps its formats and options
#   - ALTER INDEX / ALTER TABLE ... SET TABLESPACE on a sift_col index, a sift BM25 index and a sift_table: the answers
#     stay, the files are in the new tablespace, and the same answers come back after a crash and a restart
#   - the cluster has data checksums, and pg_checksums --check passes on it when the test is done (EXT-03)
# The answers are checked against the heap and its twin, never against the index under test.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use SiftCompat;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $src = sift_node('src', extra => ['--data-checksums']);
$src->append_conf('postgresql.conf', 'sift.test_create_format1 = off');
$src->start;

my ($ts1, $ts2) = (PostgreSQL::Test::Utils::tempdir('ts1'), PostgreSQL::Test::Utils::tempdir('ts2'));

sub tablespaces { my ($node, $a, $b) = @_; sql($node, "CREATE TABLESPACE ts1 LOCATION '$a'; CREATE TABLESPACE ts2 LOCATION '$b'") }

sub restore_node
{
	my ($name) = @_;
	my $node = sift_node($name);
	$node->append_conf('postgresql.conf', 'sift.test_create_format1 = off');
	$node->start;
	tablespaces($node, PostgreSQL::Test::Utils::tempdir("${name}_ts1"), PostgreSQL::Test::Utils::tempdir("${name}_ts2"));
	return $node;
}

# the sift_col indexes of table ev3 race in a parallel restore: three on one table
my $EXTRA = q{
	CREATE TABLE ev3 (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, c int NOT NULL);
	INSERT INTO ev3 SELECT i, i % 7, i % 11, i % 13 FROM generate_series(1, 2000) i;
	CREATE INDEX ev3_a ON ev3 USING sift_col (id, a);
	CREATE INDEX ev3_b ON ev3 USING sift_col (id, b);
	CREATE INDEX ev3_c ON ev3 USING sift_col (id, c);
	CREATE TABLE ev_ts (id int PRIMARY KEY, g int NOT NULL, v int) TABLESPACE ts1;
	INSERT INTO ev_ts SELECT i, i % 5, i % 17 FROM generate_series(1, 1500) i;
	CREATE INDEX ev_ts_col ON ev_ts USING sift_col (id, g, v) TABLESPACE ts2;
	SELECT sift.table_set_option('st', 'key_order', 'grp, id');
};
my %EXTRA_QUERIES = (
	ev3_a => q{SELECT a, count(*), sum(id) FROM ev3 GROUP BY a},
	ev3_b => q{SELECT b, count(*), sum(id) FROM ev3 GROUP BY b},
	ev3_c => q{SELECT c, count(*), sum(id) FROM ev3 GROUP BY c},
	ev_ts => q{SELECT g, count(*), sum(v) FROM ev_ts GROUP BY g},
);

sub everything
{
	my ($node) = @_;
	my $d = snapshot($node);
	delete $d->{ranked};    # the scores of an index counts its dead documents until the next merge: a rebuilt index ranks them out
	$d->{$_} = digest($node, $EXTRA_QUERIES{$_}) for keys %EXTRA_QUERIES;
	return $d;
}

tablespaces($src, $ts1, $ts2);
sql($src, 'CREATE EXTENSION pg_sift');
sql($src, fixture_sql());
sql($src, $EXTRA);
sql($src, dml(0, @ALL_DML));
sql($src, 'ANALYZE');
my $want = everything($src);

for my $rel (@SIFT_COL, qw(ev3_a ev3_b ev3_c ev_ts_col), @TABLES) { is(format_of($src, $rel), '2.0', "$rel is Format 2.0") }
my $bm25_format = format_of($src, $BM25);
like($bm25_format, qr/^1\.[67]$/, "$BM25 is Format 1");

# one internal tracking trigger per sift_col index of a table, none on a partition parent and none in a dump
sub trigger_state
{
	my ($node, $table) = @_;
	return sql($node, "SELECT (SELECT count(*) FROM pg_trigger WHERE tgrelid = '$table'::regclass AND tgname LIKE 'sift_track_%' AND tgisinternal) || ' of '
		|| (SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = '$table'::regclass AND a.amname = 'sift_col')");
}
my %triggers = map { $_ => trigger_state($src, $_) } qw(ev ev3 ev_ts);
is($triggers{ev3}, '3 of 3', 'ev3: one tracking trigger for each of its three sift_col indexes');

# ------------------------------------------------------------------ the checks every restored node passes
sub restored_ok
{
	my ($node, $label) = @_;
	is_deeply(everything($node), $want, "$label: every query answers as the source");
	is_deeply(formats($node, @SIFT_COL, qw(ev3_a ev3_b ev3_c ev_ts_col), @TABLES), formats($src, @SIFT_COL, qw(ev3_a ev3_b ev3_c ev_ts_col), @TABLES),
		"$label: the formats of the source");
	like(format_of($node, $BM25), qr/^1\.[4-7]$/, "$label: the BM25 index is Format 1");
	is_deeply({ map { $_ => trigger_state($node, $_) } qw(ev ev3 ev_ts) }, \%triggers, "$label: one tracking trigger per sift_col index, none twice, none missing");
	is(sql($node, "SELECT count(*) FROM pg_trigger WHERE tgname LIKE 'sift_track_%' AND NOT tgisinternal"), '0', "$label: no tracking trigger is a user trigger");
	is(sql($node, "SELECT string_agg(name || '=' || value, ',' ORDER BY name) FROM sift.table_option_values WHERE rel::oid = 'st'::regclass::oid"),
		'key_order=grp, id', "$label: the options of the sift_table are restored");
	is(sql($node, "SELECT count(*) FROM sift.check()"), '0', "$label: sift.check() finds no problem");
	is(sql($node, "SELECT string_agg(spcname, ',' ORDER BY spcname) FROM pg_tablespace WHERE spcname LIKE 'ts%'"), 'ts1,ts2', "$label: the tablespaces are there");
	is(sql($node, "SELECT t.spcname FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE c.relname = 'ev_ts_col'"), 'ts2',
		"$label: the sift_col index is in the tablespace of the source");

	sql($node, dml(1, @ALL_DML));
	sql($node, 'VACUUM (ANALYZE) ev, ev3, ev_ts, docs, st, st_heap');
	agree_with_heap($node, "$label after writes");
	is(sql($node, 'SELECT count(*) FROM sift.check(NULL, true)'), '0', "$label: sift.check(heapallindexed) finds no problem after VACUUM");
}

sub options_unapplied { my ($node) = @_; return sql($node, "SELECT count(*) FROM sift.health('st') WHERE check_name = 'table_options_unapplied'") }

# ------------------------------------------------------------------ EXT-09: pg_dump | psql in one transaction
my $plain = $src->basedir . '/dump.sql';
$src->command_ok([ 'pg_dump', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $plain ], 'EXT-09: pg_dump');
unlike(slurp_file($plain), qr/CREATE TRIGGER sift_track_/, 'EXT-09: the dump holds no internal tracking trigger');
{
	my $dst = restore_node('dump_psql');
	$dst->command_ok([ 'psql', '-X', '-q', '-1', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $plain ],
		'EXT-09: the restore in one transaction has no error');
	cmp_ok(options_unapplied($dst), '>', 0, 'EXT-09: the options that the one-transaction restore loaded after the rows are reported as unapplied before VACUUM FULL');
	sql($dst, 'VACUUM FULL st');
	is(options_unapplied($dst), '0', 'EXT-09: VACUUM FULL applies the restored options');
	restored_ok($dst, 'EXT-09 pg_dump | psql');
	$dst->stop;
}

# ------------------------------------------------------------------ EXT-10: pg_dump -Fc, pg_restore -j 4
my $custom = $src->basedir . '/dump.fc';
$src->command_ok([ 'pg_dump', '-Fc', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $custom ], 'EXT-10: pg_dump -Fc');
my $toc = `pg_restore -l $custom`;
is($?, 0, 'EXT-10: pg_restore -l');
like($toc, qr/TABLE DATA sift table_option_values/, 'EXT-09: the dump carries the options of the sift_tables as extension configuration data');
{
	my $dst = restore_node('restore_j4');
	$dst->command_ok([ 'pg_restore', '-j', '4', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', $custom ],
		'EXT-10: pg_restore -j 4 has no error and no deadlock');
	sql($dst, 'VACUUM FULL st');
	restored_ok($dst, 'EXT-10 pg_restore -j 4');
	$dst->stop;
}

# the options before the rows: nothing is left to rewrite
{
	my $dst = restore_node('restore_two_phase');
	my @r = ('-p', $dst->port, '-h', $dst->host, '-d', 'postgres');
	$dst->command_ok([ 'pg_restore', '--section=pre-data', @r, $custom ], 'EXT-09: pg_restore, pre-data');
	my $option_data = qr/TABLE DATA sift (table_option_values|table_paths) /;
	my ($options, $rest) = map { $src->basedir . "/$_.list" } qw(options rest);
	append_to_file($options, join('', grep { $_ =~ $option_data } split(/^/m, $toc)));
	append_to_file($rest, join('', grep { $_ !~ $option_data } split(/^/m, $toc)));
	$dst->command_ok([ 'pg_restore', '--section=data', '-L', $options, @r, $custom ], 'EXT-09: pg_restore, the data of the option tables');
	$dst->command_ok([ 'pg_restore', '--section=data', '-L', $rest, @r, $custom ], 'EXT-09: pg_restore, the rest of the data');
	$dst->command_ok([ 'pg_restore', '--section=post-data', @r, $custom ], 'EXT-09: pg_restore, post-data');
	is(options_unapplied($dst), '0', 'EXT-09: restored in two phases, no part predates the restored options');
	restored_ok($dst, 'EXT-09 two-phase pg_restore');
	$dst->stop;
}

# ------------------------------------------------------------------ EXT-17: SET TABLESPACE
sub tablespace_of
{
	my ($node, $rel) = @_;
	return sql($node, "SELECT coalesce((SELECT spcname FROM pg_tablespace WHERE oid = reltablespace), 'pg_default') FROM pg_class WHERE relname = '$rel'");
}

sub in_tablespace_dir { my ($node, $rel) = @_; return sql($node, "SELECT pg_relation_filepath('$rel') LIKE 'pg_tblspc/%'") }

my $before = everything($src);
my $phase = 1;
for my $move (
	[ 'ALTER INDEX ev_col SET TABLESPACE ts1',      'ev_col',    'ts1' ],
	[ 'ALTER INDEX ev_trk SET TABLESPACE ts2',      'ev_trk',    'ts2' ],
	[ 'ALTER INDEX docs_body SET TABLESPACE ts1',   'docs_body', 'ts1' ],
	[ 'ALTER TABLE ev SET TABLESPACE ts2',          'ev',        'ts2' ],
	[ 'ALTER TABLE st SET TABLESPACE ts1',          'st',        'ts1' ],
	[ 'ALTER INDEX ev_ts_col SET TABLESPACE pg_default', 'ev_ts_col', 'pg_default' ],
)
{
	my ($stmt, $rel, $to) = @$move;
	sql($src, $stmt);
	is(tablespace_of($src, $rel), $to, "EXT-17: $stmt");
	is(in_tablespace_dir($src, $rel), $to eq 'pg_default' ? 'f' : 't', "EXT-17: the file of $rel is where the tablespace says");
	is_deeply(everything($src), $before, "EXT-17: after $stmt every query answers as before");
	is(sql($src, 'SELECT count(*) FROM sift.check(NULL, true)'), '0', "EXT-17: sift.check(heapallindexed) after $stmt");
	sql($src, dml(++$phase, @ALL_DML));
	for my $name (sort keys %HEAP_QUERIES)
	{
		is(digest($src, $HEAP_QUERIES{$name}), digest($src, $HEAP_QUERIES{$name}, 1), "EXT-17: $name is the heap's answer after writes past $stmt");
	}
	$before = everything($src);
}
for my $rel (@SIFT_COL, @TABLES) { is(format_of($src, $rel), '2.0', "EXT-17: $rel is still Format 2.0") }

sql($src, 'CHECKPOINT');
sql($src, dml(++$phase, @ALL_DML));
$before = everything($src);
$src->stop('immediate');
$src->start;
is_deeply(everything($src), $before, 'EXT-17: after a crash the moved relations answer as before it');
$src->restart;
is_deeply(everything($src), $before, 'EXT-17: after a restart the moved relations answer as before it');
is(sql($src, 'SELECT count(*) FROM sift.check(NULL, true)'), '0', 'EXT-17: sift.check(heapallindexed) after the crash and the restart');
sql($src, 'REINDEX INDEX ev_col');
is(tablespace_of($src, 'ev_col'), 'ts1', 'EXT-17: REINDEX keeps the index in its tablespace');
is_deeply(everything($src), $before, 'EXT-17: and answers as before');

$src->stop;
command_ok([ 'pg_checksums', '--check', '-D', $src->data_dir ], 'EXT-03: pg_checksums --check finds no bad checksum after the dumps, the moves and the crash');
done_testing();
