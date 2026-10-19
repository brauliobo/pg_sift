#!/usr/bin/env python3
"""Unit tests of the pure parts of the benchmark harness: bench/steps/benchlib.py (cache keys and rules, row replacement, comparison of two result sets) and the helpers of
bench/steps/store.py that need no server. No docker, no PostgreSQL:  python3 -I bench/test/test_benchlib.py   (or  bench/bench test)"""
import json, os, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import benchlib  # noqa: E402

FAKE = {'/proc/cpuinfo': 'processor\t: 0\nmodel name\t: Test CPU @ 3 GHz\nprocessor\t: 1\nmodel name\t: Test CPU @ 3 GHz\n', '/proc/meminfo': 'MemTotal:       16777216 kB\n',
        '/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor': 'performance\n'}


def fp(**kw):
    return benchlib.host_fingerprint(read=lambda p: FAKE.get(p, ''), docker_version='29.0', kernel='7.0-test', **kw)


class Fingerprint(unittest.TestCase):
    def test_fields(self):
        f = fp()
        self.assertEqual((f['cpu'], f['threads'], f['ram'], f['governor']), ('Test CPU @ 3 GHz', 2, '16 GiB', 'performance'))

    def test_id_changes_with_the_host(self):
        other = benchlib.host_fingerprint(read=lambda p: FAKE.get(p, ''), docker_version='29.0', kernel='7.1-test')
        self.assertNotEqual(fp()['id'], other['id'])
        self.assertEqual(fp()['id'], fp()['id'])

    def test_same_host_compares_what_both_records_have(self):
        f = fp()
        self.assertTrue(benchlib.same_host({'cpu': f['cpu'], 'threads': 2, 'kernel': '7.0-test'}, f))
        self.assertFalse(benchlib.same_host({'cpu': f['cpu'], 'kernel': 'other'}, f))
        self.assertFalse(benchlib.same_host({}, f))


class Keys(unittest.TestCase):
    def test_key_is_stable_and_sensitive(self):
        a = {'x': 1, 'y': [1, 2]}
        self.assertEqual(benchlib.key_of(a), benchlib.key_of({'y': [1, 2], 'x': 1}))
        self.assertNotEqual(benchlib.key_of(a), benchlib.key_of({'x': 1, 'y': [2, 1]}))
        self.assertEqual(len(benchlib.key_of(a)), 16)

    def test_groups_and_plan(self):
        self.assertEqual([benchlib.group_of(e) for e in ('es', 'es_cached', 'ch_mut', 'ch_rmt', 'pg')], ['es', 'es', 'ch', 'ch', 'pg'])
        self.assertFalse(benchlib.cacheable('sift_table'))
        lanes = ['pg', 'sift', 'sift_table', 'citus', 'es', 'es_cached', 'ch']
        run, untimed = benchlib.plan_engines(lanes, {'pg', 'citus', 'es', 'ch'})
        self.assertEqual((run, untimed), (['pg', 'sift', 'sift_table'], ['pg']))
        self.assertEqual(benchlib.plan_engines(lanes, set()), (lanes, []))
        self.assertEqual(benchlib.plan_engines(lanes, {'pg', 'sift', 'sift_table', 'citus', 'es', 'ch'}), ([], []))
        self.assertEqual(benchlib.plan_engines(['sift', 'es'], {'es'}), (['sift'], []))   # no PostgreSQL among the lanes: nothing to add


class CacheRules(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.now = 1_800_000_000.0
        self.fp = fp()
        self.cache = benchlib.Cache(self.tmp.name, self.fp, max_age_days=30, now=lambda: self.now)
        self.inputs = {'group': 'es', 'image_id': 'sha256:aa', 'method': {'runs': 5}}

    def tearDown(self):
        self.tmp.cleanup()

    def test_miss_then_hit(self):
        self.assertEqual(self.cache.lookup('cells', 'es', self.inputs)[0], None)
        self.cache.put('cells', 'es', self.inputs, dict(rows=[{'tier': 't'}]))
        entry, why = self.cache.lookup('cells', 'es', self.inputs)
        self.assertEqual((why, entry['rows'], entry['fp']), ('', [{'tier': 't'}], self.fp['id']))

    def test_any_input_is_part_of_the_key(self):
        self.cache.put('cells', 'es', self.inputs, dict(rows=[]))
        for changed in ({**self.inputs, 'image_id': 'sha256:bb'}, {**self.inputs, 'method': {'runs': 7}}):
            self.assertIsNone(self.cache.lookup('cells', 'es', changed)[0])

    def test_too_old_is_measured_again(self):
        self.cache.put('cells', 'es', self.inputs, dict(rows=[]))
        self.now += 31 * 86400
        entry, why = self.cache.lookup('cells', 'es', self.inputs)
        self.assertIsNone(entry)
        self.assertIn('older than 30 days', why)

    def test_other_host_is_never_used(self):
        other = benchlib.Cache(self.tmp.name, benchlib.host_fingerprint(read=lambda p: FAKE.get(p, ''), docker_version='29.0', kernel='6.9'), now=lambda: self.now)
        other.put('cells', 'es', self.inputs, dict(rows=[]))
        entry, why = self.cache.lookup('cells', 'es', self.inputs)
        self.assertIsNone(entry)
        self.assertIn('another host', why)

    def test_a_file_copied_from_another_host_is_refused(self):
        other = benchlib.Cache(self.tmp.name, benchlib.host_fingerprint(read=lambda p: FAKE.get(p, ''), docker_version='29.0', kernel='6.9'), now=lambda: self.now)
        src = other.put('cells', 'es', self.inputs, dict(rows=[]))
        os.replace(src, self.cache.path('cells', 'es', self.inputs))   # under the key of this host, with the fingerprint of the other
        with self.assertRaises(benchlib.ForeignHost):
            self.cache.lookup('cells', 'es', self.inputs)

    def test_older_method_is_not_used(self):
        p = self.cache.put('cells', 'es', self.inputs, dict(rows=[]))
        e = json.load(open(p))
        e['epoch'] = benchlib.EPOCH - 1
        json.dump(e, open(p, 'w'))
        self.assertIsNone(self.cache.lookup('cells', 'es', self.inputs)[0])

    def test_homogeneous(self):
        e = dict(name='es', key='k', date='d', fp=self.fp['id'])
        benchlib.check_homogeneous([e], self.fp)
        with self.assertRaises(benchlib.ForeignHost):
            benchlib.check_homogeneous([e, dict(e, fp='other')], self.fp)


class Rows(unittest.TestCase):
    def test_replace_rows_never_keeps_both_measurements(self):
        old = [{'tier': 't', 'engine': 'es', 'class': '05', 'v': 1}, {'tier': 't', 'engine': 'es', 'class': '06', 'v': 2}, {'tier': 't', 'engine': 'ch', 'class': '05', 'v': 3}]
        new = [{'tier': 't', 'engine': 'es', 'class': '05', 'v': 9}]
        got = benchlib.replace_rows(old, new)
        self.assertEqual(sorted((r['engine'], r['class'], r['v']) for r in got), [('ch', '05', 3), ('es', '05', 9), ('es', '06', 2)])


class Stable(unittest.TestCase):
    def test_same_story(self):
        a = {('sift', '01'): 1.0, ('pg', '01'): 10.0, ('es', '01'): 5.0}
        b = {('sift', '01'): 2.0, ('pg', '01'): 21.0, ('es', '01'): 9.0}
        r = benchlib.stable(a, b)
        self.assertTrue(r['ok'])
        self.assertEqual(r['queries']['01']['pairs'], 3)

    def test_drift_beyond_the_tolerance(self):
        r = benchlib.stable({('sift', '01'): 1.0, ('pg', '01'): 10.0}, {('sift', '01'): 1.0, ('pg', '01'): 40.0})
        self.assertFalse(r['ok'])
        self.assertEqual(r['violations'][0][-1], 'ratio')

    def test_order_flip_counts_only_when_separated(self):
        flip = benchlib.stable({('sift', '01'): 1.0, ('es', '01'): 4.0}, {('sift', '01'): 4.0, ('es', '01'): 1.0}, tol=10)
        self.assertEqual(flip['queries']['01']['flips'], 1)
        tie = benchlib.stable({('sift', '01'): 1.0, ('es', '01'): 1.2}, {('sift', '01'): 1.2, ('es', '01'): 1.0}, tol=10)
        self.assertEqual(tie['queries']['01']['flips'], 0)

    def test_pairs_restrict_the_comparison(self):
        a = {('sift', '01'): 1.0, ('pg', '01'): 10.0, ('es', '01'): 5.0}
        b = {('sift', '01'): 1.0, ('pg', '01'): 10.0, ('es', '01'): 50.0}
        self.assertTrue(benchlib.stable(a, b, pairs=[('sift', 'pg')])['ok'])
        self.assertFalse(benchlib.stable(a, b, pairs=[('sift', 'es')])['ok'])


class StoreHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import store   # reads the query files and asks quiet.sh for the pressure; starts nothing
        cls.store = store

    def test_noisy_cells_of_each_kind(self):
        meta = {'noisy': ['05: 6.9 MiB/s of other disk traffic', 'cold es 03: 1.2 cores', 'cold sift_table 06: x', 'updates ch: y', 'not a cell']}
        self.assertEqual(self.store.noisy_cells(meta), [('warm', '05'), ('cold', 'es', '03'), ('cold', 'sift_table', '06'), ('updates', 'ch')])

    def test_patch_replaces_the_cells_and_their_noise(self):
        def part(rows, noisy):
            return {'meta': {'noisy': noisy, 'rerun': []}, 'rows': rows}
        row = lambda e, c, v: {'tier': 't', 'engine': e, 'class': c, 'variant': 'warm', 'metric': 'latency_ms', 'value': v}
        with tempfile.TemporaryDirectory() as d:
            old, new = os.path.join(d, 'old.json'), os.path.join(d, 'new.json')
            json.dump(part([row('sift', '05', 9.0), row('sift', '06', 1.0)], ['05: noisy', '06: noisy too']), open(old, 'w'))
            json.dump(part([row('sift', '05', 2.0)], []), open(new, 'w'))
            args = type('A', (), dict(file=old, fresh=new))
            self.store.cmd_patch(args)
            got = json.load(open(old))
        self.assertEqual(sorted((r['class'], r['value']) for r in got['rows']), [('05', 2.0), ('06', 1.0)])
        self.assertEqual(got['meta']['noisy'], ['06: noisy too'])


if __name__ == '__main__':
    unittest.main(warnings='ignore')
