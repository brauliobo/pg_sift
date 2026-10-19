# sift_table settings (sift.table_*) are plain server settings, read by the backend that does the work. Which setting a segment is written
# under: the writer's session (SET, SET LOCAL, role or database default) for inline promotion, the session that runs VACUUM, and for
# autovacuum the server configuration and the database defaults, never the owner's role or the writer's session.
# The compression written is read back from the size of the column segments (sift.table_segments): the rows are one compressible text.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $inline = 12000;		# rows that close several write buffers, promoted while they are inserted
my $settled = 6000;		# rows that stay in the write buffer (sift.table_inline_promote = off) until a VACUUM
my $none = "SET sift.table_compression = 'none';";
my $keep = 'SET sift.table_inline_promote = off;';

my $node = sift_node('scope');
$node->append_conf('postgresql.conf', "autovacuum_naptime = 1s\nlog_autovacuum_min_duration = 0\nautovacuum_vacuum_insert_threshold = 500\nautovacuum_vacuum_insert_scale_factor = 0\n"
	. "autovacuum_vacuum_threshold = 500\nautovacuum_vacuum_scale_factor = 0");
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE ROLE writer LOGIN; CREATE ROLE owner_none LOGIN; GRANT ALL ON SCHEMA public TO PUBLIC');
my $boot = $node->safe_psql('postgres', 'SELECT rolname FROM pg_roles WHERE oid = 10');

# autovacuum stays off for a table until a test hands it over (ALTER TABLE ... SET (autovacuum_enabled = true))
sub create { my ($t) = @_; return "CREATE TABLE $t (id int, note text) USING sift_table WITH (autovacuum_enabled = false);\n" }
# batches of 2000 rows, one transaction each: a write buffer is promoted once its rows are settled, by the insert after them
sub load { my ($t, $n) = @_; return join('', map { my $lo = $_ * 2000 + 1; "INSERT INTO $t SELECT g, repeat('compressible payload ', 30) || g FROM generate_series($lo, " . ($lo + 1999) . ") g;\n" } 0 .. $n / 2000 - 1) }
sub bytes { my ($t) = @_; return $node->safe_psql('postgres', "SELECT coalesce(sum(bytes), 0) FROM sift.table_segments('$t') WHERE kind = 'columns'") }
sub promoted { my ($t) = @_; return $node->poll_query_until('postgres', "SELECT count(*) > 0 FROM sift.table_segments('$t') WHERE kind = 'columns'") }
# 'plain' when written uncompressed, 'auto' when like the default policy
sub policy { my ($t, $auto) = @_; my $b = bytes($t); return $b > 2 * $auto ? 'plain' : $b > $auto / 2 && $b < 1.5 * $auto ? 'auto' : "other ($b against $auto)" }

# the default policy (auto), for the sizes below
$node->safe_psql('postgres', create('base') . load('base', $inline));
my $auto = bytes('base');
cmp_ok($auto, '>', 0, 'inline promotion wrote column segments');

subtest 'inline promotion in the writer: its own session settings, however set' => sub {
	$node->safe_psql('postgres', $none . create('i_set') . load('i_set', $inline));
	is(policy('i_set', $auto), 'plain', 'SET in the session');
	# the rows are settled when the transaction that sets the value begins: it promotes what earlier transactions closed
	$node->safe_psql('postgres', $keep . create('i_local') . load('i_local', $inline - 2000));
	$node->safe_psql('postgres', "BEGIN; SET LOCAL sift.table_compression = 'none';" . load('i_local', 2000) . 'COMMIT;');
	is(policy('i_local', $auto), 'plain', 'SET LOCAL');
	$node->safe_psql('postgres', 'ALTER ROLE writer SET sift.table_compression = $$none$$');
	$node->safe_psql('postgres', create('i_role') . load('i_role', $inline), extra_params => [ '-U', 'writer' ]);
	is(policy('i_role', $auto), 'plain', 'default of the writer role');
	$node->safe_psql('postgres', create('i_role_other') . load('i_role_other', $inline));
	is(policy('i_role_other', $auto), 'auto', 'another role of the same database is not affected');
	$node->safe_psql('postgres', 'ALTER ROLE writer RESET sift.table_compression; ALTER DATABASE postgres SET sift.table_compression = $$none$$');
	$node->safe_psql('postgres', create('i_db') . load('i_db', $inline));
	is(policy('i_db', $auto), 'plain', 'default of the database');
	$node->safe_psql('postgres', 'ALTER DATABASE postgres RESET sift.table_compression');
	$node->safe_psql('postgres', "$none " . create('i_two') . load('i_two', $inline));
	is(policy('i_two', $auto), 'plain', 'two tables of one database, the other written under the default stays auto');
	is(policy('base', $auto), 'auto', 'the table written under the default');
};

subtest 'a manual VACUUM promotes under the settings of the session that runs it' => sub {
	$node->safe_psql('postgres', $keep . create('v_none') . load('v_none', $settled));
	$node->safe_psql('postgres', $none . ' VACUUM v_none');
	is(policy('v_none', $auto), 'plain', 'VACUUM under SET compression = none');
	# the settings of the session that wrote the rows are not kept for it
	$node->safe_psql('postgres', "$keep $none" . create('v_late') . load('v_late', $settled));
	$node->safe_psql('postgres', 'VACUUM v_late');
	is(policy('v_late', $auto), 'auto', 'rows written under none, VACUUM under the default: the default');
	$node->safe_psql('postgres', 'ALTER ROLE writer SET sift.table_compression = $$none$$');
	$node->safe_psql('postgres', $keep . create('v_role') . load('v_role', $settled), extra_params => [ '-U', 'writer' ]);
	$node->safe_psql('postgres', 'VACUUM v_role', extra_params => [ '-U', 'writer' ]);
	is(policy('v_role', $auto), 'plain', 'VACUUM by a role with the setting as its default');
	$node->safe_psql('postgres', 'ALTER ROLE writer RESET sift.table_compression');
};

# autovacuum: a worker has no session of anyone's; each table below is loaded with promotion off and autovacuum off, then handed to autovacuum
sub background
{
	my ($t, $load_prefix, $owner) = @_;
	my @as = $owner ? (extra_params => [ '-U', $owner ]) : ();
	$node->safe_psql('postgres', $keep . ($load_prefix // '') . create($t) . load($t, $settled), @as);
	# a VACUUM seals only the rows older than every snapshot: the ANALYZE of the worker that sealed the previous table still holds one, and the
	# only autovacuum of this table would find its rows unsettled and leave them in the write buffer for good
	$node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'");
	$node->safe_psql('postgres', "ALTER TABLE $t SET (autovacuum_enabled = true)");
	ok(promoted($t), "autovacuum promoted $t") or diag($node->safe_psql('postgres', "SELECT row_to_json(s) FROM pg_stat_user_tables s WHERE relname = '$t'") . ' ' .
		$node->safe_psql('postgres', "SELECT string_agg(pid || ':' || coalesce(wait_event, '-') || ':' || state, ' ') FROM pg_stat_activity WHERE backend_type LIKE 'autovacuum%'"));
	return policy($t, $auto);
}

subtest 'autovacuum does not see the writer session or the role of the table owner' => sub {
	$node->safe_psql('postgres', 'ALTER ROLE owner_none SET sift.table_compression = $$none$$');
	is(background('a_session', $none), 'auto', 'rows written under SET compression = none');
	is(background('a_owner', undef, 'owner_none'), 'auto', 'table owned by a role whose default is none');
};

subtest 'autovacuum sees the database and server settings' => sub {
	$node->safe_psql('postgres', 'ALTER DATABASE postgres SET sift.table_compression = $$none$$');
	is(background('a_db'), 'plain', 'ALTER DATABASE ... SET');
	$node->safe_psql('postgres', 'ALTER DATABASE postgres RESET sift.table_compression');
	$node->safe_psql('postgres', 'ALTER SYSTEM SET sift.table_compression = $$none$$; SELECT pg_reload_conf()');
	$node->poll_query_until('postgres', 'SELECT current_setting($$sift.table_compression$$) = $$none$$');
	is(background('a_server'), 'plain', 'postgresql.conf / ALTER SYSTEM');
	$node->safe_psql('postgres', 'ALTER DATABASE postgres SET sift.table_compression = $$auto$$');
	is(background('a_db_over'), 'auto', 'the database setting wins over the server setting');
	$node->safe_psql('postgres', 'ALTER DATABASE postgres RESET sift.table_compression; ALTER SYSTEM RESET sift.table_compression; SELECT pg_reload_conf()');
};

subtest 'the role of the bootstrap superuser' => sub {
	$node->safe_psql('postgres', qq(ALTER ROLE "$boot" SET sift.table_compression = \$\$none\$\$));
	my $seen = background('a_boot');
	diag("autovacuum under the bootstrap superuser's role default: $seen");
	is($seen, 'plain', 'autovacuum runs as the bootstrap superuser: its role defaults apply');
	$node->safe_psql('postgres', qq(ALTER ROLE "$boot" RESET sift.table_compression));
};

$node->stop;
done_testing();
