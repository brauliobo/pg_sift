# sift_table options (sift.table_set_option) are read by the backend that does the work. Which option a segment is written under: the option
# of the table, whoever writes or runs VACUUM, for inline promotion, for the session that runs VACUUM and for autovacuum alike; a table that
# sets none is written under the default, and so is every other table of the database.
# The compression written is read back from the size of the column segments (sift.table_segments): the rows are one compressible text.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $inline = 12000;		# rows that close several write buffers, promoted while they are inserted
my $settled = 6000;		# rows that stay in the write buffer (inline_promote = off) until a VACUUM

my $node = sift_node('scope');
$node->append_conf('postgresql.conf', "autovacuum_naptime = 1s\nlog_autovacuum_min_duration = 0\nautovacuum_vacuum_insert_threshold = 500\nautovacuum_vacuum_insert_scale_factor = 0\n"
	. "autovacuum_vacuum_threshold = 500\nautovacuum_vacuum_scale_factor = 0");
$node->start;
install_sift($node);
$node->safe_psql('postgres', 'CREATE ROLE writer LOGIN; GRANT ALL ON SCHEMA public TO PUBLIC');

# autovacuum stays off for a table until a test hands it over (ALTER TABLE ... SET (autovacuum_enabled = true)); the options of the table follow its creation
sub create
{
	my ($t, %opt) = @_;
	return "CREATE TABLE $t (id int, note text) USING sift_table WITH (autovacuum_enabled = false);\n"
		. join('', map { "SELECT sift.table_set_option('$t', '$_', '$opt{$_}');\n" } sort keys %opt);
}
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

subtest 'inline promotion in the writer: the option of the table, whoever writes' => sub {
	$node->safe_psql('postgres', create('i_opt', compression => 'none') . load('i_opt', $inline));
	is(policy('i_opt', $auto), 'plain', 'the option of the table');
	$node->safe_psql('postgres', create('i_role', compression => 'none') . load('i_role', $inline), extra_params => [ '-U', 'writer' ]);
	is(policy('i_role', $auto), 'plain', 'the option applies to a role that is not the owner');
	$node->safe_psql('postgres', create('i_other') . load('i_other', $inline));
	is(policy('i_other', $auto), 'auto', 'another table of the same database is not affected');
	$node->safe_psql('postgres', create('i_reset', compression => 'none') . "SELECT sift.table_reset_option('i_reset', 'compression');\n" . load('i_reset', $inline));
	is(policy('i_reset', $auto), 'auto', 'a table that took the option off is written under the default');
	is(policy('base', $auto), 'auto', 'the table written under the default');
};

subtest 'a manual VACUUM promotes under the option of the table' => sub {
	$node->safe_psql('postgres', create('v_none', inline_promote => 'off', compression => 'none') . load('v_none', $settled));
	$node->safe_psql('postgres', 'VACUUM v_none');
	is(policy('v_none', $auto), 'plain', 'VACUUM of a table with compression = none');
	# the option of the table when the rows were written is not kept for them: VACUUM reads the option it finds
	$node->safe_psql('postgres', create('v_late', inline_promote => 'off', compression => 'none') . load('v_late', $settled));
	$node->safe_psql('postgres', "SELECT sift.table_reset_option('v_late', 'compression'); VACUUM v_late");
	is(policy('v_late', $auto), 'auto', 'rows written under none, VACUUM after the option was taken off: the default');
	$node->safe_psql('postgres', create('v_role', inline_promote => 'off', compression => 'none') . load('v_role', $settled), extra_params => [ '-U', 'writer' ]);
	$node->safe_psql('postgres', 'VACUUM v_role', extra_params => [ '-U', 'writer' ]);
	is(policy('v_role', $auto), 'plain', 'VACUUM by a role that does not own the table');
};

# autovacuum: a worker has no session of anyone's; each table below is loaded with promotion off and autovacuum off, then handed to autovacuum
sub background
{
	my ($t, %opt) = @_;
	$node->safe_psql('postgres', create($t, inline_promote => 'off', %opt) . load($t, $settled));
	# a VACUUM seals only the rows older than every snapshot: the ANALYZE of the worker that sealed the previous table still holds one, and the
	# only autovacuum of this table would find its rows unsettled and leave them in the write buffer for good
	$node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'");
	$node->safe_psql('postgres', "ALTER TABLE $t SET (autovacuum_enabled = true)");
	ok(promoted($t), "autovacuum promoted $t") or diag($node->safe_psql('postgres', "SELECT row_to_json(s) FROM pg_stat_user_tables s WHERE relname = '$t'") . ' ' .
		$node->safe_psql('postgres', "SELECT string_agg(pid || ':' || coalesce(wait_event, '-') || ':' || state, ' ') FROM pg_stat_activity WHERE backend_type LIKE 'autovacuum%'"));
	return policy($t, $auto);
}

subtest 'autovacuum reads the option of the table' => sub {
	is(background('a_none', compression => 'none'), 'plain', 'compression = none');
	is(background('a_auto', compression => 'auto'), 'auto', 'compression = auto');
	is(background('a_default'), 'auto', 'no option');
};

$node->stop;
done_testing();
