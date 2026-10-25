# Elasticsearch (the release image of the store benchmark, one node) over its SQL endpoint: every column of the suites is a field (numbers long or double, dates date, text keyword), and a case runs
# its pg statement with the typed literals written the Elasticsearch way (date '..' to CAST). ES SQL has no joins and no window functions: those cases are not run, with the error it gives as the
# reason; a case with a `--@ es` section runs that text.
import csv
import http.client
import json
import os
import re
import time

from suite_engine import Server, Unsupported, image

NUMBER = {'smallint': 'long', 'integer': 'long', 'bigint': 'long', 'double precision': 'double', 'real': 'double'}


def es_type(pg):
    return NUMBER.get(pg) or ('double' if pg.startswith('numeric') else 'date' if pg.startswith(('date', 'timestamp')) else 'keyword')


def dialect(sql):
    """PostgreSQL's typed literals in Elasticsearch SQL: date '1994-01-01' is CAST('1994-01-01' AS DATE), and a number of days added to it an INTERVAL"""
    sql = re.sub(r"\b(date|timestamp)\s+'([^']+)'", lambda m: "CAST('%s' AS %s)" % (m.group(2), m.group(1).upper()), sql)
    return re.sub(r"(CAST\('[^']+' AS DATE\))\s*([-+])\s*(\d+)\b", r"\1 \2 INTERVAL \3 DAY", sql)


class Engine(Server):
    section = 'es'

    def __init__(self, ctx):
        super().__init__(ctx)
        self.conn = None

    def up(self):
        c = self.ctx.cpus
        heap = max(1, int(re.match(r'\d+', self.ctx.memory).group()) // 4)
        self.docker_run(9200, image('STORE_ES_IMAGE'), env=['discovery.type=single-node', 'xpack.security.enabled=false', 'xpack.ml.enabled=false', 'node.processors=%d' % c,
                                                      'ES_JAVA_OPTS=-Xms%dg -Xmx%dg -XX:ActiveProcessorCount=%d' % (heap, heap, c)])
        for _ in range(180):
            try:
                if json.loads(self.request('GET', '/_cluster/health?wait_for_status=yellow&timeout=1s')).get('status') in ('yellow', 'green'):
                    return
            except (OSError, ValueError, RuntimeError, http.client.HTTPException):
                self.conn = None
            time.sleep(1)
        raise SystemExit('generic: Elasticsearch did not start (docker logs %s)' % self.name)

    def request(self, method, path, body=None, ctype='application/json'):
        for attempt in (0, 1):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection('127.0.0.1', self.ctx.port, timeout=600)
                self.conn.request(method, path, body=body, headers={'Content-Type': ctype})
                r = self.conn.getresponse()
                data = r.read().decode(errors='replace')
                if r.status >= 400:
                    raise RuntimeError(data)
                return data
            except (http.client.HTTPException, OSError) as e:
                self.conn = None
                if attempt:
                    raise RuntimeError(str(e))

    def load(self, suite, work):
        schema = json.load(open(os.path.join(work, suite, 'schema.json')))
        for table, cols in schema.items():
            kinds = [es_type(t) for _, t in cols]
            try:
                self.request('DELETE', '/' + table)   # a retried request after a reset connection must not find its own index
            except RuntimeError:
                pass
            self.request('PUT', '/' + table, json.dumps({'settings': {'number_of_shards': 1, 'number_of_replicas': 0, 'refresh_interval': -1},
                                                         'mappings': {'properties': {n: {'type': k} for (n, _), k in zip(cols, kinds)}}}))
            batch = []

            def flush():
                if batch:
                    out = json.loads(self.request('POST', '/_bulk', ''.join(batch), 'application/x-ndjson'))
                    if out.get('errors'):
                        raise SystemExit('generic: Elasticsearch refused rows of %s: %s' % (table, json.dumps(out['items'][0])[:300]))
                    batch.clear()
            with open(os.path.join(work, suite, table + '.csv'), newline='') as f:
                for row in csv.reader(f):
                    doc = {n: (int(v) if k == 'long' else float(v) if k == 'double' else v.replace(' ', 'T') if k == 'date' else v) for (n, _), k, v in zip(cols, kinds, row) if v != '' or k == 'keyword'}   # the suites have no NULL: an empty text is an empty keyword
                    batch.append('{"index":{"_index":"%s"}}\n%s\n' % (table, json.dumps(doc)))
                    if len(batch) >= 10000:
                        flush()
            flush()
            self.request('POST', '/%s/_refresh' % table)
            self.request('POST', '/%s/_forcemerge?max_num_segments=1' % table)

    def unload(self, suite, work):
        for table in json.load(open(os.path.join(work, suite, 'schema.json'))):
            self.request('DELETE', '/' + table)

    def approximate(self, sql):
        return 'count(distinct' in sql.lower()   # cardinality aggregation: a HyperLogLog++ sketch, not an exact count

    def statement(self, case, arm):
        return dialect(super().statement(case, arm))

    def runner(self, arm, sql):
        def go():
            t = time.perf_counter()
            try:
                out = json.loads(self.request('POST', '/_sql?format=json', json.dumps({'query': sql.rstrip(';'), 'fetch_size': 10000})))
            except RuntimeError as e:
                m = re.search(r'"reason":\s*"([^"]+)"', str(e))
                raise Unsupported('Elasticsearch SQL: ' + (m.group(1) if m else str(e))[:200])
            return (time.perf_counter() - t) * 1000, [[('' if v is None else str(v)) for v in r] for r in out['rows']]
        return go
