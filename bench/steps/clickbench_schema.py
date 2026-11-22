"""The canonical 105-column ClickBench `hits` schema and everything derived from it: the DDL of every arm, the projections that pg_sift and
ClickHouse share, and the deterministic synthetic generator (bench/sql/clickbench/gen.sql). One definition; `clickbench.py emit-sql` writes the files."""

COLUMNS = [(c, k) for k, names in [
    ('i8', 'WatchID'), ('i2', 'JavaEnable'), ('text', 'Title'), ('i2', 'GoodEvent'), ('ts', 'EventTime'), ('date', 'EventDate'),
    ('i4', 'CounterID ClientIP RegionID'), ('i8', 'UserID'), ('i2', 'CounterClass OS UserAgent'), ('text', 'URL Referer'),
    ('i2', 'IsRefresh RefererCategoryID'), ('i4', 'RefererRegionID'), ('i2', 'URLCategoryID'), ('i4', 'URLRegionID'),
    ('i2', 'ResolutionWidth ResolutionHeight ResolutionDepth FlashMajor FlashMinor'), ('text', 'FlashMinor2'),
    ('i2', 'NetMajor NetMinor UserAgentMajor'), ('vc', 'UserAgentMinor'), ('i2', 'CookieEnable JavascriptEnable IsMobile MobilePhone'),
    ('text', 'MobilePhoneModel Params'), ('i4', 'IPNetworkID'), ('i2', 'TraficSourceID SearchEngineID'), ('text', 'SearchPhrase'),
    ('i2', 'AdvEngineID IsArtifical WindowClientWidth WindowClientHeight ClientTimeZone'), ('ts', 'ClientEventTime'),
    ('i2', 'SilverlightVersion1 SilverlightVersion2'), ('i4', 'SilverlightVersion3'), ('i2', 'SilverlightVersion4'),
    ('text', 'PageCharset'), ('i4', 'CodeVersion'), ('i2', 'IsLink IsDownload IsNotBounce'), ('i8', 'FUniqID'), ('text', 'OriginalURL'),
    ('i4', 'HID'), ('i2', 'IsOldCounter IsEvent IsParameter DontCountHits WithHash'), ('char', 'HitColor'), ('ts', 'LocalEventTime'),
    ('i2', 'Age Sex Income Interests Robotness'), ('i4', 'RemoteIP WindowName OpenerName'), ('i2', 'HistoryLength'),
    ('text', 'BrowserLanguage BrowserCountry SocialNetwork SocialAction'), ('i2', 'HTTPError'),
    ('i4', 'SendTiming DNSTiming ConnectTiming ResponseStartTiming ResponseEndTiming FetchTiming'), ('i2', 'SocialSourceNetworkID'),
    ('text', 'SocialSourcePage'), ('i8', 'ParamPrice'), ('text', 'ParamOrderID ParamCurrency'), ('i2', 'ParamCurrencyID'),
    ('text', 'OpenstatServiceName OpenstatCampaignID OpenstatAdID OpenstatSourceID UTMSource UTMMedium UTMCampaign UTMContent UTMTerm FromTag'),
    ('i2', 'HasGCLID'), ('i8', 'RefererHash URLHash'), ('i4', 'CLID')] for c in names.split()]

assert len(COLUMNS) == 105 and len({c for c, _ in COLUMNS}) == 105

TYPES = {
    'pg':     dict(i2='SMALLINT', i4='INTEGER', i8='BIGINT', text='TEXT', vc='VARCHAR(255)', char='CHAR', ts='TIMESTAMP', date='DATE'),
    'ch':     dict(i2='Int16', i4='Int32', i8='Int64', text='String', vc='String', char='String', ts='DateTime', date='Date'),
    'duckdb': dict(i2='SMALLINT', i4='INTEGER', i8='BIGINT', text='VARCHAR', vc='VARCHAR', char='VARCHAR', ts='TIMESTAMP', date='DATE'),
}

SORT_KEY = ['CounterID', 'EventDate', 'UserID', 'EventTime', 'WatchID']
SKIP_TRIGRAM = ['url', 'title']
KEY_ORDER = ', '.join(SORT_KEY).lower()
SKIP_INDEX = ', '.join('%s:trigram' % c for c in SKIP_TRIGRAM)

PROJECTIONS = [
    dict(name='p_by_adv',         kind='agg',  keys=['AdvEngineID'],            measures=['count(*)']),
    dict(name='p_by_region',      kind='agg',  keys=['RegionID'],               measures=['count(*)', 'sum(AdvEngineID)', 'sum(ResolutionWidth)']),
    dict(name='p_by_counter_day', kind='agg',  keys=['CounterID', 'EventDate'], measures=['count(*)']),
    dict(name='p_by_user',        kind='sort', order=['UserID', 'EventTime'],   include=['SearchPhrase']),
]


def columns_ddl(dialect, quote=False):
    t = TYPES[dialect]
    return ',\n'.join('  %s %s' % ('"%s"' % c if quote else c, t[k] + (' NOT NULL' if dialect != 'ch' else '')) for c, k in COLUMNS)


def pg_ddl():
    return 'CREATE TABLE hits (\n%s\n);' % columns_ddl('pg')


def ch_table_ddl(table='hits'):
    return 'CREATE TABLE %s (\n%s\n) ENGINE = MergeTree ORDER BY (%s);' % (table, columns_ddl('ch'), ', '.join(SORT_KEY))


def duckdb_ddl():
    return 'CREATE TABLE hits (\n%s\n);' % columns_ddl('duckdb')


def sift_projection_spec(p):
    if p['kind'] == 'agg':
        return 'GROUP BY %s MEASURES %s' % (', '.join(p['keys']), ', '.join(p['measures']))
    return 'ORDER BY %s INCLUDE (%s)' % (', '.join(p['order']), ', '.join(p['include']))


def sift_projection_sql(p, relation):
    return "SELECT sift.create_projection('%s', '%s', '%s');" % (relation, p['name'], sift_projection_spec(p))


def ch_projection_sql(p, table='hits_proj'):
    if p['kind'] == 'agg':
        cols = ', '.join(p['keys'] + [m.replace('(*)', '()') for m in p['measures']])
        body = 'SELECT %s GROUP BY %s' % (cols, ', '.join(p['keys']))
    else:
        body = 'SELECT %s ORDER BY %s' % (', '.join(p['order'] + p['include']), ', '.join(p['order']))
    return 'ALTER TABLE %s ADD PROJECTION %s (%s);\nALTER TABLE %s MATERIALIZE PROJECTION %s SETTINGS mutations_sync = 2;' % (
        table, p['name'], body, table, p['name'])


U = lambda k: 'pg_temp.u(g, %d)' % k
PICK = lambda k, values, n=None: '(ARRAY[%s])[1 + floor(%s * %d)::int]' % (', '.join(values), U(k), n or len(values))
HOST = ["'www.google.com'", "'google.com'", "'maps.google.ru'", "'example.com'", "'news.example'", "'shop.example'", "'forum.example'", "'blog.example'"]
SITE = ["'example.com'", "'www.example.com'", "'news.example'", "'www.google.com'"]
TIME = "date_trunc('second', timestamp '2013-07-01' + (g::float8 / :n) * 31 * interval '1 day')"

GEN = dict(
    WatchID='g::bigint',
    Title="CASE WHEN %s < 0.1 THEN '' WHEN %s < 0.2 THEN 'Google Search ' WHEN %s < 0.25 THEN 'Café ' ELSE 'Page ' END || floor(%s * 5000)::int" % (
        U(20), U(20), U(20), U(21)),
    EventTime=TIME,
    EventDate='(%s)::date' % TIME,
    CounterID='CASE WHEN %s < 0.12 THEN 62 ELSE 1 + floor(power(%s, 2) * 30000)::int END' % (U(1), U(2)),
    ClientIP='1 + (hashint8extended(g, 2) & 1073741823)::int',
    RegionID='1 + floor(power(%s, 3) * 9000)::int' % U(3),
    UserID='CASE WHEN %s < 0.0002 THEN 435090932899640449 ELSE 1000003 * (1 + floor(greatest(:n / 10, 100) * power(%s, 2.2))::bigint) + 17 END' % (
        U(4), U(5)),
    OS='(%s * 30)::int2' % U(6),
    UserAgent='(%s * 20)::int2' % U(7),
    URL="'http://' || %s || '/' || floor(%s * 100000)::int" % (PICK(8, HOST), U(9)),
    Referer="CASE WHEN %s < 0.5 THEN '' ELSE (ARRAY['http://', 'https://'])[1 + floor(%s * 2)::int] || %s || '/r' || floor(%s * 1000)::int END" % (
        U(10), U(11), PICK(12, SITE), U(13)),
    IsRefresh='(%s < 0.1)::int::int2' % U(14),
    ResolutionWidth='%s::int2' % PICK(15, ['1024', '1280', '1366', '1440', '1600', '1920', '2560']),
    MobilePhone='CASE WHEN %s < 0.9 THEN 0 ELSE 1 + floor(%s * 40)::int END::int2' % (U(16), U(17)),
    MobilePhoneModel="CASE WHEN %s < 0.9 THEN '' ELSE 'model' || floor(%s * 200)::int END" % (U(16), U(18)),
    TraficSourceID='%s::int2' % PICK(19, ['-1', '1', '2', '3', '4', '5', '6', '7']),
    SearchEngineID='CASE WHEN %s < 0.9 THEN 0 ELSE 1 + floor(%s * 25)::int END::int2' % (U(22), U(23)),
    SearchPhrase="CASE WHEN %s < 0.9 THEN '' WHEN %s < 0.05 THEN 'ключ ' || floor(%s * 100)::int ELSE 'phrase ' || floor(power(%s, 3) * 50000)::int END" % (
        U(22), U(24), U(25), U(26)),
    AdvEngineID='CASE WHEN %s < 0.9 THEN 0 ELSE 1 + floor(%s * 20)::int END::int2' % (U(27), U(28)),
    WindowClientWidth='%s::int2' % PICK(29, ['0', '1024', '1280', '1366', '1920']),
    WindowClientHeight='%s::int2' % PICK(30, ['0', '600', '768', '1080']),
    ClientEventTime='%s - interval \'60 seconds\'' % TIME,
    LocalEventTime='%s + interval \'3600 seconds\'' % TIME,
    IsLink='(%s < 0.2)::int::int2' % U(31),
    IsDownload='(%s < 0.03)::int::int2' % U(32),
    DontCountHits='(%s < 0.05)::int::int2' % U(33),
    RefererHash='CASE WHEN %s < 0.01 THEN 3594120000172545465 ELSE hashint8extended(g %% 997, 7) END' % U(34),
    URLHash='CASE WHEN %s < 0.01 THEN 2868770270353813622 ELSE hashint8extended(g %% 9973, 8) END' % U(35),
)

GEN_DEFAULT = dict(i2='0::int2', i4='0', i8='0::bigint', text="''", vc="''", char="'0'", ts="timestamp '2013-07-01'", date="date '2013-07-01'")


def gen_sql():
    exprs = ',\n'.join('  %s AS %s' % (GEN.get(c, GEN_DEFAULT[k]), c) for c, k in COLUMNS)
    return ('-- ClickBench-shaped synthetic `hits`: 105 canonical columns, deterministic (every draw is a hash of the row number), skewed so that every\n'
            '-- query of Q0-Q42 returns rows at 10M. Writes CSV to stdout:   psql -X -qAt -v n=10000000 -f gen.sql > hits.csv\n'
            '\\set ON_ERROR_STOP on\n'
            'CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE\n'
            '  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;\n'
            'COPY (SELECT\n%s\nFROM generate_series(1, :n) g) TO STDOUT WITH (FORMAT csv);\n') % exprs


def sift_table_sql():
    return ('-- pg_sift `sift_table` (Format 2.0): the table itself in column format, ordered by the ClickBench sort key, trigram skip indexes for\n'
            '-- the LIKE filters. The options are set before the load (PostgreSQL rejects reloptions on table access methods).\n'
            '--@ table\nCREATE EXTENSION IF NOT EXISTS pg_sift;\n%s\n'
            "SELECT sift.table_set_option('hits', 'key_order', '%s');\n"
            "SELECT sift.table_set_option('hits', 'skip_index', '%s');\n"
            '--@ after\nVACUUM (FREEZE, ANALYZE) hits;\n%s') % (
        pg_ddl().replace(');', ') USING sift_table;'), KEY_ORDER, SKIP_INDEX, projections_section('hits'))


def projections_section(relation):
    return ('--@ projections\n-- run only when the build has the projection functions (SIFT_PT_PROJDIR); arm ch_proj then gets the same list\n%s\n' %
            '\n'.join(sift_projection_sql(p, relation) for p in PROJECTIONS))


def parquet_fixture_sql(csv, out):
    """DuckDB statement that turns the CSV of gen.sql into a file shaped like the real hits.parquet: dates as days (UINT16), times as epoch seconds, strings as BYTE_ARRAY"""
    cols = ', '.join("'%s': '%s'" % (c, TYPES['duckdb'][k]) for c, k in COLUMNS)
    blobs = ', '.join('encode(%s) AS %s' % (c, c) for c, k in COLUMNS if k in ('text', 'vc', 'char'))
    times = ', '.join('epoch(%s)::BIGINT AS %s' % (c, c) for c in ('EventTime', 'ClientEventTime', 'LocalEventTime'))
    return ("COPY (SELECT * REPLACE ((EventDate - DATE '1970-01-01')::USMALLINT AS EventDate, %s, %s) FROM read_csv('%s', header = false, "
            "columns = {%s}, allow_quoted_nulls = false)) TO '%s' (FORMAT parquet);" % (times, blobs, csv, cols, out))


def used_columns():
    """the columns the 43 queries name (SELECT * of Q23 reads the heap): PostgreSQL indexes at most 32 columns, these are 25"""
    import re
    from clickbench_queries import QUERIES
    words = {w for q in QUERIES for w in re.findall(r'\b[A-Za-z_]\w*\b', q.sql)}
    return [c for c, _ in COLUMNS if c in words]


def sift_col_sql():
    cols = ', '.join(used_columns())
    return ('-- pg_sift `sift_col`: a heap table with one column index over the columns the queries use (an index has at most 32 columns), ordered by the ClickBench sort key; the projections belong to the index.\n'
            '--@ table\nCREATE EXTENSION IF NOT EXISTS pg_sift;\n%s\n'
            '--@ after\nCREATE INDEX hits_col ON hits USING sift_col (%s)\n  WITH (key_order = \'%s\');\nVACUUM (FREEZE, ANALYZE) hits;\n%s') % (
        pg_ddl(), cols, KEY_ORDER, projections_section('hits_col'))


def pg_sql():
    return ('-- stock PostgreSQL: the heap with the primary key and the ClickBench sort key as B-trees. The reference of every md5_equal.\n'
            '--@ table\n%s\n--@ after\nALTER TABLE hits ADD PRIMARY KEY (WatchID);\n'
            'CREATE INDEX hits_sort ON hits (%s);\nVACUUM (FREEZE, ANALYZE) hits;\n') % (pg_ddl(), ', '.join(SORT_KEY))


def ch_sql():
    return ('-- ClickHouse: MergeTree on the ClickBench sort key (arm `ch`) and the same table with the projections of pg_sift (arm `ch_proj`).\n'
            '--@ table\n%s\n--@ proj_table\n%s\n--@ proj_fill\nINSERT INTO hits_proj SELECT * FROM hits;\n%s\n--@ after\nOPTIMIZE TABLE hits FINAL;\n') % (
        ch_table_ddl(), ch_table_ddl('hits_proj'), '\n'.join(ch_projection_sql(p) for p in PROJECTIONS))


def duckdb_sql():
    return ('-- DuckDB: the same 105 columns in a persistent database file.\n--@ table\n%s\n') % duckdb_ddl()
