# Secondary projections of a sift_col index and of a sift_table survive pg_dump and pg_restore. The definitions are config data (sift.projection_defs)
# that a restore loads before it builds the sift_col index, so the row of the index names a relation that does not exist yet and is bound to it by the build.
use strict;
use warnings FATAL => 'all';
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use SiftCompat;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $src = sift_node('src');
$src->start;
sql($src, 'CREATE EXTENSION pg_sift');
sql($src, q{
	CREATE TABLE h (k int, g int, v int);
	INSERT INTO h SELECT i, i % 7, i % 13 FROM generate_series(1, 1000) i;
	CREATE INDEX h_sift ON h USING sift_col (k, g, v);
	CREATE TABLE t (k int, g int, v int) USING sift_table;
	INSERT INTO t SELECT i, i % 7, i % 13 FROM generate_series(1, 1000) i;
	SELECT sift.create_projection('h_sift', 'by_g', 'ORDER BY g INCLUDE (v)');
	SELECT sift.create_projection('t', 'agg_g', 'GROUP BY g MEASURES sum(v)');
});

my $LIST = q{SELECT string_agg(relation::text || '/' || name || '/' || kind, ',' ORDER BY relation::text, name) FROM sift.secondary_projections};
my $want = 'h_sift/by_g/sort,t/agg_g/aggregate';
is(sql($src, $LIST), $want, 'the source lists both projections');

sub restored
{
	my ($name, $label) = @_;
	my $dst = sift_node($name);
	$dst->start;
	return $dst;
}

sub check_restored
{
	my ($dst, $label) = @_;
	is(sql($dst, 'SELECT count(*) FROM sift.projection_defs'), '2', "$label: both definitions are restored");
	is(sql($dst, $LIST), $want, "$label: both projections are listed");
	sql($dst, "SELECT sift.maintain('h_sift'); SELECT sift.maintain('t')");
	is(sql($dst, "SELECT string_agg(name || '/' || state, ',' ORDER BY name) FROM sift.secondary_projections"), 'agg_g/ready,by_g/ready',
		"$label: both projections build from the restored definitions");
	is(sql($dst, 'SELECT count(*) FROM sift.check()'), '0', "$label: sift.check() finds no problem");
}

my $plain = $src->basedir . '/dump.sql';
$src->command_ok([ 'pg_dump', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $plain ], 'pg_dump');
{
	my $dst = restored('dump_psql');
	$dst->command_ok([ 'psql', '-X', '-q', '-1', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $plain ],
		'pg_dump | psql in one transaction has no error');
	check_restored($dst, 'psql');
	$dst->stop;
}

my $custom = $src->basedir . '/dump.fc';
$src->command_ok([ 'pg_dump', '-Fc', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $custom ], 'pg_dump -Fc');
{
	my $dst = restored('restore_j4');
	$dst->command_ok([ 'pg_restore', '-j', '4', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', $custom ], 'pg_restore -j 4 has no error');
	check_restored($dst, 'pg_restore -j 4');
	$src->stop;
	$dst->stop;
}

done_testing();
