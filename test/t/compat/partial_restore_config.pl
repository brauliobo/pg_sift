# A dump without one table (pg_dump --exclude-table) restores the TTL policies and the declared paths of the others: the rows of the table that is
# not there are skipped, as those of sift.table_option_values and sift.projection_defs are, and no COPY aborts.
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
for my $t (qw(t_keep t_drop))
{
	sql($src, qq{
		CREATE TABLE $t (id int, at timestamptz NOT NULL, doc jsonb) USING sift_table;
		INSERT INTO $t SELECT i, now(), jsonb_build_object('k', i) FROM generate_series(1, 100) i;
		SELECT sift.set_ttl('$t', 'at', interval '30 days');
		SELECT sift.table_set_paths('$t', 'doc', 'doc.k int4');
	});
}

my $LIST = q{SELECT (SELECT string_agg(relid::text, ',' ORDER BY relid::text) FROM sift.ttl_policies) || ' / ' ||
	(SELECT string_agg(relid::text || '.' || attname, ',' ORDER BY relid::text) FROM sift.table_paths)};
is(sql($src, $LIST), 't_drop,t_keep / t_drop.doc,t_keep.doc', 'the source holds the policies and the paths of both tables');

my $plain = $src->basedir . '/dump.sql';
$src->command_ok([ 'pg_dump', '--exclude-table=public.t_drop', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $plain ], 'pg_dump --exclude-table');

my $dst = sift_node('dst');
$dst->start;
$dst->command_ok([ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $plain ],
	'the restore of the dump without t_drop has no error');
is(sql($dst, $LIST), 't_keep / t_keep.doc', 'the policy and the paths of the table that is there are restored, those of the other are skipped');
$dst->stop;
$src->stop;
done_testing();
