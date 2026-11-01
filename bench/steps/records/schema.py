"""The record of one measured run: one row per statement x arm x temperature x repetition, fixed columns, TSV.

Every harness writes this record and nothing else is quoted as a number: a table in a document is generated from records (report.py) and a comparison of arms is accepted only
when check.py finds one dataset, one table state and one set of settings in it. The columns are the definitions: what a figure means is written once, here.

    ARM         the structure that answered, chosen by forcing it on the same data (heap: sequential scan, btree, gin, sift), with its option (`default`, `key_order=ts`, ...)
    DEFAULT     the plan the planner chooses with no setting forced (mode `default`); a forced arm has `picked` = 1 when its structure and option are the default plan's
    STRUCTURE   the structure the EXPLAIN of the run names (heap, btree, gin, sift, mixed): the arm must equal it
    TEMP        warm: the statement ran before in this session; cold: the shared buffers of its relations were evicted first (`os_cache_dropped` says whether the OS cache was too)
"""
import hashlib
import re
import time

STRUCTURES = ('heap', 'btree', 'gin', 'sift')
STATES = ('fresh_vacuumed', 'fresh_unvacuumed', 'churned_vacuumed', 'churned_unvacuumed')
ORDERS = ('arrival', 'sorted', 'hash', 'other')
KINDS = ('read', 'maintenance', 'load')
MAINTENANCE = ('default', 'disabled')
IDENTITY = ('data_directory', 'config_file', 'hba_file', 'ident_file', 'external_pid_file', 'port', 'unix_socket_directories', 'unix_socket_group', 'unix_socket_permissions', 'listen_addresses',
            'application_name', 'cluster_name', 'client_encoding', 'DateStyle', 'TimeZone', 'log_timezone', 'default_text_search_config', 'lc_messages', 'lc_monetary', 'lc_numeric', 'lc_time',
            'lc_collate', 'lc_ctype', 'log_directory', 'log_filename', 'logging_collector', 'log_line_prefix', 'log_destination', 'shared_preload_libraries', 'dynamic_library_path',
            'max_connections', 'IntervalStyle', 'extra_float_digits', 'server_encoding', 'bytea_output', 'client_min_messages', 'log_min_messages', 'statement_timeout', 'search_path')   # names, not behaviour
MODES = ('forced', 'default')
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
    ('structures', 's', True, 'the structures on the table, comma separated and sorted (btree,heap,sift): what was built, not what the arm used'),
    ('statement', 's', True, 'the name of the statement'),
    ('sql_hash', 's', True, 'hash of the statement text without its white space: one name is one text'),
    ('variant', 's', True, 'the table the arm ran on when a comparison has several (a heap in arrival order and one sorted by the key, an index built with other options): `main` when there is one; the planner picks within a variant'),
    ('kind', 's', True, 'read, maintenance (VACUUM, CREATE INDEX, merge) or load'),
    ('protocol', 's', True, 'what one repetition is: single, or the rounds it averages (rounds=12): a figure of one round is not the average of twelve'),
    ('mode', 's', True, 'forced (the arm was selected with settings) or default (the planner chose)'),
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
    ('workers_requested', 'i', True, 'parallel workers the plan asked for'),
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
    ('wal_bytes', 'i', False, 'WAL written by the run'),
    ('fpw', 'b', True, 'full_page_writes of the server'),
    ('cpus', 'f', True, 'the CPU quota the server ran with (cpus)'),
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
TYPE = {f[0]: f[1] for f in FIELDS}
REQUIRED = [f[0] for f in FIELDS if f[2]]
BLOCKS = [f[0] for f in FIELDS if f[0].endswith(('_hit', '_read'))]
LIBRARY = 'records-1'


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
        f.write('\t'.join(COLUMNS) + '\n')
        for r in records:
            unknown = set(r) - set(COLUMNS)
            if unknown:
                raise ValueError('unknown columns: %s' % ', '.join(sorted(unknown)))
            f.write('\t'.join(cell(c, r.get(c)) for c in COLUMNS) + '\n')


def read(path):
    """the records of a file as dicts (None for an empty cell), each with `_line`; a file whose header is not COLUMNS is refused"""
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        if head != COLUMNS:
            raise ValueError('%s: the header is not the record schema (%s)' % (path, LIBRARY))
        out = []
        for n, line in enumerate(f, 2):
            cells = line.rstrip('\n').split('\t')
            if len(cells) != len(COLUMNS):
                raise ValueError('%s:%d: %d cells, %d expected' % (path, n, len(cells), len(COLUMNS)))
            r = {c: parse(c, v) for c, v in zip(COLUMNS, cells)}
            r['_line'] = n
            r['_file'] = path
            out.append(r)
    return out


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
