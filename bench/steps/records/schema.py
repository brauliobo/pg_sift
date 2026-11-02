"""The record of one measured run: one row per statement x arm x temperature x repetition, fixed columns, TSV.

Every harness writes this record and nothing else is quoted as a number: a table in a document is generated from records (report.py) and a comparison of arms is accepted only
when check.py finds one dataset, one table state and one set of settings in it. The columns are the definitions: what a figure means is written once, here.

    ARM         the structure that answered, chosen by forcing it on the same data (heap: sequential scan, btree, gin, sift), with its option (`default`, `key_order=ts`, ...)
    DEFAULT     the plan the planner chooses with no setting forced (mode `default`); a forced arm has `picked` = 1 when its structure and option are the default plan's
    STRUCTURE   the structure the EXPLAIN of the run names (heap, btree, gin, sift, mixed): the arm must equal it
    TEMP        warm: the statement ran before in this session; cold: the shared buffers of its relations were evicted first (`os_cache_dropped` says whether the OS cache was too)
"""
import hashlib
import os
import re
import time

STRUCTURES = ('heap', 'btree', 'gin', 'sift')
STATES = ('fresh_vacuumed', 'fresh_unvacuumed', 'churned_vacuumed', 'churned_unvacuumed')
ORDERS = ('arrival', 'sorted', 'hash', 'other')
KINDS = ('read', 'maintenance', 'load', 'build')
WRITES = ('maintenance', 'load', 'build')   # the kinds that change the table: no plan to pick, one structure each
MAINTENANCE = ('default', 'disabled')
IDENTITY = ('data_directory', 'config_file', 'hba_file', 'ident_file', 'external_pid_file', 'port', 'unix_socket_directories', 'unix_socket_group', 'unix_socket_permissions', 'listen_addresses',
            'application_name', 'cluster_name', 'client_encoding', 'DateStyle', 'TimeZone', 'log_timezone', 'default_text_search_config', 'lc_messages', 'lc_monetary', 'lc_numeric', 'lc_time',
            'lc_collate', 'lc_ctype', 'log_directory', 'log_filename', 'logging_collector', 'log_line_prefix', 'log_destination', 'shared_preload_libraries', 'dynamic_library_path',
            'max_connections', 'IntervalStyle', 'extra_float_digits', 'server_encoding', 'bytea_output', 'client_min_messages', 'log_min_messages', 'statement_timeout', 'search_path')   # names, not behaviour
MODES = ('forced', 'default', 'unavailable')
TEMPS = ('warm', 'cold')
SCOPES = ('backend', 'cluster')

# (name, type, required, what it is). Types: s text, i integer, f number, b 0/1.
FIELDS = [
    ('run_id', 's', True, 'the run that wrote the row: tip, UTC time and four hex digits; every number in a document cites one'),
    ('tip', 's', True, 'the commit of the build measured (12 hex digits)'),
    ('harness', 's', True, 'the program that measured, with the version of its records library: rows of two harnesses are not compared'),
    ('dataset', 's', True, 'the name of the data (generator and parameters)'),
    ('rows', 'i', True, 'rows of the table'),
    ('keys', 'i', False, 'distinct values of the key column'),
    ('rows_per_key', 'f', False, 'rows / keys'),
    ('heap_order', 's', True, 'how the heap holds the key: arrival, sorted, hash or other'),
    ('heap_corr', 'f', False, 'the correlation of the key with the heap, measured (pg_stats), not the order the generator meant'),
    ('table_state', 's', True, 'fresh_vacuumed, fresh_unvacuumed, churned_vacuumed or churned_unvacuumed'),
    ('allvisible_pct', 'f', False, 'pages marked all-visible, in percent'),
    ('churn_history', 's', False, 'what was done to the table before the arms: upd=, ins=, del= (percent of the rows a round) and rounds= when the harness churned it with apply_churn, else the tuples inserted, updated and deleted since its statistics began (ins=, upd=, del=); empty for a fresh table'),
    ('vacuums', 'i', False, 'VACUUMs run on the table (pg_stat_user_tables vacuum_count) when the arms started'),
    ('autovacuums', 'i', False, 'autovacuums run on the table (autovacuum_count) when the arms started'),
    ('settled', 'b', False, '1 when the harness waited for the maintenance (VACUUM, the worker pass) to settle before the arms; needs a VACUUM or a pass on record'),
    ('settled_s', 'f', False, 'seconds from the last VACUUM, autovacuum or sift maintenance pass of the table to the start of the arms'),
    ('dead_pct', 'f', False, 'dead tuples in percent of live and dead (pg_stat_user_tables) at the start'),
    ('tombstone_pct', 'f', False, 'dead rows in percent of the rows of the segments of the sift index (sift.col_segments) at the start, when the table has one'),
    ('structures', 's', True, 'the structures on the table, comma separated and sorted (btree,heap,sift): what was built, not what the arm used; `constraint` stands for the indexes of a primary key or unique constraint, the table\'s definition, which a write pays for in every arm'),
    ('statement', 's', True, 'the name of the statement'),
    ('sql_hash', 's', True, 'hash of the statement text without its white space: one name is one text'),
    ('variant', 's', True, 'the table the arm ran on when a comparison has several (a heap in arrival order and one sorted by the key, an index built with other options): `main` when there is one; the planner picks within a variant'),
    ('kind', 's', True, 'read, maintenance (VACUUM, merge), load or build (CREATE INDEX of a sift index: the instructions of the leader and of the workers, the segments it wrote, the options it was built with)'),
    ('protocol', 's', True, 'what one repetition is: single, or the rounds it averages (rounds=12): a figure of one round is not the average of twelve'),
    ('mode', 's', True, 'forced (the arm was selected with settings), default (the planner chose) or unavailable (no setting makes the plan of this arm: one row without times, `structure` names what the plans were)'),
    ('arm', 's', True, 'heap, btree, gin or sift; `default` rows carry the structure the plan named'),
    ('arm_option', 's', True, 'the option of the structure: default, key_order=ts, key_cut, ...'),
    ('picked', 'b', True, '1 when this structure and option are the plan the planner chooses by default'),
    ('structure', 's', True, 'the structure the EXPLAIN of this run names: heap, btree, gin, sift or mixed'),
    ('temp', 's', True, 'warm or cold'),
    ('rep', 'i', True, 'the repetition, from 1'),
    ('est_cost', 'f', False, 'the planner total cost of the plan that ran'),
    ('plan_ms', 'f', False, 'planning time (EXPLAIN SUMMARY), apart from the execution'),
    ('exec_ms', 'f', False, 'execution time (EXPLAIN ANALYZE), planning not included'),
    ('wall_ms', 'f', True, 'the statement from the client: planning, execution and the transfer of the rows'),
    ('instr_m', 'f', False, 'millions of user instructions of the statement'),
    ('cycles_m', 'f', False, 'millions of user cycles'),
    ('instr_scope', 's', False, 'cluster (the postmaster tree: backend and parallel workers) or backend (the backend alone: only valid with no worker launched)'),
    ('instr_n', 'i', False, 'statements run in the window the instructions were counted over (a statement under 5 ms is run again and again in the one backend between two readings and the count divided: the number is of one run); 1 for a long one'),
    ('instr_leader_m', 'f', False, 'build: millions of user instructions of the leader backend alone, counted in a run of its own after the cluster count (the same statement on the same prepared table)'),
    ('instr_workers_m', 'f', False, 'build: instr_m - instr_leader_m, the instructions of the parallel workers (0 when none launched)'),
    ('workers_requested', 'i', True, 'parallel workers the plan asked for (a maintenance statement or a load has no plan: the workers it started)'),
    ('workers_launched', 'i', True, 'parallel workers that ran'),
    ('heap_hit', 'i', False, 'blocks of heap relations found in shared buffers'),
    ('heap_read', 'i', False, 'blocks of heap relations read'),
    ('btree_hit', 'i', False, 'b-tree index blocks found in shared buffers'),
    ('btree_read', 'i', False, 'b-tree index blocks read'),
    ('gin_hit', 'i', False, 'gin index blocks found in shared buffers'),
    ('gin_read', 'i', False, 'gin index blocks read'),
    ('sift_hit', 'i', False, 'sift index blocks found in shared buffers'),
    ('sift_read', 'i', False, 'sift index blocks read'),
    ('other_hit', 'i', False, 'blocks of other relations (toast, other index kinds) found in shared buffers'),
    ('other_read', 'i', False, 'blocks of other relations read'),
    ('os_cache_dropped', 'b', False, 'cold rows: 1 when the OS page cache was emptied too, 0 when only the shared buffers were'),
    ('table_bytes', 'i', False, 'pg_table_size of the table after the run (heap, TOAST and free space map; a load names the schema, the sum of its tables)'),
    ('index_bytes', 'i', False, 'pg_relation_size of the indexes of the arm\'s structure after the run (sift, b-tree or GIN; none for the heap arm): what a CREATE INDEX or a rewrite built'),
    ('segments', 'i', False, 'segments in the directory of the sift index of the arm after the run (all kinds: sift.col_segments, sift.segments); none for an arm without a sift index'),
    ('immutable_segments', 'i', False, 'of them the immutable ones (a build or merge wrote them)'),
    ('buffer_segments', 'i', False, 'of them the write buffers, open or sealed (rows the maintenance has not yet written as a segment)'),
    ('segments_declared', 'i', False, 'the segment count the harness says the index has (a case that names it, `-- segments: N`): a row whose count is another is refused'),
    ('index_options', 's', False, 'the options the sift index of the arm was built with (its reloptions, name=value, sorted, `;` between), `default` when it has none'),
    ('wal_bytes', 'i', False, 'WAL written by the run'),
    ('fpw', 'b', True, 'full_page_writes of the server'),
    ('cpus', 'f', True, 'the CPU quota the server ran with (cpus)'),
    ('noise', 'b', False, '1 when the host CPU or IO pressure was above 10 points during the run: a flag, not a refusal (the load of the host is a fact about the run)'),
    ('psi_cpu_start', 'f', False, 'host CPU pressure (some, avg10) at the start of the run'),
    ('psi_cpu_end', 'f', False, 'host CPU pressure at its end'),
    ('psi_io_start', 'f', False, 'host IO pressure at the start'),
    ('psi_io_end', 'f', False, 'host IO pressure at its end'),
    ('shared_buffers_mb', 'f', True, 'shared_buffers'),
    ('work_mem_mb', 'f', True, 'work_mem'),
    ('pg_version', 's', True, 'the server version'),
    ('settings', 's', False, 'every setting that is not the default (name=value, sorted, `;` between), the cluster and the session before the arm forced anything'),
    ('settings_hash', 's', True, 'ten hex digits of `settings`: two rows compared have the same one'),
    ('maintenance', 's', True, 'default, or disabled when a switch that turns the maintenance off is set (autovacuum, sift.col_auto_merge, sift.async_maintenance, a sift.test_* setting)'),
]
COLUMNS = [f[0] for f in FIELDS]
from history import HISTORY  # noqa: E402  (the columns of every version, frozen)
TYPE = {f[0]: f[1] for f in FIELDS}
REQUIRED = [f[0] for f in FIELDS if f[2]]
BLOCKS = [f[0] for f in FIELDS if f[0].endswith(('_hit', '_read'))]
LIBRARY = 'records-1'
VERSION = 6   # the schema of the file: history.py keeps the columns of each version, MIGRATIONS the step from it to the next
MARK = '#records-schema '


def sql_hash(sql):
    """12 hex digits of the statement with its white space collapsed: two spellings of one text are one statement"""
    return hashlib.sha1(re.sub(r'\s+', ' ', sql.strip().rstrip(';')).encode()).hexdigest()[:12]


def run_id(tip, now=None):
    return '%s-%s-%s' % (tip[:12], time.strftime('%Y%m%dT%H%M%SZ', time.gmtime(now)), hashlib.sha1(repr((time.time_ns(), id(tip))).encode()).hexdigest()[:4])


def cell(name, value):
    if value is None or value == '':
        return ''
    t = TYPE[name]
    if t == 'b':
        return '1' if value in (1, True, '1') else '0'
    if t == 'i':
        return str(int(round(float(value))))
    if t == 'f':
        return '%.6g' % float(value)
    return str(value).replace('\t', ' ').replace('\n', ' ')


def parse(name, text):
    """the value of a TSV cell: None where empty; a ValueError names the column"""
    if text == '':
        return None
    t = TYPE[name]
    try:
        return int(text) if t in 'ib' else float(text) if t == 'f' else text
    except ValueError:
        raise ValueError('%s: "%s" is not a number' % (name, text))


def write(path, records):
    with open(path, 'w') as f:
        f.write('%s%d\n' % (MARK, VERSION))
        f.write('\t'.join(COLUMNS) + '\n')
        for r in records:
            unknown = set(r) - set(COLUMNS)
            if unknown:
                raise ValueError('unknown columns: %s' % ', '.join(sorted(unknown)))
            f.write('\t'.join(cell(c, r.get(c)) for c in COLUMNS) + '\n')


# The step from a version to the next, on the row of that version: a column that was added gets its value, from what the row says or as "not recorded". A change of FIELDS is a new VERSION,
# the columns of the new one in history.py, a step here, a frozen sample in bench/test/records_samples (test_records.py fails until all three exist).
def _from_1(r):
    r['variant'] = 'main'


def _from_2(r):
    if r['table_state'].startswith('churned') and r['kind'] == 'read':
        r['churn_history'] = 'not recorded (schema 2)'


def _from_3(r):
    r['noise'] = int(max(r.get(c) or 0 for c in ('psi_cpu_start', 'psi_cpu_end', 'psi_io_start', 'psi_io_end')) > 10.0)


def _from_4(r):
    r['instr_n'] = 1 if r.get('instr_m') is not None else None


def _from_5(r):
    """the segments, the leader/worker split and the options were not recorded: they stay empty (a count cannot be rebuilt from a row), which the checker accepts for a row of schema 5"""


MIGRATIONS = {1: _from_1, 2: _from_2, 3: _from_3, 4: _from_4, 5: _from_5}


def header_of(path):
    """(schema version, line number of the first record) of a records file: the mark of its first line, else the version whose columns are its header (files before the mark)"""
    with open(path) as f:
        first = f.readline().rstrip('\n')
        if first.startswith(MARK):
            version, head, start = int(first[len(MARK):]), f.readline().rstrip('\n').split('\t'), 3
        else:
            version, head, start = None, first.split('\t'), 2
    if version is None:
        version = next((v for v, cols in HISTORY.items() if cols == head), None)
    if version not in HISTORY or HISTORY[version] != head:
        raise ValueError('%s: the header is none of the %d schema versions of the records library (current %d): not a records file or from a newer library' % (path, len(HISTORY), VERSION))
    return version, start


def upgrade(row, version):
    """ROW of schema VERSION as a row of the current schema"""
    for v in range(version, VERSION):
        MIGRATIONS[v](row)
    for c in COLUMNS:
        row.setdefault(c, None)
    return row


def read(path):
    """the records of a file as dicts (None for an empty cell), each with `_line`, `_file` and `_schema` (the version it was written with), upgraded in memory to the current schema;
    `migrate` writes the file again in it"""
    version, start = header_of(path)
    cols = HISTORY[version]
    out = []
    with open(path) as f:
        for _ in range(start - 1):
            f.readline()
        for n, line in enumerate(f, start):
            cells = line.rstrip('\n').split('\t')
            if len(cells) != len(cols):
                raise ValueError('%s:%d: %d cells, %d expected' % (path, n, len(cells), len(cols)))
            r = upgrade({c: parse(c, v) for c, v in zip(cols, cells)}, version)
            r['_line'], r['_file'], r['_schema'] = n, path, version
            out.append(r)
    return out


def read_files(paths, say=None):
    """the records of FILES; SAY (a function of a line) is told of each file that is of an older schema and was upgraded in memory"""
    out = []
    for f in paths:
        rs = read(f)
        if rs and rs[0]['_schema'] != VERSION and say:
            say('%s is schema %d, read as schema %d: `bench records migrate %s` writes it again in the current one' % (f, rs[0]['_schema'], VERSION, f))
        out += rs
    return out


def migrate(path):
    """write PATH again in the current schema (the old file stays next to it as PATH.schemaN) and return its old version; a file in the current one is left alone (None)"""
    version, _ = header_of(path)
    if version == VERSION:
        return None
    rows = read(path)
    os.replace(path, '%s.schema%d' % (path, version))
    write(path, [{k: v for k, v in r.items() if not k.startswith('_')} for r in rows])
    return version


def doc_table():
    """the table of the columns for the documentation, generated from FIELDS"""
    lines = ['| column | type | required | meaning |', '|---|---|---|---|']
    names = {'s': 'text', 'i': 'integer', 'f': 'number', 'b': '0 or 1'}
    for name, t, req, doc in FIELDS:
        lines.append('| `%s` | %s | %s | %s |' % (name, names[t], 'yes' if req else '', doc))
    return '\n'.join(lines)


if __name__ == '__main__':
    print(doc_table())


def settings_text(pairs):
    """name=value;... of the settings that are not the default, without the names that only identify the server (IDENTITY), sorted"""
    return ';'.join('%s=%s' % (k, v) for k, v in sorted(pairs.items()) if k not in IDENTITY)


def settings_hash(pairs):
    return hashlib.sha1(settings_text(pairs).encode()).hexdigest()[:10]


def maintenance_switches(pairs):
    """the settings that turn the maintenance off: autovacuum = off, a sift setting of automatic or asynchronous maintenance off, any sift.test_* setting. A state measured with one of them
    (dead rows no VACUUM took, segments no merge joined) may not exist with the defaults"""
    out = []
    for k, v in sorted(pairs.items()):
        low = str(v).lower()
        if k.startswith('sift.test_') or (k == 'autovacuum' and low in ('off', 'false', '0')) or (k.startswith('sift.') and re.search(r'auto|async|maint', k) and low in ('off', 'false', '0')):
            out.append('%s=%s' % (k, v))
    return out
