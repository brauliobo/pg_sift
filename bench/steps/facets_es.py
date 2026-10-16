"""Elasticsearch side of bench/steps/facets.sh: loads the documents, then times a terms aggregation per query.

  python3 -I bench/steps/facets_es.py URL DOCS.ndjson QUERIES.tsv SECS ROUNDS

DOCS: one JSON object per line (id, body, lo, hi). QUERIES: wl, n, mode (and|or), space separated words.
Prints `check<TAB>col<TAB>words<TAB>top10` for the first query of every workload, then `col<TAB>wl<TAB>qps`
per round and cell. One client, keep-alive connection, client-observed latency.
"""
import http.client
import json
import sys
import time
import urllib.parse

url, docs, queries, secs, rounds = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), int(sys.argv[5])
u = urllib.parse.urlparse(url)
conn = http.client.HTTPConnection(u.hostname, u.port, timeout=600)


def call(method, path, body=None, ctype="application/json"):
    conn.request(method, path, body=body.encode() if body else None, headers={"Content-Type": ctype})
    r = conn.getresponse()
    data = r.read()
    if r.status >= 300:
        raise RuntimeError(f"{method} {path}: {r.status} {data[:300]!r}")
    return json.loads(data)


call("PUT", "/docs", json.dumps({
    "settings": {"number_of_shards": 1, "number_of_replicas": 0, "refresh_interval": "-1"},
    "mappings": {"dynamic": False, "_source": {"enabled": False}, "properties": {
        "body": {"type": "text"}, "lo": {"type": "keyword"}, "hi": {"type": "keyword"}}}}))
batch = []


def flush():
    if batch:
        r = call("POST", "/docs/_bulk", "".join(batch), "application/x-ndjson")
        if r["errors"]:
            raise RuntimeError("bulk errors")
        batch.clear()


with open(docs, encoding="utf-8") as f:
    for line in f:
        batch.append('{"index":{"_id":%s}}\n' % json.dumps(json.loads(line)["id"]))
        batch.append(line)
        if len(batch) >= 10000:
            flush()
flush()
call("POST", "/docs/_refresh")
call("POST", "/docs/_forcemerge?max_num_segments=1")

cells = {}
for line in open(queries):
    wl, n, mode, words = line.rstrip("\n").split("\t")
    cells.setdefault(wl, []).append((mode, words))


def request(col, mode, words):
    return json.dumps({"size": 0, "track_total_hits": False,
                       "query": {"match": {"body": {"query": words, "operator": mode}}},
                       "aggs": {"f": {"terms": {"field": col, "size": 10}}}})


def top10(col, mode, words):
    r = call("POST", "/docs/_search?request_cache=false", request(col, mode, words))
    return " ".join(f"{b['key']}={b['doc_count']}" for b in r["aggregations"]["f"]["buckets"])


for wl, qs in cells.items():
    for col in ("lo", "hi"):
        print("check", col, wl, qs[0][1], top10(col, *qs[0]), sep="\t")
for rnd in range(rounds):
    for wl, qs in cells.items():
        for col in ("lo", "hi"):
            reqs = [request(col, *q) for q in qs]
            for r in reqs:
                call("POST", "/docs/_search?request_cache=false", r)
            n, t0 = 0, time.monotonic()
            while time.monotonic() - t0 < secs:
                call("POST", "/docs/_search?request_cache=false", reqs[n % len(reqs)])
                n += 1
            print(col, wl, f"{n / (time.monotonic() - t0):.1f}", sep="\t", flush=True)
