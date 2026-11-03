#!/usr/bin/env python3
"""Where the tombstones of an index with track_changes land: in the units of the index and the pages of the heap, and what the index weighs in each state of its life.

    python3 -I bench/churn/heapfree_locality.py --host DIR --port N --out FILE --mode locality|size [--rows 300000,1000000]

locality: one table per (rows, heap order of the key, layout of the index, which rows are updated, share): a covered column of the rows is updated (a new entry, a tombstone for the
old one) and the units are counted (sift._test_col_unit_tombs): units with a tombstone of a row of theirs, tombstones per such unit, units whose pages carry no tombstone at all (the
scan takes them with no work), the pages of the heap with a tombstone, and how far the new version of a row is from the old one (in pages and in superblocks of 256 pages).
size: one table per (rows, track_changes, which rows, share): the index and its log in each state (fresh, after the update, promoted, merged, vacuumed).
No timing: the numbers are counts of a deterministic run.
"""
import argparse
import subprocess

LAYOUTS = (('heap_units', "key_cut = 'off'"), ('key_cut_group', "key_cut = 'tenant_id'"), ('key_order', "key_order = 'ts'"))
PATTERNS = {'recent': 'ts > {rows} - {rows} * {share} / 100', 'random': '(hashint4(id) & 2147483647) % 10000 < {share} * 100'}

TABLE = """
SET client_min_messages = warning; SET max_parallel_workers_per_gather = 0;
CREATE EXTENSION IF NOT EXISTS pg_sift; CREATE EXTENSION IF NOT EXISTS pg_sift_test;
DROP TABLE IF EXISTS w;
CREATE TABLE w (id int PRIMARY KEY, ts int NOT NULL, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO w SELECT i, ts, i % 10, i % 1000, md5(i::text) FROM (SELECT i, {ts} AS ts FROM generate_series(1, {rows}) i) q ORDER BY i;
"""
TS = {'sorted': 'i', 'scattered': '(row_number() OVER (ORDER BY md5(i::text)))::int'}   # the heap is in the order of i either way

LOCALITY = """
CREATE INDEX w_c ON w USING sift_col (id, ts, tenant_id, counter_a) WITH (track_changes = 'id', {opts});
VACUUM (FREEZE, ANALYZE) w;
CREATE TEMP TABLE bef AS SELECT id, (ctid::text::point)[0]::int AS ob FROM w WHERE {pred};
UPDATE w SET counter_a = counter_a + 1 WHERE {pred};
CREATE TEMP TABLE mv AS SELECT b.id, b.ob, (w.ctid::text::point)[0]::int AS nb FROM bef b JOIN w USING (id);
CREATE TEMP TABLE u AS SELECT * FROM sift._test_col_unit_tombs('w_c');
SELECT '{rows}', '{order}', '{layout}', '{pattern}', {share},
	(SELECT count(*) FROM u), (SELECT bool_or(keyed) FROM u),
	(SELECT count(*) FROM mv),
	(SELECT count(*) FROM u WHERE tombs > 0),
	(SELECT round(avg(tombs) FILTER (WHERE tombs > 0), 1) FROM u), (SELECT max(tombs) FROM u),
	(SELECT count(*) FROM u WHERE pages_any = 0),
	(SELECT round(100.0 * sum(pages_any) / sum(pages), 1) FROM u),
	(SELECT round(100.0 * sum(pages_own) / sum(pages), 1) FROM u),
	(SELECT count(DISTINCT ob) FROM mv), (SELECT relpages FROM pg_class WHERE relname = 'w'),
	(SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY abs(nb - ob)) FROM mv),
	(SELECT percentile_disc(0.9) WITHIN GROUP (ORDER BY abs(nb - ob)) FROM mv),
	(SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY abs(nb / 256 - ob / 256)) FROM mv),
	(SELECT round(100.0 * count(*) FILTER (WHERE nb / 256 = ob / 256) / count(*), 1) FROM mv),
	(SELECT round(100.0 * count(*) FILTER (WHERE nb = ob) / count(*), 1) FROM mv);
"""
LOCALITY_COLUMNS = ('rows', 'heap_order', 'layout', 'updated', 'share_pct', 'units', 'listed_pages', 'updated_rows', 'units_with_own_tombstone', 'tombstones_per_such_unit', 'max_tombstones_in_a_unit',
                    'units_no_tombstone_on_pages', 'pct_unit_pages_with_any_tombstone', 'pct_unit_pages_with_own_tombstone', 'heap_pages_with_tombstone', 'heap_pages',
                    'old_new_distance_median_pages', 'old_new_distance_p90_pages', 'old_new_distance_median_superblocks', 'pct_new_in_old_superblock', 'pct_new_on_old_page')

SIZE = """
CREATE INDEX w_c ON w USING sift_col (id, ts, tenant_id, counter_a) WITH ({opts}key_cut = 'off');
VACUUM (FREEZE, ANALYZE) w;
CREATE TEMP TABLE r (state text, index_bytes bigint, log_entries bigint, segments int);
CREATE FUNCTION pg_temp.snap(s text) RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	INSERT INTO r SELECT s, pg_relation_size('w_c'), CASE WHEN {on} THEN (SELECT count(*) FROM sift._test_col_log_entries('w_c')) ELSE 0 END,
		(SELECT count(*) FROM sift.col_segments('w_c') WHERE kind = 'immutable');
END $f$;
SELECT pg_temp.snap('fresh');
UPDATE w SET counter_a = counter_a + 1 WHERE {pred};
SELECT pg_temp.snap('updated');
SELECT sift.col_promote('w_c');
SELECT pg_temp.snap('promoted');
SELECT sift.col_merge('w_c', true);
SELECT pg_temp.snap('merged');
VACUUM w;
SELECT pg_temp.snap('vacuumed');
SELECT '{rows}', '{track}', '{pattern}', {share}, state, index_bytes, log_entries, segments FROM r;
"""
SIZE_COLUMNS = ('rows', 'track_changes', 'updated', 'share_pct', 'state', 'index_bytes', 'log_entries', 'segments')


def psql(a, sql):
    r = subprocess.run(['psql', '-X', '-q', '-A', '-t', '-F', '\t', '-U', 'postgres', '-h', a.host, '-p', str(a.port), '-d', a.db, '-v', 'ON_ERROR_STOP=1'], input=sql, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return [l for l in r.stdout.splitlines() if '\t' in l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--mode', choices=('locality', 'size'), required=True)
    ap.add_argument('--rows', default='300000,1000000')
    ap.add_argument('--db', default='heapfree_locality')
    a = ap.parse_args()
    subprocess.run(['psql', '-X', '-q', '-U', 'postgres', '-h', a.host, '-p', str(a.port), '-d', 'postgres', '-c', 'DROP DATABASE IF EXISTS %s' % a.db, '-c', 'CREATE DATABASE %s' % a.db], check=True)
    with open(a.out, 'w') as out:
        out.write('\t'.join(LOCALITY_COLUMNS if a.mode == 'locality' else SIZE_COLUMNS) + '\n')
        for rows in map(int, a.rows.split(',')):
            if a.mode == 'locality':
                for order, ts in TS.items():
                    for layout, opts in LAYOUTS:
                        for pattern, pred in PATTERNS.items():
                            for share in (1, 5, 20):
                                sql = (TABLE + LOCALITY).format(rows=rows, ts=ts, order=order, layout=layout, opts=opts, pattern=pattern, share=share, pred=pred.format(rows=rows, share=share))
                                for line in psql(a, sql):
                                    out.write(line + '\n')
                                out.flush()
            else:
                for track in ('off', 'on'):
                    for pattern, pred in PATTERNS.items():
                        for share in (1, 5, 20):
                            opts = "track_changes = 'id', " if track == 'on' else ''
                            sql = (TABLE + SIZE).format(rows=rows, ts=TS['sorted'], opts=opts, on='true' if track == 'on' else 'false', track=track, pattern=pattern, share=share,
                                                        pred=pred.format(rows=rows, share=share))
                            for line in psql(a, sql):
                                out.write(line + '\n')
                            out.flush()


if __name__ == '__main__':
    main()
